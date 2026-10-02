/* Query NVIDIA RM state through the nvidia_rm devices. */
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
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl2080/ctrl2080rc.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

int main(void)
{
	NvRmApi rm = { .fd = open("/dev/nvidiactl", O_RDWR), .nodeName = "/dev/nvidiactl" };
	if (rm.fd < 0) { perror("open"); return 1; }
	nv_ioctl_card_info_t ci[8];
	memset(ci, 0, sizeof(ci));
	NvU32 st = nvRmApiCardInfo(&rm, ci, sizeof(ci));
	printf("card info: %#x gpu_id %#x\n", st, ci[0].gpu_id);
	NvHandle hClient = 0;
	st = nvRmApiAlloc(&rm, 0, &hClient, NV01_ROOT_CLIENT, NULL);
	printf("client: %#x\n", st);
	rm.hClient = hClient;
	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci[0].gpu_id };
	st = nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
	printf("id info: %#x dev %u sub %u\n", st, id.deviceInstance, id.subDeviceInstance);
	NV0080_ALLOC_PARAMETERS ap0080 = { .deviceId = id.deviceInstance, .hClientShare = hClient };
	NV2080_ALLOC_PARAMETERS ap2080 = { .subDeviceId = id.subDeviceInstance };
	NvHandle hDevice = 0, hSubdevice = 0;
	printf("device: %#x\n", nvRmApiAlloc(&rm, hClient, &hDevice, NV01_DEVICE_0, &ap0080));
	printf("subdevice: %#x\n", nvRmApiAlloc(&rm, hDevice, &hSubdevice, NV20_SUBDEVICE_0, &ap2080));

	NV2080_CTRL_RC_GET_WATCHDOG_INFO_PARAMS wd = {0};
	st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_RC_GET_WATCHDOG_INFO, &wd, sizeof(wd));
	printf("watchdog: %#x flags %#x\n", st, wd.watchdogStatusFlags);

	NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS eng = {0};
	st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_GPU_GET_ENGINES_V2, &eng, sizeof(eng));
	printf("engines: %#x count %u:", st, eng.engineCount);
	for (NvU32 i = 0; i < eng.engineCount; i++)
		printf(" %#x", eng.engineList[i]);
	printf("\n");
	NV2080_CTRL_RC_GET_ERROR_COUNT_PARAMS ec = {0};
	st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_RC_GET_ERROR_COUNT, &ec, sizeof(ec));
	printf("rc error count: %#x %u\n", st, ec.errorCount);

	for (NvU32 i = 0; i < ec.errorCount && i < 4; i++) {
		static NV2080_CTRL_RC_GET_ERROR_V2_PARAMS er;
		memset(&er, 0, sizeof(er));
		er.whichBuffer = i;
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_RC_GET_ERROR_V2, &er, sizeof(er));
		printf("rc error %u: %#x size %u\n", i, st, er.outputRecordSize);
		for (NvU32 j = 0; j < er.outputRecordSize && j < 256; j++)
			printf("%02x%s", er.recordBuffer[j], (j % 32) == 31 ? "\n" : " ");
		printf("\n");
	}

	if (getenv("RMPROBE_ENABLE_WATCHDOG") != NULL) {
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_RC_RELEASE_WATCHDOG_REQUESTS, NULL, 0);
		printf("release watchdog requests: %#x\n", st);
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_RC_ENABLE_WATCHDOG, NULL, 0);
		printf("enable watchdog: %#x\n", st);
		sleep(getenv("RMPROBE_WAIT") ? atoi(getenv("RMPROBE_WAIT")) : 15);
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_RC_GET_WATCHDOG_INFO, &wd, sizeof(wd));
		printf("watchdog: %#x flags %#x\n", st, wd.watchdogStatusFlags);
	}
	return 0;
}
