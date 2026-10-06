/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "SandConvert.h"

#include <algorithm>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif


static const uint32_t kColumnBytes = 128;
static const uint32_t kColumnSamples30 = 96;
// Keep both the column input and the row output local while converting.
static const uint32_t kTileRows = 8;


/*!	Split a row of interleaved chroma pairs into two rows. */
static void
split_row8(const uint8_t* rows, uint32_t columnStride, uint32_t firstPair,
	uint32_t pairs, uint8_t* cb, uint8_t* cr)
{
	while (pairs > 0) {
		uint32_t column = firstPair / (kColumnBytes / 2);
		uint32_t offset = firstPair % (kColumnBytes / 2);
		uint32_t run = std::min(kColumnBytes / 2 - offset, pairs);
		const uint8_t* in = rows + (size_t)column * columnStride + 2 * offset;

		uint32_t i = 0;
#if defined(__aarch64__)
		for (; i + 16 <= run; i += 16) {
			uint8x16x2_t both = vld2q_u8(in + 2 * i);
			vst1q_u8(cb + i, both.val[0]);
			vst1q_u8(cr + i, both.val[1]);
		}
#endif
		for (; i < run; i++) {
			cb[i] = in[2 * i];
			cr[i] = in[2 * i + 1];
		}

		cb += run;
		cr += run;
		firstPair += run;
		pairs -= run;
	}
}


static void
copy_plane8(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* out, uint32_t outStride)
{
	for (uint32_t row = 0; row < height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, height - row);
		for (uint32_t x = 0; x < width;) {
			uint32_t position = first + x;
			uint32_t run = std::min(kColumnBytes - position % kColumnBytes,
				width - x);
			const uint8_t* column = in
				+ (size_t)(position / kColumnBytes) * columnStride
				+ position % kColumnBytes + (size_t)row * kColumnBytes;
			uint8_t* target = out + (size_t)row * outStride + x;
			for (uint32_t y = 0; y < rows; y++) {
				memcpy(target + (size_t)y * outStride,
					column + y * kColumnBytes, run);
			}
			x += run;
		}
	}
}


static void
split_plane8(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* cb, uint8_t* cr, uint32_t outStride)
{
	for (uint32_t row = 0; row < height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, height - row);
		for (uint32_t x = 0; x < width;) {
			uint32_t run = std::min(
				kColumnBytes / 2 - (first + x) % (kColumnBytes / 2), width - x);
			for (uint32_t y = 0; y < rows; y++) {
				split_row8(in + (size_t)(row + y) * kColumnBytes, columnStride,
					first + x, run, cb + (size_t)(row + y) * outStride + x,
					cr + (size_t)(row + y) * outStride + x);
			}
			x += run;
		}
	}
}


void
sand8_to_i420(const SandPicture& picture, uint8_t* luma, uint32_t lumaStride,
	uint8_t* cb, uint8_t* cr, uint32_t chromaStride)
{
	copy_plane8(picture.data + (size_t)picture.top * kColumnBytes,
		picture.columnStride, picture.left, picture.width, picture.height,
		luma, lumaStride);
	split_plane8(picture.data + picture.chromaOffset
			+ (size_t)(picture.top / 2) * kColumnBytes,
		picture.columnStride, picture.left / 2, (picture.width + 1) / 2,
		(picture.height + 1) / 2, cb, cr, chromaStride);
}


void
sand8_to_nv12(const SandPicture& picture, uint8_t* luma, uint32_t lumaStride,
	uint8_t* chroma, uint32_t chromaStride)
{
	copy_plane8(picture.data + (size_t)picture.top * kColumnBytes,
		picture.columnStride, picture.left, picture.width, picture.height,
		luma, lumaStride);
	copy_plane8(picture.data + picture.chromaOffset
			+ (size_t)(picture.top / 2) * kColumnBytes,
		picture.columnStride, 2 * (picture.left / 2), 2 * ((picture.width + 1) / 2),
		(picture.height + 1) / 2, chroma, chromaStride);
}


/*!	\a words words of three ten bit samples each to sixteen bit samples. */
static void
unpack30(const uint8_t* in, uint32_t words, uint16_t* out)
{
	uint32_t i = 0;
#if defined(__aarch64__)
	const uint32x4_t mask = vdupq_n_u32(0x3ff);
	for (; i + 8 <= words; i += 8, out += 24) {
		uint32x4_t low = vreinterpretq_u32_u8(vld1q_u8(in + 4 * i));
		uint32x4_t high = vreinterpretq_u32_u8(vld1q_u8(in + 4 * i + 16));
		uint16x8x3_t samples;
		samples.val[0] = vshlq_n_u16(vcombine_u16(
			vmovn_u32(vandq_u32(low, mask)),
			vmovn_u32(vandq_u32(high, mask))), 6);
		samples.val[1] = vshlq_n_u16(vcombine_u16(
			vmovn_u32(vandq_u32(vshrq_n_u32(low, 10), mask)),
			vmovn_u32(vandq_u32(vshrq_n_u32(high, 10), mask))), 6);
		samples.val[2] = vshlq_n_u16(vcombine_u16(
			vmovn_u32(vandq_u32(vshrq_n_u32(low, 20), mask)),
			vmovn_u32(vandq_u32(vshrq_n_u32(high, 20), mask))), 6);
		vst3q_u16(out, samples);
	}
#endif
	for (; i < words; i++, out += 3) {
		uint32_t word;
		memcpy(&word, in + 4 * i, 4);
		out[0] = (uint16_t)((word & 0x3ff) << 6);
		out[1] = (uint16_t)(((word >> 10) & 0x3ff) << 6);
		out[2] = (uint16_t)(((word >> 20) & 0x3ff) << 6);
	}
}


/*!	\a count samples of a ten bit row from its sample \a first on. */
static void
unpack_row30(const uint8_t* rows, uint32_t columnStride, uint32_t first,
	uint32_t count, uint16_t* out)
{
	while (count > 0) {
		uint32_t column = first / kColumnSamples30;
		uint32_t offset = first % kColumnSamples30;
		uint32_t run = std::min(kColumnSamples30 - offset, count);
		const uint8_t* in = rows + (size_t)column * columnStride;

		if (offset == 0 && run == kColumnSamples30)
			unpack30(in, kColumnSamples30 / 3, out);
		else {
			uint16_t samples[kColumnSamples30];
			unpack30(in, kColumnSamples30 / 3, samples);
			memcpy(out, samples + offset, run * sizeof(uint16_t));
		}

		out += run;
		first += run;
		count -= run;
	}
}


static void
unpack_plane30(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* out, uint32_t outStride)
{
	for (uint32_t row = 0; row < height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, height - row);
		for (uint32_t x = 0; x < width;) {
			uint32_t run = std::min(
				kColumnSamples30 - (first + x) % kColumnSamples30, width - x);
			for (uint32_t y = 0; y < rows; y++) {
				unpack_row30(in + (size_t)(row + y) * kColumnBytes, columnStride,
					first + x, run,
					(uint16_t*)(out + (size_t)(row + y) * outStride) + x);
			}
			x += run;
		}
	}
}


void
sand30_to_p010(const SandPicture& picture, uint8_t* luma, uint32_t lumaStride,
	uint8_t* chroma, uint32_t chromaStride)
{
	unpack_plane30(picture.data + (size_t)picture.top * kColumnBytes,
		picture.columnStride, picture.left, picture.width, picture.height,
		luma, lumaStride);
	unpack_plane30(picture.data + picture.chromaOffset
			+ (size_t)(picture.top / 2) * kColumnBytes,
		picture.columnStride, 2 * (picture.left / 2), 2 * ((picture.width + 1) / 2),
		(picture.height + 1) / 2, chroma, chromaStride);
}
