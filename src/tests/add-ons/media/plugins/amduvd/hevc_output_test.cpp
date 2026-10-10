/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "HevcOutput.h"
#include <cmath>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <utility>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

static unsigned Read(const HevcFrame& f, size_t sample)
{
	return (unsigned(f.pixels[sample * 2]) | unsigned(f.pixels[sample * 2 + 1]) << 8) >> (16 - f.bitDepth);
}
static int Clip(double value)
{
	return value <= 0 ? 0 : value >= 255 ? 255 : int(std::floor(value + 0.5));
}
static void Pixels()
{
	HevcFrame f = {};
	f.p010 = true; f.pitch = 32; f.codedHeight = 16;
	f.width = 8; f.height = 6; f.cropLeft = 2; f.cropTop = 4;
	f.pixels.resize(32 * 16 * 3 / 2);
	unsigned maxError = 0;
	for (unsigned bits : {8u, 10u}) for (bool full : {false, true}) {
		f.bitDepth = bits; f.fullRange = full;
		for (unsigned pattern = 0; pattern < 128; pattern++) {
			for (size_t i = 0; i < f.pixels.size() / 2; i++) {
				unsigned sample = (i * 127 + pattern * 13) & ((1u << bits) - 1);
				unsigned word = sample << (16 - bits);
				f.pixels[2 * i] = word; f.pixels[2 * i + 1] = word >> 8;
			}
			for (int matrix : {1, 5, 6}) for (auto format : {HevcOutput::P010,
				HevcOutput::NV12, HevcOutput::I420, HevcOutput::YCbCr422, HevcOutput::RGB32}) {
				f.matrix = matrix;
				size_t size = HevcOutput::Bytes(f.width, f.height, format);
				std::vector<uint8_t> out(size + 32, 0xa5);
				CHECK(!HevcOutput::Copy(f, format, out.data() + 16, size - 1));
				for (auto byte : out) CHECK(byte == 0xa5);
				CHECK(HevcOutput::Copy(f, format, out.data() + 16, size));
				for (unsigned i = 0; i < 16; i++) CHECK(out[i] == 0xa5 && out[16 + size + i] == 0xa5);
				const uint8_t* p = out.data() + 16;
				for (int y = 0; y < f.height; y++) for (int x = 0; x < f.width; x++) {
					unsigned yy = Read(f, (y + 4) * 16 + x + 2);
					unsigned uv = Read(f, 256 + (y / 2 + 2) * 16 + x + 2);
					auto down = [&](unsigned value) { return bits == 8 ? value : std::min(255u, (value + 2) / 4); };
					if (format == HevcOutput::P010) {
						unsigned luma = unsigned(p[2 * (y * 8 + x)]) | unsigned(p[2 * (y * 8 + x) + 1]) << 8;
						unsigned chroma = unsigned(p[96 + 2 * (y / 2 * 8 + x)]) | unsigned(p[97 + 2 * (y / 2 * 8 + x)]) << 8;
						CHECK(luma == yy << (16 - bits) && chroma == uv << (16 - bits));
					} else if (format == HevcOutput::YCbCr422) {
						CHECK(p[y * 16 + x * 2] == down(yy) && p[y * 16 + x * 2 + 1] == down(uv));
					} else if (format == HevcOutput::NV12 || format == HevcOutput::I420) {
						CHECK(p[y * 8 + x] == down(yy));
						size_t index = format == HevcOutput::NV12 ? y / 2 * 8 + x : (x & 1) * 12 + y / 2 * 4 + x / 2;
						CHECK(p[48 + index] == down(uv));
					} else {
						// Independent floating-point colour equations, not the implementation's integer constants.
						double kr = matrix == 1 ? .2126 : .299, kb = matrix == 1 ? .0722 : .114;
						double scale = 1u << (bits - 8), maximum = (1u << bits) - 1;
						double l = (yy - (full ? 0. : 16. * scale)) / (full ? maximum : 219. * scale);
						double u = (Read(f, 256 + (y / 2 + 2) * 16 + (x & ~1) + 2) - 128. * scale) / (full ? maximum : 224. * scale);
						double v = (Read(f, 256 + (y / 2 + 2) * 16 + (x & ~1) + 3) - 128. * scale) / (full ? maximum : 224. * scale);
						int expected[] = {Clip((l + (2 - 2 * kb) * u) * 255),
							Clip((l - 2 * kb * (1 - kb) / (1 - kr - kb) * u - 2 * kr * (1 - kr) / (1 - kr - kb) * v) * 255),
							Clip((l + (2 - 2 * kr) * v) * 255), 255};
						for (unsigned c = 0; c < 4; c++) {
							unsigned error = std::abs(int(p[4 * (y * 8 + x) + c]) - expected[c]);
							maxError = std::max(maxError, error); CHECK(error <= 1);
						}
					}
				}
			}
		}
	}
	std::vector<uint8_t> out(256, 0xa5);
	f.matrix = 9; CHECK(!HevcOutput::Copy(f, HevcOutput::RGB32, out.data(), out.size()));
	f.cropLeft = INT_MAX - 1; CHECK(!HevcOutput::Copy(f, HevcOutput::P010, out.data(), out.size()));
	for (auto byte : out) CHECK(byte == 0xa5);
	// Eight-bit NV12 storage can be preserved as P010 without dropping codes.
	f = {}; f.width = f.height = f.codedHeight = 2; f.pitch = 2;
	f.pixels = {0, 16, 235, 255, 128, 240}; f.p010 = false;
	CHECK(HevcOutput::Copy(f, HevcOutput::P010, out.data(), out.size()));
	for (size_t i = 0; i < f.pixels.size(); i++) CHECK(out[2 * i] == 0 && out[2 * i + 1] == f.pixels[i]);
	printf("PASS: cropped P010 exact, 8/10-bit YUV, 601/709 full/limited RGB max error %u, capacity and guards\n", maxError);
}

static void Queue()
{
	HevcOutput q;
	HevcFrame f = {}; f.sequence = 1; f.poc = 2; f.time = 0; f.p010 = true; f.bitDepth = 10;
	CHECK(q.Push(std::move(f), 1, false));
	f = {}; f.sequence = 1; f.poc = 0; f.time = -2000;
	CHECK(q.Push(std::move(f), 1, false));
	CHECK(q.Ready(false) && !q.Front().p010 && q.Front().time == -2000); q.Pop();
	CHECK(!q.Ready(false));
	f = {}; f.sequence = 2; f.poc = 0;
	CHECK(q.Push(std::move(f), 1, false));
	CHECK(q.Ready(false) && q.Front().p010 && q.Front().bitDepth == 10 && q.Front().time == 0); q.Pop();
	CHECK(q.Ready(true) && q.Front().sequence == 2); q.Pop();
	CHECK(!q.Ready(true));
	puts("PASS: mixed storage/depth and timestamps survive shared output ordering and epoch changes");
}

int main() { Pixels(); Queue(); }
