/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include <create_display_modes.h>
#include <edid.h>

#include "DisplayModeSet.h"


using namespace RK3588Display;


// CEA-861 video identification code for the AVI infoframe (0 when the mode
// is not one the PLL table drives).
static uint32
cea_vic(const display_mode& mode)
{
	return CeaVideoCode(mode.virtual_width, mode.virtual_height, mode.timing.pixel_clock);
}


static bool
mode_can_be_set(const display_mode& mode)
{
	if (mode.space != B_RGB32 || mode.h_display_start != 0 || mode.v_display_start != 0)
		return false;
	if (mode.timing.h_display != mode.virtual_width || mode.timing.v_display != mode.virtual_height)
		return false;
	ModeRequest request = {};
	request.pixelClockKHz = mode.timing.pixel_clock;
	request.hDisplay = mode.timing.h_display;
	request.hSyncStart = mode.timing.h_sync_start;
	request.hSyncEnd = mode.timing.h_sync_end;
	request.hTotal = mode.timing.h_total;
	request.vDisplay = mode.timing.v_display;
	request.vSyncStart = mode.timing.v_sync_start;
	request.vSyncEnd = mode.timing.v_sync_end;
	request.vTotal = mode.timing.v_total;
	return FindPllConfig(request.pixelClockKHz) != NULL && ModeFitsFrameBuffer(request);
}


// The single supported mode: the firmware's video-port timing as the driver
// decoded it, at the frame buffer size, in 32-bit colour.
display_mode
current_display_mode(void)
{
	const SharedInfo& shared = *gInfo->shared_info;
	display_mode mode;
	memset(&mode, 0, sizeof(mode));
	mode.timing.pixel_clock = shared.pixelClockKHz;
	mode.timing.h_display = shared.width;
	mode.timing.h_sync_start = shared.hSyncStart;
	mode.timing.h_sync_end = shared.hSyncEnd;
	mode.timing.h_total = shared.hTotal;
	mode.timing.v_display = shared.height;
	mode.timing.v_sync_start = shared.vSyncStart;
	mode.timing.v_sync_end = shared.vSyncEnd;
	mode.timing.v_total = shared.vTotal;
	mode.timing.flags = ((shared.syncFlags & kModePositiveHSync) != 0 ? B_POSITIVE_HSYNC : 0)
		| ((shared.syncFlags & kModePositiveVSync) != 0 ? B_POSITIVE_VSYNC : 0);
	mode.space = B_RGB32;
	mode.virtual_width = shared.width;
	mode.virtual_height = shared.height;
	return mode;
}


static bool
same_mode(const display_mode& a, const display_mode& b)
{
	return a.space == b.space && a.virtual_width == b.virtual_width
		&& a.virtual_height == b.virtual_height && a.h_display_start == b.h_display_start
		&& a.v_display_start == b.v_display_start;
}


static bool
is_mode_supported(display_mode* mode)
{
	if (mode == NULL)
		return false;
	display_mode current = current_display_mode();
	if (same_mode(*mode, current))
		return true;
	// Other modes need the mode-set profile and a PLL configuration.
	return (gInfo->info.flags & kAccelerantModeSet) != 0 && mode_can_be_set(*mode);
}


// The mode list: the firmware mode plus, with the mode-set profile, the EDID
// modes the PHY table can drive at or below the frame buffer size.
status_t
create_mode_list(void)
{
	const color_space spaces[] = {B_RGB32};
	display_mode mode = current_display_mode();
	edid1_info edid;
	edid1_info* edidInfo = NULL;
	if ((gInfo->info.flags & (kAccelerantEdid | kAccelerantModeSet))
			== (kAccelerantEdid | kAccelerantModeSet)) {
		edid_decode(&edid, (const edid1_raw*)gInfo->shared_info->edid);
		edidInfo = &edid;
	}
	gInfo->mode_list_area = create_display_modes("rk3588 display modes", edidInfo, &mode, 1,
		spaces, 1, is_mode_supported, &gInfo->mode_list, &gInfo->shared_info->modeCount);
	if (gInfo->mode_list_area < 0)
		return gInfo->mode_list_area;
	gInfo->shared_info->modeListArea = gInfo->mode_list_area;
	return B_OK;
}


uint32
rk3588_accelerant_mode_count(void)
{
	return gInfo->shared_info->modeCount;
}


status_t
rk3588_get_mode_list(display_mode* list)
{
	memcpy(list, gInfo->mode_list, gInfo->shared_info->modeCount * sizeof(display_mode));
	return B_OK;
}


status_t
rk3588_set_display_mode(display_mode* mode)
{
	if (mode == NULL || !is_mode_supported(mode))
		return B_UNSUPPORTED;
	display_mode current = current_display_mode();
	if (same_mode(*mode, current) && memcmp(&mode->timing, &current.timing, sizeof(display_timing)) == 0)
		return B_OK;
	if ((gInfo->info.flags & kAccelerantModeSet) == 0)
		return B_UNSUPPORTED;
	ModeRequest request = {};
	request.version = kModeVersion;
	request.flags = ((mode->timing.flags & B_POSITIVE_HSYNC) != 0 ? kModePositiveHSync : 0)
		| ((mode->timing.flags & B_POSITIVE_VSYNC) != 0 ? kModePositiveVSync : 0);
	request.pixelClockKHz = mode->timing.pixel_clock;
	request.hDisplay = mode->timing.h_display;
	request.hSyncStart = mode->timing.h_sync_start;
	request.hSyncEnd = mode->timing.h_sync_end;
	request.hTotal = mode->timing.h_total;
	request.vDisplay = mode->timing.v_display;
	request.vSyncStart = mode->timing.v_sync_start;
	request.vSyncEnd = mode->timing.v_sync_end;
	request.vTotal = mode->timing.v_total;
	request.vic = cea_vic(*mode);
	if (ioctl(gInfo->device, kSetDisplayMode, &request, sizeof(request)) != 0)
		return errno != 0 ? errno : B_ERROR;
	if (request.result != kModeOK)
		return B_ERROR;
	return B_OK;
}


status_t
rk3588_get_display_mode(display_mode* mode)
{
	*mode = current_display_mode();
	return B_OK;
}


status_t
rk3588_get_edid_info(void* info, size_t size, uint32* _version)
{
	if ((gInfo->info.flags & kAccelerantEdid) == 0)
		return B_ERROR;
	if (size < sizeof(edid1_info))
		return B_BUFFER_OVERFLOW;
	edid_decode((edid1_info*)info, (const edid1_raw*)gInfo->shared_info->edid);
	*_version = EDID_VERSION_1;
	return B_OK;
}


status_t
rk3588_get_frame_buffer_config(frame_buffer_config* config)
{
	config->frame_buffer = gInfo->frame_buffer;
	config->frame_buffer_dma = (void*)(addr_t)gInfo->info.frameBufferPhysical;
	config->bytes_per_row = gInfo->shared_info->bytesPerRow;
	return B_OK;
}


status_t
rk3588_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	// HDMI TX1 TMDS: 25 MHz to 600 MHz; the port runs at least 48 Hz.
	uint32 totalPixels = (uint32)mode->timing.h_total * (uint32)mode->timing.v_total;
	*_low = totalPixels * 48 / 1000;
	*_high = 600000;
	if (*_low < 25000)
		*_low = 25000;
	return *_low <= *_high ? B_OK : B_ERROR;
}


// DPMS: every off state stops the port and powers the PHY down, which the
// sink sees as no signal; on repeats the current mode's mode set.
uint32
rk3588_dpms_capabilities(void)
{
	return B_DPMS_ON | B_DPMS_STAND_BY | B_DPMS_SUSPEND | B_DPMS_OFF;
}


uint32
rk3588_dpms_mode(void)
{
	if (gInfo->shared_info->powerMode == kPowerOn)
		return B_DPMS_ON;
	if (gInfo->dpms_mode == B_DPMS_STAND_BY || gInfo->dpms_mode == B_DPMS_SUSPEND)
		return gInfo->dpms_mode;
	return B_DPMS_OFF;
}


status_t
rk3588_set_dpms_mode(uint32 mode)
{
	if ((gInfo->info.flags & kAccelerantModeSet) == 0)
		return B_UNSUPPORTED;
	PowerRequest request = {};
	request.version = kPowerVersion;
	switch (mode) {
		case B_DPMS_ON:
			request.mode = kPowerOn;
			break;
		case B_DPMS_STAND_BY:
		case B_DPMS_SUSPEND:
		case B_DPMS_OFF:
			request.mode = kPowerOff;
			break;
		default:
			return B_BAD_VALUE;
	}
	if (ioctl(gInfo->device, kSetPowerMode, &request, sizeof(request)) != 0)
		return errno != 0 ? errno : B_ERROR;
	if (request.result != kModeOK)
		return B_ERROR;
	gInfo->dpms_mode = mode;
	return B_OK;
}
