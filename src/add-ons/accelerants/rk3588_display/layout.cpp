/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "DisplayEdid.h"
#include "DisplayModeSet.h"


using namespace RK3588Display;


// The two-display desktop (kAccelerantDual): HDMI1 and DP1 each run CEA
// 1080p60 on their screen of one buffer. app_server may put them side by
// side, in either order, or have one mirror the other; both are always on,
// and the display engine scales neither, so both are drawn at one scale.

static const uint32 kHdmiOutput = 1;
static const uint32 kDpOutput = 2;

// CEA 1080p60, what both ports run (DisplayPort.h's kDp1080p60).
static const uint32 kClock = 148500;
static const uint16 kHSyncStart = 2008;
static const uint16 kHSyncEnd = 2052;
static const uint16 kHTotal = 2200;
static const uint16 kVSyncStart = 1084;
static const uint16 kVSyncEnd = 1089;
static const uint16 kVTotal = 1125;


bool
is_dual(void)
{
	return gInfo != NULL && (gInfo->info.flags & kAccelerantDual) != 0;
}


static display_timing
screen_timing(void)
{
	display_timing timing;
	memset(&timing, 0, sizeof(timing));
	timing.pixel_clock = kClock;
	timing.h_display = kFrameWidth;
	timing.h_sync_start = kHSyncStart;
	timing.h_sync_end = kHSyncEnd;
	timing.h_total = kHTotal;
	timing.v_display = kFrameHeight;
	timing.v_sync_start = kVSyncStart;
	timing.v_sync_end = kVSyncEnd;
	timing.v_total = kVTotal;
	timing.flags = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC;
	return timing;
}


/*!	The frame buffer of a desktop \a width wide: one screen, or two side by
	side described as one mode with the horizontal timing doubled, so that
	it still reads as 60 Hz.
*/
display_mode
dual_layout_mode(uint32 width)
{
	display_mode mode;
	memset(&mode, 0, sizeof(mode));
	mode.timing = screen_timing();
	if (width > kFrameWidth) {
		mode.timing.pixel_clock *= 2;
		mode.timing.h_display = width;
		mode.timing.h_sync_start *= 2;
		mode.timing.h_sync_end *= 2;
		mode.timing.h_total *= 2;
	}
	mode.space = B_RGB32;
	mode.virtual_width = width;
	mode.virtual_height = kFrameHeight;
	return mode;
}


/*!	The desktop the layout app_server asked for last, until it is set, or
	the one on screen.
*/
display_mode
dual_preferred_mode(void)
{
	if (gInfo->dual_pending)
		return dual_layout_mode(gInfo->dual_pending_width);
	return current_display_mode();
}


/*!	Points the windows where the layout app_server asked for has them. */
status_t
apply_dual_layout(void)
{
	DualLayout layout = {};
	layout.version = kDualLayoutVersion;
	layout.hdmiX = gInfo->dual_pending_hdmi_x;
	layout.dpX = gInfo->dual_pending_dp_x;
	if (ioctl(gInfo->device, kSetDualLayout, &layout, sizeof(layout)) != 0)
		return errno != 0 ? errno : B_ERROR;

	SharedInfo& shared = *gInfo->shared_info;
	shared.layoutScale = gInfo->dual_pending_scale;
	shared.mirrorOutput = gInfo->dual_pending_mirror;
	gInfo->info.width = layout.width;
	gInfo->info.bytesPerRow = layout.bytesPerRow;
	gInfo->dual_pending = false;
	return B_OK;
}


uint32
rk3588_display_output_count(void)
{
	return 2;
}


status_t
rk3588_get_display_outputs(display_output* outputs, uint32* _count)
{
	const SharedInfo& shared = *gInfo->shared_info;
	uint16 scale = shared.layoutScale != 0 ? shared.layoutScale : 100;
	uint32 count = 0;
	for (uint32 id = kHdmiOutput; id <= kDpOutput && count < *_count; id++) {
		display_output& output = outputs[count++];
		memset(&output, 0, sizeof(output));
		output.version = B_DISPLAY_OUTPUT_VERSION;
		output.id = id;
		strlcpy(output.name, id == kHdmiOutput ? "HDMI-1" : "DP-1",
			sizeof(output.name));
		output.flags = B_DISPLAY_OUTPUT_CONNECTED | B_DISPLAY_OUTPUT_ENABLED;
		if (shared.mirrorOutput == id && shared.hdmiX == shared.dpX)
			output.flags |= B_DISPLAY_OUTPUT_MIRROR;
		output.x = id == kHdmiOutput ? shared.hdmiX : shared.dpX;
		output.y = 0;
		output.width = kFrameWidth;
		output.height = kFrameHeight;
		output.scale = scale;
		output.render_scale = scale;
		output.native_timing = screen_timing();
		output.timing = screen_timing();
		if (id == kHdmiOutput && shared.hdmiEdidResult == kEdidOK) {
			output.edid_length = sizeof(shared.hdmiEdid);
			memcpy(output.edid, shared.hdmiEdid, sizeof(shared.hdmiEdid));
		} else if (id == kDpOutput && (gInfo->info.flags & kAccelerantEdid) != 0) {
			output.edid_length = sizeof(shared.edid);
			memcpy(output.edid, shared.edid, sizeof(shared.edid));
		}
	}
	*_count = count;
	return B_OK;
}


status_t
rk3588_get_display_output_modes(uint32 id, display_mode* modes, uint32* _count)
{
	if (id != kHdmiOutput && id != kDpOutput)
		return B_ENTRY_NOT_FOUND;
	if (*_count < 1) {
		*_count = 0;
		return B_OK;
	}
	modes[0] = dual_layout_mode(kFrameWidth);
	*_count = 1;
	return B_OK;
}


/*!	Takes the layout app_server wants; the windows move when the mode it
	returns is set. Each screen starts at desktop column 0 or 1920 in frame
	buffer pixels, on row 0.
*/
status_t
rk3588_set_display_layout(const display_output_config* configs, uint32 count,
	display_mode* _mode)
{
	bool seen[2] = { false, false };
	uint32 x[2] = { 0, 0 };
	uint32 mirror = 0;
	uint16 scale = 0;
	for (uint32 i = 0; i < count; i++) {
		const display_output_config& config = configs[i];
		if (config.id != kHdmiOutput && config.id != kDpOutput)
			return B_ENTRY_NOT_FOUND;
		if ((config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0)
			return B_NOT_SUPPORTED;
		uint16 renderScale = config.render_scale != 0 ? config.render_scale : 100;
		if (config.scale != renderScale || (scale != 0 && config.scale != scale))
			return B_NOT_SUPPORTED;
		scale = config.scale;
		if (config.timing.h_display != 0 && (config.timing.h_display != kFrameWidth
				|| config.timing.v_display != kFrameHeight))
			return B_BAD_VALUE;
		int32 left = (config.x * renderScale + 50) / 100;
		int32 top = (config.y * renderScale + 50) / 100;
		// a scale that does not divide the screen evenly can leave the
		// second screen a pixel or two off
		if (abs(left - (int32)kFrameWidth) <= 2)
			left = kFrameWidth;
		if (top != 0 || (left != 0 && left != (int32)kFrameWidth))
			return B_NOT_SUPPORTED;
		seen[config.id - 1] = true;
		x[config.id - 1] = left;
		if ((config.flags & B_DISPLAY_OUTPUT_MIRROR) != 0)
			mirror = config.id;
	}
	if (!seen[0] || !seen[1])
		return B_NOT_SUPPORTED;
	if (x[0] == x[1]) {
		if (x[0] != 0)
			return B_BAD_VALUE;
		if (mirror == 0)
			mirror = kDpOutput;
	} else
		mirror = 0;

	gInfo->dual_pending = true;
	gInfo->dual_pending_hdmi_x = x[0];
	gInfo->dual_pending_dp_x = x[1];
	gInfo->dual_pending_width = x[0] == x[1] ? kFrameWidth : 2 * kFrameWidth;
	gInfo->dual_pending_scale = scale;
	gInfo->dual_pending_mirror = mirror;
	*_mode = dual_layout_mode(gInfo->dual_pending_width);
	return B_OK;
}
