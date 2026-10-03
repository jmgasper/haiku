/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Broadcom V3D 4.2, the 3D core of the BCM2711 (Raspberry Pi 4).

	This is the render-only device that Mesa's v3d and v3dv drivers talk to
	(see <graphics/v3d/v3d_haiku.h>); scan-out is somebody else's business.

	The core works in a 4 GB address space of its own, through an MMU with a
	flat page table. Buffer objects are areas of locked, uncached RAM with a
	range of that address space; user space gets them cloned into its team.

	Jobs (a binner and a render control list, a texture formatting job, a
	compute dispatch) run one at a time, in the order they were submitted, on
	one executor thread. That makes every ordering question trivial -- a job
	sees the results of all jobs before it -- at the price of not binning the
	next frame while the previous one renders. Each job has a sequence
	number; waiting for a buffer or a sync object means waiting for a number.

	Hardware handling follows Linux' v3d driver (drivers/gpu/drm/v3d).

	The device tree's node is "disabled" unless the firmware was told to hand
	the display pipeline to the OS (vc4-kms-v3d); the core is there all the
	same and the driver ignores the status. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <new>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include <condition_variable.h>
#include <kernel.h>
#include <lock.h>
#include <rpi_firmware.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <graphics/v3d/v3d_drm.h>
#include <graphics/v3d/v3d_haiku.h>

#include "v3d_regs.h"


#define INFO(x...)	dprintf("v3d: " x)
#define ERROR(x...)	dprintf("v3d: " x)

#define V3D_DRIVER_MODULE_NAME	"drivers/graphics/v3d/driver_v1"
#define V3D_DEVICE_MODULE_NAME	"drivers/graphics/v3d/device_v1"

// The power management block is not a child of the V3D node and the FDT
// bus offers no way to walk to it; these are its addresses on the BCM2711.
#define BCM2711_PM_BASE			0xfe100000
#define BCM2711_RPIVID_ASB_BASE	0xfec11000


#define V3D_PAGE_SHIFT			12
#define V3D_PAGE_COUNT			(1u << 20)		// 4 GB of GPU address space
#define V3D_OVERFLOW_SIZE		(256 * 1024)
#define V3D_MAX_BO_SIZE			(512u << 20)
#define V3D_MAX_JOB_BOS			4096

// events from the interrupt handler: the core's interrupt bits as they are,
// the hub's moved up
#define EVENT_TFU_DONE			(1 << 16)
#define EVENT_MMU_ERROR			(1 << 17)

enum {
	JOB_CL,
	JOB_TFU,
	JOB_CSD
};

struct v3d_bo {
	v3d_bo*		next;		// in the device's list, sorted by page
	area_id		area;
	uint8*		address;
	size_t		size;
	uint32		page;		// first page in the GPU's address space
	uint32		pageCount;
	int32		references;	// handles and jobs
	uint64		lastJob;
};

struct v3d_job {
	v3d_job*	next;
	uint32		type;
	union {
		drm_v3d_submit_cl	cl;
		drm_v3d_submit_tfu	tfu;
		drm_v3d_submit_csd	csd;
	};
	v3d_bo**	bos;
	uint32		boCount;
	uint64		seqno;
};

struct v3d_info {
	device_node*	node;
	uint64			hubBase;
	uint64			hubSize;
	uint64			coreBase;
	uint64			coreSize;
	uint32			interrupt;

	area_id			hubArea;
	area_id			coreArea;
	area_id			pmArea;
	area_id			asbArea;
	volatile uint8*	hub;
	volatile uint8*	core;
	volatile uint8*	pm;
	volatile uint8*	asb;

	uint32			version;	// 42 for V3D 4.2
	uint32			clockRate;
	bool			interruptInstalled;

	mutex			lock;
		// buffers, address space, page table, handles, the job queue

	area_id			pageTableArea;
	uint32*			pageTable;
	phys_addr_t		pageTableAddress;
	area_id			scratchArea;
	phys_addr_t		scratchAddress;
	v3d_bo*			buffers;

	v3d_job*		firstJob;
	v3d_job*		lastJob;
	uint64			submitted;
	uint64			completed;
	sem_id			jobSemaphore;
	thread_id		executor;
	bool			stopping;
	ConditionVariable completedCondition;

	int32			events;
	ConditionVariable eventCondition;
	uint32			resets;
};

struct v3d_file {
	v3d_info*		device;
	team_id			team;
	v3d_bo**		buffers;	// handle - 1 is the index
	uint32			bufferCount;
	uint64*			syncs;		// sequence number + 1; 0: free
	uint32			syncCount;
};


static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;


static inline uint32
read32(volatile uint8* base, uint32 reg)
{
	return *(volatile uint32*)(base + reg);
}


static inline void
write32(volatile uint8* base, uint32 reg, uint32 value)
{
	*(volatile uint32*)(base + reg) = value;
}


static status_t
map(const char* name, uint64 base, uint64 size, area_id& area,
	volatile uint8*& address)
{
	uint64 offset = base & (B_PAGE_SIZE - 1);
	void* mapped;
	area = map_physical_memory(name, base - offset,
		ROUNDUP(size + offset, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapped);
	if (area < 0)
		return area;

	address = (volatile uint8*)mapped + offset;
	return B_OK;
}


static status_t
asb_enable(v3d_info* info, uint32 reg)
{
	write32(info->asb, reg,
		PM_PASSWORD | (read32(info->asb, reg) & ~ASB_REQ_STOP));

	bigtime_t timeout = system_time() + 10000;
	while ((read32(info->asb, reg) & ASB_ACK) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(5);
	}
	return B_OK;
}


/*!	The V3D power domain: take the core out of reset with its clock running
	and open the bus bridges to it (Linux: bcm2835_asb_power_on()).
*/
static status_t
power_on(v3d_info* info)
{
	// The firmware owns the clock.
	uint32 maximum = 0;
	sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_V3D, true, &maximum);

	status_t status = sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, true);
	if (status != B_OK)
		return status;
	spin(1);
	sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, false);

	write32(info->pm, PM_GRAFX,
		PM_PASSWORD | read32(info->pm, PM_GRAFX) | PM_V3DRSTN);

	status = sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, true);
	if (status != B_OK)
		return status;
	if (maximum != 0)
		sFirmware->set_clock_rate(RPI_FIRMWARE_CLOCK_V3D, maximum);
	sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_V3D, false, &info->clockRate);

	status = asb_enable(info, ASB_V3D_M_CTRL);
	if (status == B_OK)
		status = asb_enable(info, ASB_V3D_S_CTRL);
	if (status != B_OK)
		ERROR("the bus bridges to the core do not open\n");
	return status;
}


//	#pragma mark - MMU and buffers


static status_t
make_uncached(area_id area, void* address, size_t size)
{
	// The core reads and writes RAM behind the CPU caches' back. Take what
	// the allocation left in the cache out, and keep the memory out of it:
	// clones of the area inherit the memory type.
	for (addr_t line = (addr_t)address; line < (addr_t)address + size;
			line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();
	return vm_set_area_memory_type(area, 0, B_WRITE_COMBINING_MEMORY);
}


static status_t
mmu_flush(v3d_info* info)
{
	// let a flush in progress finish
	bigtime_t timeout = system_time() + 100000;
	while ((read32(info->hub, V3D_MMUC_CONTROL) & V3D_MMUC_CONTROL_FLUSHING)
			!= 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}

	write32(info->hub, V3D_MMUC_CONTROL,
		V3D_MMUC_CONTROL_FLUSH | V3D_MMUC_CONTROL_ENABLE);
	while ((read32(info->hub, V3D_MMUC_CONTROL) & V3D_MMUC_CONTROL_FLUSHING)
			!= 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}

	write32(info->hub, V3D_MMU_CTL,
		read32(info->hub, V3D_MMU_CTL) | V3D_MMU_CTL_TLB_CLEAR);
	while ((read32(info->hub, V3D_MMU_CTL) & V3D_MMU_CTL_TLB_CLEARING) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}

	return B_OK;
}


static void
mmu_enable(v3d_info* info)
{
	write32(info->hub, V3D_MMU_PT_PA_BASE,
		info->pageTableAddress >> V3D_PAGE_SHIFT);
	write32(info->hub, V3D_MMU_CTL, V3D_MMU_CTL_ENABLE
		| V3D_MMU_CTL_PT_INVALID_ENABLE | V3D_MMU_CTL_PT_INVALID_ABORT
		| V3D_MMU_CTL_PT_INVALID_INT | V3D_MMU_CTL_WRITE_VIOLATION_ABORT
		| V3D_MMU_CTL_WRITE_VIOLATION_INT | V3D_MMU_CTL_CAP_EXCEEDED_ABORT
		| V3D_MMU_CTL_CAP_EXCEEDED_INT);
	write32(info->hub, V3D_MMU_ILLEGAL_ADDR,
		(info->scratchAddress >> V3D_PAGE_SHIFT) | V3D_MMU_ILLEGAL_ADDR_ENABLE);
	write32(info->hub, V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_ENABLE);

	if (mmu_flush(info) != B_OK)
		ERROR("MMU flush timed out\n");
}


/*!	Creates a buffer with its range of GPU address space and its page table
	entries. Called with the device locked. The buffer starts with one
	reference.
*/
static status_t
create_buffer(v3d_info* info, size_t size, v3d_bo*& _buffer)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	if (size == 0 || size > V3D_MAX_BO_SIZE)
		return B_BAD_VALUE;

	v3d_bo* buffer = (v3d_bo*)calloc(1, sizeof(v3d_bo));
	if (buffer == NULL)
		return B_NO_MEMORY;

	buffer->size = size;
	buffer->pageCount = size >> V3D_PAGE_SHIFT;
	buffer->references = 1;

	// first fit in the address space; page 0 stays unmapped
	uint32 page = 1;
	v3d_bo** link = &info->buffers;
	while (*link != NULL && (*link)->page - page < buffer->pageCount) {
		page = (*link)->page + (*link)->pageCount;
		link = &(*link)->next;
	}
	if (page + buffer->pageCount > V3D_PAGE_COUNT) {
		free(buffer);
		return B_NO_MEMORY;
	}
	buffer->page = page;

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	buffer->area = create_area_etc(B_SYSTEM_TEAM, "v3d buffer", size,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions,
		(void**)&buffer->address);
	if (buffer->area < 0) {
		status_t status = buffer->area;
		free(buffer);
		return status;
	}

	// the page table entries, run by run of the physical memory
	size_t offset = 0;
	while (offset < size) {
		physical_entry entry;
		status_t status = get_memory_map(buffer->address + offset,
			size - offset, &entry, 1);
		if ((status != B_OK && status != B_BUFFER_OVERFLOW)
			|| entry.size == 0) {
			delete_area(buffer->area);
			free(buffer);
			return B_ERROR;
		}

		for (size_t i = 0; i < entry.size; i += B_PAGE_SIZE) {
			info->pageTable[page++] = V3D_PTE_VALID | V3D_PTE_WRITEABLE
				| (uint32)((entry.address + i) >> V3D_PAGE_SHIFT);
		}
		offset += entry.size;
	}
	memory_full_barrier();

	status_t status = make_uncached(buffer->area, buffer->address, size);
	if (status != B_OK) {
		for (uint32 i = 0; i < buffer->pageCount; i++)
			info->pageTable[buffer->page + i] = 0;
		delete_area(buffer->area);
		free(buffer);
		return status;
	}

	if (mmu_flush(info) != B_OK)
		ERROR("MMU flush timed out\n");

	buffer->next = *link;
	*link = buffer;

	_buffer = buffer;
	return B_OK;
}


/*!	Drops a reference; the last one frees the buffer. Device locked. */
static void
put_buffer(v3d_info* info, v3d_bo* buffer)
{
	if (--buffer->references > 0)
		return;

	for (v3d_bo** link = &info->buffers; *link != NULL;
			link = &(*link)->next) {
		if (*link == buffer) {
			*link = buffer->next;
			break;
		}
	}

	for (uint32 i = 0; i < buffer->pageCount; i++)
		info->pageTable[buffer->page + i] = 0;
	memory_full_barrier();
	if (mmu_flush(info) != B_OK)
		ERROR("MMU flush timed out\n");

	delete_area(buffer->area);
	free(buffer);
}


static v3d_bo*
lookup_buffer(v3d_file* file, uint32 handle)
{
	if (handle == 0 || handle > file->bufferCount)
		return NULL;
	return file->buffers[handle - 1];
}


//	#pragma mark - hardware


static void
invalidate_caches(v3d_info* info)
{
	// from the outside in: the texture cache, then the slices' caches
	write32(info->core, V3D_CTL_L2TCACTL,
		V3D_L2TCACTL_L2TFLS | V3D_L2TCACTL_FLM_FLUSH);
	write32(info->core, V3D_CTL_SLCACTL, 0x0f0f0f0f);
}


/*!	Writes back what a job wrote through the texture units, for the CPU to
	read (requested by the job).
*/
static void
clean_caches(v3d_info* info)
{
	bigtime_t timeout = system_time() + 100000;
	while ((read32(info->core, V3D_CTL_L2TCACTL) & V3D_L2TCACTL_L2TFLS) != 0
		&& system_time() < timeout) {
		spin(10);
	}

	write32(info->core, V3D_CTL_L2TCACTL, V3D_L2TCACTL_TMUWCF);
	while ((read32(info->core, V3D_CTL_L2TCACTL) & V3D_L2TCACTL_TMUWCF) != 0
		&& system_time() < timeout) {
		spin(10);
	}

	write32(info->core, V3D_CTL_L2TCACTL,
		V3D_L2TCACTL_L2TFLS | V3D_L2TCACTL_FLM_CLEAN);
	while ((read32(info->core, V3D_CTL_L2TCACTL) & V3D_L2TCACTL_L2TFLS) != 0
		&& system_time() < timeout) {
		spin(10);
	}
}


static void
init_hardware(v3d_info* info)
{
	// a texture cache flush always covers everything
	write32(info->core, V3D_CTL_L2TFLSTA, 0);
	write32(info->core, V3D_CTL_L2TFLEND, 0xffffffff);

	mmu_enable(info);

	// interrupts: clear what is pending, unmask what the driver handles
	const uint32 coreInterrupts = V3D_INT_OUTOMEM | V3D_INT_FLDONE
		| V3D_INT_FRDONE | V3D_INT_CSDDONE | V3D_INT_GMPV;
	const uint32 hubInterrupts = V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI
		| V3D_HUB_INT_MMU_CAP | V3D_HUB_INT_TFUC;

	write32(info->core, V3D_CTL_INT_MSK_SET, 0xffffffff);
	write32(info->hub, V3D_HUB_INT_MSK_SET, 0xffffffff);
	write32(info->core, V3D_CTL_INT_CLR, 0xffffffff);
	write32(info->hub, V3D_HUB_INT_CLR, 0xffffffff);
	write32(info->core, V3D_CTL_INT_MSK_CLR, coreInterrupts);
	write32(info->hub, V3D_HUB_INT_MSK_CLR, hubInterrupts);
}


/*!	After a job that never finished: reset the core through its power
	domain's reset line and set it up again.
*/
static void
reset_hardware(v3d_info* info)
{
	ERROR("resetting the core; error status %#" B_PRIx32 ", bin at %#"
		B_PRIx32 ", render at %#" B_PRIx32 "\n",
		read32(info->core, V3D_ERR_STAT), read32(info->core, V3D_CLE_CT0CA),
		read32(info->core, V3D_CLE_CT1CA));

	write32(info->core, V3D_CTL_INT_MSK_SET, 0xffffffff);
	write32(info->hub, V3D_HUB_INT_MSK_SET, 0xffffffff);

	write32(info->pm, PM_GRAFX,
		PM_PASSWORD | (read32(info->pm, PM_GRAFX) & ~PM_V3DRSTN));
	spin(10);
	write32(info->pm, PM_GRAFX,
		PM_PASSWORD | read32(info->pm, PM_GRAFX) | PM_V3DRSTN);
	spin(10);

	init_hardware(info);
	info->resets++;
}


static int32
v3d_interrupt(void* data)
{
	v3d_info* info = (v3d_info*)data;
	uint32 events = 0;

	uint32 status = read32(info->core, V3D_CTL_INT_STS);
	if (status != 0) {
		write32(info->core, V3D_CTL_INT_CLR, status);
		events |= status & (V3D_INT_OUTOMEM | V3D_INT_FLDONE | V3D_INT_FRDONE
			| V3D_INT_CSDDONE);
		if ((status & V3D_INT_GMPV) != 0)
			dprintf("v3d: memory protection violation\n");
	}

	uint32 hubStatus = read32(info->hub, V3D_HUB_INT_STS);
	if (hubStatus != 0) {
		write32(info->hub, V3D_HUB_INT_CLR, hubStatus);
		if ((hubStatus & V3D_HUB_INT_TFUC) != 0)
			events |= EVENT_TFU_DONE;
		if ((hubStatus & (V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI
				| V3D_HUB_INT_MMU_CAP)) != 0) {
			dprintf("v3d: MMU error at %#" B_PRIx32 "000 (AXI id %#" B_PRIx32
				"):%s%s%s\n", read32(info->hub, V3D_MMU_VIO_ADDR),
				read32(info->hub, V3D_MMU_VIO_ID) & 0xff,
				(hubStatus & V3D_HUB_INT_MMU_WRV) != 0 ? " write violation" : "",
				(hubStatus & V3D_HUB_INT_MMU_PTI) != 0 ? " invalid entry" : "",
				(hubStatus & V3D_HUB_INT_MMU_CAP) != 0 ? " out of range" : "");
			// writing the status bits back acknowledges them
			write32(info->hub, V3D_MMU_CTL, read32(info->hub, V3D_MMU_CTL));
			events |= EVENT_MMU_ERROR;
		}
	}

	if (status == 0 && hubStatus == 0)
		return B_UNHANDLED_INTERRUPT;

	if (events != 0) {
		atomic_or(&info->events, events);
		info->eventCondition.NotifyAll();
	}
	return B_HANDLED_INTERRUPT;
}


/*!	Waits for one of the events in \a mask; returns the ones that came and
	takes them off the pending set.
*/
static uint32
wait_for_event(v3d_info* info, uint32 mask, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		ConditionVariableEntry entry;
		info->eventCondition.Add(&entry);

		uint32 events = atomic_get(&info->events) & mask;
		if (events != 0) {
			atomic_and(&info->events, ~events);
			return events;
		}

		if (entry.Wait(B_ABSOLUTE_TIMEOUT, deadline) == B_TIMED_OUT) {
			events = atomic_get(&info->events) & mask;
			atomic_and(&info->events, ~events);
			return events;
		}
	}
}


//	#pragma mark - jobs


static status_t
run_cl(v3d_info* info, v3d_job* job, v3d_bo*& overflow)
{
	const drm_v3d_submit_cl& args = job->cl;

	if (args.bcl_start != args.bcl_end) {
		atomic_set(&info->events, 0);

		// no overflow memory left over from the job before
		write32(info->core, V3D_PTB_BPOS, 0);
		invalidate_caches(info);

		if (args.qma != 0) {
			write32(info->core, V3D_CLE_CT0QMA, args.qma);
			write32(info->core, V3D_CLE_CT0QMS, args.qms);
		}
		if (args.qts != 0) {
			write32(info->core, V3D_CLE_CT0QTS,
				V3D_CLE_CT0QTS_ENABLE | args.qts);
		}
		// writing the end address starts the list
		write32(info->core, V3D_CLE_CT0QBA, args.bcl_start);
		write32(info->core, V3D_CLE_CT0QEA, args.bcl_end);

		while (true) {
			uint32 events = wait_for_event(info,
				V3D_INT_FLDONE | V3D_INT_OUTOMEM | EVENT_MMU_ERROR, 3000000);
			if ((events & V3D_INT_FLDONE) != 0)
				break;
			if ((events & V3D_INT_OUTOMEM) != 0) {
				// The binner ran out of memory for its tile lists: give
				// it more. The render list still needs it; the job keeps
				// it until it is done.
				MutexLocker locker(info->lock);
				v3d_bo* more;
				if (create_buffer(info, V3D_OVERFLOW_SIZE, more) != B_OK)
					return B_NO_MEMORY;
				more->lastJob = (addr_t)overflow;
					// chained through an otherwise unused field
				overflow = more;
				locker.Unlock();

				write32(info->core, V3D_PTB_BPOA,
					more->page << V3D_PAGE_SHIFT);
				write32(info->core, V3D_PTB_BPOS, more->size);
				continue;
			}
			return (events & EVENT_MMU_ERROR) != 0 ? B_BAD_ADDRESS
				: B_TIMED_OUT;
		}
	}

	atomic_set(&info->events, 0);
	invalidate_caches(info);
	write32(info->core, V3D_CLE_CT1QBA, args.rcl_start);
	write32(info->core, V3D_CLE_CT1QEA, args.rcl_end);

	uint32 events = wait_for_event(info, V3D_INT_FRDONE | EVENT_MMU_ERROR,
		5000000);
	if ((events & V3D_INT_FRDONE) == 0)
		return (events & EVENT_MMU_ERROR) != 0 ? B_BAD_ADDRESS : B_TIMED_OUT;

	if ((args.flags & DRM_V3D_SUBMIT_CL_FLUSH_CACHE) != 0)
		clean_caches(info);
	return B_OK;
}


static status_t
run_tfu(v3d_info* info, v3d_job* job)
{
	const drm_v3d_submit_tfu& args = job->tfu;

	atomic_set(&info->events, 0);
	write32(info->hub, V3D_TFU_IIA, args.iia);
	write32(info->hub, V3D_TFU_IIS, args.iis);
	write32(info->hub, V3D_TFU_ICA, args.ica);
	write32(info->hub, V3D_TFU_IUA, args.iua);
	write32(info->hub, V3D_TFU_IOA, args.ioa);
	write32(info->hub, V3D_TFU_IOS, args.ios);
	write32(info->hub, V3D_TFU_COEF0, args.coef[0]);
	if ((args.coef[0] & V3D_TFU_COEF0_USECOEF) != 0) {
		write32(info->hub, V3D_TFU_COEF1, args.coef[1]);
		write32(info->hub, V3D_TFU_COEF2, args.coef[2]);
		write32(info->hub, V3D_TFU_COEF3, args.coef[3]);
	}
	// the configuration register starts the job
	write32(info->hub, V3D_TFU_ICFG, args.icfg | V3D_TFU_ICFG_IOC);

	uint32 events = wait_for_event(info, EVENT_TFU_DONE | EVENT_MMU_ERROR,
		3000000);
	if ((events & EVENT_TFU_DONE) == 0)
		return (events & EVENT_MMU_ERROR) != 0 ? B_BAD_ADDRESS : B_TIMED_OUT;
	return B_OK;
}


static status_t
run_csd(v3d_info* info, v3d_job* job)
{
	const drm_v3d_submit_csd& args = job->csd;

	// A workgroup count of 0 would mean 65536 to the hardware; to user
	// space it means there is nothing to dispatch.
	if ((args.cfg[0] >> 16) == 0 || (args.cfg[1] >> 16) == 0
		|| (args.cfg[2] >> 16) == 0) {
		return B_OK;
	}

	atomic_set(&info->events, 0);
	invalidate_caches(info);
	for (int i = 1; i <= 6; i++)
		write32(info->core, V3D_CSD_QUEUED_CFG0 + 4 * i, args.cfg[i]);
	// the first configuration register starts the dispatch
	write32(info->core, V3D_CSD_QUEUED_CFG0, args.cfg[0]);

	uint32 events = wait_for_event(info, V3D_INT_CSDDONE | EVENT_MMU_ERROR,
		10000000);
	if ((events & V3D_INT_CSDDONE) == 0)
		return (events & EVENT_MMU_ERROR) != 0 ? B_BAD_ADDRESS : B_TIMED_OUT;

	clean_caches(info);
	return B_OK;
}


static status_t
executor_thread(void* data)
{
	v3d_info* info = (v3d_info*)data;

	while (true) {
		if (acquire_sem(info->jobSemaphore) != B_OK || info->stopping)
			break;

		MutexLocker locker(info->lock);
		v3d_job* job = info->firstJob;
		if (job == NULL)
			continue;
		info->firstJob = job->next;
		if (info->firstJob == NULL)
			info->lastJob = NULL;
		locker.Unlock();

		v3d_bo* overflow = NULL;
		status_t status;
		switch (job->type) {
			case JOB_CL:
				status = run_cl(info, job, overflow);
				break;
			case JOB_TFU:
				status = run_tfu(info, job);
				break;
			default:
				status = run_csd(info, job);
				break;
		}

		if (status != B_OK) {
			ERROR("job %" B_PRIu64 " (type %" B_PRIu32 ") failed: %s\n",
				job->seqno, job->type, strerror(status));
			reset_hardware(info);
		}

		locker.Lock();
		while (overflow != NULL) {
			v3d_bo* next = (v3d_bo*)(addr_t)overflow->lastJob;
			put_buffer(info, overflow);
			overflow = next;
		}
		for (uint32 i = 0; i < job->boCount; i++)
			put_buffer(info, job->bos[i]);
		info->completed = job->seqno;
		locker.Unlock();

		info->completedCondition.NotifyAll();
		free(job->bos);
		free(job);
	}

	return B_OK;
}


/*!	Waits until job \a seqno is done. \a timeout is relative, in
	nanoseconds; negative waits for good.
*/
static status_t
wait_for_job(v3d_info* info, uint64 seqno, int64 timeout)
{
	bigtime_t deadline = timeout < 0
		? B_INFINITE_TIMEOUT : system_time() + timeout / 1000;

	while (true) {
		ConditionVariableEntry entry;
		info->completedCondition.Add(&entry);

		mutex_lock(&info->lock);
		bool done = info->completed >= seqno;
		mutex_unlock(&info->lock);
		if (done)
			return B_OK;
		if (timeout == 0)
			return B_WOULD_BLOCK;

		status_t status = entry.Wait(
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, deadline);
		if (status == B_TIMED_OUT || status == B_INTERRUPTED)
			return status;
	}
}


/*!	Queues \a job with the buffers that \a handles name. Takes over \a job
	in any case. Returns the job's sequence number in \a _seqno.
*/
static status_t
submit_job(v3d_file* file, v3d_job* job, const uint32* handles, uint32 count,
	uint32 outSync)
{
	v3d_info* info = file->device;

	job->bos = (v3d_bo**)calloc(std::max(count, (uint32)1), sizeof(v3d_bo*));
	if (job->bos == NULL) {
		free(job);
		return B_NO_MEMORY;
	}

	MutexLocker locker(info->lock);

	if (outSync != 0 && (outSync > file->syncCount
			|| file->syncs[outSync - 1] == 0)) {
		free(job->bos);
		free(job);
		return B_BAD_VALUE;
	}

	for (uint32 i = 0; i < count; i++) {
		if (handles[i] == 0)
			continue;
		v3d_bo* buffer = lookup_buffer(file, handles[i]);
		if (buffer == NULL) {
			for (uint32 j = 0; j < job->boCount; j++)
				job->bos[j]->references--;
			free(job->bos);
			free(job);
			return B_BAD_VALUE;
		}
		buffer->references++;
		job->bos[job->boCount++] = buffer;
	}

	job->seqno = ++info->submitted;
	for (uint32 i = 0; i < job->boCount; i++)
		job->bos[i]->lastJob = job->seqno;
	if (outSync != 0)
		file->syncs[outSync - 1] = job->seqno + 1;

	job->next = NULL;
	if (info->lastJob != NULL)
		info->lastJob->next = job;
	else
		info->firstJob = job;
	info->lastJob = job;
	locker.Unlock();

	release_sem(info->jobSemaphore);
	return B_OK;
}


//	#pragma mark - device


static status_t
v3d_init_device(void* _info, void** _cookie)
{
	v3d_info* info = (v3d_info*)_info;

	status_t status = map("v3d hub", info->hubBase, info->hubSize,
		info->hubArea, info->hub);
	if (status == B_OK) {
		status = map("v3d core", info->coreBase, info->coreSize,
			info->coreArea, info->core);
	}
	if (status == B_OK) {
		status = map("v3d pm", BCM2711_PM_BASE, 0x200, info->pmArea,
			info->pm);
	}
	if (status == B_OK) {
		status = map("v3d asb", BCM2711_RPIVID_ASB_BASE, 0x24, info->asbArea,
			info->asb);
	}
	if (status != B_OK)
		return status;

	status = power_on(info);
	if (status != B_OK) {
		ERROR("power on failed: %s\n", strerror(status));
		return status;
	}

	uint32 hubIdent0 = read32(info->hub, V3D_HUB_IDENT0);
	uint32 hubIdent1 = read32(info->hub, V3D_HUB_IDENT1);
	uint32 hubIdent2 = read32(info->hub, V3D_HUB_IDENT2);
	uint32 hubIdent3 = read32(info->hub, V3D_HUB_IDENT3);
	uint32 coreIdent0 = read32(info->core, V3D_CTL_IDENT0);

	// "VHUB" and "V3D" plus the technology version
	if (hubIdent0 != ('V' | 'H' << 8 | 'U' << 16 | 'B' << 24)
		|| (coreIdent0 & 0x00ffffff) != ('V' | '3' << 8 | 'D' << 16)) {
		ERROR("no V3D behind the registers: hub %#" B_PRIx32 ", core %#"
			B_PRIx32 "\n", hubIdent0, coreIdent0);
		return B_DEVICE_NOT_FOUND;
	}

	info->version = V3D_HUB_IDENT1_TVER(hubIdent1) * 10
		+ V3D_HUB_IDENT1_REV(hubIdent1);

	mutex_init(&info->lock, "v3d");
	info->completedCondition.Init(info, "v3d job done");
	info->eventCondition.Init(info, "v3d event");

	// the page table: one word per page of the 4 GB, in uncached memory
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	size_t tableSize = V3D_PAGE_COUNT * sizeof(uint32);
	info->pageTableArea = create_area_etc(B_SYSTEM_TEAM, "v3d page table",
		tableSize, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0,
		0, &virtualRestrictions, &physicalRestrictions,
		(void**)&info->pageTable);
	if (info->pageTableArea < 0)
		return info->pageTableArea;

	void* scratch;
	info->scratchArea = create_area_etc(B_SYSTEM_TEAM, "v3d scratch page",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0,
		0, &virtualRestrictions, &physicalRestrictions, &scratch);
	if (info->scratchArea < 0)
		return info->scratchArea;

	physical_entry entry;
	get_memory_map(info->pageTable, tableSize, &entry, 1);
	info->pageTableAddress = entry.address;
	get_memory_map(scratch, B_PAGE_SIZE, &entry, 1);
	info->scratchAddress = entry.address;

	make_uncached(info->pageTableArea, info->pageTable, tableSize);
	make_uncached(info->scratchArea, scratch, B_PAGE_SIZE);

	info->jobSemaphore = create_sem(0, "v3d jobs");
	info->stopping = false;

	status = install_io_interrupt_handler(info->interrupt, v3d_interrupt,
		info, 0);
	if (status != B_OK)
		return status;
	info->interruptInstalled = true;

	init_hardware(info);

	info->executor = spawn_kernel_thread(executor_thread, "v3d executor",
		B_DISPLAY_PRIORITY, info);
	resume_thread(info->executor);

	INFO("V3D %" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 ", %"
		B_PRIu32 " core(s)%s%s, clock %" B_PRIu32 " Hz, interrupt %" B_PRIu32
		"\n", info->version / 10, info->version % 10, (hubIdent3 >> 8) & 0xff,
		hubIdent3 & 0xff, V3D_HUB_IDENT1_NCORES(hubIdent1),
		(hubIdent1 & V3D_HUB_IDENT1_WITH_TFU) != 0 ? ", TFU" : "",
		(hubIdent2 & V3D_HUB_IDENT2_WITH_MMU) != 0 ? ", MMU" : "",
		info->clockRate, info->interrupt);

	*_cookie = info;
	return B_OK;
}


static void
v3d_uninit_device(void* cookie)
{
	v3d_info* info = (v3d_info*)cookie;

	info->stopping = true;
	delete_sem(info->jobSemaphore);
	status_t result;
	wait_for_thread(info->executor, &result);

	write32(info->core, V3D_CTL_INT_MSK_SET, 0xffffffff);
	write32(info->hub, V3D_HUB_INT_MSK_SET, 0xffffffff);
	if (info->interruptInstalled) {
		remove_io_interrupt_handler(info->interrupt, v3d_interrupt, info);
		info->interruptInstalled = false;
	}

	delete_area(info->pageTableArea);
	delete_area(info->scratchArea);
	mutex_destroy(&info->lock);

	delete_area(info->hubArea);
	delete_area(info->coreArea);
	delete_area(info->pmArea);
	delete_area(info->asbArea);
}


static status_t
v3d_open(void* _info, const char* path, int openMode, void** _cookie)
{
	v3d_file* file = (v3d_file*)calloc(1, sizeof(v3d_file));
	if (file == NULL)
		return B_NO_MEMORY;

	file->device = (v3d_info*)_info;
	file->team = team_get_current_team_id();

	*_cookie = file;
	return B_OK;
}


static status_t
v3d_close(void* cookie)
{
	return B_OK;
}


static status_t
v3d_free(void* cookie)
{
	v3d_file* file = (v3d_file*)cookie;
	v3d_info* info = file->device;

	// Buffers that queued jobs still use live on until those are done.
	MutexLocker locker(info->lock);
	for (uint32 i = 0; i < file->bufferCount; i++) {
		if (file->buffers[i] != NULL)
			put_buffer(info, file->buffers[i]);
	}
	locker.Unlock();

	free(file->buffers);
	free(file->syncs);
	free(file);
	return B_OK;
}


template<typename Type>
static status_t
copy_in(Type& request, void* buffer, size_t length)
{
	if (buffer == NULL || !IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;
	return user_memcpy(&request, buffer, sizeof(Type));
}


static status_t
get_param(v3d_info* info, drm_v3d_get_param& request)
{
	switch (request.param) {
		case DRM_V3D_PARAM_V3D_UIFCFG:
			request.value = read32(info->hub, V3D_HUB_UIFCFG);
			break;
		case DRM_V3D_PARAM_V3D_HUB_IDENT1:
			request.value = read32(info->hub, V3D_HUB_IDENT1);
			break;
		case DRM_V3D_PARAM_V3D_HUB_IDENT2:
			request.value = read32(info->hub, V3D_HUB_IDENT2);
			break;
		case DRM_V3D_PARAM_V3D_HUB_IDENT3:
			request.value = read32(info->hub, V3D_HUB_IDENT3);
			break;
		case DRM_V3D_PARAM_V3D_CORE0_IDENT0:
			request.value = read32(info->core, V3D_CTL_IDENT0);
			break;
		case DRM_V3D_PARAM_V3D_CORE0_IDENT1:
			request.value = read32(info->core, V3D_CTL_IDENT1);
			break;
		case DRM_V3D_PARAM_V3D_CORE0_IDENT2:
			request.value = read32(info->core, V3D_CTL_IDENT2);
			break;
		case DRM_V3D_PARAM_SUPPORTS_TFU:
		case DRM_V3D_PARAM_SUPPORTS_CSD:
		case DRM_V3D_PARAM_SUPPORTS_CACHE_FLUSH:
			request.value = 1;
			break;
		case DRM_V3D_PARAM_SUPPORTS_PERFMON:
		case DRM_V3D_PARAM_SUPPORTS_MULTISYNC_EXT:
		case DRM_V3D_PARAM_SUPPORTS_CPU_QUEUE:
		case DRM_V3D_PARAM_MAX_PERF_COUNTERS:
		case DRM_V3D_PARAM_SUPPORTS_SUPER_PAGES:
			request.value = 0;
			break;
		case DRM_V3D_PARAM_GLOBAL_RESET_COUNTER:
		case DRM_V3D_PARAM_CONTEXT_RESET_COUNTER:
			request.value = info->resets;
			break;
		default:
			return B_BAD_VALUE;
	}
	return B_OK;
}


static status_t
v3d_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	v3d_file* file = (v3d_file*)cookie;
	v3d_info* info = file->device;

	switch (op) {
		case V3D_HAIKU_GET_PARAM:
		{
			drm_v3d_get_param request;
			status_t status = copy_in(request, buffer, length);
			if (status == B_OK)
				status = get_param(info, request);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case V3D_HAIKU_CREATE_BO:
		{
			drm_v3d_create_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);

			// a free handle, growing the table as needed
			uint32 index = 0;
			while (index < file->bufferCount && file->buffers[index] != NULL)
				index++;
			if (index == file->bufferCount) {
				uint32 count = std::max(file->bufferCount * 2, (uint32)64);
				v3d_bo** table = (v3d_bo**)realloc(file->buffers,
					count * sizeof(v3d_bo*));
				if (table == NULL)
					return B_NO_MEMORY;
				memset(table + file->bufferCount, 0,
					(count - file->bufferCount) * sizeof(v3d_bo*));
				file->buffers = table;
				file->bufferCount = count;
			}

			v3d_bo* bo;
			status = create_buffer(info, request.size, bo);
			if (status != B_OK)
				return status;
			file->buffers[index] = bo;

			request.handle = index + 1;
			request.offset = bo->page << V3D_PAGE_SHIFT;
			status = user_memcpy(buffer, &request, sizeof(request));
			if (status != B_OK) {
				file->buffers[index] = NULL;
				put_buffer(info, bo);
			}
			return status;
		}

		case V3D_HAIKU_MMAP_BO:
		{
			drm_v3d_mmap_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			v3d_bo* bo = lookup_buffer(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			area_id source = bo->area;
			locker.Unlock();

			void* address = NULL;
			area_id area = vm_clone_area(file->team, "v3d buffer mapping",
				&address, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0,
				source, true);
			if (area < 0)
				return area;

			request.offset = (addr_t)address;
			status = user_memcpy(buffer, &request, sizeof(request));
			if (status != B_OK)
				vm_delete_area(file->team, area, true);
			return status;
		}

		case V3D_HAIKU_GET_BO_OFFSET:
		{
			drm_v3d_get_bo_offset request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			v3d_bo* bo = lookup_buffer(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			request.offset = bo->page << V3D_PAGE_SHIFT;
			locker.Unlock();
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case V3D_HAIKU_WAIT_BO:
		{
			drm_v3d_wait_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			v3d_bo* bo = lookup_buffer(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			uint64 seqno = bo->lastJob;
			locker.Unlock();

			status = wait_for_job(info, seqno, request.timeout_ns);
			return status == B_WOULD_BLOCK ? B_TIMED_OUT : status;
		}

		case V3D_HAIKU_CLOSE_BO:
		{
			v3d_haiku_handle request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			v3d_bo* bo = lookup_buffer(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			file->buffers[request.handle - 1] = NULL;
			put_buffer(info, bo);
			return B_OK;
		}

		case V3D_HAIKU_SUBMIT_CL:
		{
			v3d_job* job = (v3d_job*)calloc(1, sizeof(v3d_job));
			if (job == NULL)
				return B_NO_MEMORY;
			job->type = JOB_CL;

			status_t status = copy_in(job->cl, buffer, length);
			uint32 count = job->cl.bo_handle_count;
			if (status == B_OK && (count > V3D_MAX_JOB_BOS
					|| (job->cl.flags & ~DRM_V3D_SUBMIT_CL_FLUSH_CACHE) != 0)) {
				status = B_BAD_VALUE;
			}

			uint32* handles = NULL;
			if (status == B_OK && count != 0) {
				handles = (uint32*)malloc(count * sizeof(uint32));
				if (handles == NULL)
					status = B_NO_MEMORY;
				else if (!IS_USER_ADDRESS(job->cl.bo_handles)
					|| user_memcpy(handles, (void*)(addr_t)job->cl.bo_handles,
						count * sizeof(uint32)) != B_OK) {
					status = B_BAD_ADDRESS;
				}
			}

			if (status != B_OK) {
				free(handles);
				free(job);
				return status;
			}

			status = submit_job(file, job, handles, count, job->cl.out_sync);
			free(handles);
			return status;
		}

		case V3D_HAIKU_SUBMIT_TFU:
		{
			v3d_job* job = (v3d_job*)calloc(1, sizeof(v3d_job));
			if (job == NULL)
				return B_NO_MEMORY;
			job->type = JOB_TFU;

			status_t status = copy_in(job->tfu, buffer, length);
			if (status != B_OK) {
				free(job);
				return status;
			}

			uint32 handles[4];
			memcpy(handles, job->tfu.bo_handles, sizeof(handles));
			return submit_job(file, job, handles, 4, job->tfu.out_sync);
		}

		case V3D_HAIKU_SUBMIT_CSD:
		{
			v3d_job* job = (v3d_job*)calloc(1, sizeof(v3d_job));
			if (job == NULL)
				return B_NO_MEMORY;
			job->type = JOB_CSD;

			status_t status = copy_in(job->csd, buffer, length);
			uint32 count = job->csd.bo_handle_count;
			if (status == B_OK && count > V3D_MAX_JOB_BOS)
				status = B_BAD_VALUE;

			uint32* handles = NULL;
			if (status == B_OK && count != 0) {
				handles = (uint32*)malloc(count * sizeof(uint32));
				if (handles == NULL)
					status = B_NO_MEMORY;
				else if (!IS_USER_ADDRESS(job->csd.bo_handles)
					|| user_memcpy(handles, (void*)(addr_t)job->csd.bo_handles,
						count * sizeof(uint32)) != B_OK) {
					status = B_BAD_ADDRESS;
				}
			}

			if (status != B_OK) {
				free(handles);
				free(job);
				return status;
			}

			status = submit_job(file, job, handles, count, job->csd.out_sync);
			free(handles);
			return status;
		}

		case V3D_HAIKU_SYNC_CREATE:
		{
			MutexLocker locker(info->lock);

			uint32 index = 0;
			while (index < file->syncCount && file->syncs[index] != 0)
				index++;
			if (index == file->syncCount) {
				uint32 count = std::max(file->syncCount * 2, (uint32)16);
				uint64* table = (uint64*)realloc(file->syncs,
					count * sizeof(uint64));
				if (table == NULL)
					return B_NO_MEMORY;
				memset(table + file->syncCount, 0,
					(count - file->syncCount) * sizeof(uint64));
				file->syncs = table;
				file->syncCount = count;
			}

			// in use, standing for "job 0", which is always done
			file->syncs[index] = 1;
			locker.Unlock();

			v3d_haiku_handle request = {index + 1, 0};
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case V3D_HAIKU_SYNC_DESTROY:
		{
			v3d_haiku_handle request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			if (request.handle == 0 || request.handle > file->syncCount
				|| file->syncs[request.handle - 1] == 0) {
				return B_BAD_VALUE;
			}
			file->syncs[request.handle - 1] = 0;
			return B_OK;
		}

		case V3D_HAIKU_SYNC_WAIT:
		case V3D_HAIKU_SYNC_GET:
		case V3D_HAIKU_SEQNO_WAIT:
		{
			v3d_haiku_sync request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			if (op != V3D_HAIKU_SEQNO_WAIT) {
				MutexLocker locker(info->lock);
				if (request.handle == 0 || request.handle > file->syncCount
					|| file->syncs[request.handle - 1] == 0) {
					return B_BAD_VALUE;
				}
				request.seqno = file->syncs[request.handle - 1] - 1;
			}

			if (op == V3D_HAIKU_SYNC_GET)
				return user_memcpy(buffer, &request, sizeof(request));

			status = wait_for_job(info, request.seqno, request.timeout_ns);
			return status == B_WOULD_BLOCK ? B_TIMED_OUT : status;
		}
	}

	// Not a display driver: app_server asks every device under graphics/
	// for an accelerant and has to be turned away.
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
v3d_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "brcm,2711-v3d") != 0) {
		return 0.0f;
	}

	return 1.0f;
}


static status_t
v3d_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Broadcom V3D"}},
		{}
	};

	return sDeviceManager->register_node(parent, V3D_DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
v3d_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	v3d_info* info = (v3d_info*)calloc(1, sizeof(v3d_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;

	// reg: "hub", "core0"
	uint64 interrupt;
	if (!fdt->get_reg(device, 0, &info->hubBase, &info->hubSize)
		|| !fdt->get_reg(device, 1, &info->coreBase, &info->coreSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		free(info);
		return B_BAD_DATA;
	}
	info->interrupt = interrupt;

	*_cookie = info;
	return B_OK;
}


static void
v3d_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
v3d_register_child_devices(void* cookie)
{
	v3d_info* info = (v3d_info*)cookie;
	return sDeviceManager->publish_device(info->node, "graphics/v3d/0",
		V3D_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware},
	{}
};

static device_module_info sV3dDevice = {
	{
		V3D_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	v3d_init_device,
	v3d_uninit_device,
	NULL,	// removed
	v3d_open,
	v3d_close,
	v3d_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	v3d_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sV3dDriver = {
	{
		V3D_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	v3d_supports_device,
	v3d_register_device,
	v3d_init_driver,
	v3d_uninit_driver,
	v3d_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sV3dDriver,
	(module_info*)&sV3dDevice,
	NULL
};
