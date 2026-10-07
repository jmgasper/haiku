/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Exercise the actual strlen entry point, including strings beside inaccessible
// pages and zero bytes before an unaligned string. The expected length comes
// from constructing the string, independently of the implementation under test.
#include <OS.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using Length = size_t (*)(const char*);
static Length volatile sLength = strlen;
static size_t sCases;


static void
Check(unsigned char* begin, size_t length, unsigned pattern)
{
	for (size_t i = 0; i < length; i++)
		begin[i] = pattern != 0 ? pattern : 1 + (i * 113 + 17) % 255;
	begin[length] = 0;
	size_t actual = sLength((const char*)begin);
	if (actual != length) {
		fprintf(stderr, "FAIL length=%zu address=%p pattern=%u actual=%zu\n",
			length, begin, pattern, actual);
		exit(1);
	}
	sCases++;
}


static void
Benchmark()
{
	std::vector<unsigned char> bytes(65536 + 128);
	unsigned char* aligned = (unsigned char*)(((uintptr_t)bytes.data() + 63)
		& ~uintptr_t(63));
	volatile size_t sink = 0;
	for (unsigned round = 0; round < 4; round++) {
		for (size_t length : {size_t(0), size_t(3), size_t(8), size_t(16),
			size_t(32), size_t(128), size_t(512), size_t(4096), size_t(32768)}) {
			for (unsigned offset : {0u, 3u, 15u, 31u, 63u}) {
				memset(aligned, 0x87, length + 64);
				aligned[offset + length] = 0;
				Length function = sLength;
				size_t count = 0;
				bigtime_t start = system_time(), end;
				do {
					for (unsigned i = 0; i < 1024; i++)
						sink += function((const char*)aligned + offset);
					count += 1024;
					end = system_time();
				} while (end - start < 50000);
				printf("BENCH round=%u length=%zu offset=%u ns=%.3f\n",
					round, length, offset, double(end - start) * 1000 / count);
			}
		}
	}
	if (sink == 1)
		puts("unreachable sink");
}


int
main(int argc, char** argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--benchmark") != 0)) {
		fprintf(stderr, "usage: %s [--benchmark]\n", argv[0]);
		return 2;
	}
	size_t page = sysconf(_SC_PAGESIZE);
	unsigned char* mapping = (unsigned char*)mmap(NULL, page * 4, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED || mprotect(mapping + page, page * 2,
		PROT_READ | PROT_WRITE) != 0) {
		perror("guarded mapping");
		return 2;
	}
	unsigned char* data = mapping + page;
	for (unsigned pattern : {0u, 1u, 127u, 128u, 255u}) {
		for (size_t length = 0; length <= 2048; length++) {
			for (size_t offset = 0; offset < 64; offset++) {
				memset(data, 0, offset);
				Check(data + offset, length, pattern);
			}
		}
		for (size_t length = 0; length < page * 2; length++) {
			Check(data, length, pattern);
			unsigned char* start = data + page * 2 - length - 1;
			if (start > data)
				start[-1] = 0;
			Check(start, length, pattern);
		}
	}
	printf("PASS strlen cases=%zu alignments=64 guarded_pages=yes "
		"byte_patterns=5\n", sCases);
	munmap(mapping, page * 4);
	if (argc == 2)
		Benchmark();
	return 0;
}
