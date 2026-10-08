/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _AUXDISPLAY_H
#define _AUXDISPLAY_H


#include <Drivers.h>
#include <OS.h>


/*	Auxiliary displays: small screens beside the desktop that programs draw
	on directly, without app_server. A DSI panel on a Raspberry Pi, a USB
	LCD in a PC case. Drivers publish them under /dev/auxdisplay/<driver>/<n>
	and speak this protocol; the Device Kit's BAuxDisplay wraps it.

	A display shows one of the driver's buffers (B_RGB32 unless the info
	says otherwise). A program clones a buffer into its own address space
	(AUX_DISPLAY_CLONE_BUFFER), draws into it and presents it
	(AUX_DISPLAY_PRESENT); the change shows from the next frame. A driver
	without AUX_DISPLAY_FLAG_BUFFERS takes whole frames through write()
	instead. Touch events, when the display has a touch panel, come from
	AUX_DISPLAY_WAIT_TOUCH. */

#define AUX_DISPLAY_DIRECTORY		"/dev/auxdisplay"
#define AUX_DISPLAY_API_VERSION		1

enum {
	// aux_display_info
	AUX_DISPLAY_GET_INFO = B_DEVICE_OP_CODES_END + 0x4100,
	// uint32: 1 switches the display on (clocks, panel), 0 off
	AUX_DISPLAY_SET_POWER,
	// uint32: backlight brightness in percent
	AUX_DISPLAY_SET_BACKLIGHT,
	// aux_display_buffer: index in, the buffer cloned into the caller's team
	AUX_DISPLAY_CLONE_BUFFER,
	// uint32: the index of the buffer to show from the next frame on
	AUX_DISPLAY_PRESENT,
	// aux_display_touch_events: waits up to timeout for touch events
	AUX_DISPLAY_WAIT_TOUCH
};

// aux_display_info::flags
#define AUX_DISPLAY_FLAG_BUFFERS	0x01	// CLONE_BUFFER and PRESENT work
#define AUX_DISPLAY_FLAG_TOUCH		0x02	// has a touch panel
#define AUX_DISPLAY_FLAG_BACKLIGHT	0x04	// SET_BACKLIGHT works
#define AUX_DISPLAY_FLAG_POWER		0x08	// SET_POWER works
#define AUX_DISPLAY_FLAG_ON			0x10	// currently switched on

typedef struct aux_display_info {
	uint32	version;			// AUX_DISPLAY_API_VERSION
	uint32	flags;
	char	name[64];			// the panel, for people
	uint32	width;				// pixels
	uint32	height;
	uint32	bytes_per_row;
	uint32	color_space;		// a Haiku color_space value
	uint32	buffer_count;
	uint32	buffer_size;		// bytes, each
	uint32	width_mm;			// 0 when unknown
	uint32	height_mm;
	uint32	refresh_rate;		// in mHz, 0 when unknown
	uint32	reserved[8];
} aux_display_info;

typedef struct aux_display_buffer {
	uint32	index;				// in
	area_id	area;				// out: an area in the caller's team
	void*	address;
	uint32	size;
} aux_display_buffer;

// aux_display_touch_event::flags
#define AUX_DISPLAY_TOUCH_DOWN		0x1
#define AUX_DISPLAY_TOUCH_UP		0x2
#define AUX_DISPLAY_TOUCH_MOVE		0x4

typedef struct aux_display_touch_event {
	bigtime_t	time;
	uint16		x;				// display pixels
	uint16		y;
	uint16		pressure;
	uint8		id;				// the contact, for multi-touch
	uint8		flags;
} aux_display_touch_event;

#define AUX_DISPLAY_MAX_TOUCH_EVENTS	32

typedef struct aux_display_touch_events {
	bigtime_t	timeout;		// in: relative, B_INFINITE_TIMEOUT waits
	uint32		count;			// in: capacity, out: events returned
	aux_display_touch_event events[AUX_DISPLAY_MAX_TOUCH_EVENTS];
} aux_display_touch_events;


#endif	/* _AUXDISPLAY_H */
