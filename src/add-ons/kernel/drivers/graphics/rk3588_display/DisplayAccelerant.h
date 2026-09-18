/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_ACCELERANT_H
#define RK3588_DISPLAY_ACCELERANT_H


#include <stdint.h>


namespace RK3588Display {

// Accelerant interface of the rk3588_display device, opt-in through the
// rock5-itx-edk2-v1.1-display-accelerant profile. The primary accelerant
// acquires the frame buffer: the driver allocates a contiguous
// write-combining buffer of the firmware mode, points the live HDMI1 window
// at it with the qualified scanout swap and moves the kernel console there.
// Closing the acquiring handle restores the firmware frame buffer. Clones
// only read the shared information and map the buffer.
static const uint32_t kAcquireFrameBuffer = 0x52444904; // writable handle only
static const uint32_t kGetAccelerantInfo = 0x52444905;
static const uint32_t kCloneFrameBuffer = 0x52444906; // fills an area_info
static const uint32_t kGetDeviceName = 0x52444907; // B_PATH_NAME_LENGTH bytes
static const uint32_t kAccelerantVersion = 1;
static const char kAccelerantSignature[] = "rk3588_display.accelerant";
static const char kDevicePath[] = "graphics/rk3588_display/0";

static const uint32_t kFrameWidth = 1920;
static const uint32_t kFrameHeight = 1080;
static const uint32_t kFrameBytesPerRow = kFrameWidth * 4;
static const uint32_t kFrameBytes = kFrameBytesPerRow * kFrameHeight;

static const uint32_t kAccelerantAcquired = 1; // flags
static const uint32_t kAccelerantEdid = 2; // shared EDID block is valid

struct AccelerantInfo {
	uint32_t version; // in: kAccelerantVersion
	uint32_t flags;
	int32_t sharedArea; // cloneable area holding SharedInfo
	uint32_t frameBufferPhysical;
	uint32_t firmwareAddress;
	uint32_t port;
	uint32_t window;
	uint32_t polls; // REG_CFG_DONE polls of the acquiring swap
	uint32_t width;
	uint32_t height;
	uint32_t bytesPerRow;
	uint32_t reserved;
};

// Shared between the driver and every accelerant instance. Timing values are
// the firmware's video-port programming, decoded by the driver from the VOP2
// registers of the port that feeds HDMI1, in the Accelerant.h convention.
struct SharedInfo {
	uint32_t version;
	uint32_t flags;
	int32_t modeListArea; // set by the primary accelerant
	uint32_t modeCount;
	uint32_t width;
	uint32_t height;
	uint32_t bytesPerRow;
	uint32_t pixelClockKHz;
	uint32_t hSyncStart;
	uint32_t hSyncEnd;
	uint32_t hTotal;
	uint32_t vSyncStart;
	uint32_t vSyncEnd;
	uint32_t vTotal;
	uint32_t portTiming[4]; // HTOTAL_HS_END, HACT_ST_END, VTOTAL_VS_END, VACT_ST_END
	uint32_t edidResult;
	uint8_t edid[128];
	char name[32];
};


// Decodes the VOP2 video-port timing words into sync positions. The port
// starts its line and frame with the sync pulse; active video follows the
// back porch. Returns false when the words do not describe the frame size.
inline bool
DecodePortTiming(const uint32_t words[4], uint32_t width, uint32_t height, SharedInfo& info)
{
	uint32_t hTotal = words[0] >> 16 & 0x1fff, hSyncEnd = words[0] & 0x1fff;
	uint32_t hStart = words[1] >> 16 & 0x1fff, hEnd = words[1] & 0x1fff;
	uint32_t vTotal = words[2] >> 16 & 0x1fff, vSyncEnd = words[2] & 0x1fff;
	uint32_t vStart = words[3] >> 16 & 0x1fff, vEnd = words[3] & 0x1fff;
	if (hEnd <= hStart || vEnd <= vStart || hEnd - hStart != width || vEnd - vStart != height)
		return false;
	if (hStart < hSyncEnd || vStart < vSyncEnd || hTotal <= hEnd || vTotal <= vEnd)
		return false;
	info.hTotal = hTotal;
	info.vTotal = vTotal;
	info.hSyncStart = width + (hTotal - hEnd);
	info.hSyncEnd = info.hSyncStart + hSyncEnd;
	info.vSyncStart = height + (vTotal - vEnd);
	info.vSyncEnd = info.vSyncStart + vSyncEnd;
	return info.hSyncEnd <= hTotal && info.vSyncEnd <= vTotal;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_ACCELERANT_H
