/*
 * Check the actual memcpy entry point with alignment and protected-page cases.
 * Distributed under the terms of the MIT License.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef COPY_FUNCTION
#define COPY_FUNCTION memcpy
#else
extern "C" void* COPY_FUNCTION(void*, const void*, size_t);
#endif

static void* (*volatile sCopy)(void*, const void*, size_t) = COPY_FUNCTION;
static unsigned long sCases;


static uint8_t
Pattern(size_t index)
{
	return (uint8_t)((index * 37 + 53) ^ (index >> 8));
}


static bool
Check(uint8_t* dest, const uint8_t* source, size_t capacity,
	size_t destOffset, size_t sourceOffset, size_t count)
{
	if (destOffset > capacity || count > capacity - destOffset
		|| sourceOffset > capacity || count > capacity - sourceOffset)
		return false;
	memset(dest, 0xa5, capacity);
	if (sCopy(dest + destOffset, source + sourceOffset, count)
		!= dest + destOffset)
		return false;
	for (size_t i = 0; i < capacity; i++) {
		uint8_t expected = i >= destOffset && i - destOffset < count
			? Pattern(sourceOffset + i - destOffset) : 0xa5;
		if (dest[i] != expected || source[i] != Pattern(i)) {
			fprintf(stderr, "ROCK5_MEMCPY_MISMATCH length=%zu source=%zu "
				"destination=%zu index=%zu\n", count, sourceOffset, destOffset, i);
			return false;
		}
	}
	sCases++;
	return true;
}


static bool
Alignments()
{
	const size_t capacity = 65535 + 64;
	uint8_t* source = (uint8_t*)malloc(capacity);
	uint8_t* dest = (uint8_t*)malloc(capacity);
	if (source == NULL || dest == NULL) {
		free(source);
		free(dest);
		return false;
	}
	for (size_t i = 0; i < capacity; i++)
		source[i] = Pattern(i);
	bool passed = true;
	for (size_t count = 0; count <= 128 && passed; count++) {
		for (size_t s = 0; s < 16 && passed; s++) {
			for (size_t d = 0; d < 16 && passed; d++)
				passed = Check(dest, source, 192, d, s, count);
		}
	}
	static const size_t sizes[] = {255, 256, 257, 511, 512, 513,
		1023, 1024, 1025, 1500, 1514, 2046, 2048, 4095, 4096, 4097, 16384, 65535};
	for (size_t count : sizes) {
		for (size_t s = 0; s < 8 && passed; s++) {
			for (size_t d = 0; d < 8 && passed; d++)
				passed = Check(dest, source, count + 32, d, s, count);
		}
	}
	free(source);
	free(dest);
	return passed;
}


static bool
GuardPages()
{
	long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize < 4096)
		return false;
	size_t page = pageSize;
	uint8_t* sourceMap = (uint8_t*)mmap(NULL, 3 * page, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	uint8_t* destMap = (uint8_t*)mmap(NULL, 3 * page, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (sourceMap == MAP_FAILED || destMap == MAP_FAILED) {
		if (sourceMap != MAP_FAILED)
			munmap(sourceMap, 3 * page);
		if (destMap != MAP_FAILED)
			munmap(destMap, 3 * page);
		return false;
	}
	uint8_t* source = sourceMap + page;
	uint8_t* dest = destMap + page;
	bool passed = mprotect(source, page, PROT_READ | PROT_WRITE) == 0
		&& mprotect(dest, page, PROT_READ | PROT_WRITE) == 0;
	if (passed) {
		for (size_t i = 0; i < page; i++)
			source[i] = Pattern(i);
		passed = mprotect(source, page, PROT_READ) == 0;
	}
	if (passed) {
		// Zero-length copies may point directly into inaccessible guard pages.
		passed = sCopy(destMap, sourceMap, 0) == destMap;
		sCases++;
	}
	for (size_t count = 0; count <= 512 && passed; count++) {
		for (size_t offset = 0; offset < 16 && passed; offset++) {
			passed = Check(dest, source, page, offset, page - count, count)
				&& Check(dest, source, page, page - count, offset, count);
		}
		if (passed)
			passed = Check(dest, source, page, page - count, page - count, count);
	}
	static const size_t ends[] = {1023, 1024, 1500, 1514, 2046, 2048};
	for (size_t count : ends) {
		for (size_t offset = 0; offset < 16 && passed; offset++) {
			passed = Check(dest, source, page, offset, page - count, count)
				&& Check(dest, source, page, page - count, offset, count);
		}
	}
	if (passed) {
		passed = Check(dest, source, page, 0, 0, page)
			&& Check(dest, source, page, 0, 1, page - 1)
			&& Check(dest, source, page, 1, 0, page - 1)
			&& sCopy(dest, dest, page) == dest;
	}
	if (munmap(sourceMap, 3 * page) != 0)
		passed = false;
	if (munmap(destMap, 3 * page) != 0)
		passed = false;
	return passed;
}


static void
Fault(int signalNumber)
{
	(void)signalNumber;
	static const char message[] = "ROCK5_MEMCPY_FAULT\n";
	ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
	(void)written;
	_exit(1);
}


int
main()
{
	signal(SIGSEGV, Fault);
	signal(SIGBUS, Fault);
	alarm(120);
	if (!Alignments() || !GuardPages()) {
		fprintf(stderr, "ROCK5_MEMCPY_FAIL cases=%lu errno=%d\n", sCases, errno);
		return 1;
	}
	alarm(0);
	printf("ROCK5_MEMCPY_PASS cases=%lu alignments=16 guarded_pages=yes\n", sCases);
	return 0;
}
