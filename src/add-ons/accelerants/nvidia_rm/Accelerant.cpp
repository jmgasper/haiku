#include <Accelerant.h>

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <atomic>
#include <mutex>
#include <vector>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include "NvUtils.h"
#include "NvKmsBitmap.h"
#include "nv-haiku.h"

extern "C" {
#include "ctrl/ctrl2080/ctrl2080gpu.h" // NV2080_CTRL_CMD_GPU_GET_NAME_STRING
}

#include "common/edid.h"


static inline status_t ToErrorCode(const std::system_error &ex)
{
	if (ex.code().category() == std::generic_category()) {
		return ex.code().value();
	}
	return B_ERROR;
}


static uint32 CalcRefreshRate(const display_timing &haikuModeTimings)
{
	uint64 multiplier = (B_TIMING_INTERLACED & haikuModeTimings.flags) != 0 ? 2000000LL : 1000000LL;
	return (uint32)(multiplier * haikuModeTimings.pixel_clock / ((uint64)haikuModeTimings.h_total * haikuModeTimings.v_total));
}

static display_timing ToHaikuModeTimings(const NvModeTimings &nvKmsModeTimings) {
	display_timing haikuModeTimings {
		.pixel_clock  = nvKmsModeTimings.pixelClockHz / 1000,
		.h_display    = nvKmsModeTimings.hVisible,
		.h_sync_start = nvKmsModeTimings.hSyncStart,
		.h_sync_end   = nvKmsModeTimings.hSyncEnd,
		.h_total      = nvKmsModeTimings.hTotal,
		.v_display    = nvKmsModeTimings.vVisible,
		.v_sync_start = nvKmsModeTimings.vSyncStart,
		.v_sync_end   = nvKmsModeTimings.vSyncEnd,
		.v_total      = nvKmsModeTimings.vTotal,
		.flags        =
			(nvKmsModeTimings.interlaced ? B_TIMING_INTERLACED : 0) |
			(nvKmsModeTimings.hSyncPos ? B_POSITIVE_HSYNC : 0) |
			(nvKmsModeTimings.vSyncPos ? B_POSITIVE_VSYNC : 0),
	};
	return haikuModeTimings;
}

static NvModeTimings ToNvKmsModeTimings(const display_timing &haikuModeTimings) {
	NvModeTimings nvKmsModeTimings {
		.RRx1k        = CalcRefreshRate(haikuModeTimings),
		.pixelClockHz = haikuModeTimings.pixel_clock * 1000,
		.hVisible     = haikuModeTimings.h_display,
		.hSyncStart   = haikuModeTimings.h_sync_start,
		.hSyncEnd     = haikuModeTimings.h_sync_end,
		.hTotal       = haikuModeTimings.h_total,
		.vVisible     = haikuModeTimings.v_display,
		.vSyncStart   = haikuModeTimings.v_sync_start,
		.vSyncEnd     = haikuModeTimings.v_sync_end,
		.vTotal       = haikuModeTimings.v_total,
		.interlaced   = (B_TIMING_INTERLACED & haikuModeTimings.flags) != 0,
		.hSyncPos     = (B_POSITIVE_HSYNC & haikuModeTimings.flags) != 0,
		.hSyncNeg     = (B_POSITIVE_HSYNC & haikuModeTimings.flags) == 0,
		.vSyncPos     = (B_POSITIVE_VSYNC & haikuModeTimings.flags) != 0,
		.vSyncNeg     = (B_POSITIVE_VSYNC & haikuModeTimings.flags) == 0,
	};
	return nvKmsModeTimings;
}

static display_mode ToHaikuMode(const NvKmsMode &nvKmsMode) {
	display_mode haikuMode {
		.timing = ToHaikuModeTimings(nvKmsMode.timings),
		.space = B_RGB32,
		.virtual_width  = nvKmsMode.timings.hVisible,
		.virtual_height = nvKmsMode.timings.vVisible,
	};
	return haikuMode;
}

static NvKmsMode ToNvKmsMode(const display_mode &haikuMode) {
	NvKmsMode nvKmsMode {
		.timings = ToNvKmsModeTimings(haikuMode.timing),
	};
	return nvKmsMode;
}


class NvAccelerant {
private:
	static NvAccelerant *sInstance;

	FileDesc fDevFd;
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvKmsApi fKms;
	NvKmsDevice fKmsDev;
	NvKmsDispHandle fDisp;

	// A connected display and the head that drives it. When more than one
	// display is connected, the desktop spans all of them from left to right.
	struct Output {
		NVDpyId dpyId;
		NvU32 head;
		NvKmsMode preferredMode;
		int32 x;
	};
	std::vector<Output> fOutputs;
	NVDpyId fDpyId;	// primary display
	NvU32 fHead;	// primary head

	std::vector<NvKmsMode> fModeList;
	NvKmsMode fSpanMode {};
	NvKmsMode fCurrentMode {};
	display_mode fCurrentHaikuMode {};
	bool fSpanning = false;
	NvKmsDpyAttributeDpmsValue fDpmsState = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
	NvKmsBitmap fOldFramebuffer, fFramebuffer;

	NvKmsBitmap fCursor, fNewCursor;
	struct {
		int32 x;
		int32 y;
	} fCursorPos, fCursorHotSpot {};
	bool fCursorVisible = false;

	// last cursor image, the surface contents do not survive suspend
	struct {
		uint16 width = 0;
		uint16 height = 0;
		uint16 hotX = 0;
		uint16 hotY = 0;
		color_space colorSpace = B_RGBA32;
		uint16 bytesPerRow = 0;
		std::vector<uint8> data;
	} fCursorImage;

	// Serializes NVKMS state changes between app_server and the resume thread.
	std::recursive_mutex fLock;
	thread_id fResumeThread = -1;
	std::atomic<bool> fQuitResumeThread {false};
	sem_id fDisplayRestoredSem = -1;

	NvAccelerant(int devFd);

	void ApplyMode(const display_mode &mode, NvKmsBitmap &framebuffer);
	void ApplySpanningMode(NvKmsBitmap &framebuffer);
	static status_t ResumeThreadEntry(void *arg);
	void ResumeThread();
	void RestoreAfterResume();

	NVDpyId FindConnectedDisplay(NVDpyIdList validDpys);
	void FindOutputs(NVDpyIdList validDpys);
	bool ValidateMode(const NvKmsMode &mode);
	NvKmsMode PreferredMode(NVDpyId dpyId);
	void BuildSpanMode();
	bool IsSpanMode(const display_timing &timing) const;
	void SetSpanningMode(const display_mode &mode);

	void UpdateCursor(bool updateImage, bool updatePos);

public:
	~NvAccelerant();

	static NvAccelerant *Instance() {return sInstance;}

	static void Init(int fd);
	static void Clone(void* data);
	void Uninit();

	ssize_t CloneInfoSize();
	void GetCloneInfo(void* data);
	void GetDeviceInfo(accelerant_device_info* adi);
	uint32 ModeCount();
	void GetModeList(display_mode* modes);
	status_t ProposeMode(display_mode *target, display_mode *low, display_mode *high);
	void SetDisplayMode(display_mode* modeToSet);
	void GetDisplayMode(display_mode* currentMode);
	void GetFrameBufferConfig(frame_buffer_config* frameBuffer);
	void GetPixelClockLimits(display_mode* dm, uint32* low /* in kHz */, uint32* high);

	uint32 DpmsCapabilities();
	uint32 DpmsMode();
	void SetDpmsMode(uint32 dpms_flags);
	void GetPreferredDisplayMode(display_mode* preferredMode);
	void GetMonitorInfo(monitor_info* info);
	void GetEdidInfo(void* info, uint32 size, uint32* _version);
	status_t WaitForDisplayRestore(bigtime_t timeout);

	void MoveCursor(uint16 x, uint16 y);
	void ShowCursor(bool isVisible);
	void SetCursorShape(uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask);
	void SetCursorBitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData);
};


NvAccelerant *NvAccelerant::sInstance {};


NvAccelerant::NvAccelerant(int devFd):
	fDevFd(dup(devFd)),
	fRmDev(fRm, 0), // TODO: do not hardcode rmDeviceId
	fKmsDev(fKms, 0)
{
	if (!(fKmsDev.Info().numDisps > 0)) {
		RaiseErrno(ENODEV);
	}
	fDisp = fKmsDev.Info().dispHandles[0];

	NVDpyIdList validDpys;
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params)));

		validDpys = params.reply.validDpys;
	}
	const int32 totalAttempts = 10;
	for (int32 i = 0; i < totalAttempts; i++) {
		fDpyId = FindConnectedDisplay(validDpys);
		if (!nvDpyIdIsInvalid(fDpyId)) {
			break;
		}
		debug_printf("nvidia_rm: [%" B_PRId32 "/%" B_PRId32 "]: no connected displays\n", i, totalAttempts);
		snooze(100000);
	}
	if (nvDpyIdIsInvalid(fDpyId)) {
		RaiseErrno(ENODEV);
	}

	FindOutputs(validDpys);
	fDpyId = fOutputs[0].dpyId;
	fHead = fOutputs[0].head;

	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		CheckErrno(fKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params)));
	}

	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {
			.request = {
				.deviceHandle = fKmsDev.Get(),
				.dispHandle = fDisp,
				.dpyId = fDpyId,
				.modeIndex = i,
			},
		};
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end) {
			break;
		}
		if (!ValidateMode(params.reply.mode)) {
			continue;
		}
		fModeList.push_back(params.reply.mode);
	}

	BuildSpanMode();

	fDisplayRestoredSem = create_sem(0, "nvidia_rm display restored");
	fResumeThread = spawn_thread(ResumeThreadEntry, "nvidia_rm resume",
		B_DISPLAY_PRIORITY, this);
	if (fResumeThread >= 0)
		resume_thread(fResumeThread);
}

NvAccelerant::~NvAccelerant()
{
	if (fResumeThread >= 0) {
		fQuitResumeThread = true;
		status_t result;
		wait_for_thread(fResumeThread, &result);
	}
	if (fDisplayRestoredSem >= 0)
		delete_sem(fDisplayRestoredSem);
}

status_t NvAccelerant::WaitForDisplayRestore(bigtime_t timeout)
{
	status_t status = acquire_sem_etc(fDisplayRestoredSem, 1, B_RELATIVE_TIMEOUT,
		timeout);
	return status == B_WOULD_BLOCK ? B_TIMED_OUT : status;
}

status_t NvAccelerant::ResumeThreadEntry(void *arg)
{
	static_cast<NvAccelerant*>(arg)->ResumeThread();
	return B_OK;
}

// The display hardware state is lost while the system is suspended. The
// kernel driver bumps a generation counter after resuming, restore the mode
// and cursor when it changes.
void NvAccelerant::ResumeThread()
{
	nv_haiku_resume_params params {};
	if (ioctl(fDevFd.Get(), NV_HAIKU_BASE + NV_HAIKU_WAIT_FOR_RESUME, &params,
			sizeof(params)) < 0) {
		debug_printf("nvidia_rm: resume notifications not supported\n");
		return;
	}
	uint32 generation = params.generation;

	while (!fQuitResumeThread) {
		params.generation = generation;
		params.timeout = 500000;
		if (ioctl(fDevFd.Get(), NV_HAIKU_BASE + NV_HAIKU_WAIT_FOR_RESUME,
				&params, sizeof(params)) < 0) {
			snooze(500000);
			continue;
		}
		if (params.generation == generation)
			continue;

		generation = params.generation;
		debug_printf("nvidia_rm: restoring display state after resume\n");
		try {
			RestoreAfterResume();
			release_sem(fDisplayRestoredSem);
		} catch (const std::system_error &ex) {
			debug_printf("[!] nvidia_rm: restoring after resume failed: %s\n",
				ex.what());
		}
	}
}

void NvAccelerant::RestoreAfterResume()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (fCurrentMode.timings.hVisible == 0 || !fFramebuffer.IsSet())
		return;

	if (fSpanning)
		ApplySpanningMode(fFramebuffer);
	else
		ApplyMode(fCurrentHaikuMode, fFramebuffer);

	if (!fCursorImage.data.empty()) {
		SetCursorBitmap(fCursorImage.width, fCursorImage.height,
			fCursorImage.hotX, fCursorImage.hotY, fCursorImage.colorSpace,
			fCursorImage.bytesPerRow, fCursorImage.data.data());
	}
	if (fCursorVisible)
		UpdateCursor(true, true);

	if (fDpmsState != NV_KMS_DPY_ATTRIBUTE_DPMS_ON)
		SetDpmsMode(B_DPMS_OFF);
}

// Testing aid: "force_connected <connector>" lines in
// ~/config/settings/nvidia_rm_accelerant force a connector on, using the EDID
// of the first connected display, so that multi-display layouts can be tested
// without monitors.
static bool IsForcedConnected(const char *name)
{
	FILE *file = fopen("/boot/home/config/settings/nvidia_rm_accelerant", "r");
	if (file == NULL)
		return false;
	bool forced = false;
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		char connector[128];
		if (sscanf(line, "force_connected %127s", connector) == 1 && strcmp(connector, name) == 0)
			forced = true;
	}
	fclose(file);
	return forced;
}

void NvAccelerant::FindOutputs(NVDpyIdList validDpys)
{
	NvKmsQueryDpyDynamicDataReply firstConnected {};
	bool haveFirstConnected = false;
	NVDpyIdList forcedDpys = nvEmptyDpyIdList();
	for (NVDpyId dpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
			!nvDpyIdIsInvalid(dpyId);
			dpyId = nvNextDpyIdInDpyIdListUnsorted(dpyId, validDpys)) {
		NvKmsQueryDpyDynamicDataParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = dpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));
		debug_printf("nvidia_rm: connector %s%s\n", params.reply.name,
			params.reply.connected ? " (connected)" : "");
		if (!haveFirstConnected && params.reply.connected && params.reply.edid.valid) {
			firstConnected = params.reply;
			haveFirstConnected = true;
		} else if (haveFirstConnected && !params.reply.connected && IsForcedConnected(params.reply.name)) {
			NvKmsQueryDpyDynamicDataParams forceParams {};
			forceParams.request.deviceHandle = fKmsDev.Get();
			forceParams.request.dispHandle = fDisp;
			forceParams.request.dpyId = dpyId;
			forceParams.request.forceConnected = true;
			forceParams.request.overrideEdid = true;
			forceParams.request.edid.bufferSize = firstConnected.edid.bufferSize;
			memcpy(forceParams.request.edid.buffer, firstConnected.edid.buffer,
				sizeof(forceParams.request.edid.buffer));
			CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &forceParams, sizeof(forceParams)));
			debug_printf("nvidia_rm: forcing %s connected: %d\n", params.reply.name,
				forceParams.reply.connected);
			if (forceParams.reply.connected)
				forcedDpys = nvAddDpyIdToDpyIdList(dpyId, forcedDpys);
		}
	}

	NvU32 usedHeads = 0;
	for (NVDpyId dpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
			!nvDpyIdIsInvalid(dpyId);
			dpyId = nvNextDpyIdInDpyIdListUnsorted(dpyId, validDpys)) {
		NvKmsQueryDpyDynamicDataParams dynamicParams {};
		dynamicParams.request.deviceHandle = fKmsDev.Get();
		dynamicParams.request.dispHandle = fDisp;
		dynamicParams.request.dpyId = dpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &dynamicParams, sizeof(dynamicParams)));
		if (!dynamicParams.reply.connected && !dynamicParams.reply.edid.valid
			&& !nvDpyIdIsInDpyIdList(dpyId, forcedDpys))
			continue;

		NvKmsQueryDpyStaticDataParams staticParams {};
		staticParams.request.deviceHandle = fKmsDev.Get();
		staticParams.request.dispHandle = fDisp;
		staticParams.request.dpyId = dpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_STATIC_DATA, &staticParams, sizeof(staticParams)));

		NvU32 freeHeads = staticParams.reply.headMask & ~usedHeads;
		if (freeHeads == 0) {
			debug_printf("nvidia_rm: no free head for display %s\n", dynamicParams.reply.name);
			continue;
		}
		NvU32 head = __builtin_ctz(freeHeads);
		usedHeads |= 1U << head;

		Output output {
			.dpyId = dpyId,
			.head = head,
			.preferredMode = PreferredMode(dpyId),
			.x = 0,
		};
		debug_printf("nvidia_rm: display %s on head %" B_PRIu32 ", %" B_PRIu32 "x%" B_PRIu32 "\n",
			dynamicParams.reply.name, head, (uint32)output.preferredMode.timings.hVisible,
			(uint32)output.preferredMode.timings.vVisible);
		fOutputs.push_back(output);
	}

	if (fOutputs.empty())
		RaiseErrno(ENODEV);

	int32 x = 0;
	for (auto &output: fOutputs) {
		output.x = x;
		x += output.preferredMode.timings.hVisible;
	}
}

NvKmsMode NvAccelerant::PreferredMode(NVDpyId dpyId)
{
	NvKmsMode firstMode {};
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end)
			break;
		if (!params.reply.valid)
			continue;
		if (params.reply.preferredMode)
			return params.reply.mode;
		if (firstMode.timings.hVisible == 0)
			firstMode = params.reply.mode;
	}
	return firstMode;
}

void NvAccelerant::BuildSpanMode()
{
	if (fOutputs.size() < 2)
		return;

	const NvModeTimings &primary = fOutputs[0].preferredMode.timings;
	NvU32 width = 0;
	NvU32 height = 0;
	for (const auto &output: fOutputs) {
		width += output.preferredMode.timings.hVisible;
		height = std::max(height, (NvU32)output.preferredMode.timings.vVisible);
	}

	// Synthetic timings: the primary display's blanking around the whole
	// desktop. They only describe the desktop to app_server; every head is
	// programmed with its own display's timings.
	fSpanMode = fOutputs[0].preferredMode;
	NvModeTimings &t = fSpanMode.timings;
	NvU32 extraH = width - primary.hVisible;
	NvU32 extraV = height - primary.vVisible;
	t.hVisible += extraH;
	t.hSyncStart += extraH;
	t.hSyncEnd += extraH;
	t.hTotal += extraH;
	t.vVisible += extraV;
	t.vSyncStart += extraV;
	t.vSyncEnd += extraV;
	t.vTotal += extraV;
	t.pixelClockHz = (NvU32)((uint64)primary.RRx1k * t.hTotal * t.vTotal / 1000);
}

bool NvAccelerant::IsSpanMode(const display_timing &timing) const
{
	return fOutputs.size() >= 2
		&& timing.h_display == fSpanMode.timings.hVisible
		&& timing.v_display == fSpanMode.timings.vVisible;
}

NVDpyId NvAccelerant::FindConnectedDisplay(NVDpyIdList validDpys)
{
	NVDpyId curDpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
	while (!nvDpyIdIsInvalid(curDpyId)) {
		NvKmsQueryDpyDynamicDataParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = curDpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));
		if (params.reply.connected || params.reply.edid.valid) {
			break;
		}
		curDpyId = nvNextDpyIdInDpyIdListUnsorted(curDpyId, validDpys);
	}
	return curDpyId;
}

bool NvAccelerant::ValidateMode(const NvKmsMode &mode)
{
	NvKmsValidateModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.dpyId = fDpyId;
	params.request.mode = mode;
	CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
	return params.reply.valid;
}

void NvAccelerant::Init(int fd)
{
	debug_printf("NvAccelerant::Init\n");

	sInstance = new NvAccelerant(fd);
}

static const char kCloneInfo[] = "/dev/graphics/nvidia0"; // FIXME

ssize_t NvAccelerant::CloneInfoSize()
{
	debug_printf("NvAccelerant::CloneInfoSize\n");

	return strlen(kCloneInfo) + 1;
}

void NvAccelerant::GetCloneInfo(void* data)
{
	debug_printf("NvAccelerant::GetCloneInfo\n");

	strcpy((char*)data, kCloneInfo);
}

void NvAccelerant::Clone(void* data)
{
	debug_printf("NvAccelerant::Clone\n");

	FileDesc devFd(open((char*)data, O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	sInstance = new NvAccelerant(devFd.Get());
}

void NvAccelerant::Uninit()
{
	debug_printf("NvAccelerant::Uninit\n");

	delete sInstance;
	sInstance = nullptr;
}

void NvAccelerant::GetDeviceInfo(accelerant_device_info* adi)
{
	debug_printf("NvAccelerant::GetDeviceInfo\n");

	NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS getNameParams = {
		.gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
	};
	fRmDev.Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &getNameParams, sizeof(getNameParams));

	NV2080_CTRL_GPU_GET_SHORT_NAME_STRING_PARAMS getShortNameParams {};
	fRmDev.Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING, &getShortNameParams, sizeof(getShortNameParams));

	adi->version = B_ACCELERANT_VERSION;
	strlcpy(adi->name, (char*)getNameParams.gpuNameString.ascii, sizeof(adi->name));
	strlcpy(adi->chipset, (char*)getShortNameParams.gpuShortNameString, sizeof(adi->chipset));
	strcpy(adi->serial_no, "?");
	adi->memory = 0x20000000; // FIXME
	adi->dac_speed = 0;
}


uint32 NvAccelerant::ModeCount()
{
	debug_printf("NvAccelerant::ModeCount\n");

	return fModeList.size() + (fOutputs.size() >= 2 ? 1 : 0);
}

void NvAccelerant::GetModeList(display_mode* mode)
{
	debug_printf("NvAccelerant::GetModeList\n");

	int32 i = 0;
	if (fOutputs.size() >= 2)
		mode[i++] = ToHaikuMode(fSpanMode);
	for (const auto &nvKmsMode: fModeList) {
		mode[i++] = ToHaikuMode(nvKmsMode);
	}
}

static bool ModeTimingsEqual(const display_timing &a, const display_timing &b)
{
	return
		a.pixel_clock  == b.pixel_clock &&
		a.h_display    == b.h_display &&
		a.h_sync_start == b.h_sync_start &&
		a.h_sync_end   == b.h_sync_end &&
		a.h_total      == b.h_total &&
		a.v_display    == b.v_display &&
		a.v_sync_start == b.v_sync_start &&
		a.v_sync_end   == b.v_sync_end &&
		a.v_total      == b.v_total &&
		a.flags        == b.flags;
}

bool IsDisplayModeWithinBounds(display_mode &mode, const display_mode &low, const display_mode &high)
{
	if (mode.timing.h_display < low.timing.h_display
		|| mode.timing.h_display > high.timing.h_display
		|| mode.timing.h_sync_start < low.timing.h_sync_start
		|| mode.timing.h_sync_start > high.timing.h_sync_start
		|| mode.timing.h_sync_end < low.timing.h_sync_end
		|| mode.timing.h_sync_end > high.timing.h_sync_end
		|| mode.timing.h_total < low.timing.h_total
		|| mode.timing.h_total > high.timing.h_total)
		return false;

	if (mode.timing.v_display < low.timing.v_display
		|| mode.timing.v_display > high.timing.v_display
		|| mode.timing.v_sync_start < low.timing.v_sync_start
		|| mode.timing.v_sync_start > high.timing.v_sync_start
		|| mode.timing.v_sync_end < low.timing.v_sync_end
		|| mode.timing.v_sync_end > high.timing.v_sync_end
		|| mode.timing.v_total < low.timing.v_total
		|| mode.timing.v_total > high.timing.v_total)
		return false;

	if (mode.timing.pixel_clock > high.timing.pixel_clock
		|| mode.timing.pixel_clock < low.timing.pixel_clock)
		return false;

	if (mode.virtual_width > high.virtual_width
		|| mode.virtual_width < low.virtual_width)
		return false;

	if (mode.virtual_height > high.virtual_height
		|| mode.virtual_height < low.virtual_height)
		return false;

	return true;
}

status_t NvAccelerant::ProposeMode(display_mode *target, display_mode *low, display_mode *high)
{
	debug_printf("NvAccelerant::ProposeMode\n");

	if (IsSpanMode(target->timing)) {
		target->timing = ToHaikuModeTimings(fSpanMode.timings);
		target->virtual_width = target->timing.h_display;
		target->virtual_height = target->timing.v_display;
		return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
	}

	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto mode = ToHaikuMode(nvKmsMode);
		if (ModeTimingsEqual(target->timing, mode.timing)) {
			return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
		}
	}

	uint32 reqRefreshRate = CalcRefreshRate(target->timing);
	int32 bestCandidateIdx = -1;
	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto timings = ToHaikuModeTimings(nvKmsMode.timings);
		if (
			timings.h_display == target->timing.h_display &&
			timings.v_display == target->timing.v_display &&
			(timings.flags & B_TIMING_INTERLACED) == (target->timing.flags & B_TIMING_INTERLACED)
		) {
			if (bestCandidateIdx < 0 || std::abs((int64)nvKmsMode.timings.RRx1k - (int64)reqRefreshRate) < std::abs((int64)fModeList[bestCandidateIdx].timings.RRx1k - (int64)reqRefreshRate)) {
				bestCandidateIdx = i;
			}
		}
	}
	if (bestCandidateIdx < 0) {
		return B_ERROR;
	}

	target->timing = ToHaikuModeTimings(fModeList[bestCandidateIdx].timings);

	return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
}

void NvAccelerant::ApplyMode(const display_mode &mode, NvKmsBitmap &framebuffer)
{
	NvKmsSetModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.commit = true;
	params.request.requestedDispsBitMask |= 1U << 0;
	params.request.disp[0].requestedHeadsBitMask |= 1U << 0;
	params.request.disp[0].head[0].dpyIdList = nvAddDpyIdToEmptyDpyIdList(fDpyId);
	params.request.disp[0].head[0].mode = ToNvKmsMode(mode);
	params.request.disp[0].head[0].modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
	params.request.disp[0].head[0].viewPortOut = {.x = 0, .y = 0, .width = mode.timing.h_display, .height = mode.timing.v_display};
	params.request.disp[0].head[0].viewPortSizeIn = {.width = mode.timing.h_display, .height = mode.timing.v_display};
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].surface.handle[0] = framebuffer.Surface().Get();
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].surface.specified = true;
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeIn.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeIn.specified = true;
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeOut.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeOut.specified = true;

	// turn off the heads of other displays
	for (const auto &output: fOutputs) {
		if (output.head != 0)
			params.request.disp[0].requestedHeadsBitMask |= 1U << output.head;
	}

	try {
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params)));
	} catch (const std::system_error&) {
		debug_printf("[!] NvAccelerant: SetMode failed\n");
		debug_printf("  status: %d\n", params.reply.status);
		debug_printf("  disp[0].status: %d\n", params.reply.disp[0].status);
		debug_printf("  disp[0].head[0].status: %d\n", params.reply.disp[0].head[0].status);
		throw;
	}
}

void NvAccelerant::SetDisplayMode(display_mode* modeToSet)
{
	debug_printf("NvAccelerant::SetDisplayMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (IsSpanMode(modeToSet->timing)) {
		SetSpanningMode(*modeToSet);
		return;
	}

	if (
		modeToSet->virtual_width != modeToSet->timing.h_display ||
		modeToSet->virtual_height != modeToSet->timing.v_display ||
		modeToSet->h_display_start != 0 ||
		modeToSet->v_display_start != 0
	) {
		RaiseErrno(EINVAL);
	}

	{
		char infoString[NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH] {};
		NvKmsValidateModeParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		params.request.modeValidation.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		params.request.mode = ToNvKmsMode(*modeToSet);
		params.request.infoStringSize = NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH;
		params.request.pInfoString = nvKmsPointerToNvU64(infoString);
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
		if (!params.reply.valid) {
			debug_printf("[!] mode validation failed\n");
			debug_printf("%s", infoString);
			RaiseErrno(EINVAL);
		}
	}

	NvKmsBitmap newFramebuffer(fRmDev, fKmsDev, modeToSet->virtual_width, modeToSet->virtual_height, (color_space)modeToSet->space);

	ApplyMode(*modeToSet, newFramebuffer);

	fCurrentMode = ToNvKmsMode(*modeToSet);
	fCurrentHaikuMode = *modeToSet;
	fSpanning = false;

	fOldFramebuffer = std::move(fFramebuffer);
	fFramebuffer = std::move(newFramebuffer);
}

void NvAccelerant::ApplySpanningMode(NvKmsBitmap &framebuffer)
{
	NvKmsSetModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.commit = true;
	params.request.requestedDispsBitMask |= 1U << 0;
	for (const auto &output: fOutputs) {
		const NvModeTimings &timings = output.preferredMode.timings;
		NvKmsSetModeOneHeadRequest &head = params.request.disp[0].head[output.head];
		params.request.disp[0].requestedHeadsBitMask |= 1U << output.head;
		head.dpyIdList = nvAddDpyIdToEmptyDpyIdList(output.dpyId);
		head.mode = output.preferredMode;
		head.modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		head.viewPortOut = {.x = 0, .y = 0, .width = timings.hVisible, .height = timings.vVisible};
		head.viewPortSizeIn = {.width = timings.hVisible, .height = timings.vVisible};
		head.flip.viewPortIn.specified = true;
		head.flip.viewPortIn.point = {.x = (NvU16)output.x, .y = 0};
		auto &layer = head.flip.layer[NVKMS_MAIN_LAYER];
		layer.surface.handle[0] = framebuffer.Surface().Get();
		layer.surface.specified = true;
		// The layer covers the whole desktop; viewPortIn selects this head's part.
		layer.sizeIn.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
		layer.sizeIn.specified = true;
		layer.sizeOut.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
		layer.sizeOut.specified = true;
	}

	try {
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params)));
	} catch (const std::system_error&) {
		debug_printf("[!] NvAccelerant: spanning SetMode failed, status %d\n", params.reply.status);
		for (const auto &output: fOutputs) {
			debug_printf("  head %" B_PRIu32 " status: %d\n", output.head,
				params.reply.disp[0].head[output.head].status);
		}
		throw;
	}
}

void NvAccelerant::SetSpanningMode(const display_mode &mode)
{
	NvKmsBitmap newFramebuffer(fRmDev, fKmsDev, fSpanMode.timings.hVisible,
		fSpanMode.timings.vVisible, (color_space)mode.space);

	ApplySpanningMode(newFramebuffer);

	fCurrentMode = fSpanMode;
	fCurrentHaikuMode = mode;
	fCurrentHaikuMode.timing = ToHaikuModeTimings(fSpanMode.timings);
	fCurrentHaikuMode.virtual_width = fSpanMode.timings.hVisible;
	fCurrentHaikuMode.virtual_height = fSpanMode.timings.vVisible;
	fSpanning = true;

	fOldFramebuffer = std::move(fFramebuffer);
	fFramebuffer = std::move(newFramebuffer);
}

void NvAccelerant::GetDisplayMode(display_mode* currentMode)
{
	debug_printf("NvAccelerant::GetDisplayMode\n");

	if (fCurrentMode.timings.hVisible == 0) {
		RaiseErrno(ENOENT);
	}
	*currentMode = fCurrentHaikuMode;
}

void NvAccelerant::GetFrameBufferConfig(frame_buffer_config* frameBuffer)
{
	debug_printf("NvAccelerant::GetFrameBufferConfig\n");

	if (!fFramebuffer.IsSet()) {
		RaiseErrno(ENOENT);
	}
	frameBuffer->frame_buffer = fFramebuffer.Bits();
	frameBuffer->frame_buffer_dma = nullptr;
	frameBuffer->bytes_per_row = fFramebuffer.BytesPerRow();
}

void NvAccelerant::GetPixelClockLimits(display_mode* dm, uint32* low, uint32* high)
{
	debug_printf("NvAccelerant::GetPixelClockLimits\n");

	*low = UINT32_MAX;
	*high = 0;

	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto timing = ToHaikuModeTimings(nvKmsMode.timings);
		if (
			timing.h_display == dm->timing.h_display &&
			timing.v_display == dm->timing.v_display &&
			(timing.flags & B_TIMING_INTERLACED) == (dm->timing.v_display & B_TIMING_INTERLACED)
		) {
			*low = std::min(*low, timing.pixel_clock);
			*high = std::max(*high, timing.pixel_clock);
		}
	}
	if (*low > *high) {
		RaiseErrno(B_ERROR);
	}
}

uint32 NvAccelerant::DpmsCapabilities()
{
	debug_printf("NvAccelerant::DpmsCapabilities\n");

	return B_DPMS_ON | B_DPMS_OFF;
}

uint32 NvAccelerant::DpmsMode()
{
	debug_printf("NvAccelerant::DpmsMode\n");

	switch (fDpmsState) {
		case NV_KMS_DPY_ATTRIBUTE_DPMS_ON:
			return B_DPMS_ON;
		case NV_KMS_DPY_ATTRIBUTE_DPMS_OFF:
			return B_DPMS_OFF;
		default:
			return B_DPMS_OFF;
	}
}

void NvAccelerant::SetDpmsMode(uint32 dpms_flags)
{
	debug_printf("NvAccelerant::SetDpmsMode\n");

	std::lock_guard<std::recursive_mutex> lock(fLock);

	NvS64 value;
	switch (dpms_flags) {
		case B_DPMS_ON:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
			break;
		case B_DPMS_OFF:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_OFF;
			break;
		default:
			RaiseErrno(EINVAL);
	}

	for (const auto &output: fOutputs) {
		if (!fSpanning && !nvDpyIdsAreEqual(output.dpyId, fDpyId))
			continue;
		NvKmsSetDpyAttributeParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = output.dpyId;
		params.request.attribute = NV_KMS_DPY_ATTRIBUTE_DPMS;
		params.request.value = value;
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_DPY_ATTRIBUTE, &params, sizeof(params)));
	}
	fDpmsState = (NvKmsDpyAttributeDpmsValue)value;
}

void NvAccelerant::GetPreferredDisplayMode(display_mode* preferredMode)
{
	debug_printf("NvAccelerant::GetPreferredDisplayMode\n");

	if (fOutputs.size() >= 2) {
		*preferredMode = ToHaikuMode(fSpanMode);
		return;
	}

	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		params.request.modeIndex = i;
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end)
			break;

		if (params.reply.preferredMode) {
			*preferredMode = ToHaikuMode(params.reply.mode);
			return;
		}
	}

	RaiseErrno(ENOENT);
}

void NvAccelerant::GetMonitorInfo(monitor_info* info)
{
	debug_printf("NvAccelerant::GetMonitorInfo\n");

	RaiseErrno(ENOSYS);
}

void NvAccelerant::GetEdidInfo(void* info, uint32 size, uint32* _version)
{
	debug_printf("NvAccelerant::GetEdidInfo\n");

	if (size < sizeof(struct edid1_info)) {
		RaiseErrno(B_BUFFER_OVERFLOW);
	}

	{
		NvKmsQueryDpyDynamicDataParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));
		if (!params.reply.edid.valid) {
			RaiseErrno(B_ERROR);
		}
		edid_decode((edid1_info*)info, (const edid1_raw*)params.reply.edid.buffer);
		*_version = EDID_VERSION_1;
	}
}


void NvAccelerant::MoveCursor(uint16 x, uint16 y)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	fCursorPos.x = x;
	fCursorPos.y = y;
	UpdateCursor(false, true);
}

void NvAccelerant::ShowCursor(bool isVisible)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (fCursorVisible == isVisible) {
		return;
	}
	fCursorVisible = !fCursorVisible;
	UpdateCursor(true, false);
}

void NvAccelerant::UpdateCursor(bool updateImage, bool updatePos)
{
	for (const auto &output: fOutputs) {
		if (!fSpanning && output.head != fHead)
			continue;

		if (updateImage) {
			NvKmsSetCursorImageParams params {
				.request = {
					.deviceHandle = fKmsDev.Get(),
					.dispHandle = fDisp,
					.head = output.head,
					.common = {
						.surfaceHandle = {
							fCursorVisible ? (fNewCursor.IsSet() ? fNewCursor.Surface().Get() : fCursor.Surface().Get()) : 0,
						},
						.cursorCompParams = {
							.blendingMode = {
								NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA,
								NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA,
							}
						},
					},
				},
			};
			CheckErrno(fKms.Control(NVKMS_IOCTL_SET_CURSOR_IMAGE, &params, sizeof(params)));
		}

		if (updatePos) {
			NvKmsMoveCursorParams params {
				.request = {
					.deviceHandle = fKmsDev.Get(),
					.dispHandle = fDisp,
					.head = output.head,
					.common = {
						.x = (NvS16)(fCursorPos.x - fCursorHotSpot.x - (fSpanning ? output.x : 0)),
						.y = (NvS16)(fCursorPos.y - fCursorHotSpot.y),
					},
				},
			};
			CheckErrno(fKms.Control(NVKMS_IOCTL_MOVE_CURSOR, &params, sizeof(params)));
		}
	}

	if (updateImage && fNewCursor.IsSet()) {
		fCursor = std::move(fNewCursor);
	}
}

void NvAccelerant::SetCursorShape(uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask)
{
	RaiseErrno(ENOSYS);
}

void NvAccelerant::SetCursorBitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData)
{
	int32 cursorWidth;
	int32 cursorHeight;
	if (width > 256 || height > 256) {
		RaiseErrno(EINVAL);
	} else if (width > 128 || height > 128) {
		cursorWidth = 256;
		cursorHeight = 256;
	} else if (width > 64 || height > 64) {
		cursorWidth = 128;
		cursorHeight = 128;
	} else if (width > 32 || height > 32) {
		cursorWidth = 64;
		cursorHeight = 64;
	} else {
		cursorWidth = 32;
		cursorHeight = 32;
	}

	std::lock_guard<std::recursive_mutex> lock(fLock);

	if (bitmapData != fCursorImage.data.data()) {
		fCursorImage.width = width;
		fCursorImage.height = height;
		fCursorImage.hotX = hotX;
		fCursorImage.hotY = hotY;
		fCursorImage.colorSpace = colorSpace;
		fCursorImage.bytesPerRow = bytesPerRow;
		fCursorImage.data.assign(bitmapData, bitmapData + bytesPerRow * height);
	}

	NvKmsBitmap newCursor(fRmDev, fKmsDev, cursorWidth, cursorHeight, colorSpace);
	memset(newCursor.Bits(), 0, newCursor.BitsLength());
	auto dstLine = (uint8*)newCursor.Bits();
	auto srcLine = bitmapData;
	for (int32 y = 0; y < height; y++) {
		memcpy(dstLine, srcLine, 4*width /* FIXME */);
		dstLine += newCursor.BytesPerRow();
		srcLine += bytesPerRow;
	}

	fNewCursor = std::move(newCursor);
	fCursorHotSpot.x = hotX;
	fCursorHotSpot.y = hotY;

	if (fCursorVisible) {
		UpdateCursor(true, true);
	}
}


_EXPORT void *get_accelerant_hook(uint32 feature, void *data)
{
	switch (feature) {
		case B_INIT_ACCELERANT: {
			init_accelerant fn = [](int fd) {
				try {
					NvAccelerant::Init(fd);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_ACCELERANT_CLONE_INFO_SIZE: {
			accelerant_clone_info_size fn = []() {
				return NvAccelerant::Instance()->CloneInfoSize();
			};
			return (void*)fn;
		}
		case B_GET_ACCELERANT_CLONE_INFO: {
			get_accelerant_clone_info fn = [](void* data) {
				return NvAccelerant::Instance()->GetCloneInfo(data);
			};
			return (void*)fn;
		}
		case B_CLONE_ACCELERANT: {
			clone_accelerant fn = [](void *data) {
				try {
					NvAccelerant::Clone(data);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_UNINIT_ACCELERANT: {
			uninit_accelerant fn = []() {
				try {
					NvAccelerant::Instance()->Uninit();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
		case B_GET_ACCELERANT_DEVICE_INFO: {
			get_accelerant_device_info fn = [](accelerant_device_info* adi) {
				try {
					NvAccelerant::Instance()->GetDeviceInfo(adi);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}

		case B_ACCELERANT_MODE_COUNT: {
			accelerant_mode_count fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->ModeCount();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_GET_MODE_LIST: {
			get_mode_list fn = [](display_mode* modes) {
				try {
					NvAccelerant::Instance()->GetModeList(modes);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_PROPOSE_DISPLAY_MODE: {
			propose_display_mode fn = [](display_mode *target, display_mode *low, display_mode *high) {
				try {
					return NvAccelerant::Instance()->ProposeMode(target, low, high);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_SET_DISPLAY_MODE: {
			set_display_mode fn = [](display_mode* modeToSet) {
				try {
					NvAccelerant::Instance()->SetDisplayMode(modeToSet);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_DISPLAY_MODE: {
			get_display_mode fn = [](display_mode* currentMode) {
				try {
					NvAccelerant::Instance()->GetDisplayMode(currentMode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_FRAME_BUFFER_CONFIG: {
			get_frame_buffer_config fn = [](frame_buffer_config* frameBuffer) {
				try {
					NvAccelerant::Instance()->GetFrameBufferConfig(frameBuffer);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_PIXEL_CLOCK_LIMITS: {
			get_pixel_clock_limits fn = [](display_mode* dm, uint32* low, uint32* high) {
				try {
					NvAccelerant::Instance()->GetPixelClockLimits(dm, low, high);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}

		case B_DPMS_CAPABILITIES: {
			dpms_capabilities fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DpmsCapabilities();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_DPMS_MODE: {
			dpms_mode fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DpmsMode();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return B_DPMS_ON;
				}
			};
			return (void*)fn;
		}
		case B_SET_DPMS_MODE: {
			set_dpms_mode fn = [](uint32 dpms_flags) {
				try {
					NvAccelerant::Instance()->SetDpmsMode(dpms_flags);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_PREFERRED_DISPLAY_MODE: {
			get_preferred_display_mode fn = [](display_mode* preferredMode) {
				try {
					NvAccelerant::Instance()->GetPreferredDisplayMode(preferredMode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
#if 0
		case B_GET_MONITOR_INFO: {
			get_monitor_info fn = [](monitor_info* info) {
				try {
					NvAccelerant::Instance()->GetMonitorInfo(info);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_GET_EDID_INFO: {
			get_edid_info fn = [](void* info, uint32 size, uint32* _version) {
				try {
					NvAccelerant::Instance()->GetEdidInfo(info, size, _version);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
		case B_WAIT_FOR_DISPLAY_RESTORE: {
			wait_for_display_restore fn = [](bigtime_t timeout) {
				return NvAccelerant::Instance()->WaitForDisplayRestore(timeout);
			};
			return (void*)fn;
		}
#endif

#if 0
		case B_MOVE_CURSOR: {
			move_cursor fn = [](uint16 x, uint16 y) {
				try {
					NvAccelerant::Instance()->MoveCursor(x, y);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
#if 0
		case B_SET_CURSOR_SHAPE: {
			set_cursor_shape fn = [](uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask) {
				try {
					NvAccelerant::Instance()->SetCursorShape(width, height, hotX, hotY, andMask, xorMask);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
#endif
		case B_SHOW_CURSOR: {
			show_cursor fn = [](bool isVisible) {
				try {
					NvAccelerant::Instance()->ShowCursor(isVisible);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
		case B_SET_CURSOR_BITMAP: {
			set_cursor_bitmap fn = [](uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData) {
				try {
					NvAccelerant::Instance()->SetCursorBitmap(width, height, hotX, hotY, colorSpace, bytesPerRow, bitmapData);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_rm: %s\n", ex.what());
					return ToErrorCode(ex);
				}
			};
			return (void*)fn;
		}
#endif

		default:
			return nullptr;
	}
}
