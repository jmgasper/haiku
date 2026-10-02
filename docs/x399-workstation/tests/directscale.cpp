/*
 * directscale: does a direct window stay connected on a desktop drawn at a
 * higher density, and is it told where it is in frame buffer pixels?
 *
 * A BDirectWindow with B_DIRECT_DEVICE_PIXELS paints its visible rectangles
 * itself, into the frame buffer and into the window system's own copy of the
 * screen (drawing_bits_area), twenty times a second as a real renderer
 * would, and prints what it was told on each DirectConnected(). It moves
 * itself once half way through, so the second connection shows the
 * clipping following it.
 *
 *   directscale [seconds]       (default 6)
 *
 * The pattern has a one pixel border, a bar per 100 frame buffer pixels and
 * a different colour per corner, so a screenshot (which reads the window
 * system's copy) and `nvscanout --dump` (the frame buffer) both show whether
 * it landed where app_server would have drawn the window.
 *
 * Builds against headers from before the new fields: they are the first
 * three words of _reserved1 there.
 *
 * SPDX-License-Identifier: MIT
 */

#include <Application.h>
#include <Autolock.h>
#include <DirectWindow.h>
#include <Locker.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef B_DIRECT_DEVICE_PIXELS
#	define B_DIRECT_DEVICE_PIXELS 0x00000400
#	define DEVICE_SCALE(info) ((info)->_reserved1[0])
#	define DRAWING_AREA(info) ((area_id)(info)->_reserved1[1])
#	define DRAWING_BPR(info) ((int32)(info)->_reserved1[2])
#else
#	define DEVICE_SCALE(info) ((info)->device_scale)
#	define DRAWING_AREA(info) ((info)->drawing_bits_area)
#	define DRAWING_BPR(info) ((info)->drawing_bytes_per_row)
#endif


class TestWindow : public BDirectWindow {
public:
	TestWindow(BRect frame)
		:
		BDirectWindow(frame, "directscale", B_TITLED_WINDOW,
			B_DIRECT_DEVICE_PIXELS | B_QUIT_ON_WINDOW_CLOSE),
		fFront(NULL), fFrontArea(-1), fFrontSource(-1),
		fDrawing(NULL), fDrawingArea(-1), fDrawingSource(-1),
		fConnections(0)
	{
	}

	~TestWindow()
	{
		free(fInfo);
		if (fFrontArea >= 0)
			delete_area(fFrontArea);
		if (fDrawingArea >= 0)
			delete_area(fDrawingArea);
	}

	virtual void DirectConnected(direct_buffer_info* info)
	{
		int32 mode = info->buffer_state & B_DIRECT_MODE_MASK;
		printf("connection %" B_PRId32 ": %s state=0x%x scale=%" B_PRIu32
			" bounds=(%" B_PRId32 ",%" B_PRId32 ")-(%" B_PRId32 ",%" B_PRId32
			") clips=%" B_PRIu32 " bpr=%" B_PRId32 " bits_area=%" B_PRId32
			" drawing_area=%" B_PRId32 " drawing_bpr=%" B_PRId32 "\n",
			++fConnections, mode == B_DIRECT_START ? "start"
				: mode == B_DIRECT_STOP ? "stop" : "modify",
			(unsigned)info->buffer_state, DEVICE_SCALE(info),
			info->window_bounds.left, info->window_bounds.top,
			info->window_bounds.right, info->window_bounds.bottom,
			info->clip_list_count, info->bytes_per_row, info->bits_area,
			DRAWING_AREA(info), DRAWING_BPR(info));
		for (uint32 i = 0; i < info->clip_list_count && i < 8; i++) {
			const clipping_rect& r = info->clip_list[i];
			printf("  clip %" B_PRIu32 ": (%" B_PRId32 ",%" B_PRId32 ")-(%"
				B_PRId32 ",%" B_PRId32 ")\n", i, r.left, r.top, r.right,
				r.bottom);
		}
		fflush(stdout);

		BAutolock _(fLock);
		fConnected = mode != B_DIRECT_STOP && info->bits_per_pixel == 32;
		if (!fConnected)
			return;
		size_t size = sizeof(direct_buffer_info)
			+ (info->clip_list_count - 1) * sizeof(clipping_rect);
		fInfo = (direct_buffer_info*)realloc(fInfo, size);
		memcpy(fInfo, info, size);

		fFrontBits = (uint8*)_Map(info->bits_area, fFrontSource,
			fFrontArea, fFront);
		if (fFrontBits == NULL)
			fFrontBits = (uint8*)info->bits;
		fDrawingBits = (uint8*)_Map(DRAWING_AREA(info), fDrawingSource,
			fDrawingArea, fDrawing);
		_PaintAll();
	}

	void Repaint()
	{
		BAutolock _(fLock);
		if (fConnected)
			_PaintAll();
	}

private:
	void _PaintAll()
	{
		for (uint32 i = 0; i < fInfo->clip_list_count; i++) {
			if (fFrontBits != NULL)
				_Paint(fFrontBits, fInfo->bytes_per_row, fInfo->window_bounds,
					fInfo->clip_list[i]);
			if (fDrawingBits != NULL)
				_Paint(fDrawingBits, DRAWING_BPR(fInfo), fInfo->window_bounds,
					fInfo->clip_list[i]);
		}
	}

	void* _Map(area_id source, area_id& mapped, area_id& clone, void*& bits)
	{
		if (source < 0)
			return NULL;
		if (source != mapped) {
			if (clone >= 0)
				delete_area(clone);
			clone = clone_area("directscale buffer", &bits, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, source);
			mapped = source;
			if (clone < 0) {
				printf("  cannot clone area %" B_PRId32 ": %s\n", source,
					strerror(clone));
				bits = NULL;
			}
		}
		return clone >= 0 ? bits : NULL;
	}

	static void _Paint(uint8* bits, int32 bytesPerRow,
		const clipping_rect& window, const clipping_rect& clip)
	{
		int32 width = window.right - window.left + 1;
		int32 height = window.bottom - window.top + 1;
		for (int32 y = clip.top; y <= clip.bottom; y++) {
			uint32* row = (uint32*)(bits + (size_t)y * bytesPerRow);
			int32 wy = y - window.top;
			for (int32 x = clip.left; x <= clip.right; x++) {
				int32 wx = x - window.left;
				uint32 color;
				if (wx == 0 || wy == 0 || wx == width - 1 || wy == height - 1)
					color = 0xff000000;
				else if (wx % 100 < 4)
					color = 0xffffffff;
				else {
					uint8 r = wx < width / 2 ? 230 : 30;
					uint8 g = wy < height / 2 ? 200 : 60;
					uint8 b = (wx < width / 2) == (wy < height / 2) ? 40 : 220;
					color = 0xff000000 | (r << 16) | (g << 8) | b;
				}
				row[x] = color;
			}
		}
	}

	BLocker		fLock;
	bool		fConnected = false;
	direct_buffer_info* fInfo = NULL;
	uint8*		fFrontBits = NULL;
	uint8*		fDrawingBits = NULL;
	void*		fFront;
	area_id		fFrontArea;
	area_id		fFrontSource;
	void*		fDrawing;
	area_id		fDrawingArea;
	area_id		fDrawingSource;
	int32		fConnections;
};


int
main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 6;
	BApplication app("application/x-vnd.x399-directscale");
	printf("supports window mode: %d\n",
		BDirectWindow::SupportsWindowMode() ? 1 : 0);
	TestWindow* window = new TestWindow(BRect(200, 200, 599, 449));
	window->Show();
	for (int i = 0; i < seconds * 20; i++) {
		if (i == seconds * 10 && window->Lock()) {
			window->MoveBy(120, 60);
			window->Unlock();
		}
		window->Repaint();
		snooze(50000);
	}
	if (window->Lock())
		window->Quit();
	return 0;
}
