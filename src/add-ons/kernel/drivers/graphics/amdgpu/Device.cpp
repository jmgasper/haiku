/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Device.h"
#include "Sdma.h"
#include "Gart.h"
#include "VramAllocator.h"
#include <KernelExport.h>
#include <condition_variable.h>
#include <lock.h>
#include <team.h>
#include <vm/vm.h>
#include <fcntl.h>
#include <stdlib.h>

struct Buffer {
	Buffer* next;
	uint64 handle, offset, bytes, gpu;
	area_id area;
	volatile uint32* cpu;
	uint32 references;
	bool poisoned, system;
};

struct AmdgpuClient {
	team_id team;
	uint32 flags, references, bufferCount, pending;
	uint64 bytes, systemBytes, submitted, completed, failedFence;
	status_t failure;
	Buffer* buffers;
};

struct Job {
	Job* next;
	AmdgpuClient* client;
	Buffer* source;
	Buffer* destination;
	amdgpu_dma_submit command;
};

static mutex sMutex = MUTEX_INITIALIZER("amdgpu buffers");
static ConditionVariable sCompleted;
static VramAllocator sAllocator = {};
static VramAllocator sGartAllocator = {};
static Gart sGart = {};
static SdmaEngine sEngine = {};
static amdgpu_info sInfo;
static bool sActive, sStopping;
static status_t sFault;
static sem_id sJobs = -1;
static thread_id sWorker = -1;
static Job* sFirst;
static Job* sLast;
static uint32 sPending;
static uint64 sNextHandle = 1;

static void
PutBuffer(Buffer* bo)
{
	if (bo == NULL || --bo->references != 0)
		return;
	// RAM must remain wired on failure: deleting its area would return pages
	// to the OS while a late GPU write may still be possible. VRAM mappings
	// can be deleted, but the allocator still quarantines their physical pages.
	if (bo->system && !bo->poisoned && sFault == B_OK) {
		status_t status = sGart.Unbind(bo->offset, bo->bytes);
		if (status != B_OK)
			sFault = status;
	}
	if (!bo->system || (!bo->poisoned && sFault == B_OK))
		delete_area(bo->area);
	if (!bo->poisoned && sFault == B_OK) {
		VramAllocator& allocator = bo->system ? sGartAllocator : sAllocator;
		allocator.Free(bo->offset, bo->bytes);
	}
	free(bo);
}

static void
PutClient(AmdgpuClient* client)
{
	if (--client->references == 0)
		free(client);
}

static Buffer*
Lookup(AmdgpuClient* client, uint64 handle)
{
	for (Buffer* bo = client->buffers; bo != NULL; bo = bo->next) {
		if (bo->handle == handle)
			return bo;
	}
	return NULL;
}

static status_t
Executor(void*)
{
	while (acquire_sem(sJobs) == B_OK) {
		mutex_lock(&sMutex);
		Job* job = sFirst;
		if (job == NULL) {
			bool stop = sStopping;
			mutex_unlock(&sMutex);
			if (stop)
				break;
			continue;
		}
		sFirst = job->next;
		if (sFirst == NULL)
			sLast = NULL;
		status_t status = sFault;
		mutex_unlock(&sMutex);
		if (status == B_OK) {
			const amdgpu_dma_submit& c = job->command;
			uint64 source = job->source != NULL
				? job->source->gpu + c.source_offset : 0;
			uint64 destination = job->destination->gpu
				+ c.destination_offset;
			status = sEngine.Execute(c.operation, source, destination, c.bytes, c.value);
		}
		mutex_lock(&sMutex);
		if (status != B_OK) {
			sFault = status;
			if (job->client->failedFence == 0) {
				job->client->failedFence = job->command.fence;
				job->client->failure = status;
			}
		}
		job->client->completed = job->command.fence;
		job->client->pending--;
		sPending--;
		PutBuffer(job->source);
		PutBuffer(job->destination);
		PutClient(job->client);
		free(job);
		sCompleted.NotifyAll();
		mutex_unlock(&sMutex);
	}
	return B_OK;
}

AmdgpuClient*
amdgpu_client_open(uint32 flags)
{
	AmdgpuClient* client = (AmdgpuClient*)calloc(1, sizeof(AmdgpuClient));
	if (client != NULL) {
		client->team = team_get_current_team_id();
		client->flags = flags;
		client->references = 1;
	}
	return client;
}

void
amdgpu_client_free(AmdgpuClient* client)
{
	mutex_lock(&sMutex);
	while (client->buffers != NULL) {
		Buffer* bo = client->buffers;
		client->buffers = bo->next;
		if (vm_change_clones_to_null_areas(bo->area) != B_OK)
			bo->poisoned = true;
		PutBuffer(bo);
	}
	PutClient(client); // queued jobs retain their own reference
	mutex_unlock(&sMutex);
}

bool
amdgpu_device_active()
{
	return sActive;
}

static status_t
Start(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware, const amdgpu::AtomVramReservation& reservation)
{
	if (sActive)
		return B_BUSY;
	// The first 64 MiB contains scanout and all kernel ring/SMU allocations.
	// Account separately for ATOM's firmware/driver ranges and active cursors.
	if (info.bar_size[0] <= (64ULL << 20) || !sAllocator.Init(info.vram_size))
		return B_NO_MEMORY;
	uint64 scratchEnd = reservation.start != 0 ? reservation.start : info.bar_size[0];
	bool valid = sAllocator.Reserve(0, 64ULL << 20)
		&& sAllocator.Reserve(reservation.start, reservation.size)
		&& reservation.driverScratchSize <= scratchEnd
		&& sAllocator.Reserve(scratchEnd - reservation.driverScratchSize,
			reservation.driverScratchSize);
	const uint32 heads[] = {0x1a00, 0x1c00, 0x1e00, 0x4000, 0x4200, 0x4400};
	for (uint32 base : heads) {
		if ((regs[base + 0x19c] & 1) == 0 || (regs[base + 0x66] & 1) == 0)
			continue;
		uint64 cursor = (uint64)regs[base + 0x69] << 32 | (regs[base + 0x67] & ~0xffu);
		valid &= cursor >= info.vram_gpu_base
			&& sAllocator.Reserve(cursor - info.vram_gpu_base, 128 * 128 * 4);
	}
	if (!valid) {
		sAllocator.Uninit();
		return B_BAD_DATA;
	}
	sJobs = create_sem(0, "amdgpu jobs");
	if (sJobs < 0) {
		sAllocator.Uninit();
		return sJobs;
	}
	status_t status = sEngine.Initialize(regs, info, firmware, reservation);
	if (status != B_OK) {
		delete_sem(sJobs);
		sJobs = -1;
		sAllocator.Uninit();
		return status;
	}
	if (!sGartAllocator.Init(Gart::kSize))
		status = B_NO_MEMORY;
	else
		status = sGart.Initialize(regs, info, reservation);
	if (status != B_OK) {
		sEngine.Uninitialize();
		sGartAllocator.Uninit();
		delete_sem(sJobs);
		sJobs = -1;
		sAllocator.Uninit();
		return status;
	}
	sInfo = info;
	sFault = B_OK;
	sStopping = false;
	sCompleted.Init(&sCompleted, "amdgpu fence");
	sWorker = spawn_kernel_thread(Executor, "amdgpu DMA", B_NORMAL_PRIORITY, NULL);
	if (sWorker < 0) {
		sGart.Uninitialize(false);
		sGartAllocator.Uninit();
		sEngine.Uninitialize();
		delete_sem(sJobs);
		sJobs = -1;
		sAllocator.Uninit();
		return sWorker;
	}
	sActive = true;
	resume_thread(sWorker);
	dprintf("amdgpu: client DMA ready, %" B_PRIu64 " MiB visible VRAM\n", info.bar_size[0] >> 20);
	return B_OK;
}

status_t
amdgpu_device_start(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware, const amdgpu::AtomVramReservation& reservation)
{
	mutex_lock(&sMutex);
	status_t status = Start(regs, info, firmware, reservation);
	mutex_unlock(&sMutex);
	return status;
}

template<typename T> static status_t
ReadRequest(T& request, void* data, size_t length)
{
	if (length != sizeof(T))
		return B_BAD_VALUE;
	if (user_memcpy(&request, data, sizeof(T)) != B_OK)
		return B_BAD_ADDRESS;
	return request.version == AMDGPU_HAIKU_ABI_VERSION && request.size == sizeof(T)
		? B_OK : B_BAD_VALUE;
}

static status_t
Control(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	if (!sActive)
		return B_DEV_NOT_READY;
	if (client->team != team_get_current_team_id()
		|| (client->flags & O_ACCMODE) != O_RDWR)
		return B_NOT_ALLOWED;
	if (op == AMDGPU_MEMORY_INFO) {
		amdgpu_memory_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK)
			return status;
		info.total_vram = sInfo.vram_size;
		info.visible_vram = sInfo.bar_size[0];
		info.allocated_bytes = sAllocator.allocated;
		info.client_bytes = client->bytes;
		info.submitted = client->submitted;
		info.completed = client->completed;
		info.pending_jobs = sPending;
		info.faulted = sFault != B_OK;
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_GART_INFO) {
		amdgpu_gart_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK)
			return status;
		info.total_bytes = Gart::kSize;
		info.allocated_bytes = sGartAllocator.allocated;
		info.client_bytes = client->systemBytes;
		info.bound_pages = sGart.boundPages;
		info.scatter_boundaries = sGart.scatterBoundaries;
		info.vm_fault_status = sGart.regs[0x536];
		info.vm_fault_address = sGart.regs[0x53e];
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_WAIT_FENCE) {
		amdgpu_fence_wait request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK)
			return status;
		if (request.reserved != 0 || request.fence == 0 || request.fence > client->submitted
			|| request.timeout_us < 0 || request.timeout_us > 5000000)
			return B_BAD_VALUE;
		bigtime_t deadline = system_time() + request.timeout_us;
		while (client->completed < request.fence) {
			if (request.timeout_us == 0)
				return B_WOULD_BLOCK;
			status = sCompleted.Wait(&sMutex, B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, deadline);
			if (status != B_OK && client->completed < request.fence)
				return status;
		}
		request.status = client->failedFence != 0 && request.fence >= client->failedFence
			? client->failure : B_OK;
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_SUBMIT_DMA) {
		amdgpu_dma_submit c;
		status_t status = ReadRequest(c, data, length);
		if (status != B_OK)
			return status;
		if (sFault != B_OK)
			return B_DEV_NOT_READY;
		if ((c.operation != AMDGPU_DMA_COPY && c.operation != AMDGPU_DMA_FILL)
			|| c.bytes == 0 || c.bytes > (64ULL << 20)
			|| ((c.source_offset | c.destination_offset | c.bytes) & 3) != 0)
			return B_BAD_VALUE;
		Buffer* dst = Lookup(client, c.destination);
		Buffer* src = c.operation == AMDGPU_DMA_COPY ? Lookup(client, c.source) : NULL;
		if (dst == NULL || c.destination_offset > dst->bytes
			|| c.bytes > dst->bytes - c.destination_offset)
			return B_BAD_VALUE;
		if (c.operation == AMDGPU_DMA_COPY) {
			if (src == NULL || c.source_offset > src->bytes || c.bytes > src->bytes - c.source_offset)
				return B_BAD_VALUE;
			if (src == dst && (c.source_offset < c.destination_offset
				? c.destination_offset - c.source_offset < c.bytes
				: c.source_offset - c.destination_offset < c.bytes))
				return B_BAD_VALUE;
		} else if (c.source != 0 || c.source_offset != 0)
			return B_BAD_VALUE;
		if (sPending >= 256 || client->pending >= 64 || client->submitted == UINT64_MAX)
			return B_WOULD_BLOCK;
		Job* job = (Job*)calloc(1, sizeof(Job));
		if (job == NULL)
			return B_NO_MEMORY;
		c.fence = client->submitted + 1;
		status = user_memcpy(data, &c, sizeof(c));
		if (status != B_OK) {
			free(job);
			return status;
		}
		job->client = client;
		job->source = src;
		job->destination = dst;
		job->command = c;
		client->submitted = c.fence;
		client->pending++;
		client->references++;
		dst->references++;
		if (src != NULL)
			src->references++;
		if (sLast != NULL)
			sLast->next = job;
		else
			sFirst = job;
		sLast = job;
		sPending++;
		release_sem(sJobs);
		return B_OK;
	}
	if (op != AMDGPU_CREATE_BUFFER && op != AMDGPU_CREATE_SYSTEM_BUFFER
		&& op != AMDGPU_MAP_BUFFER && op != AMDGPU_FREE_BUFFER)
		return B_DEV_INVALID_IOCTL;
	amdgpu_buffer request;
	status_t status = ReadRequest(request, data, length);
	if (status != B_OK)
		return status;
	if (request.reserved != 0)
		return B_BAD_VALUE;
	if (op == AMDGPU_CREATE_BUFFER || op == AMDGPU_CREATE_SYSTEM_BUFFER) {
		if (sFault != B_OK)
			return B_DEV_NOT_READY;
		if (request.bytes == 0 || request.bytes > (64ULL << 20))
			return B_BAD_VALUE;
		if (client->bufferCount >= 256 || sNextHandle == 0)
			return B_NO_MEMORY;
		Buffer* bo = (Buffer*)calloc(1, sizeof(Buffer));
		if (bo == NULL)
			return B_NO_MEMORY;
		bo->bytes = (request.bytes + 4095) & ~4095ULL;
		bo->system = op == AMDGPU_CREATE_SYSTEM_BUFFER;
		VramAllocator& allocator = bo->system ? sGartAllocator : sAllocator;
		uint64 limit = bo->system ? Gart::kSize : sInfo.bar_size[0];
		if (!allocator.Allocate(bo->bytes, 4096, limit, bo->offset)) {
			free(bo);
			return B_NO_MEMORY;
		}
		if (bo->system) {
			virtual_address_restrictions va = {};
			physical_address_restrictions pa = {};
			bo->area = create_area_etc(B_SYSTEM_TEAM, "amdgpu client RAM", bo->bytes,
				B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &va, &pa,
				(void**)&bo->cpu);
		} else {
			bo->area = map_physical_memory("amdgpu client VRAM", sInfo.bar_address[0] + bo->offset,
				bo->bytes, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				(void**)&bo->cpu);
		}
		if (bo->area < 0) {
			status = bo->area;
			allocator.Free(bo->offset, bo->bytes);
			free(bo);
			return status;
		}
		for (uint64 i = 0; i < bo->bytes / 4; i++)
			bo->cpu[i] = 0;
		__sync_synchronize();
		(void)bo->cpu[bo->bytes / 4 - 1];
		bo->references = 1;
		bo->gpu = (bo->system ? Gart::kBase : sInfo.vram_gpu_base) + bo->offset;
		if (bo->system) {
			status = sGart.Bind(bo->offset, bo->bytes, (const void*)bo->cpu);
			if (status != B_OK) {
				// An invalidation timeout requires retaining backing RAM.
				if (status == B_TIMED_OUT)
					sFault = status;
				PutBuffer(bo);
				return status;
			}
		}
		bo->handle = sNextHandle++;
		request.handle = bo->handle;
		request.bytes = bo->bytes;
		request.address = 0;
		request.area = -1;
		status = user_memcpy(data, &request, sizeof(request));
		if (status != B_OK) {
			PutBuffer(bo);
			return status;
		}
		bo->next = client->buffers;
		client->buffers = bo;
		client->bufferCount++;
		if (bo->system)
			client->systemBytes += bo->bytes;
		else
			client->bytes += bo->bytes;
		return B_OK;
	}
	Buffer* bo = Lookup(client, request.handle);
	if (bo == NULL)
		return B_BAD_VALUE;
	if (op == AMDGPU_MAP_BUFFER) {
		void* address = NULL;
		area_id area = vm_clone_area(client->team, "amdgpu buffer", &address,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0, bo->area, true);
		if (area < 0)
			return area;
		request.area = area;
		request.address = (addr_t)address;
		request.bytes = bo->bytes;
		status = user_memcpy(data, &request, sizeof(request));
		if (status != B_OK)
			vm_delete_area(client->team, area, true);
		return status;
	}
	status = vm_change_clones_to_null_areas(bo->area);
	if (status != B_OK)
		return status;
	Buffer** link = &client->buffers;
	while (*link != bo)
		link = &(*link)->next;
	*link = bo->next;
	client->bufferCount--;
	if (bo->system)
		client->systemBytes -= bo->bytes;
	else
		client->bytes -= bo->bytes;
	PutBuffer(bo);
	return B_OK;
}

status_t
amdgpu_client_control(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	mutex_lock(&sMutex);
	status_t status = Control(client, op, data, length);
	mutex_unlock(&sMutex);
	return status;
}

void
amdgpu_device_stop()
{
	if (!sActive)
		return;
	mutex_lock(&sMutex);
	sStopping = true;
	mutex_unlock(&sMutex);
	release_sem(sJobs);
	status_t result;
	wait_for_thread(sWorker, &result);
	delete_sem(sJobs);
	sJobs = sWorker = -1;
	sEngine.Uninitialize();
	sGart.Uninitialize(sFault != B_OK);
	sGartAllocator.Uninit();
	sAllocator.Uninit();
	sActive = false;
}
