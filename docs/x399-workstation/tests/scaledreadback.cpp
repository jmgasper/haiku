/*
 * scaledreadback: the averaging screenshot of a desktop drawn at a higher
 * density (app_server's DrawingEngine::ReadBitmap), new against old.
 *
 * Compiles app_server's ScaledReadback.cpp as it is, and next to it the
 * copying version ReadBitmap used before (the frame buffer rectangle copied
 * into a BBitmap, the cursor blended into the copy, then each logical pixel
 * averaged with floorf() per pixel, then copied again). Both read a
 * 7680x2160 buffer of random pixels (the X399's drawing buffer at 200%) for
 * many rectangles, with and without a cursor, at 2x and 1.5x; every output
 * byte has to match. Then both are timed for the whole screen and for a
 * band of 128 rows.
 *
 *   g++ -O2 -Isrc/servers/app/drawing scaledreadback.cpp \
 *       src/servers/app/drawing/ScaledReadback.cpp -lbe -o scaledreadback
 *
 * SPDX-License-Identifier: MIT
 */

#include <Bitmap.h>
#include <OS.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "ScaledReadback.h"


static const int32 kBufferWidth = 7680;
static const int32 kBufferHeight = 2160;


struct Cursor {
	std::vector<uint8> bits;
	int32 width, height;
	int32 left, top;	// frame buffer position of its top left
};


// The old ReadBitmap at a scale other than 1, minus the server objects.
static void
old_readback(const uint8* buffer, uint32 bufferBPR, int32 bufferWidth,
	int32 bufferHeight, float scale, BRect bounds, const Cursor* cursor,
	std::vector<uint8>& out, int32& outWidth, int32& outHeight)
{
	BRect logicalClip(0, 0, roundf(bufferWidth / scale) - 1,
		roundf(bufferHeight / scale) - 1);
	bounds = bounds & logicalClip;
	BRect deviceBounds(floorf(bounds.left * scale), floorf(bounds.top * scale),
		floorf((bounds.right + 1) * scale) - 1,
		floorf((bounds.bottom + 1) * scale) - 1);
	deviceBounds = deviceBounds & BRect(0, 0, bufferWidth - 1, bufferHeight - 1);

	int32 width = bounds.IntegerWidth() + 1;
	int32 height = bounds.IntegerHeight() + 1;
	int32 deviceWidth = deviceBounds.IntegerWidth() + 1;
	int32 deviceHeight = deviceBounds.IntegerHeight() + 1;
	BBitmap device(BRect(0, 0, deviceWidth - 1, deviceHeight - 1),
		B_BITMAP_NO_SERVER_LINK, B_RGB32);
	device.ImportBits(buffer, bufferBPR * bufferHeight, bufferBPR, B_RGB32,
		deviceBounds.LeftTop(), BPoint(0, 0),
		BSize(deviceWidth - 1, deviceHeight - 1));

	if (cursor != NULL) {
		// ReadBitmap: floorf(position * scale) - deviceBounds.left - hotspot;
		// the cursor's left/top here are that before the deviceBounds part.
		BPoint position(cursor->left - deviceBounds.left,
			cursor->top - deviceBounds.top);
		uint8* bits = (uint8*)device.Bits();
		uint32 bpr = device.BytesPerRow();
		const uint8* cursorBits = cursor->bits.data();
		for (int32 y = 0; y < cursor->height; y++) {
			int32 dy = (int32)position.y + y;
			for (int32 x = 0; x < cursor->width; x++, cursorBits += 4) {
				int32 dx = (int32)position.x + x;
				if (dx < 0 || dy < 0 || dx >= deviceWidth || dy >= deviceHeight)
					continue;
				uint8* d = bits + dy * bpr + dx * 4;
				uint8 alpha = 255 - cursorBits[3];
				d[0] = ((d[0] * alpha) >> 8) + cursorBits[0];
				d[1] = ((d[1] * alpha) >> 8) + cursorBits[1];
				d[2] = ((d[2] * alpha) >> 8) + cursorBits[2];
			}
		}
	}

	BBitmap logical(BRect(0, 0, width - 1, height - 1),
		B_BITMAP_NO_SERVER_LINK, B_RGB32);
	const uint8* src = (const uint8*)device.Bits();
	uint32 srcBPR = device.BytesPerRow();
	uint8* dst = (uint8*)logical.Bits();
	uint32 dstBPR = logical.BytesPerRow();
	for (int32 y = 0; y < height; y++) {
		int32 y0 = (int32)floorf((bounds.top + y) * scale)
			- (int32)deviceBounds.top;
		int32 y1 = (int32)floorf((bounds.top + y + 1) * scale) - 1
			- (int32)deviceBounds.top;
		y0 = max_c(0, y0);
		y1 = min_c(deviceHeight - 1, max_c(y0, y1));
		uint8* d = dst + y * dstBPR;
		for (int32 x = 0; x < width; x++, d += 4) {
			int32 x0 = (int32)floorf((bounds.left + x) * scale)
				- (int32)deviceBounds.left;
			int32 x1 = (int32)floorf((bounds.left + x + 1) * scale) - 1
				- (int32)deviceBounds.left;
			x0 = max_c(0, x0);
			x1 = min_c(deviceWidth - 1, max_c(x0, x1));
			uint32 sum[3] = {0, 0, 0};
			uint32 samples = 0;
			for (int32 sy = y0; sy <= y1; sy++) {
				const uint8* s = src + sy * srcBPR + x0 * 4;
				for (int32 sx = x0; sx <= x1; sx++, s += 4) {
					sum[0] += s[0];
					sum[1] += s[1];
					sum[2] += s[2];
					samples++;
				}
			}
			if (samples == 0)
				samples = 1;
			d[0] = sum[0] / samples;
			d[1] = sum[1] / samples;
			d[2] = sum[2] / samples;
			d[3] = 255;
		}
	}

	// The final ImportBits() into the caller's bitmap.
	out.resize((size_t)width * height * 4);
	for (int32 y = 0; y < height; y++)
		memcpy(out.data() + (size_t)y * width * 4, dst + y * dstBPR, width * 4);
	outWidth = width;
	outHeight = height;
}


// The new ReadBitmap, with the same clipping.
static void
new_readback(const uint8* buffer, uint32 bufferBPR, int32 bufferWidth,
	int32 bufferHeight, float scale, BRect bounds, const Cursor* cursor,
	std::vector<uint8>& out, int32& outWidth, int32& outHeight)
{
	BRect logicalClip(0, 0, roundf(bufferWidth / scale) - 1,
		roundf(bufferHeight / scale) - 1);
	bounds = bounds & logicalClip;
	BRect deviceBounds(floorf(bounds.left * scale), floorf(bounds.top * scale),
		floorf((bounds.right + 1) * scale) - 1,
		floorf((bounds.bottom + 1) * scale) - 1);
	deviceBounds = deviceBounds & BRect(0, 0, bufferWidth - 1, bufferHeight - 1);
	int32 width = bounds.IntegerWidth() + 1;
	int32 height = bounds.IntegerHeight() + 1;

	ScaledReadbackPixels source = { buffer, bufferBPR };
	ScaledReadbackCursor info;
	if (cursor != NULL) {
		info.bits = cursor->bits.data();
		info.width = cursor->width;
		info.height = cursor->height;
		info.left = cursor->left;
		info.top = cursor->top;
	}
	out.assign((size_t)width * height * 4, 0x5a);
	status_t status = scaled_readback(source, scale, bounds.left, bounds.top,
		width, height, (int32)deviceBounds.left, (int32)deviceBounds.top,
		(int32)deviceBounds.right, (int32)deviceBounds.bottom,
		cursor != NULL ? &info : NULL, out.data(), width * 4);
	if (status != B_OK)
		printf("scaled_readback: %s\n", strerror(status));
	outWidth = width;
	outHeight = height;
}


static Cursor
make_cursor(int32 width, int32 height, int32 left, int32 top)
{
	Cursor cursor;
	cursor.width = width;
	cursor.height = height;
	cursor.left = left;
	cursor.top = top;
	cursor.bits.resize(width * height * 4);
	for (int32 i = 0; i < width * height; i++) {
		// premultiplied: colour <= alpha; a third clear, a third opaque
		uint8 alpha = i % 3 == 0 ? 0 : i % 3 == 1 ? 255 : rand() % 256;
		cursor.bits[i * 4 + 0] = alpha ? rand() % (alpha + 1) : 0;
		cursor.bits[i * 4 + 1] = alpha ? rand() % (alpha + 1) : 0;
		cursor.bits[i * 4 + 2] = alpha ? rand() % (alpha + 1) : 0;
		cursor.bits[i * 4 + 3] = alpha;
	}
	return cursor;
}


int
main()
{
	uint32 bpr = kBufferWidth * 4;
	std::vector<uint8> buffer((size_t)bpr * kBufferHeight);
	srand(1);
	for (size_t i = 0; i < buffer.size(); i++)
		buffer[i] = rand();

	struct Case {
		float scale;
		BRect bounds;
		bool cursor;
		int32 cursorX, cursorY;	// logical position, hot spot at 3,2
	};
	std::vector<Case> cases;
	BRect rects[] = {
		BRect(0, 0, 3839, 1079),		// the whole screen
		BRect(0, 0, 3839, 127),			// a band
		BRect(0, 1024, 3839, 1079),		// the last band
		BRect(17, 33, 1056, 792),		// a window
		BRect(1, 1, 2, 2),
		BRect(3830, 1070, 3900, 1100),	// past the edge
		BRect(-10, -10, 20, 20),		// before it
		BRect(0, 0, 0, 0),
	};
	for (float scale : { 2.0f, 1.5f }) {
		for (const BRect& rect : rects) {
			cases.push_back({ scale, rect, false, 0, 0 });
			cases.push_back({ scale, rect, true, (int32)rect.left + 5,
				(int32)rect.top + 7 });
			cases.push_back({ scale, rect, true, (int32)rect.left - 4,
				(int32)rect.top - 3 });
			cases.push_back({ scale, rect, true, (int32)rect.right - 2,
				(int32)rect.bottom });
		}
	}

	int failures = 0;
	for (const Case& test : cases) {
		Cursor cursor = make_cursor(22, 22,
			(int32)(floorf(test.cursorX * test.scale) - 3),
			(int32)(floorf(test.cursorY * test.scale) - 2));
		int32 bufferWidth = (int32)(kBufferWidth * test.scale / 2);
		int32 bufferHeight = (int32)(kBufferHeight * test.scale / 2);
		std::vector<uint8> expected, actual;
		int32 ew, eh, aw, ah;
		old_readback(buffer.data(), bpr, bufferWidth, bufferHeight, test.scale,
			test.bounds, test.cursor ? &cursor : NULL, expected, ew, eh);
		new_readback(buffer.data(), bpr, bufferWidth, bufferHeight, test.scale,
			test.bounds, test.cursor ? &cursor : NULL, actual, aw, ah);
		size_t differ = 0;
		size_t firstDiffer = 0;
		if (ew != aw || eh != ah)
			differ = (size_t)-1;
		else {
			for (size_t i = 0; i < expected.size(); i++) {
				if (expected[i] != actual[i] && differ++ == 0)
					firstDiffer = i;
			}
		}
		if (differ != 0) {
			failures++;
			printf("FAIL scale %.1f rect %.0f,%.0f-%.0f,%.0f cursor %d: %zu bytes"
				" differ (first at %zu, %dx%d vs %dx%d)\n", test.scale,
				test.bounds.left, test.bounds.top, test.bounds.right,
				test.bounds.bottom, test.cursor, differ, firstDiffer, ew, eh, aw,
				ah);
		}
	}
	printf("%zu cases, %d failed\n", cases.size(), failures);

	Cursor cursor = make_cursor(22, 22, 2000, 1000);
	for (BRect rect : { BRect(0, 0, 3839, 1079), BRect(0, 512, 3839, 639) }) {
		std::vector<uint8> out;
		int32 w, h;
		for (int pass = 0; pass < 2; pass++) {
			bigtime_t best = B_INFINITE_TIMEOUT;
			for (int i = 0; i < 5; i++) {
				bigtime_t start = system_time();
				if (pass == 0) {
					old_readback(buffer.data(), bpr, kBufferWidth, kBufferHeight,
						2, rect, &cursor, out, w, h);
				} else {
					new_readback(buffer.data(), bpr, kBufferWidth, kBufferHeight,
						2, rect, &cursor, out, w, h);
				}
				best = min_c(best, system_time() - start);
			}
			printf("%s %dx%d at 2x: %.1f ms\n", pass == 0 ? "old" : "new", w, h,
				best / 1000.0);
		}
	}
	return failures != 0;
}
