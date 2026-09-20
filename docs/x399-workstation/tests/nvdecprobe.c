/* Ask the GPU what video engines and classes it has.
 *
 * The decoder is a separate engine from graphics, with its own classes, and
 * nothing so far has asked resman whether this card exposes them at all.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "nvRmApi.h"
#include "nvos.h"
#include "nvstatus.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl2080_notification.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

static const char *engineName(NvU32 type)
{
	switch (type) {
	case NV2080_ENGINE_TYPE_GRAPHICS: return "graphics";
	case NV2080_ENGINE_TYPE_COPY0: return "copy0";
	case NV2080_ENGINE_TYPE_COPY1: return "copy1";
	case NV2080_ENGINE_TYPE_COPY2: return "copy2";
	case NV2080_ENGINE_TYPE_COPY3: return "copy3";
	case NV2080_ENGINE_TYPE_COPY4: return "copy4";
	case NV2080_ENGINE_TYPE_COPY5: return "copy5";
	case NV2080_ENGINE_TYPE_NVDEC0: return "nvdec0";
	case NV2080_ENGINE_TYPE_NVDEC1: return "nvdec1";
	case NV2080_ENGINE_TYPE_NVENC0: return "nvenc0";
	case NV2080_ENGINE_TYPE_NVENC1: return "nvenc1";
	case NV2080_ENGINE_TYPE_SEC2: return "sec2";
	case NV2080_ENGINE_TYPE_NVJPEG0: return "nvjpeg0";
	case NV2080_ENGINE_TYPE_OFA: return "ofa";
	case NV2080_ENGINE_TYPE_SW: return "software";
	case NV2080_ENGINE_TYPE_CIPHER: return "cipher";
	case NV2080_ENGINE_TYPE_VIC: return "vic";
	case NV2080_ENGINE_TYPE_ME: return "me";
	case NV2080_ENGINE_TYPE_PPP: return "ppp";
	default: return "";
	}
}

/* Video decoder classes end in b0, encoders in b7, copy engines in b5. */
static const char *className(NvU32 cls)
{
	switch (cls) {
	case 0xa0b0: return "NVA0B0 video decoder (Maxwell)";
	case 0xb0b0: return "NVB0B0 video decoder (GM20x)";
	case 0xb6b0: return "NVB6B0 video decoder (GP100)";
	case 0xc1b0: return "NVC1B0 video decoder (GP10x)";
	case 0xc2b0: return "NVC2B0 video decoder";
	case 0xc3b0: return "NVC3B0 video decoder";
	case 0xc4b0: return "NVC4B0 video decoder (Turing)";
	case 0xa0b7: return "NVA0B7 video encoder";
	case 0xc0b7: return "NVC0B7 video encoder";
	case 0xc1b7: return "NVC1B7 video encoder (GP10x)";
	case 0xb0b5: return "NVB0B5 copy engine (Maxwell)";
	case 0xc0b5: return "NVC0B5 copy engine (Pascal)";
	case 0xc1b5: return "NVC1B5 copy engine";
	case 0xc36f: return "NVC36F channel gpfifo";
	case 0xc06f: return "NVC06F channel gpfifo (Pascal)";
	case 0xb06f: return "NVB06F channel gpfifo (Maxwell)";
	case 0xc097: return "NVC097 3D (Pascal)";
	case 0xc0c0: return "NVC0C0 compute (Pascal)";
	case 0xc1c0: return "NVC1C0 compute";
	default: return NULL;
	}
}

int main(void)
{
	NvRmApi rm = { .fd = open("/dev/nvidiactl", O_RDWR), .nodeName = "/dev/nvidiactl" };
	if (rm.fd < 0) { perror("open /dev/nvidiactl"); return 1; }
	nv_ioctl_card_info_t ci[8];
	memset(ci, 0, sizeof(ci));
	NvU32 st = nvRmApiCardInfo(&rm, ci, sizeof(ci));
	if (st != NV_OK) { printf("card info failed: %#x\n", st); return 1; }

	NvHandle hClient = 0;
	if ((st = nvRmApiAlloc(&rm, 0, &hClient, NV01_ROOT_CLIENT, NULL)) != NV_OK) {
		printf("client failed: %#x\n", st); return 1;
	}
	rm.hClient = hClient;

	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci[0].gpu_id };
	if ((st = nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id))) != NV_OK) {
		printf("id info failed: %#x\n", st); return 1;
	}

	NV0080_ALLOC_PARAMETERS ap0080 = { .deviceId = id.deviceInstance, .hClientShare = hClient };
	NV2080_ALLOC_PARAMETERS ap2080 = { .subDeviceId = id.subDeviceInstance };
	NvHandle hDevice = 0, hSubdevice = 0;
	if ((st = nvRmApiAlloc(&rm, hClient, &hDevice, NV01_DEVICE_0, &ap0080)) != NV_OK) {
		printf("device failed: %#x\n", st); return 1;
	}
	if ((st = nvRmApiAlloc(&rm, hDevice, &hSubdevice, NV20_SUBDEVICE_0, &ap2080)) != NV_OK) {
		printf("subdevice failed: %#x\n", st); return 1;
	}

	NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS eng;
	memset(&eng, 0, sizeof(eng));
	st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_GPU_GET_ENGINES_V2, &eng, sizeof(eng));
	printf("engines (%#x, %u):\n", st, eng.engineCount);
	for (NvU32 i = 0; i < eng.engineCount; i++)
		printf("  %#06x %s\n", eng.engineList[i], engineName(eng.engineList[i]));

	static NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS cl;
	memset(&cl, 0, sizeof(cl));
	st = nvRmApiControl(&rm, hDevice, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &cl, sizeof(cl));
	printf("classes (%#x, %u):\n", st, cl.numClasses);
	for (NvU32 i = 0; i < cl.numClasses && i < NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE; i++) {
		const char *name = className(cl.classList[i]);
		if (name != NULL)
			printf("  %#06x  %s\n", cl.classList[i], name);
	}
	printf("all classes:");
	for (NvU32 i = 0; i < cl.numClasses && i < NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE; i++)
		printf(" %04x", cl.classList[i]);
	printf("\n");
	return 0;
}
