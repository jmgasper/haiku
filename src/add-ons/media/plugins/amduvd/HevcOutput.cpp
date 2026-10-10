/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "HevcOutput.h"
#include <string.h>

namespace {
int Sample(const uint8_t* row, int x, bool p010, unsigned bits)
{
	return p010 ? (unsigned(row[2 * x]) | unsigned(row[2 * x + 1]) << 8) >> (16 - bits) : row[x];
}
uint8_t Eight(int sample, unsigned bits)
{
	if (bits == 10) sample = (sample + 2) >> 2;
	return sample > 255 ? 255 : sample;
}
uint8_t Clip(int value)
{
	value = (value + 32768) >> 16;
	return value < 0 ? 0 : value > 255 ? 255 : value;
}
}

size_t HevcOutput::Bytes(int width, int height, Format format)
{
	if (format < NV12 || format > P010) return 0;
	size_t bytes = H264Output::Bytes(width, height, H264Output::NV12);
	if (format == P010) return bytes * 2;
	return H264Output::Bytes(width, height, static_cast<H264Output::Format>(format));
}

bool HevcOutput::Copy(const HevcFrame& f, Format format, uint8_t* output, size_t capacity)
{
	size_t bytes = Bytes(f.width, f.height, format);
	unsigned sampleBytes = f.p010 ? 2 : 1;
	if (!output || !bytes || capacity < bytes || (f.bitDepth != 8 && f.bitDepth != 10)
		|| (!f.p010 && f.bitDepth != 8) || f.cropLeft < 0 || f.cropTop < 0
		|| (f.cropLeft & 1) || (f.cropTop & 1)
		|| (uint64_t(f.cropLeft) + f.width) * sampleBytes > f.pitch
		|| uint64_t(f.cropTop) + f.height > f.codedHeight || (f.codedHeight & 1)
		|| f.codedHeight > 4096 || f.pitch > 4096 * sampleBytes || f.pitch % (2 * sampleBytes)
		|| f.pixels.size() != size_t(f.pitch) * f.codedHeight * 3 / 2) return false;
	if (!f.p010 && format != P010)
		return H264Output::Copy(f, static_cast<H264Output::Format>(format), output);
	const uint8_t* luma = f.pixels.data() + size_t(f.cropTop) * f.pitch + f.cropLeft * sampleBytes;
	const uint8_t* chroma = f.pixels.data() + size_t(f.codedHeight) * f.pitch
		+ size_t(f.cropTop / 2) * f.pitch + f.cropLeft * sampleBytes;
	if (format == P010) {
		for (unsigned plane = 0; plane < 2; plane++) {
			const uint8_t* source = plane ? chroma : luma;
			uint8_t* target = output + (plane ? size_t(f.width) * f.height * 2 : 0);
			int rows = plane ? f.height / 2 : f.height;
			for (int y = 0; y < rows; y++) {
				const uint8_t* row = source + size_t(y) * f.pitch;
				uint8_t* out = target + size_t(y) * f.width * 2;
				if (f.p010) memcpy(out, row, f.width * 2);
				else for (int x = 0; x < f.width; x++) { out[2 * x] = 0; out[2 * x + 1] = row[x]; }
			}
		}
		return true;
	}
	if (format == NV12 || format == I420) {
		for (int y = 0; y < f.height; y++) for (int x = 0; x < f.width; x++)
			output[size_t(y) * f.width + x] = Eight(Sample(luma + size_t(y) * f.pitch, x, true, f.bitDepth), f.bitDepth);
		uint8_t* uv = output + size_t(f.width) * f.height;
		for (int y = 0; y < f.height / 2; y++) for (int x = 0; x < f.width; x++) {
			size_t index = format == NV12 ? size_t(y) * f.width + x
				: size_t(x & 1) * f.width * f.height / 4 + size_t(y) * (f.width / 2) + x / 2;
			uv[index] = Eight(Sample(chroma + size_t(y) * f.pitch, x, true, f.bitDepth), f.bitDepth);
		}
		return true;
	}
	if (format == RGB32 && f.matrix != 1 && f.matrix != 2 && f.matrix != 5 && f.matrix != 6)
		return false;
	bool bt709 = f.matrix == 1 || (f.matrix == 2 && f.height > 576);
	// 16.16 coefficients derived from Kr/Kb and the 8/10-bit code ranges.
	// Full-range ten-bit uses 1023 levels, rather than dividing 8-bit coefficients by four.
	int yc, rv, bu, gu, gv;
	if (f.bitDepth == 10) {
		yc = f.fullRange ? 16336 : 19077;
		rv = bt709 ? (f.fullRange ? 25726 : 29372) : (f.fullRange ? 22903 : 26149);
		bu = bt709 ? (f.fullRange ? 30313 : 34610) : (f.fullRange ? 28947 : 33050);
		gu = bt709 ? (f.fullRange ? 3060 : 3494) : (f.fullRange ? 5622 : 6419);
		gv = bt709 ? (f.fullRange ? 7647 : 8731) : (f.fullRange ? 11666 : 13320);
	} else {
		yc = f.fullRange ? 65536 : 76309;
		rv = bt709 ? (f.fullRange ? 103206 : 117489) : (f.fullRange ? 91881 : 104597);
		bu = bt709 ? (f.fullRange ? 121609 : 138438) : (f.fullRange ? 116130 : 132201);
		gu = bt709 ? (f.fullRange ? 12276 : 13975) : (f.fullRange ? 22554 : 25675);
		gv = bt709 ? (f.fullRange ? 30679 : 34925) : (f.fullRange ? 46802 : 53279);
	}
	int center = 1 << (f.bitDepth - 1), offset = f.fullRange ? 0 : 16 << (f.bitDepth - 8);
	for (int y = 0; y < f.height; y++) {
		const uint8_t* yy = luma + size_t(y) * f.pitch;
		const uint8_t* uv = chroma + size_t(y / 2) * f.pitch;
		uint8_t* out = output + size_t(y) * f.width * (format == RGB32 ? 4 : 2);
		for (int x = 0; x < f.width; x += 2) {
			int u = Sample(uv, x, true, f.bitDepth), v = Sample(uv, x + 1, true, f.bitDepth);
			if (format == YCbCr422) {
				out[2 * x] = Eight(Sample(yy, x, true, f.bitDepth), f.bitDepth);
				out[2 * x + 1] = Eight(u, f.bitDepth);
				out[2 * x + 2] = Eight(Sample(yy, x + 1, true, f.bitDepth), f.bitDepth);
				out[2 * x + 3] = Eight(v, f.bitDepth);
			} else for (int i = 0; i < 2; i++) {
				int l = (Sample(yy, x + i, true, f.bitDepth) - offset) * yc;
				uint8_t* pixel = out + 4 * (x + i);
				pixel[0] = Clip(l + bu * (u - center));
				pixel[1] = Clip(l - gu * (u - center) - gv * (v - center));
				pixel[2] = Clip(l + rv * (v - center)); pixel[3] = 255;
			}
		}
	}
	return true;
}
