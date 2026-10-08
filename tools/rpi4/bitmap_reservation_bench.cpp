/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Exercise the client bitmap area's reference counting and virtual-address
// reservation lifetime. Address span measures virtual space, not physical RAM.

#include <ServerMemoryAllocator.h>

#include <OS.h>
#include <image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void
require(bool value, const char* message)
{
	if (!value) {
		fprintf(stderr, "RESERVATION_FAIL %s\n", message);
		exit(1);
	}
}


static bigtime_t
cpu()
{
	team_usage_info info = {};
	require(get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &info) == B_OK,
		"team CPU");
	return info.user_time + info.kernel_time;
}


static void
check_provider()
{
	int32 cookie = 0;
	image_info image = {};
	bool found = false;
	const char* expected = getenv("EXPECT_BE_PROVIDER");
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
		const char* leaf = strrchr(image.name, '/');
		if (leaf != NULL && strcmp(leaf + 1, "libbe.so") == 0) {
			printf("LIBBE path=%s\n", image.name);
			require(expected == NULL || strcmp(expected, image.name) == 0,
				"expected libbe provider");
			found = true;
		}
	}
	require(found, "libbe image");
}


int
main(int argc, char** argv)
{
	int count = argc > 1 ? atoi(argv[1]) : 1000;
	if (argc > 2 || count < 100 || count > 2000 || count % 10 != 0) {
		fprintf(stderr, "usage: %s [cycles 100..2000, multiple of 10]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	check_provider();

	void* original = NULL;
	area_id backing = create_area("reservation benchmark backing", &original,
		B_ANY_ADDRESS, B_PAGE_SIZE, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	require(backing >= B_OK, "backing area");
	memset(original, 0x6d, B_PAGE_SIZE);

	addr_t first = 0;
	addr_t last = 0;
	int reused = 0;
	bigtime_t total = cpu();
	bigtime_t start = system_time();
	{
		BPrivate::ServerMemoryAllocator allocator;
		for (int group = 0; group < 10; group++) {
			bigtime_t groupCPU = cpu();
			bigtime_t groupStart = system_time();
			for (int i = 0; i < count / 10; i++) {
				area_id local = -1;
				uint8* base = NULL;
				require(allocator.AddArea(backing, local, base, B_PAGE_SIZE) == B_OK,
					"first mapping");
				if (first == 0)
					first = (addr_t)base;
				if (last == (addr_t)base)
					reused++;
				last = (addr_t)base;
				require(memcmp(base, original, B_PAGE_SIZE) == 0, "cloned bytes");

				area_id same = -1;
				uint8* sameBase = NULL;
				require(allocator.AddArea(backing, same, sameBase, B_PAGE_SIZE) == B_OK,
					"second reference");
				require(same == local && sameBase == base, "same mapping");
				allocator.RemoveArea(backing);
				area_info info = {};
				require(get_area_info(local, &info) == B_OK,
					"first removal retains mapping");
				allocator.RemoveArea(backing);
				require(get_area_info(local, &info) != B_OK,
					"last removal deletes mapping");
			}
			printf("GROUP index=%d loops=%d wall_ms=%.3f cpu_ms=%.3f base=%p\n",
				group, count / 10, (system_time() - groupStart) / 1000.,
				(cpu() - groupCPU) / 1000., (void*)last);
		}
	}
	require(delete_area(backing) == B_OK, "delete backing");
	printf("RESERVATION loops=%d first=%p last=%p span_bytes=%" B_PRIu64
		" reused=%d wall_ms=%.3f cpu_ms=%.3f\n", count, (void*)first, (void*)last,
		uint64(last - first), reused, (system_time() - start) / 1000.,
		(cpu() - total) / 1000.);
	if (getenv("EXPECT_RESERVATION_REUSE") != NULL)
		require(reused == count - 1, "every released reservation reused");
	puts("RESERVATION_PASS");
	return 0;
}
