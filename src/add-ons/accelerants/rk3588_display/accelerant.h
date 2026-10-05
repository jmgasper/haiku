/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_ACCELERANT_PRIVATE_H
#define RK3588_DISPLAY_ACCELERANT_PRIVATE_H


#include <Accelerant.h>
#include <OS.h>

#include "DisplayAccelerant.h"


// The accelerant presents the firmware's HDMI1 mode on a frame buffer the
// rk3588_display driver owns and scans out. There is no acceleration; the
// mode list has one entry and the retrace semaphore is absent.
struct accelerant_info {
	int device;
	bool is_clone;
	RK3588Display::AccelerantInfo info;
	area_id shared_info_area;
	RK3588Display::SharedInfo* shared_info;
	area_id mode_list_area;
	display_mode* mode_list;
	area_id frame_buffer_area;
	void* frame_buffer;
	uint32 dpms_mode; // the primary's last accepted DPMS request, 0 before any

	// the two-display layout app_server asked for, until its mode is set
	bool dual_pending;
	uint32 dual_pending_hdmi_x;
	uint32 dual_pending_dp_x;
	uint32 dual_pending_width;
	uint32 dual_pending_scale;
	uint32 dual_pending_mirror;
};

extern accelerant_info* gInfo;

status_t create_mode_list(void);
display_mode current_display_mode(void);

bool is_dual(void);
display_mode dual_layout_mode(uint32 width);
display_mode dual_preferred_mode(void);
status_t apply_dual_layout(void);

extern "C" {
status_t rk3588_init_accelerant(int device);
ssize_t rk3588_accelerant_clone_info_size(void);
void rk3588_get_accelerant_clone_info(void* info);
status_t rk3588_clone_accelerant(void* info);
void rk3588_uninit_accelerant(void);
status_t rk3588_get_accelerant_device_info(accelerant_device_info* info);
sem_id rk3588_accelerant_retrace_semaphore(void);

uint32 rk3588_accelerant_mode_count(void);
status_t rk3588_get_mode_list(display_mode* list);
status_t rk3588_set_display_mode(display_mode* mode);
status_t rk3588_get_display_mode(display_mode* mode);
status_t rk3588_get_edid_info(void* info, size_t size, uint32* version);
status_t rk3588_get_frame_buffer_config(frame_buffer_config* config);
status_t rk3588_get_pixel_clock_limits(display_mode* mode, uint32* low, uint32* high);
uint32 rk3588_dpms_capabilities(void);
uint32 rk3588_dpms_mode(void);
status_t rk3588_set_dpms_mode(uint32 mode);
status_t rk3588_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	color_space space, uint16 bytesPerRow, const uint8* data);
void rk3588_move_cursor(uint16 x, uint16 y);
void rk3588_show_cursor(bool visible);
status_t rk3588_get_preferred_display_mode(display_mode* mode);

uint32 rk3588_display_output_count(void);
status_t rk3588_get_display_outputs(display_output* outputs, uint32* count);
status_t rk3588_get_display_output_modes(uint32 id, display_mode* modes, uint32* count);
status_t rk3588_set_display_layout(const display_output_config* configs, uint32 count,
	display_mode* mode);
}

#endif // RK3588_DISPLAY_ACCELERANT_PRIVATE_H
