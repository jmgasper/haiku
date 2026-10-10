/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	/dev/misc/sunxi_ve: the video decoder of the Allwinner A733 (its Cedar
	"VE") for the program that parses the stream (the sunxi_cedar media
	add-on); see <sunxi_ve.h>. Without the device tree node
	"allwinner,sun60i-a733-video-engine" no device is found and every open
	fails.

	The power-up follows what Allwinner's own driver does on this SoC
	(drivers/ve/cedar-ve of the BSP), with the registers' meaning from the
	A733 user manual: PCK-600 power domain 2, the decoder's reset, its AHB
	and MBUS gates and clock, its bus gate and module clock, then the
	A733's "top reset" pulse. The engine reads physical addresses through
	the IOMMU's bypass for its two bus masters, as the display engine does.

	Nothing reads the engine's registers unless the CCU says that its
	clock runs and its reset is released: with either off, a read stalls
	the bus and the whole system with it (the power domain being on says
	nothing about that). */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <sunxi_ve.h>

#include "sunxi_ve_check.h"


//#define TRACE_VE
#ifdef TRACE_VE
#	define TRACE(x...)	dprintf("sunxi_ve: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif
#define ERROR(x...)		dprintf("sunxi_ve: " x)

#define CACHE_LINE_SIZE		64
#define SLICE_TIMEOUT		2000000LL
#define POLL_TIMEOUT		10000LL

// what the device tree does not say: the SoC's clock, power and IOMMU blocks
static const phys_addr_t kCcu			= 0x02002000;
static const phys_addr_t kRCcu			= 0x07010000;
static const phys_addr_t kPpuVeDecoder	= 0x07062000;	// PCK-600 domain 2
static const phys_addr_t kIommu1		= 0x03910000;
static const phys_addr_t kSystemControl	= 0x03000000;

enum {
	CCU_IOMMU1				= 0x05b4,	// b0-2 gates, b16 reset deassert
	CCU_AHB_GATES			= 0x05c0,	// b0 VE decoder; keyed
	CCU_MBUS_GATES			= 0x05e0,	// b1 IOMMU1, b14 VE decoder; keyed
	CCU_MBUS_CLOCKS			= 0x05e4,	// b18 VE decoder; keyed
	CCU_VE_DECODER_CLOCK	= 0x0a88,	// b31 gate, [26:24] parent, [4:0] M-1
	CCU_VE_BUS				= 0x0a8c,	// b2 decoder gate, b18 decoder reset

	R_CCU_PPU				= 0x01ac,	// b0 the PCK-600's bus clock

	PPU_POLICY				= 0x000,
	PPU_STATUS				= 0x008,
	PPU_ON					= 0x8,

	IOMMU_BYPASS_VE_DEC0	= 0x404,	// IOMMU1 masters 0 and 1 (6 and 7
	IOMMU_BYPASS_VE_DEC1	= 0x604,	// of the IOMMU as a whole)

	SYSTEM_SRAM_MAP			= 0x0000,	// b0 clear: SRAM C to the engine
	SYSTEM_SRAM_BOOT		= 0x0004	// b24 clear: the same
};

static const uint32 kAhbKey = 0x010000ff;
static const uint32 kMbusGateKey = 0x41055800;
static const uint32 kMbusClockKey = 0x00040302;

// gate, pll-peri0-600m, divided by 1
static const uint32 kVeClock600MHz = 0x83000000;

// the engine's own registers
enum {
	VE_MODE				= 0x000,	// [3:0] engine, 7 is none
	VE_DECODER_IP		= 0x0e0,
	VE_ENCODER_IP		= 0x0e4,
	VE_VERSION			= 0x0f0,
	VE_TOP_RESET		= 0x804,	// A733: b0 and b4 pulsed after reset

	VE_MODE_NONE		= 7,
	VE_MODE_H264		= 1,
	VE_MODE_HEVC		= 4,

	H264_CONTROL		= 0x220,
	H264_TRIGGER		= 0x224,
	H264_STATUS			= 0x228,
	HEVC_CONTROL		= 0x530,
	HEVC_TRIGGER		= 0x534,
	HEVC_STATUS			= 0x538,

	STATUS_DONE_ERROR_REQUEST = 0x7
};


struct ve_buffer {
	area_id		area;
	uint8*		address;
	phys_addr_t	physical;
	size_t		size;
};

struct ve_client {
	mutex		lock;
	ve_buffer	buffers[SUNXI_VE_MAX_BUFFERS];
	int32		busy;
};

struct mapping {
	area_id				area;
	volatile uint8*		address;
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDeviceNames[] = { "misc/sunxi_ve", NULL };

static device_manager_info* sDeviceManager;

static mapping sVe = { -1, NULL };
static mapping sCcu = { -1, NULL };
static mapping sRCcu = { -1, NULL };
static mapping sPpu = { -1, NULL };
static mapping sIommu = { -1, NULL };
static mapping sSystem = { -1, NULL };
static phys_addr_t sVePhysical;
static uint32 sInterrupt;
static bool sInterruptInstalled;
static uint32 sAddressOffset;

static mutex sEngineLock = MUTEX_INITIALIZER("sunxi_ve engine");
static spinlock sPowerLock = B_SPINLOCK_INITIALIZER;
	// with sPowered: the interrupt handler never reads an unclocked engine
static mutex sOpenLock = MUTEX_INITIALIZER("sunxi_ve open");
static sem_id sDoneSem = -1;
static int32 sOpenCount;
static volatile bool sPowered;
static ve_client* sLastClient;		// whose slice the engine ran last
static ve_client* sHolder;			// in the middle of a picture
static bigtime_t sHoldUntil;
static bool sIdentified;
static uint32 sDecoderIp, sEncoderIp, sVersion;


static inline uint32
read32(const mapping& block, uint32 offset)
{
	return *(volatile uint32*)(block.address + offset);
}


static inline void
write32(const mapping& block, uint32 offset, uint32 value)
{
	*(volatile uint32*)(block.address + offset) = value;
	memory_full_barrier();
}


static inline void
set_bits(const mapping& block, uint32 offset, uint32 bits)
{
	write32(block, offset, read32(block, offset) | bits);
}


static inline void
clear_bits(const mapping& block, uint32 offset, uint32 bits)
{
	write32(block, offset, read32(block, offset) & ~bits);
}


/*!	The AHB and MBUS master gates take a key with every write, as the BSP's
	clock driver writes them (the display driver does the same).
*/
static void
set_keyed_gate(uint32 offset, uint32 key, uint32 bits)
{
	uint32 value = read32(sCcu, offset);
	if ((value & bits) != bits)
		write32(sCcu, offset, value | key | bits);
}


/*!	The one test for "the engine's registers may be touched". */
static bool
engine_clocked()
{
	uint32 bus = read32(sCcu, CCU_VE_BUS);
	return (read32(sCcu, CCU_VE_DECODER_CLOCK) & (1u << 31)) != 0
		&& (bus & (1u << 2)) != 0 && (bus & (1u << 18)) != 0;
}


static inline uint32
ve_read(uint32 offset)
{
	return read32(sVe, offset);
}


static inline void
ve_write(uint32 offset, uint32 value)
{
	write32(sVe, offset, value);
}


/*!	Writes the CPU cache's content of the range to memory (\a invalidate
	false) or writes it and drops it (true).
*/
static void
sync_cache(void* address, size_t size, bool invalidate)
{
#ifdef __aarch64__
	addr_t end = (addr_t)address + size;
	addr_t line = (addr_t)address & ~(addr_t)(CACHE_LINE_SIZE - 1);
	if (invalidate) {
		for (; line < end; line += CACHE_LINE_SIZE)
			asm volatile("dc civac, %0" : : "r" (line) : "memory");
	} else {
		for (; line < end; line += CACHE_LINE_SIZE)
			asm volatile("dc cvac, %0" : : "r" (line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
#endif
}


//	#pragma mark - power


static void
set_powered(bool powered)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sPowerLock);
	sPowered = powered;
	release_spinlock(&sPowerLock);
	restore_interrupts(state);
}


static status_t
power_domain_on()
{
	set_bits(sRCcu, R_CCU_PPU, 1);
	if ((read32(sPpu, PPU_STATUS) & 0xf) == PPU_ON)
		return B_OK;

	// Allwinner's PPU set-up (pck600_domains.c): device control and logic
	// power switch delays, then the policy
	write32(sPpu, 0x170, 0x001f1f1f);
	write32(sPpu, 0x174, 0x00001f1f);
	write32(sPpu, 0xc00, 0x08080808);
	write32(sPpu, 0xc04, 0x00000808);
	write32(sPpu, 0xc10, 0x00000008);
	if ((read32(sPpu, PPU_POLICY) & 0xf) == PPU_ON) {
		// a policy that says on while the domain is off: ask again
		clear_bits(sPpu, PPU_POLICY, 0xf);
		spin(10);
	}
	write32(sPpu, PPU_POLICY, (read32(sPpu, PPU_POLICY) & ~0xfu) | PPU_ON);

	bigtime_t deadline = system_time() + 10000;
	while ((read32(sPpu, PPU_STATUS) & 0xf) != PPU_ON) {
		if (system_time() > deadline)
			return B_TIMED_OUT;
		spin(10);
	}
	return B_OK;
}


static void
top_reset_pulse()
{
	ve_write(VE_TOP_RESET, ve_read(VE_TOP_RESET) | 0x11);
	ve_write(VE_TOP_RESET, ve_read(VE_TOP_RESET) & ~0x11u);
}


static void
clocks_on()
{
	set_bits(sCcu, CCU_VE_BUS, 1u << 18);
	set_keyed_gate(CCU_AHB_GATES, kAhbKey, 1u << 0);
	set_keyed_gate(CCU_MBUS_GATES, kMbusGateKey, (1u << 14) | (1u << 1));
	set_keyed_gate(CCU_MBUS_CLOCKS, kMbusClockKey, 1u << 18);
	set_bits(sCcu, CCU_VE_BUS, 1u << 2);
	write32(sCcu, CCU_VE_DECODER_CLOCK, kVeClock600MHz);
}


static status_t
power_up()
{
	status_t status = power_domain_on();
	if (status != B_OK) {
		// the GPU core's domain only came up with its clock running
		clocks_on();
		status = power_domain_on();
	}
	if (status != B_OK) {
		ERROR("power domain 2 does not come up: policy %#" B_PRIx32
			", status %#" B_PRIx32 "\n", read32(sPpu, PPU_POLICY),
			read32(sPpu, PPU_STATUS));
		return status;
	}
	clocks_on();

	// the engine's two bus masters go past IOMMU1 untranslated
	set_bits(sCcu, CCU_IOMMU1, 0x00010007);
	write32(sIommu, IOMMU_BYPASS_VE_DEC0, 1);
	write32(sIommu, IOMMU_BYPASS_VE_DEC1, 1);

	clear_bits(sSystem, SYSTEM_SRAM_MAP, 1);
	clear_bits(sSystem, SYSTEM_SRAM_BOOT, 1u << 24);

	spin(10);
	if (!engine_clocked()) {
		ERROR("the decoder's clock or reset did not come up (clock %#"
			B_PRIx32 ", bus %#" B_PRIx32 "): not touching it\n",
			read32(sCcu, CCU_VE_DECODER_CLOCK), read32(sCcu, CCU_VE_BUS));
		return B_ERROR;
	}

	top_reset_pulse();
	ve_write(VE_MODE, VE_MODE_NONE);

	sDecoderIp = ve_read(VE_DECODER_IP);
	sEncoderIp = ve_read(VE_ENCODER_IP);
	sVersion = ve_read(VE_VERSION);
	if (!sIdentified) {
		dprintf("sunxi_ve: decoder IP %#" B_PRIx32 ", encoder IP %#" B_PRIx32
			", version %#" B_PRIx32 ", clock %#" B_PRIx32 ", bus %#" B_PRIx32
			", power %#" B_PRIx32 "\n", sDecoderIp, sEncoderIp, sVersion,
			read32(sCcu, CCU_VE_DECODER_CLOCK), read32(sCcu, CCU_VE_BUS),
			read32(sPpu, PPU_STATUS));
		sIdentified = true;
	}
	// the A733's decoder (Allwinner's IC version 0x3331000021320)
	if ((sDecoderIp & 0xfffff) != 0x33310) {
		ERROR("not the A733's video engine (decoder IP %#" B_PRIx32 ")\n",
			sDecoderIp);
		return B_DEVICE_NOT_FOUND;
	}

	set_powered(true);
	return B_OK;
}


static void
power_down()
{
	if (sPowered)
		ve_write(VE_MODE, VE_MODE_NONE);
	set_powered(false);
	clear_bits(sCcu, CCU_VE_DECODER_CLOCK, 1u << 31);
	clear_bits(sCcu, CCU_VE_BUS, 1u << 2);
	clear_bits(sCcu, CCU_VE_BUS, 1u << 18);
	// the power domain stays on: nothing else is in it
}


/*!	After a slice that did not end, and before another program's: the
	decoder's reset, then what the reset clears again.
*/
static void
reset_engine()
{
	set_powered(false);
	clear_bits(sCcu, CCU_VE_BUS, 1u << 18);
	spin(10);
	set_bits(sCcu, CCU_VE_BUS, 1u << 18);
	spin(10);
	if (!engine_clocked())
		return;
	top_reset_pulse();
	ve_write(VE_MODE, VE_MODE_NONE);
	set_powered(true);
}


//	#pragma mark - interrupt


static int32
handle_interrupt()
{
	if (!sPowered)
		return B_UNHANDLED_INTERRUPT;

	uint32 control, status;
	switch (ve_read(VE_MODE) & 0xf) {
		case VE_MODE_H264:
			control = H264_CONTROL;
			status = H264_STATUS;
			break;
		case VE_MODE_HEVC:
			control = HEVC_CONTROL;
			status = HEVC_STATUS;
			break;
		default:
			return B_UNHANDLED_INTERRUPT;
	}

	uint32 enabled = ve_read(control);
	if ((ve_read(status) & STATUS_DONE_ERROR_REQUEST) == 0
		|| (enabled & STATUS_DONE_ERROR_REQUEST) == 0) {
		return B_UNHANDLED_INTERRUPT;
	}

	// the line is level triggered: mask it, the slice's caller clears the
	// status
	ve_write(control, enabled & ~0xfu);
	release_sem_etc(sDoneSem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


static int32
ve_interrupt(void* data)
{
	acquire_spinlock(&sPowerLock);
	int32 result = handle_interrupt();
	release_spinlock(&sPowerLock);
	return result;
}


//	#pragma mark - buffers


static ve_buffer*
buffer_for(ve_client* client, uint32 index)
{
	if (index >= SUNXI_VE_MAX_BUFFERS || client->buffers[index].area < 0)
		return NULL;
	return &client->buffers[index];
}


static void
free_buffer(ve_buffer& buffer)
{
	if (buffer.area >= 0)
		delete_area(buffer.area);
	buffer.area = -1;
	buffer.address = NULL;
	buffer.physical = 0;
	buffer.size = 0;
}


static status_t
allocate_buffer(ve_client* client, sunxi_ve_allocate& request)
{
	if (request.size == 0 || request.size > 256 * 1024 * 1024)
		return B_BAD_VALUE;
	size_t size = ROUNDUP(request.size, B_PAGE_SIZE);

	MutexLocker locker(client->lock);

	uint32 index = 0;
	while (index < SUNXI_VE_MAX_BUFFERS && client->buffers[index].area >= 0)
		index++;
	if (index == SUNXI_VE_MAX_BUFFERS)
		return B_NO_MEMORY;

	// the engine's address registers are 32 bits wide
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 32;

	ve_buffer& buffer = client->buffers[index];
	void* address;
	buffer.area = create_area_etc(B_SYSTEM_TEAM, "sunxi_ve buffer", size,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA
			| B_CLONEABLE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &address);
	if (buffer.area < 0) {
		status_t status = buffer.area;
		buffer.area = -1;
		return status;
	}

	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status != B_OK) {
		free_buffer(buffer);
		return status;
	}
	buffer.address = (uint8*)address;
	buffer.physical = entry.address;
	buffer.size = size;

	// nothing of it in the cache when the engine first writes there
	memset(address, 0, size);
	sync_cache(address, size, true);

	request.size = size;
	request.buffer = index;
	request.area = buffer.area;
	request.address = (uint32)(buffer.physical - sAddressOffset);
	return B_OK;
}


static status_t
sync_buffer(ve_client* client, const sunxi_ve_sync& request)
{
	MutexLocker locker(client->lock);
	ve_buffer* buffer = buffer_for(client, request.buffer);
	if (buffer == NULL || request.offset > buffer->size
		|| request.size > buffer->size - request.offset) {
		return B_BAD_VALUE;
	}
	switch (request.direction) {
		case SUNXI_VE_SYNC_FOR_DEVICE:
			sync_cache(buffer->address + request.offset, request.size, false);
			return B_OK;
		case SUNXI_VE_SYNC_FOR_CPU:
			sync_cache(buffer->address + request.offset, request.size, true);
			return B_OK;
	}
	return B_BAD_VALUE;
}


//	#pragma mark - slices


static status_t
run_slice(ve_client* client, sunxi_ve_run& request, const sunxi_ve_op* ops)
{
	bool h264 = request.trigger_register == H264_TRIGGER
		&& request.status_register == H264_STATUS;
	bool hevc = request.trigger_register == HEVC_TRIGGER
		&& request.status_register == HEVC_STATUS;
	if ((!h264 && !hevc) || request.trigger_value != 8)
		return B_BAD_VALUE;

	// the client's buffers stay while it is busy (SUNXI_VE_FREE refuses)
	ve_range ranges[SUNXI_VE_MAX_BUFFERS];
	uint32 rangeCount = 0;
	MutexLocker clientLocker(client->lock);
	client->busy++;
	for (uint32 i = 0; i < SUNXI_VE_MAX_BUFFERS; i++) {
		const ve_buffer& buffer = client->buffers[i];
		if (buffer.area < 0)
			continue;
		ranges[rangeCount].start = (uint32)(buffer.physical - sAddressOffset);
		ranges[rangeCount].end = ranges[rangeCount].start + (uint32)buffer.size;
		rangeCount++;
	}
	clientLocker.Unlock();

	ve_check check = { ranges, rangeCount, NULL, 0 };
	if (!ve_check_ops(check, ops, request.count, hevc)) {
		ERROR("slice refused: %s (op %" B_PRIu32 ")\n", check.problem,
			check.index);
		clientLocker.Lock();
		client->busy--;
		return B_NOT_ALLOWED;
	}

	// another program's picture goes first
	MutexLocker locker(sEngineLock);
	while (sHolder != NULL && sHolder != client
		&& system_time() < sHoldUntil) {
		locker.Unlock();
		snooze(500);
		locker.Lock();
	}
	if (sHolder != client)
		sHolder = NULL;

	status_t status = B_OK;
	if (!sPowered || !engine_clocked())
		status = B_NOT_ALLOWED;

	// the engine's state of the last program's slice is not this one's
	if (status == B_OK && sLastClient != NULL && sLastClient != client) {
		reset_engine();
		if (!sPowered)
			status = B_NOT_ALLOWED;
	}
	sLastClient = client;

	// an interrupt nobody waited for
	while (acquire_sem_etc(sDoneSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK) {
	}

	for (uint32 i = 0; status == B_OK && i < request.count; i++) {
		const sunxi_ve_op& op = ops[i];
		if (op.type == SUNXI_VE_OP_WRITE) {
			ve_write(op.reg, op.value);
			continue;
		}
		if (op.type == SUNXI_VE_OP_WRITE_BACK) {
			ve_write(op.reg, ve_read(op.reg));
			continue;
		}
		bigtime_t deadline = system_time() + POLL_TIMEOUT;
		while ((ve_read(op.reg) & op.value) != 0) {
			if (system_time() > deadline) {
				ERROR("register %#x stays %#" B_PRIx32 "\n", op.reg,
					ve_read(op.reg));
				status = B_TIMED_OUT;
				break;
			}
		}
	}

	request.status = 0;
	request.engine_time = 0;
	if (status == B_OK) {
		bigtime_t start = system_time();
		ve_write(request.trigger_register, request.trigger_value);
		status = acquire_sem_etc(sDoneSem, 1, B_RELATIVE_TIMEOUT,
			SLICE_TIMEOUT);
		request.engine_time = (uint32)(system_time() - start);
		request.status = ve_read(request.status_register);
		ve_write(request.status_register, STATUS_DONE_ERROR_REQUEST);
		if (status != B_OK) {
			ERROR("no interrupt from the slice: status %#" B_PRIx32 "\n",
				request.status);
		}
	}
	if (status == B_TIMED_OUT) {
		ERROR("resetting the engine\n");
		reset_engine();
	}
	if (status == B_OK && (request.flags & SUNXI_VE_RUN_PICTURE) != 0) {
		sHolder = client;
		sHoldUntil = system_time() + 100000;
	} else if (sHolder == client)
		sHolder = NULL;
	locker.Unlock();

	clientLocker.Lock();
	client->busy--;
	return status;
}


//	#pragma mark - device


static status_t
map_block(mapping& block, const char* name, phys_addr_t physical, size_t size)
{
	void* address;
	block.area = map_physical_memory(name, physical, size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (block.area < 0)
		return block.area;
	block.address = (volatile uint8*)address;
	return B_OK;
}


static void
unmap_block(mapping& block)
{
	if (block.area >= 0)
		delete_area(block.area);
	block.area = -1;
	block.address = NULL;
}


static status_t
find_engine(phys_addr_t* _registers, size_t* _size, uint32* _interrupt)
{
	device_node* root = sDeviceManager->get_root_node();
	if (root == NULL)
		return B_DEVICE_NOT_FOUND;

	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE,
			{ .string = "allwinner,sun60i-a733-video-engine" } },
		{}
	};
	device_node* node = NULL;
	if (sDeviceManager->find_child_node(root, attributes, &node) != B_OK)
		node = NULL;
	sDeviceManager->put_node(root);
	if (node == NULL)
		return B_DEVICE_NOT_FOUND;

	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&fdt, (void**)&device);
	if (status == B_OK) {
		uint64 base, size, interrupt;
		if (fdt->get_reg(device, 0, &base, &size)
			&& fdt->get_interrupt(device, 0, NULL, &interrupt)) {
			*_registers = base;
			*_size = size;
			*_interrupt = interrupt;
		} else
			status = B_BAD_DATA;
	}

	sDeviceManager->put_node(node);
	return status;
}


static void
read_settings()
{
	void* handle = load_driver_settings("sunxi_ve");
	if (handle == NULL)
		return;
	const char* offset = get_driver_parameter(handle, "address_offset",
		NULL, NULL);
	if (offset != NULL)
		sAddressOffset = strtoul(offset, NULL, 0);
	unload_driver_settings(handle);
}


static status_t
ve_open(const char* name, uint32 flags, void** _cookie)
{
	if (sVe.address == NULL)
		return B_DEVICE_NOT_FOUND;

	ve_client* client = new(std::nothrow) ve_client;
	if (client == NULL)
		return B_NO_MEMORY;
	mutex_init(&client->lock, "sunxi_ve client");
	client->busy = 0;
	for (uint32 i = 0; i < SUNXI_VE_MAX_BUFFERS; i++) {
		client->buffers[i].area = -1;
		client->buffers[i].address = NULL;
		client->buffers[i].physical = 0;
		client->buffers[i].size = 0;
	}

	MutexLocker locker(sOpenLock);
	if (sOpenCount == 0) {
		MutexLocker engineLocker(sEngineLock);
		status_t status = power_up();
		if (status != B_OK) {
			power_down();
			engineLocker.Unlock();
			mutex_destroy(&client->lock);
			delete client;
			return status;
		}
	}
	sOpenCount++;

	*_cookie = client;
	return B_OK;
}


static status_t
ve_close(void* cookie)
{
	return B_OK;
}


static status_t
ve_free(void* cookie)
{
	ve_client* client = (ve_client*)cookie;

	// a slice under way writes to the buffers until it ends
	while (true) {
		MutexLocker locker(client->lock);
		if (client->busy == 0)
			break;
		locker.Unlock();
		snooze(10000);
	}
	mutex_lock(&sEngineLock);
	if (sHolder == client)
		sHolder = NULL;
	if (sLastClient == client)
		sLastClient = NULL;
	mutex_unlock(&sEngineLock);

	for (uint32 i = 0; i < SUNXI_VE_MAX_BUFFERS; i++)
		free_buffer(client->buffers[i]);
	mutex_destroy(&client->lock);
	delete client;

	MutexLocker locker(sOpenLock);
	if (--sOpenCount == 0) {
		MutexLocker engineLocker(sEngineLock);
		power_down();
	}
	return B_OK;
}


static status_t
ve_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	ve_client* client = (ve_client*)cookie;

	switch (op) {
		case SUNXI_VE_GET_INFO:
		{
			sunxi_ve_info info;
			info.decoder_ip = sDecoderIp;
			info.encoder_ip = sEncoderIp;
			info.version = sVersion;
			info.clock = 600000000;
			info.address_offset = sAddressOffset;
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_memcpy(buffer, &info, sizeof(info));
		}

		case SUNXI_VE_ALLOCATE:
		{
			sunxi_ve_allocate request;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			status_t status = allocate_buffer(client, request);
			if (status != B_OK)
				return status;
			if (user_memcpy(buffer, &request, sizeof(request)) != B_OK) {
				MutexLocker locker(client->lock);
				free_buffer(client->buffers[request.buffer]);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case SUNXI_VE_FREE:
		{
			uint32 index;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&index, buffer, sizeof(index)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			MutexLocker locker(client->lock);
			if (buffer_for(client, index) == NULL)
				return B_BAD_VALUE;
			if (client->busy != 0)
				return B_BUSY;
			free_buffer(client->buffers[index]);
			return B_OK;
		}

		case SUNXI_VE_SYNC:
		{
			sunxi_ve_sync request;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return sync_buffer(client, request);
		}

		case SUNXI_VE_END_PICTURE:
		{
			MutexLocker locker(sEngineLock);
			if (sHolder == client)
				sHolder = NULL;
			return B_OK;
		}

		case SUNXI_VE_RUN:
		{
			sunxi_ve_run request;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			if (request.count > SUNXI_VE_MAX_OPS
				|| !IS_USER_ADDRESS(request.ops))
				return B_BAD_VALUE;
			size_t size = request.count * sizeof(sunxi_ve_op);
			sunxi_ve_op* ops = (sunxi_ve_op*)malloc(size + 1);
			if (ops == NULL)
				return B_NO_MEMORY;
			status_t status = user_memcpy(ops, request.ops, size);
			if (status == B_OK)
				status = run_slice(client, request, ops);
			free(ops);
			if (status != B_OK && status != B_TIMED_OUT)
				return status;
			status_t copied = user_memcpy(buffer, &request, sizeof(request));
			return status != B_OK ? status : copied;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
ve_read_hook(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
ve_write_hook(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	ve_open,
	ve_close,
	ve_free,
	ve_control,
	ve_read_hook,
	ve_write_hook
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&sDeviceManager);
	if (status != B_OK)
		return status;

	size_t size;
	status = find_engine(&sVePhysical, &size, &sInterrupt);
	if (status != B_OK) {
		// another board: the device is there and cannot be opened
		dprintf("sunxi_ve: no video engine in the device tree\n");
		return B_OK;
	}
	if (size < SUNXI_VE_REGISTER_WINDOW) {
		ERROR("register window of %#" B_PRIxSIZE " bytes is too small\n",
			size);
		return B_OK;
	}
	read_settings();

	sDoneSem = create_sem(0, "sunxi_ve slice done");
	if (sDoneSem < 0
		|| map_block(sCcu, "sunxi_ve ccu", kCcu, B_PAGE_SIZE) != B_OK
		|| map_block(sRCcu, "sunxi_ve r_ccu", kRCcu, B_PAGE_SIZE) != B_OK
		|| map_block(sPpu, "sunxi_ve ppu", kPpuVeDecoder, B_PAGE_SIZE) != B_OK
		|| map_block(sIommu, "sunxi_ve iommu1", kIommu1, B_PAGE_SIZE) != B_OK
		|| map_block(sSystem, "sunxi_ve sysctrl", kSystemControl,
			B_PAGE_SIZE) != B_OK
		|| map_block(sVe, "sunxi_ve registers", sVePhysical,
			SUNXI_VE_REGISTER_WINDOW) != B_OK) {
		ERROR("no memory for the device\n");
		unmap_block(sVe);
		return B_OK;
	}

	status = install_io_interrupt_handler(sInterrupt, ve_interrupt, NULL, 0);
	if (status != B_OK) {
		ERROR("interrupt %" B_PRIu32 ": %s\n", sInterrupt, strerror(status));
		unmap_block(sVe);
		return B_OK;
	}
	sInterruptInstalled = true;

	dprintf("sunxi_ve: decoder at %#" B_PRIxPHYSADDR ", interrupt %" B_PRIu32
		", bus address offset %#" B_PRIx32 "\n", sVePhysical, sInterrupt,
		sAddressOffset);
	return B_OK;
}


void
uninit_driver()
{
	if (sInterruptInstalled)
		remove_io_interrupt_handler(sInterrupt, ve_interrupt, NULL);
	sInterruptInstalled = false;
	unmap_block(sVe);
	unmap_block(sSystem);
	unmap_block(sIommu);
	unmap_block(sPpu);
	unmap_block(sRCcu);
	unmap_block(sCcu);
	if (sDoneSem >= 0)
		delete_sem(sDoneSem);
	sDoneSem = -1;
	put_module(B_DEVICE_MANAGER_MODULE_NAME);
}


const char**
publish_devices()
{
	// nothing on boards without the engine (the arm64 packages are shared)
	static const char* sNone[] = { NULL };
	return sVe.address != NULL ? sDeviceNames : sNone;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
