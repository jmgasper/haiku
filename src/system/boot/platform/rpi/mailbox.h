/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MAILBOX_H
#define MAILBOX_H


#include <SupportDefs.h>


// clock IDs of the property interface
#define MAILBOX_CLOCK_UART	2
#define MAILBOX_CLOCK_ARM	3

struct mailbox_frame_buffer {
	phys_addr_t	base;
	uint32		size;
	uint32		width;
	uint32		height;
	uint32		bytesPerRow;
};

void mailbox_init();
status_t mailbox_get_clock_rate(uint32 clock, bool maximum, uint32& _rate);
status_t mailbox_set_clock_rate(uint32 clock, uint32 rate);
status_t mailbox_get_display_size(uint32& _width, uint32& _height);
status_t mailbox_allocate_frame_buffer(uint32 width, uint32 height,
	mailbox_frame_buffer& _buffer);


#endif	/* MAILBOX_H */
