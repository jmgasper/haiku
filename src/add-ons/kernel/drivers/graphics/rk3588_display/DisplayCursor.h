/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_CURSOR_H
#define RK3588_DISPLAY_CURSOR_H


#include <stdint.h>

#include "DisplayScanout.h"


namespace RK3588Display {

// Hardware cursor: a small ARGB window (ESMART3, which the firmware left
// bound to HDMI1's video port above the desktop window) blended over the
// frame buffer by the port's alpha mixer, opt-in through the
// rock5-itx-edk2-v1.1-display-cursor profile. app_server hands the
// accelerant the pointer bitmap and its position; the frame buffer itself
// no longer carries the pointer.
static const uint32_t kSetCursorBitmap = 0x5244490b; // writable handle, frame buffer acquired
static const uint32_t kMoveCursor = 0x5244490c;
static const uint32_t kShowCursor = 0x5244490d;
static const uint32_t kGetCursor = 0x5244490e; // any handle
static const uint32_t kCursorVersion = 1;

static const uint32_t kCursorMaxSize = 64;
static const uint32_t kCursorBytesPerRow = kCursorMaxSize * 4;
static const uint32_t kCursorBufferBytes = kCursorBytesPerRow * kCursorMaxSize;

static const uint32_t kCursorOK = 0;
static const uint32_t kCursorNotAcquired = 1;
static const uint32_t kCursorUnsupported = 2; // size, stride or a position outside int16
static const uint32_t kCursorTimeout = 3; // the port never took the configuration
static const uint32_t kCursorVerifyFailed = 4; // window registers read back differently

struct CursorBitmap {
	uint32_t version; // in
	uint32_t width, height; // in: 1..64
	uint32_t hotX, hotY; // in: inside the bitmap
	uint32_t bytesPerRow; // in: of the rows in data, >= width * 4
	uint32_t result;
	uint32_t polls;
	uint8_t data[kCursorBufferBytes]; // in: B_RGBA32 rows, straight alpha
};

struct CursorMove {
	uint32_t version; // in
	int32_t x, y; // in: pointer position on the mode's frame
	uint32_t result;
	uint32_t polls;
	uint32_t displayStart; // DSP_ST read back
	uint32_t address; // YRGB_MST read back
	uint32_t reserved;
};

struct CursorShow {
	uint32_t version; // in
	uint32_t visible; // in
	uint32_t result;
	uint32_t polls;
	uint32_t regionControl; // REGION0_CTRL read back
	uint32_t reserved[3];
};

// The complete cursor state, for the probe to save and restore.
struct CursorState {
	uint32_t version;
	uint32_t width, height;
	uint32_t hotX, hotY;
	int32_t x, y;
	uint32_t visible;
	uint32_t window; // ESMART window number
	uint32_t mixer; // alpha mixer number
	uint32_t regionControl, displayStart, address; // as last programmed
	uint32_t mixWords[4]; // as last programmed
	uint8_t data[kCursorBufferBytes]; // 64-pixel rows
};

// ESMART3 feeds video port 2 on the firmware's layer map (OVL_PORT_SEL
// 0xa5a47738, OVL_LAYER_SEL 0x76543210: window 7 on layer 7, the topmost);
// layer 7 is blended by mixer 6.
static const uint32_t kVopCursorWindow = 3;
static const uint32_t kVopCursorMixer = 6;
static const uint32_t kVopEsmartControl1 = 0x04; // AXI read ids: [8:4] yrgb, [16:12] uv
static const uint32_t kVopEsmartAxiControl = 0x08; // bit 1: AXI bus 1
static const uint32_t kVopEsmartRegionScaleControl = 0x30;
static const uint32_t kVopEsmartRegionScaleFactor = 0x34;
static const uint32_t kVopEsmartColorKey = 0xd0;
static const uint32_t kVopEsmartRegionEnable = 1u << 0;
static const uint32_t kVopEsmart3AxiYrgbId = 0x0c;
static const uint32_t kVopEsmart3AxiUvId = 0x0d;
static const uint32_t kVopMixerBase = 0x650; // MIX0_SRC_COLOR_CTRL; 0x10 per mixer
static const uint32_t kVopMixerStride = 0x10;
// Mixer words for a straight-alpha ARGB source over an opaque destination
// (Linux vop2_parse_alpha: src per-pixel alpha, factor one, not premultiplied;
// dst global alpha 0xff, factor 1 - src alpha; alpha outputs likewise).
static const uint32_t kVopMixerSourceColor = 0x00ff0125;
static const uint32_t kVopMixerDestinationColor = 0x00ff0060;
static const uint32_t kVopMixerSourceAlpha = 0x00000024;
static const uint32_t kVopMixerDestinationAlpha = 0x00000074;


// The window rectangle after clipping the pointer's bitmap to the frame.
struct CursorPlacement {
	bool visible;
	uint32_t cropX, cropY; // bitmap pixels cut off at the left and top
	uint32_t displayX, displayY;
	uint32_t width, height;
};


inline CursorPlacement
PlaceCursor(const CursorState& state, uint32_t frameWidth, uint32_t frameHeight)
{
	CursorPlacement placement = {};
	int32_t left = state.x - (int32_t)state.hotX;
	int32_t top = state.y - (int32_t)state.hotY;
	int32_t right = left + (int32_t)state.width;
	int32_t bottom = top + (int32_t)state.height;
	if (state.visible == 0 || state.width == 0 || state.height == 0 || right <= 0 || bottom <= 0
		|| left >= (int32_t)frameWidth || top >= (int32_t)frameHeight) {
		return placement;
	}
	placement.visible = true;
	placement.cropX = left < 0 ? (uint32_t)-left : 0;
	placement.cropY = top < 0 ? (uint32_t)-top : 0;
	placement.displayX = left < 0 ? 0 : (uint32_t)left;
	placement.displayY = top < 0 ? 0 : (uint32_t)top;
	uint32_t clippedRight = right > (int32_t)frameWidth ? frameWidth : (uint32_t)right;
	uint32_t clippedBottom = bottom > (int32_t)frameHeight ? frameHeight : (uint32_t)bottom;
	placement.width = clippedRight - placement.displayX;
	placement.height = clippedBottom - placement.displayY;
	return placement;
}


// Programs the cursor window and the mixer for the state and commits the
// port. The window's bus ids, scaling (none), colour key (off) and the
// mixer words are written every time: they are cheap and the firmware never
// set them for this window. Read-backs after the commit fill the state.
template<class Hardware>
uint32_t
ApplyCursor(Hardware& hardware, CursorState& state, uint32_t bufferPhysical, uint32_t port,
	uint32_t frameWidth, uint32_t frameHeight, uint32_t& polls)
{
	uint32_t base = kVopEsmartBase + state.window * kVopEsmartStride;
	CursorPlacement placement = PlaceCursor(state, frameWidth, frameHeight);
	uint32_t control1 = hardware.ReadVop(base + kVopEsmartControl1);
	control1 = (control1 & ~((0x1fu << 4) | (0x1fu << 12)))
		| (kVopEsmart3AxiYrgbId << 4) | (kVopEsmart3AxiUvId << 12);
	hardware.WriteVop(base + kVopEsmartControl1, control1);
	hardware.WriteVop(base + kVopEsmartAxiControl, hardware.ReadVop(base + kVopEsmartAxiControl) | 2u);
	hardware.WriteVop(base + kVopEsmartColorKey, 0);
	hardware.WriteVop(base + kVopEsmartRegionScaleControl, 0);
	hardware.WriteVop(base + kVopEsmartRegionScaleFactor, 0);
	uint32_t mixer = kVopMixerBase + state.mixer * kVopMixerStride;
	state.mixWords[0] = kVopMixerSourceColor;
	state.mixWords[1] = kVopMixerDestinationColor;
	state.mixWords[2] = kVopMixerSourceAlpha;
	state.mixWords[3] = kVopMixerDestinationAlpha;
	for (unsigned i = 0; i < 4; i++)
		hardware.WriteVop(mixer + i * 4, state.mixWords[i]);
	uint32_t address = bufferPhysical + placement.cropY * kCursorBytesPerRow + placement.cropX * 4;
	uint32_t geometry = placement.visible
		? ((placement.height - 1) << 16) | (placement.width - 1) : 0;
	uint32_t start = placement.visible ? (placement.displayY << 16) | placement.displayX : 0;
	uint32_t regionControl = placement.visible ? kVopEsmartRegionEnable : 0; // ARGB8888, no swap
	hardware.WriteVop(base + kVopEsmartRegionVirtual, kCursorBytesPerRow / 4);
	hardware.WriteVop(base + kVopEsmartRegionAddress, address);
	hardware.WriteVop(base + kVopEsmartRegionActive, geometry);
	hardware.WriteVop(base + kVopEsmartRegionDisplay, geometry);
	hardware.WriteVop(base + kVopEsmartRegionStart, start);
	hardware.WriteVop(base + kVopEsmartRegionControl, regionControl);
	hardware.WriteVop(kVopConfigDone, kVopConfigDoneEnable | (1u << port) | ((1u << port) << 16));
	polls = 0;
	while ((hardware.ReadVop(kVopConfigDone) & (1u << port)) != 0) {
		if (polls >= kScanoutPollLimit)
			return kCursorTimeout;
		polls++;
		hardware.Pause(kScanoutPollMicros);
	}
	state.regionControl = hardware.ReadVop(base + kVopEsmartRegionControl);
	state.displayStart = hardware.ReadVop(base + kVopEsmartRegionStart);
	state.address = hardware.ReadVop(base + kVopEsmartRegionAddress);
	if (state.regionControl != regionControl || state.displayStart != start
		|| state.address != address) {
		return kCursorVerifyFailed;
	}
	return kCursorOK;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_CURSOR_H
