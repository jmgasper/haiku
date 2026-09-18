/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <errno.h>
#include <new>
#include <string.h>
#include <sys/ioctl.h>

#include "DisplayCursor.h"


using namespace RK3588Display;


// The hardware cursor: app_server hands over its pointer bitmap (straight
// alpha B_RGBA32, at most 64x64 here) and its position; the driver blends the
// window over the frame buffer, which no longer carries the pointer.
status_t
rk3588_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	color_space space, uint16 bytesPerRow, const uint8* data)
{
	if ((gInfo->info.flags & kAccelerantCursor) == 0)
		return B_UNSUPPORTED;
	if ((space != B_RGBA32 && space != B_RGB32) || width == 0 || height == 0
		|| width > kCursorMaxSize || height > kCursorMaxSize || hotX >= width || hotY >= height
		|| bytesPerRow < width * 4 || data == NULL) {
		return B_UNSUPPORTED;
	}
	CursorBitmap* bitmap = new(std::nothrow) CursorBitmap;
	if (bitmap == NULL)
		return B_NO_MEMORY;
	memset(bitmap, 0, sizeof(*bitmap));
	bitmap->version = kCursorVersion;
	bitmap->width = width;
	bitmap->height = height;
	bitmap->hotX = hotX;
	bitmap->hotY = hotY;
	bitmap->bytesPerRow = kCursorBytesPerRow;
	for (uint16 row = 0; row < height; row++) {
		uint8* target = bitmap->data + row * kCursorBytesPerRow;
		memcpy(target, data + row * bytesPerRow, width * 4);
		if (space == B_RGB32) {
			for (uint16 column = 0; column < width; column++)
				target[column * 4 + 3] = 0xff;
		}
	}
	status_t status = B_OK;
	if (ioctl(gInfo->device, kSetCursorBitmap, bitmap, sizeof(*bitmap)) != 0)
		status = errno != 0 ? errno : B_ERROR;
	else if (bitmap->result != kCursorOK)
		status = B_ERROR;
	delete bitmap;
	return status;
}


void
rk3588_move_cursor(uint16 x, uint16 y)
{
	CursorMove move = {};
	move.version = kCursorVersion;
	move.x = x;
	move.y = y;
	ioctl(gInfo->device, kMoveCursor, &move, sizeof(move));
}


void
rk3588_show_cursor(bool visible)
{
	CursorShow show = {};
	show.version = kCursorVersion;
	show.visible = visible ? 1 : 0;
	ioctl(gInfo->device, kShowCursor, &show, sizeof(show));
}
