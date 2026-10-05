/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_swap_commit [megabytes]
// Checks the commitment of an overcommitting area whose pages have been in
// the swap file: the kernel used to count a page that was swapped out as
// room for a new one, and panicked in VMAnonymousCache::Commit() when
// madvise(MADV_FREE) later gave the pages back.
//
// The steps: touch the first half of a MAP_NORESERVE mapping; fill the
// memory until that half has been written to the swap file; touch the second
// half; release the memory; read the first half back in; MADV_FREE all.


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>


static const size_t kChunk = 16 * 1024 * 1024;


static void
report(const char* what)
{
	system_info info;
	get_system_info(&info);
	printf("%-28s free %4" B_PRIu64 " MB, swap used %4" B_PRIu64 " MB\n",
		what, (info.max_pages - info.used_pages) * B_PAGE_SIZE >> 20,
		(uint64)(info.max_swap_pages - info.free_swap_pages) * B_PAGE_SIZE
			>> 20);
	fflush(stdout);
}


static uint64
swap_used()
{
	system_info info;
	get_system_info(&info);
	return (uint64)(info.max_swap_pages - info.free_swap_pages) * B_PAGE_SIZE;
}


static uint8
pattern(size_t offset)
{
	return (uint8)(offset >> 12) ^ 0x5a;
}


int
main(int argc, char** argv)
{
	size_t half = (argc > 1 ? strtoul(argv[1], NULL, 0) : 64) << 20;

	system_info info;
	get_system_info(&info);
	if (info.max_swap_pages == 0) {
		fprintf(stderr, "no swap file: nothing to test\n");
		return 2;
	}

	uint8* area = (uint8*)mmap(NULL, 2 * half, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (area == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	for (size_t i = 0; i < half; i += B_PAGE_SIZE)
		area[i] = pattern(i);
	report("first half touched");

	// fill the memory until the first half is in the swap file
	uint64 swapBefore = swap_used();
	// Not all the way: a system with nothing left at all does not come
	// back. What the system counts as used is what has been promised, so
	// promising the free memory and a little more than the half puts about
	// that much into the swap file, the idle pages first.
	get_system_info(&info);
	size_t target = (info.max_pages - info.used_pages) * B_PAGE_SIZE + half
		+ 32 * 1024 * 1024;
	if (half + 64 * 1024 * 1024
			> (uint64)info.free_swap_pages * B_PAGE_SIZE) {
		fprintf(stderr, "the swap file is too small for this size\n");
		return 2;
	}

	static void* hogs[1024];
	size_t hogCount = 0;
	for (size_t total = 0; total < target && hogCount < 1024;
			total += kChunk) {
		void* hog = mmap(NULL, kChunk, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (hog == MAP_FAILED)
			break;
		memset(hog, 1, kChunk);
		hogs[hogCount++] = hog;
	}

	// the hog stays in use, so that the idle half is what goes
	bigtime_t deadline = system_time() + 120 * 1000000LL;
	while (swap_used() < swapBefore + half / 2 && system_time() < deadline) {
		for (size_t i = 0; i < hogCount; i++) {
			for (size_t j = 0; j < kChunk; j += B_PAGE_SIZE)
				((volatile uint8*)hogs[i])[j]++;
		}
		snooze(200000);
	}
	report("memory filled");
	if (swap_used() < swapBefore + half / 4) {
		fprintf(stderr, "the first half did not go to the swap file\n");
		return 2;
	}

	for (size_t i = half; i < 2 * half; i += B_PAGE_SIZE)
		area[i] = pattern(i);
	report("second half touched");

	for (size_t i = 0; i < hogCount; i++)
		munmap(hogs[i], kChunk);
	report("memory released");

	size_t wrong = 0;
	for (size_t i = 0; i < 2 * half; i += B_PAGE_SIZE) {
		if (area[i] != pattern(i))
			wrong++;
	}
	report("all read back");
	if (wrong != 0) {
		printf("FAILED: %" B_PRIuSIZE " pages with wrong contents\n", wrong);
		return 1;
	}

	if (madvise(area, 2 * half, MADV_FREE) != 0) {
		perror("madvise");
		return 1;
	}
	report("given back (MADV_FREE)");

	// and the area still works
	for (size_t i = 0; i < 2 * half; i += B_PAGE_SIZE)
		area[i] = 1;
	munmap(area, 2 * half);
	report("touched again, unmapped");

	printf("OK\n");
	return 0;
}
