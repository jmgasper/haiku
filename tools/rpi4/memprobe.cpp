/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_memprobe <seconds>
// Copies 1 MB over and over and reports the stretches in which a copy took
// more than three times as long as the quickest: something else is using
// the memory (on the Raspberry Pi 4, the firmware's H.264 decoder does).


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


int
main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 4;
	const size_t size = 1 << 20;
	char* from = (char*)malloc(size);
	char* to = (char*)malloc(size);
	if (from == NULL || to == NULL)
		return 1;
	memset(from, 1, size);
	memset(to, 2, size);

	bigtime_t fastest = B_INFINITE_TIMEOUT;
	for (int i = 0; i < 20; i++) {
		bigtime_t start = system_time();
		memcpy(to, from, size);
		fastest = min_c(fastest, system_time() - start);
	}

	bigtime_t begin = system_time();
	bigtime_t end = begin + seconds * 1000000LL;
	int32 copies = 0;
	int32 slow = 0;
	bigtime_t slowTime = 0;
	bigtime_t stretchStart = 0;
	bigtime_t stretchEnd = 0;
	while (system_time() < end) {
		bigtime_t start = system_time();
		memcpy(to, from, size);
		bigtime_t done = system_time();
		copies++;
		if (done - start > 3 * fastest) {
			slow++;
			slowTime += done - start;
			if (stretchStart == 0)
				stretchStart = start;
			stretchEnd = done;
		} else if (stretchStart != 0) {
			if (stretchEnd - stretchStart > 15000) {
				printf("  at %.3f s: slow for %.1f ms\n",
					(stretchStart - begin) / 1e6,
					(stretchEnd - stretchStart) / 1000.0);
			}
			stretchStart = 0;
		}
	}
	printf("%" B_PRId32 " copies of 1 MB, the quickest %.2f ms; %" B_PRId32
		" took more than three times that, %.0f ms of %d s\n", copies,
		fastest / 1000.0, slow, slowTime / 1000.0, seconds);
	return 0;
}
