/* What every program here needs before it can talk to the video decoder:
 * a resman client, an address space, buffers that both sides can see, and a
 * channel on the decoder engine.
 *
 * The channel is the pre-Volta kind. Resman owns the USERD page, which is
 * reached by mapping the channel itself, and there is no doorbell: work is
 * kicked by writing GPPut and waited for by watching a semaphore.
 */
#pragma once

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "nvRmApi.h"
#include "nvos.h"
#include "nvstatus.h"
#include "class/cl0000.h"
#include "class/cl0002.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl2080_notification.h"
#include "class/cl50a0.h"
#include "class/cl90f1.h"
#include "class/cla16f.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"

#define NVC06F_CHANNEL_GPFIFO	0x0000c06f
#define NVC2B0_VIDEO_DECODER	0x0000c2b0

/* NVC2B0 shares the method numbering of every NVDEC class. */
#define NVC2B0_SET_OBJECT			0x0000
#define NVC2B0_NOP				0x0100
#define NVC2B0_SET_APPLICATION_ID		0x0200
#define NVC2B0_SET_WATCHDOG_TIMER		0x0204
#define NVC2B0_SEMAPHORE_A			0x0240
#define NVC2B0_SEMAPHORE_B			0x0244
#define NVC2B0_SEMAPHORE_C			0x0248
#define NVC2B0_EXECUTE				0x0300
#define NVC2B0_SEMAPHORE_D			0x0304
#define NVC2B0_SET_CONTROL_PARAMS		0x0400
#define NVC2B0_SET_DRV_PIC_SETUP_OFFSET		0x0404
#define NVC2B0_SET_IN_BUF_BASE_OFFSET		0x0408
#define NVC2B0_SET_PICTURE_INDEX		0x040c
#define NVC2B0_SET_SLICE_OFFSETS_BUF_OFFSET	0x0410
#define NVC2B0_SET_COLOC_DATA_OFFSET		0x0414
#define NVC2B0_SET_HISTORY_OFFSET		0x0418
#define NVC2B0_SET_DISPLAY_BUF_SIZE		0x041c
#define NVC2B0_SET_HISTOGRAM_OFFSET		0x0420
#define NVC2B0_SET_NVDEC_STATUS_OFFSET		0x0424
#define NVC2B0_SET_DISPLAY_BUF_LUMA_OFFSET	0x0428
#define NVC2B0_SET_DISPLAY_BUF_CHROMA_OFFSET	0x042c
#define NVC2B0_SET_PICTURE_LUMA_OFFSET0		0x0430
#define NVC2B0_SET_PICTURE_CHROMA_OFFSET0	0x0474
#define NVC2B0_SET_PIC_SCRATCH_BUF_OFFSET	0x04b8
#define NVC2B0_SET_EXTERNAL_MVBUFFER_OFFSET	0x04bc
#define NVC2B0_H264_SET_MBHIST_BUF_OFFSET	0x0500

#define NVDEC_APP_ID_MPEG12	1
#define NVDEC_APP_ID_VC1	2
#define NVDEC_APP_ID_H264	3
#define NVDEC_APP_ID_MPEG4	4
#define NVDEC_APP_ID_VP8	5
#define NVDEC_APP_ID_HEVC	7
#define NVDEC_APP_ID_VP9	9

#define USERD_GPGET		(0x88 / 4)
#define USERD_GPPUT		(0x8c / 4)

/* One buffer, with a place in the GPU's address space and in ours. */
typedef struct {
	NvHandle	hPhys;
	NvHandle	hVirt;
	uint64_t	gpuAddr;
	void		*map;
	NvRmApiMapping	mapping;
	uint64_t	size;
	bool		sysmem;
} NvBuffer;

typedef struct {
	NvRmApi		rm;
	int		devFd;
	NvHandle	hClient, hDevice, hSubdevice, hVaSpace;
	NvHandle	hCtxDma, hChannel, hDecoder;
	NvRmApiMapping	userdMap;
	volatile uint32_t *userd;
	NvBuffer	notifier, gpFifo, pushBuf, sem;
	uint32_t	gpPut;
	uint32_t	semValue;
	uint64_t	vaNext;
} NvDecGpu;

#define NVDEC_VA_BASE	0x00200000ull

static NvU32
nvdecAllocBuffer(NvDecGpu *gpu, NvBuffer *buf, uint64_t size, bool sysmem)
{
	memset(buf, 0, sizeof(*buf));
	size = (size + 0xfff) & ~0xfffull;
	buf->size = size;
	buf->sysmem = sysmem;

	NV_MEMORY_ALLOCATION_PARAMS phys = {
		.owner = gpu->hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = 0x1000,
	};
	if (sysmem) {
		phys.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI);
		phys.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
	} else {
		phys.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM);
		phys.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
		phys.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
	}
	NvU32 st = nvRmApiAlloc(&gpu->rm, gpu->hDevice, &buf->hPhys,
		sysmem ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &phys);
	if (st != NV_OK) return st;

	uint64_t addr = gpu->vaNext;
	gpu->vaNext += (size + 0xfffff) & ~0xfffffull;
	NV_MEMORY_ALLOCATION_PARAMS virt = {
		.owner = gpu->hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE,
		.size = size,
		.alignment = 0x1000,
		.offset = addr,
		.hVASpace = gpu->hVaSpace,
	};
	st = nvRmApiAlloc(&gpu->rm, gpu->hDevice, &buf->hVirt, NV50_MEMORY_VIRTUAL, &virt);
	if (st != NV_OK) return st;

	NvU32 mapFlags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
		| (sysmem ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
			  : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
	NvU64 dmaOffset = 0;
	st = nvRmApiMapMemoryDma(&gpu->rm, gpu->hDevice, buf->hVirt, buf->hPhys, 0, size, mapFlags, &dmaOffset);
	if (st != NV_OK) return st;
	buf->gpuAddr = dmaOffset;

	st = nvRmApiMapMemory(&gpu->rm, gpu->hDevice, buf->hPhys, 0, size, sysmem, 0, &buf->mapping);
	if (st != NV_OK) return st;
	buf->map = buf->mapping.address;
	memset(buf->map, 0, size);
	return NV_OK;
}

#define NVDEC_TRY(what, expr) do { \
	NvU32 _st = (expr); \
	if (_st != NV_OK) { printf("%s failed: %#x\n", what, _st); return _st; } \
} while (0)

/* Bring up a client, an address space and a channel on the decoder engine. */
static NvU32
nvdecOpen(NvDecGpu *gpu)
{
	memset(gpu, 0, sizeof(*gpu));
	gpu->vaNext = NVDEC_VA_BASE;

	/* Every request goes through the control node; memory on the card is
	 * mapped through the card's own node, which is what nodeName is for. */
	gpu->rm.fd = open("/dev/nvidiactl", O_RDWR);
	if (gpu->rm.fd < 0) { perror("open /dev/nvidiactl"); return NV_ERR_GENERIC; }
	gpu->rm.nodeName = "/dev/graphics/nvidia0";
	gpu->devFd = open(gpu->rm.nodeName, O_RDWR);
	if (gpu->devFd < 0) { perror("open /dev/graphics/nvidia0"); return NV_ERR_GENERIC; }

	nv_ioctl_card_info_t ci[8];
	memset(ci, 0, sizeof(ci));
	NVDEC_TRY("card info", nvRmApiCardInfo(&gpu->rm, ci, sizeof(ci)));
	NVDEC_TRY("client", nvRmApiAlloc(&gpu->rm, 0, &gpu->hClient, NV01_ROOT_CLIENT, NULL));
	gpu->rm.hClient = gpu->hClient;

	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci[0].gpu_id };
	NVDEC_TRY("gpu id", nvRmApiControl(&gpu->rm, gpu->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id)));

	NV0080_ALLOC_PARAMETERS devParams = { .deviceId = id.deviceInstance, .hClientShare = gpu->hClient };
	NVDEC_TRY("device", nvRmApiAlloc(&gpu->rm, gpu->hClient, &gpu->hDevice, NV01_DEVICE_0, &devParams));
	NV2080_ALLOC_PARAMETERS subParams = { .subDeviceId = id.subDeviceInstance };
	NVDEC_TRY("subdevice", nvRmApiAlloc(&gpu->rm, gpu->hDevice, &gpu->hSubdevice, NV20_SUBDEVICE_0, &subParams));

	NV_VASPACE_ALLOCATION_PARAMETERS vaParams = {
		.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
	};
	NVDEC_TRY("address space", nvRmApiAlloc(&gpu->rm, gpu->hDevice, &gpu->hVaSpace, FERMI_VASPACE_A, &vaParams));

	NVDEC_TRY("notifier memory", nvdecAllocBuffer(gpu, &gpu->notifier, 0x1000, true));
	NVDEC_TRY("gpfifo memory", nvdecAllocBuffer(gpu, &gpu->gpFifo, 0x1000, true));
	NVDEC_TRY("pushbuffer memory", nvdecAllocBuffer(gpu, &gpu->pushBuf, 0x10000, true));
	NVDEC_TRY("semaphore memory", nvdecAllocBuffer(gpu, &gpu->sem, 0x1000, true));

	NV_CONTEXT_DMA_ALLOCATION_PARAMS ctxDmaParams = {
		.flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) | DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),
		.hMemory = gpu->notifier.hPhys,
		.offset = 0,
		.limit = gpu->notifier.size - 1,
	};
	NVDEC_TRY("error notifier", nvRmApiAlloc(&gpu->rm, gpu->hDevice, &gpu->hCtxDma, NV01_CONTEXT_DMA, &ctxDmaParams));

	NV_CHANNEL_ALLOC_PARAMS chanParams = {
		.hObjectError = gpu->hCtxDma,
		.gpFifoOffset = gpu->gpFifo.gpuAddr,
		.gpFifoEntries = 0x100,
		.hVASpace = gpu->hVaSpace,
		.engineType = NV2080_ENGINE_TYPE_NVDEC0,
	};
	NVDEC_TRY("decoder channel", nvRmApiAlloc(&gpu->rm, gpu->hDevice, &gpu->hChannel, NVC06F_CHANNEL_GPFIFO, &chanParams));

	NVDEC_TRY("userd", nvRmApiMapMemory(&gpu->rm, gpu->hSubdevice, gpu->hChannel, 0, 0x1000, false,
		DRF_DEF(OS33, _FLAGS, _FIFO_MAPPING, _ENABLE), &gpu->userdMap));
	gpu->userd = gpu->userdMap.address;

	NVDEC_TRY("decoder object", nvRmApiAlloc(&gpu->rm, gpu->hChannel, &gpu->hDecoder, NVC2B0_VIDEO_DECODER, NULL));

	NVA06F_CTRL_BIND_PARAMS bindParams = { .engineType = NV2080_ENGINE_TYPE_NVDEC0 };
	NVDEC_TRY("bind", nvRmApiControl(&gpu->rm, gpu->hChannel, NVA06F_CTRL_CMD_BIND, &bindParams, sizeof(bindParams)));
	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS schedParams = { .bEnable = NV_TRUE };
	NVDEC_TRY("schedule", nvRmApiControl(&gpu->rm, gpu->hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &schedParams, sizeof(schedParams)));
	return NV_OK;
}

/* A Fermi method header: increasing methods, count arguments, one subchannel. */
static inline uint32_t
nvdecMthd(uint32_t method, uint32_t count)
{
	return (0x2u << 28) | (count << 16) | (0u << 13) | (method >> 2);
}

typedef struct {
	uint32_t *start;
	uint32_t *at;
} NvPush;

static inline void nvpBegin(NvPush *p, NvDecGpu *gpu)
{
	p->start = gpu->pushBuf.map;
	p->at = p->start;
}

static inline void nvpMethod(NvPush *p, uint32_t method, uint32_t value)
{
	*p->at++ = nvdecMthd(method, 1);
	*p->at++ = value;
}

/* Every address the decoder is given is in units of 256 bytes. */
static inline void nvpOffset(NvPush *p, uint32_t method, uint64_t gpuAddr)
{
	nvpMethod(p, method, (uint32_t)(gpuAddr >> 8));
}

/* Hand the decoder what has been built, and wait for it to say it is done.
 * Returns true if the engine answered before the timeout. */
static bool
nvdecSubmit(NvDecGpu *gpu, NvPush *p, int timeoutMs)
{
	volatile uint32_t *semPtr = gpu->sem.map;
	uint32_t want = ++gpu->semValue;
	*semPtr = 0;

	nvpMethod(p, NVC2B0_SEMAPHORE_A, (uint32_t)(gpu->sem.gpuAddr >> 32) & 0xff);
	nvpMethod(p, NVC2B0_SEMAPHORE_B, (uint32_t)gpu->sem.gpuAddr);
	nvpMethod(p, NVC2B0_SEMAPHORE_C, want);
	nvpMethod(p, NVC2B0_SEMAPHORE_D, 0);

	uint32_t dwords = p->at - p->start;
	uint32_t *gp = gpu->gpFifo.map;
	gp[2 * gpu->gpPut + 0] = (uint32_t)gpu->pushBuf.gpuAddr & ~3u;
	gp[2 * gpu->gpPut + 1] = ((uint32_t)(gpu->pushBuf.gpuAddr >> 32) & 0xff) | (dwords << 10);
	gpu->gpPut++;
	gpu->userd[USERD_GPPUT] = gpu->gpPut;

	for (int i = 0; i < timeoutMs; i++) {
		if (*semPtr == want) return true;
		usleep(1000);
	}
	return false;
}

/* Where the decoder puts the pixel at (x, y) of a plane. A 512 byte group
 * covers 64 columns and 8 lines; two of them stack into a block sixteen lines
 * tall, and blocks run across the picture and then down it. Measured on a
 * GP102 by decoding a picture whose every line, and then every column, was a
 * different value - the arrangement inside a group is not the one Mesa and
 * nouveau use for graphics surfaces. */
static inline size_t
nvdecTileOffset(int x, int y, int pitch)
{
	const int blockHeight = 2;		/* groups per block */
	int blocksPerRow = pitch / 64;
	int blockY = (y / 8) / blockHeight;
	int groupInBlock = (y / 8) % blockHeight;
	size_t base = ((size_t)(blockY * blocksPerRow + x / 64) * blockHeight
		+ groupInBlock) * 512;
	int xx = x % 64, yy = y % 8;
	return base + (xx / 32) * 256 + (yy / 4) * 128
		+ ((xx % 32) / 16) * 64 + (yy % 4) * 16 + (xx % 16);
}

static inline void
nvdecUntile(uint8_t *out, const uint8_t *tiled, int width, int height, int pitch)
{
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
			out[(size_t)y * width + x] = tiled[nvdecTileOffset(x, y, pitch)];
}
