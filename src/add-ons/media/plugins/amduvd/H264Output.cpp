/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "H264Output.h"
#include <string.h>

size_t H264Output::Bytes(int width, int height, Format format)
{
	if (width <= 0 || width > 4096 || height <= 0 || height > 4096
		|| (width & 1) || (height & 1) || format < NV12 || format > RGB32) return 0;
	size_t area = (size_t)width * height;
	return format == RGB32 ? area * 4 : format == YCbCr422 ? area * 2 : area * 3 / 2;
}

static uint8_t Clip(int value)
{
	value = (value + 32768) >> 16;
	return value < 0 ? 0 : value > 255 ? 255 : value;
}

bool H264Output::Copy(const H264Frame& f, Format format, uint8_t* output)
{
	if (output == NULL || Bytes(f.width, f.height, format) == 0
		|| f.cropLeft < 0 || f.cropTop < 0 || (f.cropLeft & 1) || (f.cropTop & 1)
		|| (uint64_t)f.cropLeft + f.width > f.pitch
		|| (uint64_t)f.cropTop + f.height > f.codedHeight
		|| (f.codedHeight & 1) || f.codedHeight > 4096 || f.pitch > 4096
		|| f.pixels.size() != (size_t)f.pitch * f.codedHeight * 3 / 2) return false;
	const uint8_t* luma = f.pixels.data() + (size_t)f.cropTop * f.pitch + f.cropLeft;
	const uint8_t* chroma = f.pixels.data() + (size_t)f.codedHeight * f.pitch
		+ (size_t)(f.cropTop / 2) * f.pitch + f.cropLeft;
	if (format == NV12 || format == I420) {
		for (int y = 0; y < f.height; y++)
			memcpy(output + (size_t)y * f.width, luma + (size_t)y * f.pitch, f.width);
		uint8_t* u = output + (size_t)f.width * f.height;
		uint8_t* v = u + (size_t)f.width * f.height / 4;
		for (int y = 0; y < f.height / 2; y++) {
			const uint8_t* row = chroma + (size_t)y * f.pitch;
			if (format == NV12) memcpy(u + (size_t)y * f.width, row, f.width);
			else for (int x = 0; x < f.width / 2; x++) {
				u[(size_t)y * (f.width / 2) + x] = row[x * 2];
				v[(size_t)y * (f.width / 2) + x] = row[x * 2 + 1];
			}
		}
		return true;
	}
	// Only these matrices can be converted faithfully here. YUV outputs
	// retain their samples for a caller with its own colour-management path.
	bool bt709 = f.matrix == 1 || (f.matrix == 2 && f.height > 576);
	if (format == RGB32 && f.matrix != 1 && f.matrix != 2 && f.matrix != 5 && f.matrix != 6)
		return false;
	int yc = f.fullRange ? 65536 : 76309, offset = f.fullRange ? 0 : 16;
	int rv = bt709 ? (f.fullRange ? 103206 : 117489) : (f.fullRange ? 91881 : 104597);
	int bu = bt709 ? (f.fullRange ? 121609 : 138438) : (f.fullRange ? 116130 : 132201);
	int gu = bt709 ? (f.fullRange ? 12276 : 13975) : (f.fullRange ? 22554 : 25675);
	int gv = bt709 ? (f.fullRange ? 30679 : 34925) : (f.fullRange ? 46802 : 53279);
	for (int y = 0; y < f.height; y++) {
		const uint8_t* yy = luma + (size_t)y * f.pitch;
		const uint8_t* uv = chroma + (size_t)(y / 2) * f.pitch;
		uint8_t* row = output + (size_t)y * f.width * (format == RGB32 ? 4 : 2);
		for (int x = 0; x < f.width; x += 2) {
			if (format == YCbCr422) {
				row[2 * x] = yy[x]; row[2 * x + 1] = uv[x];
				row[2 * x + 2] = yy[x + 1]; row[2 * x + 3] = uv[x + 1];
			} else {
				int u = uv[x] - 128, v = uv[x + 1] - 128;
				for (int i = 0; i < 2; i++) {
					int l = (yy[x + i] - offset) * yc;
					uint8_t* pixel = row + 4 * (x + i);
					pixel[0] = Clip(l + bu * u); pixel[1] = Clip(l - gu * u - gv * v);
					pixel[2] = Clip(l + rv * v); pixel[3] = 255;
				}
			}
		}
	}
	return true;
}
