/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <string.h>

#include <create_display_modes.h>
#include <edid.h>


using namespace RK3588Display;


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
	mode.timing.flags = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC;
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
	return same_mode(*mode, current);
}


status_t
create_mode_list(void)
{
	const color_space spaces[] = {B_RGB32};
	display_mode mode = current_display_mode();
	gInfo->mode_list_area = create_display_modes("rk3588 display modes", NULL, &mode, 1,
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
	// Only the firmware mode exists until a native mode change is qualified.
	if (mode != NULL && is_mode_supported(mode))
		return B_OK;
	return B_UNSUPPORTED;
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
