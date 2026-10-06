/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Exercise the actual memmove entry point: overlap in both directions,
// arbitrary alignment, canaries, and exact boundaries next to guard pages.
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifndef MOVE_FUNCTION
#define MOVE_FUNCTION memmove
#else
extern "C" void* MOVE_FUNCTION(void*, const void*, size_t);
#endif
static void* (*volatile sMove)(void*, const void*, size_t) = MOVE_FUNCTION;
static size_t sCases;

static uint8_t pattern(size_t i)
{
	return (i * 37 + 53) ^ (i >> 8);
}

static void check(uint8_t* buffer, size_t capacity, size_t source,
	size_t destination, size_t count)
{
	if (source + count > capacity || destination + count > capacity)
		abort();
	for (size_t i = 0; i < capacity; i++)
		buffer[i] = pattern(i);
	if (sMove(buffer + destination, buffer + source, count) != buffer + destination)
		abort();
	for (size_t i = 0; i < capacity; i++) {
		uint8_t expected = i >= destination && i - destination < count
			? pattern(source + i - destination) : pattern(i);
		if (buffer[i] != expected) {
			fprintf(stderr, "FAIL memmove source=%zu destination=%zu count=%zu byte=%zu\n",
				source, destination, count, i);
			exit(1);
		}
	}
	sCases++;
}

int main()
{
	alarm(120);
	std::vector<uint8_t> buffer(65536 + 8192);
	for (size_t n = 0; n <= 256; n++) {
		for (size_t s = 64; s < 96; s++) {
			for (int delta = -31; delta <= 31; delta++)
				check(buffer.data(), 512, s, s + delta, n);
		}
	}
	const size_t sizes[] = {511, 512, 513, 1023, 1024, 4095, 4096, 4097,
		16384, 65535, 65536};
	const size_t deltas[] = {1, 2, 7, 8, 15, 16, 31, 32, 63, 64, 65,
		127, 128, 129, 255, 256, 257, 4095};
	for (size_t n : sizes) {
		for (size_t s = 0; s < 16; s++) {
			for (size_t delta : deltas) {
				check(buffer.data(), buffer.size(), s, s + delta, n);
				check(buffer.data(), buffer.size(), s + delta, s, n);
			}
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
		check(data, 2 * page, 0, 2 * page - n, n);
		check(data, 2 * page, 2 * page - n, 0, n);
		check(data, 2 * page, 2 * page - n, 2 * page - n, n);
	}
	munmap(map, 4 * page);
	printf("PASS memmove cases=%zu overlap=both alignments=32 guarded_pages=yes\n", sCases);
	return 0;
}
