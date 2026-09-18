/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"


extern "C" void*
get_accelerant_hook(uint32 feature, void* data)
{
	switch (feature) {
		case B_INIT_ACCELERANT:
			return (void*)rk3588_init_accelerant;
		case B_UNINIT_ACCELERANT:
			return (void*)rk3588_uninit_accelerant;
		case B_CLONE_ACCELERANT:
			return (void*)rk3588_clone_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE:
			return (void*)rk3588_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO:
			return (void*)rk3588_get_accelerant_clone_info;
		case B_GET_ACCELERANT_DEVICE_INFO:
			return (void*)rk3588_get_accelerant_device_info;
		case B_ACCELERANT_RETRACE_SEMAPHORE:
			return (void*)rk3588_accelerant_retrace_semaphore;

		case B_ACCELERANT_MODE_COUNT:
			return (void*)rk3588_accelerant_mode_count;
		case B_GET_MODE_LIST:
			return (void*)rk3588_get_mode_list;
		case B_SET_DISPLAY_MODE:
			return (void*)rk3588_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)rk3588_get_display_mode;
		case B_GET_EDID_INFO:
			return (void*)rk3588_get_edid_info;
		case B_GET_FRAME_BUFFER_CONFIG:
			return (void*)rk3588_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS:
			return (void*)rk3588_get_pixel_clock_limits;
		case B_DPMS_CAPABILITIES:
		case B_DPMS_MODE:
		case B_SET_DPMS_MODE:
			// Power control rides on the mode-set profile's PHY and port paths.
			if (gInfo == NULL || (gInfo->info.flags & RK3588Display::kAccelerantModeSet) == 0)
				return NULL;
			if (feature == B_DPMS_CAPABILITIES)
				return (void*)rk3588_dpms_capabilities;
			if (feature == B_DPMS_MODE)
				return (void*)rk3588_dpms_mode;
			return (void*)rk3588_set_dpms_mode;
	}
	return NULL;
}
