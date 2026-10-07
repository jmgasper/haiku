/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Independent per-sample reference for the HEVC block's column layout.
// Runs on the host (scalar path) and ARM64 (NEON path).

#include "SandConvert.h"

#include <stdio.h>
#include <string.h>
#include <vector>


static uint32_t
random_value()
{
	static uint32_t value = 42;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	return value;
}


static uint16_t
sample(const SandPicture& picture, bool tenBit, bool chroma,
	uint32_t x, uint32_t y)
{
	uint32_t columns = tenBit ? 96 : 128;
	const uint8_t* row = picture.data + (size_t)(x / columns)
		* picture.columnStride + (size_t)y * 128;
	if (chroma)
		row += picture.chromaOffset;
	if (!tenBit)
		return row[x % columns];
	const uint8_t* packed = row + (x % columns / 3) * 4;
	uint32_t word = packed[0] | (uint32_t)packed[1] << 8
		| (uint32_t)packed[2] << 16 | (uint32_t)packed[3] << 24;
	return ((word >> (10 * (x % columns % 3))) & 1023) << 6;
}


static void
put_sample(uint8_t* target, bool tenBit, uint16_t value)
{
	*target = value;
	if (tenBit)
		target[1] = value >> 8;
}


int
main()
{
	unsigned checks = 0;
	for (unsigned test = 0; test < 502; test++) {
		SandPicture picture;
		picture.width = random_value() % 2050;
		picture.height = random_value() % 130;
		picture.left = random_value() % 280;
		picture.top = random_value() % 40;
		if (test >= 500) {
			picture.width = test == 500 ? 1920 : 3840;
			picture.height = picture.width * 9 / 16;
			picture.left = picture.top = 0;
		}
		uint32_t rows = (picture.height + picture.top + 63) & ~63u;
		picture.chromaOffset = rows * 128;
		picture.columnStride = (rows * 3 / 2 + 32) * 128;
		std::vector<uint8_t> input((size_t)picture.columnStride
			* ((picture.width + picture.left + 96) / 96) + 32);
		for (size_t i = 0; i < input.size(); i++)
			input[i] = random_value();
		const std::vector<uint8_t> sourceCopy = input;
		picture.data = input.data() + test % 16;
		for (unsigned format = 0; format < 3; format++) {
			bool tenBit = format == 2;
			uint32_t bytes = tenBit ? 2 : 1;
			uint32_t pairs = (picture.width + 1) / 2;
			uint32_t chromaRows = (picture.height + 1) / 2;
			uint32_t lumaStride = ((picture.width + 1) & ~1u) * bytes
				+ 2 * (random_value() % 33);
			uint32_t chromaStride = (format == 0 ? pairs : pairs * 2 * bytes)
				+ 2 * (random_value() % 17);
			size_t lumaBytes = (size_t)lumaStride * picture.height;
			size_t chromaBytes = (size_t)chromaStride * chromaRows;
			size_t offset = 16 + 2 * (test % 16);
			std::vector<uint8_t> output(offset + lumaBytes
				+ chromaBytes * (format == 0 ? 2 : 1) + 32, 0xa7);
			std::vector<uint8_t> expected = output;
			uint8_t* luma = expected.data() + offset;
			uint8_t* chroma = luma + lumaBytes;
			for (uint32_t y = 0; y < picture.height; y++) {
				for (uint32_t x = 0; x < picture.width; x++) {
					put_sample(luma + (size_t)y * lumaStride + x * bytes,
						tenBit, sample(picture, tenBit, false,
							picture.left + x, picture.top + y));
				}
			}
			for (uint32_t y = 0; y < chromaRows; y++) {
				for (uint32_t x = 0; x < pairs; x++) {
					for (uint32_t component = 0; component < 2; component++) {
						uint8_t* target = chroma + (size_t)y * chromaStride;
						if (format == 0)
							target += x + component * chromaBytes;
						else
							target += (x * 2 + component) * bytes;
						put_sample(target, tenBit, sample(picture, tenBit, true,
							(picture.left / 2 + x) * 2 + component,
							picture.top / 2 + y));
					}
				}
			}
			luma = output.data() + offset;
			chroma = luma + lumaBytes;
			if (format == 0) {
				sand8_to_i420(picture, luma, lumaStride, chroma,
					chroma + chromaBytes, chromaStride);
			} else if (format == 1)
				sand8_to_nv12(picture, luma, lumaStride, chroma, chromaStride);
			else
				sand30_to_p010(picture, luma, lumaStride, chroma, chromaStride);
			if (output != expected || input != sourceCopy) {
				fprintf(stderr, "FAIL format=%u size=%ux%u crop=%u,%u\n",
					format, picture.width, picture.height,
					picture.left, picture.top);
				return 1;
			}
			checks++;
		}
	}
	printf("PASS %u SAND conversions: pixels, crops, strides, canaries, source\n",
		checks);
	return 0;
}
