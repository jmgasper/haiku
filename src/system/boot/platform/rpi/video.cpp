/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The frame buffer comes from the VideoCore firmware, at the size of the
	display on HDMI0. The kernel and app_server keep using it until a display
	driver takes over. */


#include "video.h"

#include <string.h>

#include <boot/menu.h>
#include <boot/platform.h>
#include <boot/platform/generic/video.h>
#include <boot/stage2.h>
#include <boot/stdio.h>

#include "console.h"
#include "mailbox.h"


static addr_t sFrameBuffer = 0;


void
video_init()
{
	gKernelArgs.frame_buffer.enabled = false;
	gKernelArgs.vesa_modes = NULL;
	gKernelArgs.vesa_modes_size = 0;
	gKernelArgs.edid_info = NULL;

	// With no display attached the firmware reports nothing useful.
	uint32 width = 0;
	uint32 height = 0;
	if (mailbox_get_display_size(width, height) != B_OK
		|| width < 640 || height < 480) {
		width = 1920;
		height = 1080;
	}

	mailbox_frame_buffer buffer;
	status_t status = mailbox_allocate_frame_buffer(width, height, buffer);
	if (status != B_OK) {
		dprintf("video: no frame buffer from the firmware: %s\n",
			strerror(status));
		return;
	}

	dprintf("video: %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32 " bytes per row, "
		"at %#" B_PRIxPHYSADDR "\n", buffer.width, buffer.height,
		buffer.bytesPerRow, buffer.base);

	gKernelArgs.frame_buffer.physical_buffer.start = buffer.base;
	gKernelArgs.frame_buffer.physical_buffer.size = buffer.size;
	gKernelArgs.frame_buffer.width = buffer.width;
	gKernelArgs.frame_buffer.height = buffer.height;
	gKernelArgs.frame_buffer.depth = 32;
	gKernelArgs.frame_buffer.bytes_per_row = buffer.bytesPerRow;
	gKernelArgs.frame_buffer.enabled = true;

	sFrameBuffer = buffer.base;
	memset((void*)sFrameBuffer, 0, buffer.bytesPerRow * buffer.height);
}


addr_t
video_frame_buffer()
{
	return sFrameBuffer;
}


bool
video_mode_hook(Menu* menu, MenuItem* item)
{
	return true;
}


Menu*
video_mode_menu()
{
	Menu* menu = new(std::nothrow) Menu(CHOICE_MENU, "Select Video Mode");
	MenuItem* item;

	menu->AddItem(item = new(std::nothrow) MenuItem("Default"));
	item->SetMarked(true);
	item->Select(true);
	item->SetHelpText("The video mode is the one the firmware set up for the "
		"display on HDMI0.");

	menu->AddSeparatorItem();
	menu->AddItem(item = new(std::nothrow) MenuItem("Return to main menu"));
	item->SetType(MENU_ITEM_NO_CHOICE);

	return menu;
}


//	#pragma mark -


extern "C" void
platform_blit4(addr_t frameBuffer, const uint8* data, uint16 width,
	uint16 height, uint16 imageWidth, uint16 left, uint16 top)
{
}


extern "C" void
platform_set_palette(const uint8* palette)
{
}


extern "C" void
platform_switch_to_logo(void)
{
	if (sFrameBuffer == 0)
		return;

	video_display_splash(sFrameBuffer);
}


extern "C" void
platform_switch_to_text_mode(void)
{
	if (sFrameBuffer == 0)
		return;

	console_clear_screen();
}


extern "C" status_t
platform_init_video(void)
{
	// video_init() ran before the console came up
	return sFrameBuffer != 0 ? B_OK : B_ERROR;
}
