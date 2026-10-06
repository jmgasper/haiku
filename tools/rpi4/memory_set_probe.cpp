/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Check the actual memset entry point, including integer-to-byte conversion,
// arbitrary alignments, untouched neighbours, and inaccessible end pages.
#include <OS.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include <graphics/v3d/v3d_drm.h>
#include <graphics/v3d/v3d_haiku.h>

#ifndef SET_FUNCTION
#define SET_FUNCTION memset
#else
extern "C" void* SET_FUNCTION(void*, int, size_t);
#endif
using Set = void* (*)(void*, int, size_t);
static Set volatile sSet = SET_FUNCTION;
static size_t sCases;

static uint8_t pattern(size_t index)
{
	return (index * 37 + 53) ^ (index >> 8);
}

static void check(uint8_t* buffer, size_t capacity, size_t start,
	size_t count, int value)
{
	if (start > capacity || count > capacity - start)
		abort();
	for (size_t i = 0; i < capacity; i++)
		buffer[i] = pattern(i);
	if (sSet(buffer + start, value, count) != buffer + start)
		abort();
	for (size_t i = 0; i < capacity; i++) {
		uint8_t expected = i >= start && i - start < count
			? uint8_t(value) : pattern(i);
		if (buffer[i] != expected) {
			fprintf(stderr, "FAIL start=%zu count=%zu value=%x byte=%zu\n",
				start, count, value, i);
			exit(1);
		}
	}
	sCases++;
}

static void benchmark()
{
	const size_t sizes[] = {8, 32, 128, 256, 1024, 4096, 65536, 1048576,
		8388608};
	std::vector<uint8_t> buffer(8388608 + 256);
	uint8_t* aligned = (uint8_t*)((uintptr_t(buffer.data()) + 63) & ~uintptr_t(63));
	for (int round = 0; round < 4; round++) {
		for (size_t length : sizes) {
			for (int value : {0, 0x5a}) {
				for (int offset : {0, 3}) {
					Set function = round == 0 || round == 3 ? memset : sSet;
					size_t batch = std::min(size_t(4096),
						std::max(size_t(1), size_t(1048576) / length));
					size_t calls = 0;
					bigtime_t start = system_time(), end;
					do {
						for (size_t i = 0; i < batch; i++) {
							function(aligned + offset, value, length);
							calls++;
						}
						end = system_time();
					} while (end - start < 60000);
					for (size_t i = 0; i < length; i++) {
						if (aligned[offset + i] != value)
							abort();
					}
					printf("round=%d provider=%s bytes=%zu value=%d offset=%d ns=%.1f MBps=%.1f\n",
						round, round == 0 || round == 3 ? "installed" : "candidate",
						length, value, offset, double(end - start) * 1000 / calls,
						double(length) * calls / (end - start));
				}
			}
		}
	}
}

static int graphicsBuffers()
{
	int device = open(V3D_HAIKU_DEVICE_PATH, O_RDWR);
	if (device < 0)
		return 1;
	for (uint32 flags : {uint32(0), uint32(V3D_HAIKU_BO_CACHEABLE)}) {
		drm_v3d_create_bo create = {};
		create.size = 16384;
		create.flags = flags;
		if (ioctl(device, V3D_HAIKU_CREATE_BO, &create, sizeof(create)) != 0)
			return 1;
		drm_v3d_mmap_bo map = {};
		map.handle = create.handle;
		if (ioctl(device, V3D_HAIKU_MMAP_BO, &map, sizeof(map)) != 0)
			return 1;
		if (flags != 0) {
			v3d_haiku_handle prepare = {create.handle, 0};
			if (ioctl(device, V3D_HAIKU_CPU_PREPARE, &prepare, sizeof(prepare)) != 0)
				return 1;
		}
		for (size_t n : {0, 1, 7, 15, 16, 63, 64, 65, 127, 128, 129,
			255, 256, 1024, 4096, 8192}) {
			for (size_t offset = 0; offset < 64; offset++) {
				for (int value : {0, 0x5a, 0x100, -1})
					check((uint8_t*)(addr_t)map.offset, n + 128, offset, n, value);
			}
		}
		printf("PASS memset graphics buffer %s\n", flags ? "write-back" : "write-combining");
	}
	close(device);
	return 0;
}

int main(int argc, char** argv)
{
	alarm(120);
	if (argc == 2 && strcmp(argv[1], "--graphics") == 0)
		return graphicsBuffers();
	if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
		benchmark();
		return 0;
	}
	const int values[] = {0, 1, 0x55, 0xff, 0x100, 0x1a5, -1, -256,
		INT_MIN, INT_MAX};
	std::vector<uint8_t> buffer(65536 + 256);
	for (size_t n = 0; n <= 512; n++) {
		for (size_t offset = 0; offset < 64; offset++) {
			for (int value : values)
				check(buffer.data(), 768, offset, n, value);
		}
	}
	for (size_t n : {1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536}) {
		for (size_t offset = 0; offset < 64; offset++) {
			for (int value : values)
				check(buffer.data(), n + 128, offset, n, value);
		}
	}
	const size_t page = sysconf(_SC_PAGESIZE);
	uint8_t* map = (uint8_t*)mmap(NULL, 4 * page, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (map == MAP_FAILED || mprotect(map + page, 2 * page,
		PROT_READ | PROT_WRITE) != 0)
		return 1;
	uint8_t* data = map + page;
	for (size_t n = 0; n <= 2 * page; n++) {
		for (int value : values) {
			check(data, 2 * page, 0, n, value);
			check(data, 2 * page, 2 * page - n, n, value);
		}
	}
	munmap(map, 4 * page);
	printf("PASS memset cases=%zu values=10 alignments=64 guarded_pages=yes\n", sCases);
	return 0;
}
