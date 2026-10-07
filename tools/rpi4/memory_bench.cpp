/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Compare installed and isolated libroot builds with LIBRARY_PATH. Calls go
// through volatile pointers so the compiler cannot substitute its builtins.
#include <OS.h>
#include <algorithm>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using Copy = void* (*)(void*, const void*, size_t);
static Copy volatile sCopy = memcpy;
static Copy volatile sMove = memmove;

static void measure(const char* operation, Copy function, uint8_t* destination,
	const uint8_t* source, size_t length, int offset, int round)
{
	const size_t batch = std::min(size_t(4096),
		std::max(size_t(1), size_t(1048576) / length));
	size_t calls = 0;
	bigtime_t start = system_time(), end;
	do {
		for (size_t i = 0; i < batch; i++) {
			function(destination, source, length);
			calls++;
		}
		end = system_time();
	} while (end - start < 100000);
	printf("round=%d operation=%s bytes=%zu offset=%d ns=%.1f MBps=%.1f\n",
		round, operation, length, offset, double(end - start) * 1000 / calls,
		double(length) * calls / (end - start));
}

int main()
{
	Dl_info info = {};
	if (dladdr((void*)sCopy, &info) != 0)
		printf("memcpy provider: %s\n", info.dli_fname);
	if (dladdr((void*)sMove, &info) != 0)
		printf("memmove provider: %s\n", info.dli_fname);
	const size_t sizes[] = {32, 128, 1024, 4096, 65536, 1048576, 8388608};
	std::vector<uint8_t> source(8388608 + 128), destination(source.size());
	for (size_t i = 0; i < source.size(); i++)
		source[i] = (i * 37 + (i >> 8)) & 255;
	for (int round = 0; round < 3; round++) {
		for (size_t length : sizes) {
			for (int offset : {0, 3}) {
				measure("copy", sCopy, destination.data() + offset,
					source.data() + 1, length, offset, round);
				if (memcmp(destination.data() + offset, source.data() + 1,
					length) != 0)
					return 1;
			}
			for (int offset : {1, 3, 64}) {
				measure("move-backward", sMove, destination.data() + offset,
					destination.data(), length, offset, round);
				measure("move-forward", sMove, destination.data(),
					destination.data() + offset, length, offset, round);
			}
		}
	}
	return 0;
}
