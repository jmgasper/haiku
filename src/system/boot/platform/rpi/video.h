/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VIDEO_H
#define VIDEO_H


#include <SupportDefs.h>


class Menu;
class MenuItem;

void video_init();
	// asks the firmware for a frame buffer
addr_t video_frame_buffer();
	// 0 if there is none

bool video_mode_hook(Menu* menu, MenuItem* item);
Menu* video_mode_menu();


#endif	/* VIDEO_H */
