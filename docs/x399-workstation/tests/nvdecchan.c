/* Put work on the video decoder and wait for the decoder to answer.
 *
 * Nothing here decodes anything yet. It builds the smallest thing that can
 * fail: a channel bound to the decoder engine, an NVC2B0 object on it, and a
 * method stream whose only instruction is for the engine to write a value into
 * memory. If that value arrives, the falcon was given its microcode, booted,
 * and ran a method we wrote - which is everything the decoder needs before a
 * picture can be argued about.
 *
 * The channel setup is the pre-Volta one: resman owns the USERD page and there
 * is no doorbell, so work is kicked by writing GPPut and waiting.
 */
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

/* NVC2B0 shares the decoder method numbering of every NVDEC class. */
#define NVC2B0_SET_OBJECT		0x0000
#define NVC2B0_NOP			0x0100
#define NVC2B0_SET_APPLICATION_ID	0x0200
#define NVC2B0_SET_WATCHDOG_TIMER	0x0204
#define NVC2B0_SEMAPHORE_A		0x0240
#define NVC2B0_SEMAPHORE_B		0x0244
#define NVC2B0_SEMAPHORE_C		0x0248
#define NVC2B0_EXECUTE			0x0300
#define NVC2B0_SEMAPHORE_D		0x0304

#define APP_ID_H264			3

#define USERD_GPPUT			(0x8c / 4)
#define USERD_GPGET			(0x88 / 4)

#define VA_BASE				0x00200000ull

static NvRmApi rm;
static NvHandle hClient, hDevice, hSubdevice, hVaSpace;
static uint64_t vaNext = VA_BASE;

#define CHECK(what, expr) do { \
	NvU32 _st = (expr); \
	if (_st != NV_OK) { printf("%s failed: %#x\n", what, _st); return 1; } \
} while (0)

/* One buffer, with a place in the GPU's address space and in ours. */
typedef struct {
	NvHandle	hPhys;
	NvHandle	hVirt;
	uint64_t	gpuAddr;
	void		*map;
	NvRmApiMapping	mapping;
	uint64_t	size;
	bool		sysmem;
} Buffer;

static NvU32
allocBuffer(Buffer *buf, uint64_t size, bool sysmem)
{
	memset(buf, 0, sizeof(*buf));
	size = (size + 0xfff) & ~0xfffull;
	buf->size = size;
	buf->sysmem = sysmem;

	NV_MEMORY_ALLOCATION_PARAMS phys = {
		.owner = hClient,
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
	NvU32 st = nvRmApiAlloc(&rm, hDevice, &buf->hPhys, sysmem ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &phys);
	if (st != NV_OK) return st;

	uint64_t addr = vaNext;
	vaNext += (size + 0xfffff) & ~0xfffffull;
	NV_MEMORY_ALLOCATION_PARAMS virt = {
		.owner = hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE,
		.size = size,
		.alignment = 0x1000,
		.offset = addr,
		.hVASpace = hVaSpace,
	};
	st = nvRmApiAlloc(&rm, hDevice, &buf->hVirt, NV50_MEMORY_VIRTUAL, &virt);
	if (st != NV_OK) return st;

	NvU32 mapFlags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
		| (sysmem ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
			  : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
	NvU64 dmaOffset = 0;
	st = nvRmApiMapMemoryDma(&rm, hDevice, buf->hVirt, buf->hPhys, 0, size, mapFlags, &dmaOffset);
	if (st != NV_OK) return st;
	buf->gpuAddr = dmaOffset;

	st = nvRmApiMapMemory(&rm, hDevice, buf->hPhys, 0, size, sysmem, 0, &buf->mapping);
	if (st != NV_OK) return st;
	buf->map = buf->mapping.address;
	memset(buf->map, 0, size);
	return NV_OK;
}

/* A Fermi method header: increasing methods, count arguments, one subchannel. */
static inline uint32_t
mthd(uint32_t subch, uint32_t method, uint32_t count)
{
	return (0x2u << 28) | (count << 16) | (subch << 13) | (method >> 2);
}

int
main(void)
{
	bool verbose = getenv("NVDEC_VERBOSE") != NULL;
	/* Every request goes through the control node; memory on the card is
	 * mapped through the card's own node, which is what nodeName is for. */
	rm.fd = open("/dev/nvidiactl", O_RDWR);
	if (rm.fd < 0) { perror("open /dev/nvidiactl"); return 1; }
	rm.nodeName = "/dev/graphics/nvidia0";
	int devFd = open(rm.nodeName, O_RDWR);
	if (devFd < 0) { perror("open /dev/graphics/nvidia0"); return 1; }

	nv_ioctl_card_info_t ci[8];
	memset(ci, 0, sizeof(ci));
	CHECK("card info", nvRmApiCardInfo(&rm, ci, sizeof(ci)));
	CHECK("client", nvRmApiAlloc(&rm, 0, &hClient, NV01_ROOT_CLIENT, NULL));
	rm.hClient = hClient;

	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci[0].gpu_id };
	CHECK("gpu id", nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id)));

	NV0080_ALLOC_PARAMETERS devParams = { .deviceId = id.deviceInstance, .hClientShare = hClient };
	CHECK("device", nvRmApiAlloc(&rm, hClient, &hDevice, NV01_DEVICE_0, &devParams));
	NV2080_ALLOC_PARAMETERS subParams = { .subDeviceId = id.subDeviceInstance };
	CHECK("subdevice", nvRmApiAlloc(&rm, hDevice, &hSubdevice, NV20_SUBDEVICE_0, &subParams));

	NV_VASPACE_ALLOCATION_PARAMETERS vaParams = {
		.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
	};
	CHECK("address space", nvRmApiAlloc(&rm, hDevice, &hVaSpace, FERMI_VASPACE_A, &vaParams));

	Buffer notifier, gpFifo, pushBuf, sem;
	CHECK("notifier memory", allocBuffer(&notifier, 0x1000, true));
	CHECK("gpfifo memory", allocBuffer(&gpFifo, 0x1000, true));
	CHECK("pushbuffer memory", allocBuffer(&pushBuf, 0x1000, true));
	CHECK("semaphore memory", allocBuffer(&sem, 0x1000, true));

	NV_CONTEXT_DMA_ALLOCATION_PARAMS ctxDmaParams = {
		.flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) | DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),
		.hMemory = notifier.hPhys,
		.offset = 0,
		.limit = notifier.size - 1,
	};
	NvHandle hCtxDma = 0;
	CHECK("error notifier", nvRmApiAlloc(&rm, hDevice, &hCtxDma, NV01_CONTEXT_DMA, &ctxDmaParams));

	/* The channel goes on the decoder, not on graphics. */
	NvU32 engineType = NV2080_ENGINE_TYPE_NVDEC0;
	NV_CHANNEL_ALLOC_PARAMS chanParams = {
		.hObjectError = hCtxDma,
		.gpFifoOffset = gpFifo.gpuAddr,
		.gpFifoEntries = 0x100,
		.hVASpace = hVaSpace,
		.engineType = engineType,
	};
	NvHandle hChannel = 0;
	CHECK("decoder channel", nvRmApiAlloc(&rm, hDevice, &hChannel, NVC06F_CHANNEL_GPFIFO, &chanParams));
	printf("channel on the decoder engine: ok\n");

	/* Before Volta resman owns USERD, reached by mapping the channel. */
	NvRmApiMapping userdMap;
	CHECK("userd", nvRmApiMapMemory(&rm, hSubdevice, hChannel, 0, 0x1000, false,
		DRF_DEF(OS33, _FLAGS, _FIFO_MAPPING, _ENABLE), &userdMap));
	volatile uint32_t *userd = userdMap.address;

	NvHandle hDecoder = 0;
	CHECK("decoder object", nvRmApiAlloc(&rm, hChannel, &hDecoder, NVC2B0_VIDEO_DECODER, NULL));
	printf("NVC2B0 video decoder object: ok\n");

	NVA06F_CTRL_BIND_PARAMS bindParams = { .engineType = engineType };
	CHECK("bind", nvRmApiControl(&rm, hChannel, NVA06F_CTRL_CMD_BIND, &bindParams, sizeof(bindParams)));
	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS schedParams = { .bEnable = NV_TRUE };
	CHECK("schedule", nvRmApiControl(&rm, hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &schedParams, sizeof(schedParams)));
	printf("channel scheduled\n");

	/* Ask the engine itself to write a value where we can see it. */
	const uint32_t kMagic = 0x0decc0de;
	volatile uint32_t *semPtr = sem.map;
	semPtr[0] = 0;

	uint32_t *p = pushBuf.map;
	uint32_t *start = p;
	*p++ = mthd(0, NVC2B0_SET_OBJECT, 1);
	*p++ = NVC2B0_VIDEO_DECODER;
	*p++ = mthd(0, NVC2B0_SET_APPLICATION_ID, 1);
	*p++ = APP_ID_H264;
	*p++ = mthd(0, NVC2B0_SEMAPHORE_A, 3);
	*p++ = (uint32_t)(sem.gpuAddr >> 32) & 0xff;	/* SEMAPHORE_A upper */
	*p++ = (uint32_t)sem.gpuAddr;			/* SEMAPHORE_B lower */
	*p++ = kMagic;					/* SEMAPHORE_C payload */
	*p++ = mthd(0, NVC2B0_SEMAPHORE_D, 1);
	*p++ = 0;					/* release, one word */
	uint32_t pushDwords = p - start;

	uint32_t *gp = gpFifo.map;
	gp[0] = (uint32_t)pushBuf.gpuAddr & ~3u;
	gp[1] = ((uint32_t)(pushBuf.gpuAddr >> 32) & 0xff) | (pushDwords << 10);

	if (verbose) {
		printf("pushbuffer at %#llx, %u words; semaphore at %#llx\n",
			(unsigned long long)pushBuf.gpuAddr, pushDwords,
			(unsigned long long)sem.gpuAddr);
		for (uint32_t i = 0; i < pushDwords; i++)
			printf("  [%u] %08x\n", i, start[i]);
	}

	userd[USERD_GPPUT] = 1;

	uint32_t got = 0;
	int waited = 0;
	for (; waited < 2000; waited++) {
		got = semPtr[0];
		if (got == kMagic) break;
		usleep(1000);
	}

	NvNotification *notes = notifier.map;
	printf("semaphore: %#x after %d ms (wanted %#x)\n", got, waited, kMagic);
	printf("GPGet %u GPPut %u\n", userd[USERD_GPGET], userd[USERD_GPPUT]);
	printf("error notifier: info32 %#x info16 %#x status %#x\n",
		notes[0].info32, notes[0].info16, notes[0].status);

	if (got == kMagic) {
		printf("the decoder engine ran our methods\n");
		return 0;
	}
	printf("the decoder engine did not answer\n");
	return 1;
}
