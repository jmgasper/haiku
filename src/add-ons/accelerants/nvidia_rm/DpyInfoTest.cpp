// Diagnostic tool: dump what NVKMS knows about every dpy (display) of the GPU.
//
// usage: nvdpyinfo [--force] [--edid] [--rm]
//   --force  re-query each disconnected dpy with forceConnected, which makes
//            NVKMS attempt an EDID read over DDC/AUX even when it believes
//            nothing is plugged in
//   --edid   dump the raw EDID bytes of every dpy that has one
//   --rm     ask resman directly which displays it sees, and read DPCD over
//            the DisplayPort AUX channel of every display
//   --watch <seconds>
//            poll both resman and NVKMS for that long and report every change,
//            to see whether plugging a display in is noticed

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <system_error>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "class/cl0073.h" // NV04_DISPLAY_COMMON
#include "ctrl/ctrl0073/ctrl0073system.h"
#include "ctrl/ctrl0073/ctrl0073dp.h"
}


static const char *ConnectorTypeName(NvKmsConnectorType type)
{
	const char *name = NvKmsConnectorTypeString(type);
	return name != NULL ? name : "?";
}


static void PrintFirstLine(const char *label, const char *text)
{
	if (text[0] == '\0')
		return;
	char buffer[256];
	snprintf(buffer, sizeof(buffer), "%s", text);
	char *newline = strchr(buffer, '\n');
	if (newline != NULL)
		*newline = '\0';
	printf("      %s: %s\n", label, buffer);
}


// Ask resman itself what it thinks is plugged in, bypassing NVKMS, and try to
// talk to each display over the DisplayPort AUX channel.
static void RmProbe()
{
	NvRmApi rm;
	NvRmDevice rmDev(rm, 0);
	NvRmObject display = rmDev.Device().Alloc(NV04_DISPLAY_COMMON, NULL, 0);

	NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS supported {};
	display.Control(NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, &supported, sizeof(supported));
	printf("rm: supported displays 0x%x, DDC capable 0x%x\n",
		(unsigned)supported.displayMask, (unsigned)supported.displayMaskDDC);

	static const struct { const char *name; NvU32 flags; } methods[] = {
		{ "default", NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_FLAGS_METHOD_DEFAULT },
		{ "cached", NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_FLAGS_METHOD_CACHED },
		{ "hpd only", DRF_DEF(0073_CTRL, _SYSTEM_GET_CONNECT_STATE_FLAGS, _DDC, _DISABLE)
			| DRF_DEF(0073_CTRL, _SYSTEM_GET_CONNECT_STATE_FLAGS, _LOAD, _DISABLE) },
		{ "econoddc", NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_FLAGS_METHOD_ECONODDC },
	};
	for (const auto &method: methods) {
		NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS params {};
		params.displayMask = supported.displayMask;
		params.flags = method.flags;
		NvU32 tries = 0;
		do {
			params.retryTimeMs = 0;
			display.Control(NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, &params, sizeof(params));
			if (params.retryTimeMs > 0)
				snooze(params.retryTimeMs * 1000LL);
		} while (params.retryTimeMs > 0 && ++tries < 50);
		printf("rm: connect state (%s): 0x%x\n", method.name, (unsigned)params.displayMask);
	}

	{
		NV0073_CTRL_SYSTEM_GET_SET_HOTPLUG_CONFIG_PARAMS params {};
		display.Control(NV0073_CTRL_CMD_SYSTEM_GET_HOTPLUG_CONFIG, &params, sizeof(params));
		printf("rm: hotplug: event mask 0x%x, pollable 0x%x, interruptible 0x%x\n",
			(unsigned)params.hotplugEventMask, (unsigned)params.hotplugPollable,
			(unsigned)params.hotplugInterruptible);
	}
	{
		NV0073_CTRL_SYSTEM_GET_HOTPLUG_STATE_PARAMS params {};
		display.Control(NV0073_CTRL_CMD_SYSTEM_GET_HOTPLUG_STATE, &params, sizeof(params));
		printf("rm: hotplug since last edid read: 0x%x\n",
			(unsigned)params.hotplugAfterEdidMask);
	}

	for (NvU32 displayId = 1; displayId != 0; displayId <<= 1) {
		if ((supported.displayMask & displayId) == 0)
			continue;
		NV0073_CTRL_DP_AUXCH_CTRL_PARAMS aux {};
		aux.displayId = displayId;
		aux.cmd = DRF_DEF(0073_CTRL, _DP_AUXCH_CMD, _TYPE, _AUX)
			| DRF_DEF(0073_CTRL, _DP_AUXCH_CMD, _REQ_TYPE, _READ);
		aux.addr = 0x00000; // DPCD revision and link capabilities
		aux.size = 16 - 1; // the control call takes a zero-based size
		bool ok = true;
		try {
			display.Control(NV0073_CTRL_CMD_DP_AUXCH_CTRL, &aux, sizeof(aux));
		} catch (const std::system_error &ex) {
			ok = false;
			printf("rm: dpy 0x%x: AUX read failed: %s\n", (unsigned)displayId, ex.what());
		}
		if (!ok)
			continue;
		printf("rm: dpy 0x%x: AUX reply %u, %u bytes:", (unsigned)displayId,
			(unsigned)aux.replyType, (unsigned)aux.size + 1);
		for (NvU32 i = 0; i <= aux.size && i < NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE; i++)
			printf(" %02x", aux.data[i]);
		printf("\n");
	}
}


// Poll resman's and NVKMS's idea of what is connected, and report changes.
// resman sees the hot plug detect lines directly; NVKMS only learns about
// DisplayPort connections through hotplug events delivered by resman, so
// watching both tells the two apart.
static void Watch(NvKmsApi &kms, NvKmsDevice &kmsDev, int seconds)
{
	NvRmApi rm;
	NvRmDevice rmDev(rm, 0);
	NvRmObject display = rmDev.Device().Alloc(NV04_DISPLAY_COMMON, NULL, 0);

	NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS supported {};
	display.Control(NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, &supported, sizeof(supported));

	NvKmsDispHandle disp = kmsDev.Info().dispHandles[0];
	NvKmsQueryDispParams dispParams {};
	dispParams.request.deviceHandle = kmsDev.Get();
	dispParams.request.dispHandle = disp;
	CheckErrno(kms.Control(NVKMS_IOCTL_QUERY_DISP, &dispParams, sizeof(dispParams)));

	NvU32 lastRmMask = ~0u;
	NvU32 lastKmsMask = ~0u;
	bigtime_t end = system_time() + seconds * 1000000LL;
	do {
		NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS params {};
		params.displayMask = supported.displayMask;
		NvU32 tries = 0;
		do {
			params.retryTimeMs = 0;
			display.Control(NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, &params, sizeof(params));
			if (params.retryTimeMs > 0)
				snooze(params.retryTimeMs * 1000LL);
		} while (params.retryTimeMs > 0 && ++tries < 50);

		NvU32 kmsMask = 0;
		for (NVDpyId dpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(),
					dispParams.reply.validDpys);
				!nvDpyIdIsInvalid(dpyId);
				dpyId = nvNextDpyIdInDpyIdListUnsorted(dpyId, dispParams.reply.validDpys)) {
			NvKmsQueryDpyDynamicDataParams dynamicParams {};
			dynamicParams.request.deviceHandle = kmsDev.Get();
			dynamicParams.request.dispHandle = disp;
			dynamicParams.request.dpyId = dpyId;
			CheckErrno(kms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &dynamicParams,
				sizeof(dynamicParams)));
			if (dynamicParams.reply.connected)
				kmsMask |= nvDpyIdToNvU32(dpyId);
		}

		if (params.displayMask != lastRmMask || kmsMask != lastKmsMask) {
			printf("%8" B_PRId64 " ms: resman 0x%x, nvkms 0x%x\n",
				system_time() / 1000, (unsigned)params.displayMask, (unsigned)kmsMask);
			lastRmMask = params.displayMask;
			lastKmsMask = kmsMask;
		}
		snooze(500000);
	} while (system_time() < end);
}


int main(int argc, char **argv)
{
	bool force = false;
	bool dumpEdid = false;
	bool rmProbe = false;
	int watchSeconds = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--force") == 0)
			force = true;
		else if (strcmp(argv[i], "--edid") == 0)
			dumpEdid = true;
		else if (strcmp(argv[i], "--rm") == 0)
			rmProbe = true;
		else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc)
			watchSeconds = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: %s [--force] [--edid] [--rm] [--watch <seconds>]\n",
				argv[0]);
			return 1;
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	NvKmsApi kms;
	NvKmsDevice kmsDev(kms, 0);
	if (watchSeconds == 0)
		printf("device: %u disps, %u heads\n", (unsigned)kmsDev.Info().numDisps,
		(unsigned)kmsDev.Info().numHeads);

	for (NvU32 dispIndex = 0; watchSeconds == 0 && dispIndex < kmsDev.Info().numDisps;
			dispIndex++) {
		NvKmsDispHandle disp = kmsDev.Info().dispHandles[dispIndex];

		NvKmsQueryDispParams dispParams {};
		dispParams.request.deviceHandle = kmsDev.Get();
		dispParams.request.dispHandle = disp;
		CheckErrno(kms.Control(NVKMS_IOCTL_QUERY_DISP, &dispParams, sizeof(dispParams)));

		printf("disp %u: %u connectors\n", (unsigned)dispIndex,
			(unsigned)dispParams.reply.numConnectors);

		for (NVDpyId dpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(),
					dispParams.reply.validDpys);
				!nvDpyIdIsInvalid(dpyId);
				dpyId = nvNextDpyIdInDpyIdListUnsorted(dpyId, dispParams.reply.validDpys)) {
			NvKmsQueryDpyStaticDataParams staticParams {};
			staticParams.request.deviceHandle = kmsDev.Get();
			staticParams.request.dispHandle = disp;
			staticParams.request.dpyId = dpyId;
			CheckErrno(kms.Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &staticParams,
				sizeof(staticParams)));

			NvKmsQueryConnectorStaticDataParams connParams {};
			connParams.request.deviceHandle = kmsDev.Get();
			connParams.request.connectorHandle = staticParams.reply.connectorHandle;
			bool haveConnector = kms.Control(NVKMS_IOCTL_QUERY_CONNECTOR_STATIC_DATA,
				&connParams, sizeof(connParams)) >= 0;

			NvKmsQueryDpyDynamicDataParams params {};
			params.request.deviceHandle = kmsDev.Get();
			params.request.dispHandle = disp;
			params.request.dpyId = dpyId;
			CheckErrno(kms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));

			printf("  dpy %2u %-24s %s\n", (unsigned)nvDpyIdToNvU32(dpyId),
				params.reply.name, params.reply.connected ? "CONNECTED" : "disconnected");
			if (haveConnector)
			printf("      connector: %s%u (type %u.%u, phys %u.%u, signal %u)%s%s heads 0x%x\n",
				ConnectorTypeName(connParams.reply.type), (unsigned)connParams.reply.typeIndex,
				(unsigned)connParams.reply.type, (unsigned)connParams.reply.legacyTypeIndex,
				(unsigned)connParams.reply.physicalIndex,
				(unsigned)connParams.reply.physicalLocation,
				(unsigned)connParams.reply.signalFormat,
				connParams.reply.isDP ? " DP" : "",
				staticParams.reply.isDpMST ? " MST" : "",
				(unsigned)staticParams.reply.headMask);
			else
				printf("      connector: handle %u (query failed), display type %u, heads 0x%x\n",
					(unsigned)staticParams.reply.connectorHandle,
					(unsigned)staticParams.reply.type,
					(unsigned)staticParams.reply.headMask);
			if (staticParams.reply.dpAddress[0] != '\0')
				printf("      dpAddress: %s\n", staticParams.reply.dpAddress);
			printf("      edid: %u bytes%s, maxPixelClock %u kHz\n",
				(unsigned)params.reply.edid.bufferSize,
				params.reply.edid.valid ? " valid" : "",
				(unsigned)params.reply.maxPixelClockKHz);
			PrintFirstLine("edid info", params.reply.edid.infoString);
			if (params.reply.dp.guid.valid)
				printf("      dp guid: %s\n", params.reply.dp.guid.str);

			if (dumpEdid && params.reply.edid.bufferSize > 0) {
				for (NvU32 i = 0; i < params.reply.edid.bufferSize; i++) {
					printf("%s%02x", (i % 16) == 0 ? "      " : " ",
						params.reply.edid.buffer[i]);
					if ((i % 16) == 15)
						printf("\n");
				}
				printf("\n");
			}

			if (force && !params.reply.connected) {
				NvKmsQueryDpyDynamicDataParams forceParams {};
				forceParams.request.deviceHandle = kmsDev.Get();
				forceParams.request.dispHandle = disp;
				forceParams.request.dpyId = dpyId;
				forceParams.request.forceConnected = true;
				CheckErrno(kms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &forceParams,
					sizeof(forceParams)));
				printf("      forced: connected %d, edid %u bytes%s\n",
					forceParams.reply.connected,
					(unsigned)forceParams.reply.edid.bufferSize,
					forceParams.reply.edid.valid ? " valid" : "");
				PrintFirstLine("forced edid info", forceParams.reply.edid.infoString);
			}
		}
	}

	if (rmProbe)
		RmProbe();

	if (watchSeconds > 0)
		Watch(kms, kmsDev, watchSeconds);

	return 0;
}
