/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_SCANOUT_H
#define RK3588_DISPLAY_SCANOUT_H


#include "DisplayObservation.h"


namespace RK3588Display {

// Opt-in scanout buffer swap: the first VOP2 write path. With the firmware's
// video-port timing untouched, the enabled ESMART window's buffer address is
// pointed at a driver-owned test pattern and later restored to the firmware
// framebuffer. Only REGION0_YRGB_MST of that window and REG_CFG_DONE for that
// video port are written.
static const uint32_t kSwapScanout = 0x52444903;
static const uint32_t kScanoutVersion = 1;

static const uint32_t kScanoutQuery = 0;
static const uint32_t kScanoutShowPattern = 1;
static const uint32_t kScanoutRestore = 2;

static const uint32_t kScanoutOK = 0;
static const uint32_t kScanoutNotReady = 1; // VOP domain off or clocks gated
static const uint32_t kScanoutNoWindow = 2; // no single enabled window feeds the live port
static const uint32_t kScanoutUnexpectedState = 3; // window geometry or format not as observed
static const uint32_t kScanoutNoBuffer = 4; // pattern buffer allocation failed
static const uint32_t kScanoutNotSwapped = 5; // restore requested without a swap
static const uint32_t kScanoutVerifyFailed = 6; // read-back address differs
static const uint32_t kScanoutTimeout = 7; // the port never took the new configuration

// REG_CFG_DONE keeps the port's bit set until its next frame start loads the
// shadowed window registers; reads of those registers return the active set.
static const unsigned kScanoutPollMicros = 20;
static const uint32_t kScanoutPollLimit = 5000; // 100 ms, several frames

static const uint32_t kScanoutSwapped = 1; // flags: scanout currently shows the pattern

static const uint32_t kVopConfigDone = 0x000;
static const uint32_t kVopConfigDoneEnable = 1u << 15;
static const uint32_t kVopEsmartRegionControl = 0x10;
static const uint32_t kVopEsmartRegionAddress = 0x14;
static const uint32_t kVopEsmartRegionVirtual = 0x1c;
static const uint32_t kVopEsmartRegionActive = 0x20;
static const uint32_t kVopEsmartRegionDisplay = 0x24;
static const uint32_t kVopEsmartRegionStart = 0x28;
static const uint32_t kVopEsmartWindowFormatMask = 0x1f << 1; // REGION0_CTRL bits 1-5
static const uint32_t kVopPortDisplayControlStandby = 1u << 31;
static const uint32_t kVopInterfaceHdmi1 = 1u << 5;
static const unsigned kVopInterfaceHdmi1MuxShift = 18;

static const uint32_t kPatternWidth = 1920;
static const uint32_t kPatternHeight = 1080;
static const uint32_t kPatternBytes = kPatternWidth * kPatternHeight * 4;
static const uint32_t kPatternBars = 8;
static const uint32_t kPatternBorder = 32;
// Bar colours as 0xAARRGGBB words: white, yellow, cyan, green, magenta, red, blue, black.
static const uint32_t kPatternColors[kPatternBars] = {0xffffffff, 0xffffff00, 0xff00ffff,
	0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000};
static const uint32_t kPatternBorderColor = 0xff404040;

struct ScanoutRequest {
	uint32_t version; // in
	uint32_t action; // in
	uint32_t result; // out
	uint32_t flags;
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t port; // live video port
	uint32_t window; // ESMART window feeding it
	uint32_t firmwareAddress; // REGION0_YRGB_MST before the first swap
	uint32_t patternAddress; // physical address of the driver pattern buffer
	uint32_t addressBefore; // REGION0_YRGB_MST read before this action
	uint32_t addressAfter; // REGION0_YRGB_MST read after this action
	uint32_t regionControl;
	uint32_t virtualWidth;
	uint32_t activeInfo;
	uint32_t displayInfo;
	uint32_t displayStart;
	uint32_t interfaceEnable;
	uint32_t configDone;
	uint32_t polls; // REG_CFG_DONE polls until the port bit cleared
};


inline uint32_t
PatternPixel(uint32_t x, uint32_t y)
{
	if (x < kPatternBorder || y < kPatternBorder || x >= kPatternWidth - kPatternBorder
		|| y >= kPatternHeight - kPatternBorder) {
		return kPatternBorderColor;
	}
	return kPatternColors[(x - kPatternBorder) * kPatternBars / (kPatternWidth - 2 * kPatternBorder)];
}


// Locates the single enabled ESMART window that feeds the live HDMI1 video
// port and checks its geometry against the qualified observation. Every value
// comes from the mapped registers; nothing is written here.
template<class Hardware>
uint32_t
LocateScanoutWindow(Hardware& hardware, ScanoutRequest& request)
{
	request.interfaceEnable = hardware.ReadVop(kVopSystemOffsets[kVopSystemInterfaceEnable]);
	if ((request.interfaceEnable & kVopInterfaceHdmi1) == 0)
		return kScanoutNoWindow;
	request.port = (request.interfaceEnable >> kVopInterfaceHdmi1MuxShift) & 3;
	uint32_t control = hardware.ReadVop(kVopPortBase + request.port * kVopPortStride);
	if ((control & kVopPortDisplayControlStandby) != 0)
		return kScanoutNoWindow;
	uint32_t found = 0;
	for (unsigned window = 0; window < kVopEsmartCount; window++) {
		uint32_t base = kVopEsmartBase + window * kVopEsmartStride;
		if ((hardware.ReadVop(base + kVopEsmartRegionControl) & 1) == 0)
			continue;
		request.window = window;
		found++;
	}
	if (found != 1)
		return kScanoutNoWindow;
	uint32_t base = kVopEsmartBase + request.window * kVopEsmartStride;
	request.regionControl = hardware.ReadVop(base + kVopEsmartRegionControl);
	request.virtualWidth = hardware.ReadVop(base + kVopEsmartRegionVirtual);
	request.activeInfo = hardware.ReadVop(base + kVopEsmartRegionActive);
	request.displayInfo = hardware.ReadVop(base + kVopEsmartRegionDisplay);
	request.displayStart = hardware.ReadVop(base + kVopEsmartRegionStart);
	request.addressBefore = hardware.ReadVop(base + kVopEsmartRegionAddress);
	uint32_t geometry = ((kPatternHeight - 1) << 16) | (kPatternWidth - 1);
	if ((request.regionControl & kVopEsmartWindowFormatMask) != 0
		|| request.virtualWidth != kPatternWidth || request.activeInfo != geometry
		|| request.displayInfo != geometry || request.displayStart != 0) {
		return kScanoutUnexpectedState;
	}
	return kScanoutOK;
}


// Points the located window at `address` and commits the port's configuration.
template<class Hardware>
uint32_t
SwapScanoutAddress(Hardware& hardware, ScanoutRequest& request, uint32_t address)
{
	uint32_t base = kVopEsmartBase + request.window * kVopEsmartStride;
	hardware.WriteVop(base + kVopEsmartRegionAddress, address);
	request.configDone = kVopConfigDoneEnable | (1u << request.port) | ((1u << request.port) << 16);
	hardware.WriteVop(kVopConfigDone, request.configDone);
	request.polls = 0;
	while ((hardware.ReadVop(kVopConfigDone) & (1u << request.port)) != 0) {
		if (request.polls >= kScanoutPollLimit) {
			request.addressAfter = hardware.ReadVop(base + kVopEsmartRegionAddress);
			return kScanoutTimeout;
		}
		request.polls++;
		hardware.Pause(kScanoutPollMicros);
	}
	request.addressAfter = hardware.ReadVop(base + kVopEsmartRegionAddress);
	return request.addressAfter == address ? kScanoutOK : kScanoutVerifyFailed;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_SCANOUT_H
