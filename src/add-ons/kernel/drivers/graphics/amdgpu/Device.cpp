/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Device.h"
#include "Sdma.h"
#include "Gart.h"
#include "Gfx.h"
#include "Uvd.h"
#include "FirmwareLoader.h"
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
	UvdSession* video;
	bool videoBusy;
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
static GfxEngine sGfx = {};
static UvdEngine sUvd = {};
static amdgpu::AtomVramReservation sReservation;
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
static uint32 sNextVideoHandle = 1;
static uint32 sVideoSessions;
static void ReleaseVideo(AmdgpuClient* client);

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
	ReleaseVideo(client);
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
	if (!sGartAllocator.Init(Gart::kSize) || !sGartAllocator.Reserve(0, Gart::kCommandBytes))
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
	sReservation = reservation;
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
WaitDmaIdle()
{
	bigtime_t deadline = system_time() + 5000000;
	while (sPending != 0 && sFault == B_OK) {
		status_t status = sCompleted.Wait(&sMutex, B_ABSOLUTE_TIMEOUT, deadline);
		if (status != B_OK && sPending != 0) return status;
	}
	return sFault == B_OK ? B_OK : B_DEV_NOT_READY;
}

static void
ReleaseVideo(AmdgpuClient* client)
{
	UvdSession* s = client->video;
	if (s == NULL) return;
	if (s->created && sFault == B_OK) {
		amdgpu_uvd_test result = {};
		status_t status = WaitDmaIdle();
		if (status == B_OK) status = sUvd.Session(*s, 2, NULL, NULL, 0, sEngine, result);
		if (status != B_OK) {
			sFault = status;
			sUvd.faulted = true;
			sUvd.Stop();
		}
	}
	delete_area(s->area);
	if (s->readbackBytes != 0) {
		if (s->readbackBound && sFault == B_OK) {
			status_t status = sGart.Unbind(s->readbackOffset, s->readbackBytes);
			if (status != B_OK) sFault = status;
		}
		// Retain wired RAM and its GART extent when late writes are possible.
		if (sFault == B_OK) {
			if (s->readbackArea >= 0) delete_area(s->readbackArea);
			sGartAllocator.Free(s->readbackOffset, s->readbackBytes);
		}
		client->systemBytes -= s->readbackBytes;
	}
	// A failed engine may still hold addresses. Keep its VRAM quarantined.
	if (sFault == B_OK) sAllocator.Free(s->offset, s->layout.bytes);
	client->bytes -= s->layout.bytes;
	client->video = NULL;
	sVideoSessions--;
	free(s);
}

static status_t
VideoControl(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	if (op == AMDGPU_VIDEO_CREATE) {
		amdgpu_video_create request;
		status_t status = ReadRequest(request, data, length);
		amdgpu::UvdH264Layout layout;
		if (status != B_OK) return status;
		if (!amdgpu::UvdH264Size(request.config, layout)) return B_BAD_VALUE;
		if (client->video != NULL) return B_BUSY;
		if (sFault != B_OK) return B_DEV_NOT_READY;
		if (sNextVideoHandle == 0 || sVideoSessions >= 32) return B_NO_MEMORY;
		UvdSession* s = (UvdSession*)calloc(1, sizeof(UvdSession));
		if (s == NULL) return B_NO_MEMORY;
		s->readbackArea = -1;
		s->config = request.config; s->layout = layout;
		if (!sAllocator.Allocate(layout.bytes, 4096, sInfo.bar_size[0], s->offset)) {
			free(s); return B_NO_MEMORY;
		}
		s->area = map_physical_memory("amdgpu private video session",
			sInfo.bar_address[0] + s->offset, layout.bytes, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&s->cpu);
		if (s->area < 0) {
			status = s->area; sAllocator.Free(s->offset, layout.bytes); free(s); return status;
		}
		s->gpu = sInfo.vram_gpu_base + s->offset;
		s->handle = sNextVideoHandle++;
		client->video = s; client->bytes += layout.bytes; sVideoSessions++;
		status = WaitDmaIdle();
		amdgpu_uvd_test result = {};
		if (status == B_OK) {
			uint64 bytes = ((layout.outputBytes + 4095ULL) & ~4095ULL) + 8192;
			if (!sGartAllocator.Allocate(bytes, 4096, Gart::kSize, s->readbackOffset))
				status = B_NO_MEMORY;
			else {
				s->readbackBytes = bytes; client->systemBytes += bytes;
				virtual_address_restrictions va = {};
				physical_address_restrictions pa = {};
				s->readbackArea = create_area_etc(B_SYSTEM_TEAM, "amdgpu private video readback",
					bytes, B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
					0, 0, &va, &pa, &s->readback);
				status = s->readbackArea < 0 ? s->readbackArea : B_OK;
				if (status == B_OK) {
					memset(s->readback, 0x7d, bytes);
					status = sGart.Bind(s->readbackOffset, bytes, s->readback);
					if (status == B_TIMED_OUT) sFault = status;
					s->readbackBound = status == B_OK;
					s->readbackGpu = Gart::kBase + s->readbackOffset;
				}
			}
		}
		// The engine and its worker are idle, and sMutex prevents new jobs
		// from being queued until the private fill/decode/readback completes.
		for (uint64 offset = 0; status == B_OK && offset < layout.bytes;) {
			uint64 bytes = min_c(layout.bytes - offset, 64ULL << 20);
			status = sEngine.Execute(AMDGPU_DMA_FILL, 0, s->gpu + offset, bytes, 0);
			if (status != B_OK) sFault = status;
			offset += bytes;
		}
		if (status == B_OK && !sUvd.ready) {
			InstalledFirmware firmware;
			status = firmware.LoadUvd();
			if (status == B_OK)
				status = sUvd.Initialize(sEngine.regs, sInfo, sReservation, firmware.view, result);
		}
		if (status == B_OK) status = sUvd.Session(*s, 0, NULL, NULL, 0, sEngine, result);
		if (sUvd.faulted && sFault == B_OK) sFault = status;
		if (status == B_OK) {
			request.handle = s->handle; request.pitch = layout.pitch;
			request.output_bytes = layout.outputBytes; request.allocated_bytes = layout.bytes;
			status = user_memcpy(data, &request, sizeof(request));
		}
		if (status != B_OK) ReleaseVideo(client);
		return status;
	}
	if (op == AMDGPU_VIDEO_DESTROY) {
		amdgpu_video_destroy request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (client->video == NULL || request.handle != client->video->handle) return B_BAD_VALUE;
		ReleaseVideo(client);
		return sFault == B_OK ? B_OK : B_DEV_NOT_READY;
	}
	amdgpu_video_decode request;
	status_t status = ReadRequest(request, data, length);
	if (status != B_OK) return status;
	UvdSession* s = client->video;
	if (s == NULL || request.handle != s->handle || request.bitstream == 0 || request.output == 0
		|| request.output_capacity != s->layout.outputBytes
		|| !amdgpu::UvdH264Validate(s->config, request.picture, request.bitstream_bytes))
		return B_BAD_VALUE;
	if (sFault != B_OK) return B_DEV_NOT_READY;
	void* input = malloc(request.bitstream_bytes);
	status = input == NULL ? B_NO_MEMORY
		: user_memcpy(input, (void*)(addr_t)request.bitstream, request.bitstream_bytes);
	amdgpu_uvd_test result = {};
	if (status == B_OK) status = WaitDmaIdle();
	if (status == B_OK)
		status = sUvd.Session(*s, 1, &request.picture, input, request.bitstream_bytes, sEngine, result);
	if (sUvd.faulted && sFault == B_OK) sFault = status;
	if (status == B_OK)
		status = user_memcpy((void*)(addr_t)request.output, (uint8*)s->readback + 4096,
			s->layout.outputBytes);
	request.sequence = result.sequence; request.fence = result.fence;
	request.rptr = result.rptr; request.wptr = result.wptr;
	request.guard_mismatches = result.guard_mismatches;
	request.vm_fault_status = result.vm_fault_status; request.vm_fault_address = result.vm_fault_address;
	memcpy(request.feedback, result.feedback, sizeof(request.feedback));
	status_t copied = user_memcpy(data, &request, sizeof(request));
	free(input);
	return status != B_OK ? status : copied;
}

static status_t
Control(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	if (!sActive)
		return B_DEV_NOT_READY;
	if (client->team != team_get_current_team_id()
		|| (client->flags & O_ACCMODE) != O_RDWR)
		return B_NOT_ALLOWED;
	if (op >= AMDGPU_VIDEO_CREATE && op <= AMDGPU_VIDEO_DESTROY) {
		// WaitDmaIdle releases sMutex. Keep another thread on this file from
		// destroying/replacing the session while the first call holds pointers.
		if (client->videoBusy) return B_BUSY;
		client->videoBusy = true;
		status_t status = VideoControl(client, op, data, length);
		client->videoBusy = false;
		return status;
	}
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
amdgpu_client_control(AmdgpuClient* client, uint32 op, void* data, size_t length,
	status_t (*initialize)())
{
	if (client->team != team_get_current_team_id()
		|| (client->flags & O_ACCMODE) != O_RDWR)
		return B_NOT_ALLOWED;
	mutex_lock(&sMutex);
	bool needsStart = !sActive;
	mutex_unlock(&sMutex);
	if (needsStart) {
		// Validate the triggering request before loading firmware or touching
		// engines. Close/free/map/submit cannot initialize an absent device.
		status_t status;
		if (op == AMDGPU_MEMORY_INFO) {
			amdgpu_memory_info request;
			status = ReadRequest(request, data, length);
		} else if (op == AMDGPU_GART_INFO) {
			amdgpu_gart_info request;
			status = ReadRequest(request, data, length);
		} else if (op == AMDGPU_CREATE_BUFFER || op == AMDGPU_CREATE_SYSTEM_BUFFER) {
			amdgpu_buffer request;
			status = ReadRequest(request, data, length);
			if (status == B_OK && (request.reserved != 0 || request.bytes == 0
				|| request.bytes > (64ULL << 20)))
				status = B_BAD_VALUE;
		} else if (op == AMDGPU_VIDEO_CREATE) {
			amdgpu_video_create request;
			amdgpu::UvdH264Layout layout;
			status = ReadRequest(request, data, length);
			if (status == B_OK && !amdgpu::UvdH264Size(request.config, layout)) status = B_BAD_VALUE;
		} else
			return B_DEV_NOT_READY;
		if (status != B_OK)
			return status;
		// Driver startup acquires sLock before sMutex. Do not hold sMutex
		// here: concurrent first clients serialize in that startup callback.
		status = initialize();
		if (status != B_OK)
			return status;
	}
	mutex_lock(&sMutex);
	status_t status = Control(client, op, data, length);
	mutex_unlock(&sMutex);
	return status;
}

status_t
amdgpu_device_gfx_test(const amdgpu::FirmwareView firmware[4], amdgpu_gfx_test& result)
{
	mutex_lock(&sMutex);
	status_t status = !sActive || sFault != B_OK ? B_DEV_NOT_READY
		: sPending != 0 ? B_BUSY : sGfx.Test(sEngine.regs, sInfo, sReservation,
			firmware, sEngine, sGart, result);
	if (sGfx.faulted && sFault == B_OK)
		sFault = status;
	mutex_unlock(&sMutex);
	return status;
}

status_t
amdgpu_device_uvd_test(const amdgpu::FirmwareView& firmware,
	amdgpu_uvd_test& result, void* output)
{
	mutex_lock(&sMutex);
	status_t status = !sActive || sFault != B_OK ? B_DEV_NOT_READY
		: sPending != 0 ? B_BUSY : sUvd.Test(sEngine.regs, sInfo, sReservation,
			firmware, result, output);
	if (sUvd.faulted && sFault == B_OK)
		sFault = status;
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
	sUvd.Uninitialize();
	sGfx.Uninitialize();
	sEngine.Uninitialize();
	sGart.Uninitialize(sFault != B_OK);
	sGartAllocator.Uninit();
	sAllocator.Uninit();
	sActive = false;
}
