/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Compare the real entry point against a byte-wise oracle. ISO C guarantees
// the sign of strcmp's result, not the particular nonzero value it returns.
#include <OS.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifdef COMPARE_FUNCTION
extern "C" int COMPARE_FUNCTION(const char*, const char*);
#define PRIVATE_COMPARE 1
#else
#define COMPARE_FUNCTION strcmp
#endif

using Compare = int (*)(const char*, const char*);
static Compare volatile sCompare = COMPARE_FUNCTION;
static size_t sCases;

static int
Sign(int value)
{
	return (value > 0) - (value < 0);
}

static int
Reference(const unsigned char* a, const unsigned char* b)
{
	while (*a != 0 && *a == *b) {
		a++;
		b++;
	}
	return int(*a) - int(*b);
}

static void
Check(const unsigned char* a, const unsigned char* b)
{
	int expected = Reference(a, b);
	int actual = sCompare((const char*)a, (const char*)b);
	if (Sign(expected) != Sign(actual)) {
		fprintf(stderr, "FAIL case %zu expected %d actual %d\n",
			sCases, expected, actual);
		exit(1);
	}
	sCases++;
}

static void
CheckPair(unsigned char* a, unsigned char* b, size_t length)
{
	for (size_t i = 0; i < length; i++)
		a[i] = b[i] = 1 + (i * 113 + 17) % 255;
	a[length] = b[length] = 0;
	Check(a, b);
	Check(b, a);
	for (size_t position : {size_t(0), length / 2, length ? length - 1 : 0}) {
		unsigned char original = b[position];
		for (unsigned char value : {0, 1, 127, 128, 255}) {
			// Do not remove the only terminator beside an inaccessible page.
			if (position == length && value != 0)
				continue;
			b[position] = value;
			Check(a, b);
			Check(b, a);
		}
		b[position] = original;
	}
}

static void
Benchmark()
{
	volatile int64_t sink = 0;
	std::vector<unsigned char> a(8192), b(8192);
	for (int round = 0; round < 4; round++) {
		for (size_t length : {size_t(8), size_t(32), size_t(128),
			size_t(512), size_t(4096)}) {
			for (int offset : {0, 3}) {
				for (int difference : {0, 1}) {
					memset(a.data(), 0x87, a.size());
					memset(b.data(), 0x87, b.size());
					a[length] = b[length + offset] = 0;
					if (difference != 0)
						b[length / 2 + offset] = 0x88;
#ifdef PRIVATE_COMPARE
					const int steps = 2;
#else
					const int steps = 1;
#endif
					for (int step = 0; step < steps; step++) {
						bool candidate = steps == 2
							&& (step ^ (round == 1 || round == 2)) != 0;
						Compare volatile function = candidate
							? COMPARE_FUNCTION : strcmp;
						size_t count = 0;
						bigtime_t start = system_time(), end;
						do {
							for (int i = 0; i < 4096; i++) {
								sink += function((char*)a.data(),
									(char*)b.data() + offset);
							}
							count += 4096;
							end = system_time();
						} while (end - start < 60000);
						printf("BENCH round=%d length=%zu offset=%d "
							"difference=%d mode=%s ns=%.2f\n", round, length,
							offset, difference, candidate ? "candidate" : "installed",
							double(end - start) * 1000 / count);
					}
				}
			}
		}
	}
}

int
main(int argc, char** argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--benchmark") != 0)) {
		fprintf(stderr, "usage: %s [--benchmark]\n", argv[0]);
		return 2;
	}
	size_t page = sysconf(_SC_PAGESIZE);
	unsigned char* mappings[2];
	for (auto& mapping : mappings) {
		mapping = (unsigned char*)mmap(NULL, page * 3, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED
			|| mprotect(mapping + page, page, PROT_READ | PROT_WRITE) != 0) {
			perror("guarded mapping");
			return 2;
		}
	}
	for (size_t length = 0; length <= 512; length++) {
		for (size_t a = 0; a < 16; a++) {
			for (size_t b = 0; b < 16; b++)
				CheckPair(mappings[0] + page + a, mappings[1] + page + b, length);
		}
	}
	for (size_t length = 0; length < page; length++) {
		CheckPair(mappings[0] + page * 2 - length - 1,
			mappings[1] + page * 2 - length - 1, length);
		for (size_t align = 0; align < 8 && length + align + 1 <= page; align++) {
			CheckPair(mappings[0] + page * 2 - length - 1,
				mappings[1] + page + align, length);
			CheckPair(mappings[0] + page + align,
				mappings[1] + page * 2 - length - 1, length);
		}
	}
	printf("PASS strcmp cases=%zu alignments=16x16 guarded_pages=yes "
		"unsigned_bytes=yes\n", sCases);
	for (auto mapping : mappings)
		munmap(mapping, page * 3);
	if (argc == 2)
		Benchmark();
	return 0;
}
