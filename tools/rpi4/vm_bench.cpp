/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Check zero-filled resident and demand-paged allocations, then dirty and free
// them. Warm up with 67108864 64 before measuring 67108864 32: the pre-cleared
// free-page pool can otherwise make a fresh boot appear substantially faster.
// Thread CPU timings include bookkeeping; very small areas have coarse results.
#include <OS.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool ok, const char* message)
{
	if (!ok) {
		fprintf(stderr, "VM_BENCH_FAIL %s\n", message);
		exit(1);
	}
}

static bigtime_t cpu_time()
{
	thread_info info;
	require(get_thread_info(find_thread(NULL), &info) == B_OK, "thread info");
	return info.user_time + info.kernel_time;
}

int main(int argc, char** argv)
{
	const size_t size = argc > 1 ? strtoul(argv[1], NULL, 0) : 64 * 1024 * 1024;
	const unsigned rounds = argc > 2 ? strtoul(argv[2], NULL, 0) : 32;
	require(size > 0 && size <= 256 * 1024 * 1024 && size % B_PAGE_SIZE == 0,
		"page-aligned size within 256 MiB");
	require(rounds > 0 && rounds <= 1000, "round count");
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (uint32 lock : {uint32(B_FULL_LOCK), uint32(B_NO_LOCK)}) {
		const char* mode = lock == B_FULL_LOCK ? "resident" : "demand";
		for (unsigned round = 0; round < rounds; round++) {
			void* address = NULL;
			bigtime_t startCPU = cpu_time(), start = system_time();
			area_id area = create_area("vm benchmark owned area", &address,
				B_ANY_ADDRESS, size, lock, B_READ_AREA | B_WRITE_AREA);
			require(area >= 0, "create area");
			bigtime_t allocated = system_time(), allocatedCPU = cpu_time();
			const uint64_t* words = (const uint64_t*)address;
			uint64_t all = 0;
			for (size_t i = 0; i < size / sizeof(uint64_t); i++)
				all |= words[i];
			require(all == 0, "new allocation is entirely zero");
			bigtime_t read = system_time(), readCPU = cpu_time();
			memset(address, 0xa5, size);
			bigtime_t dirty = system_time(), dirtyCPU = cpu_time();
			require(delete_area(area) == B_OK, "delete area");
			bigtime_t deleted = system_time(), deletedCPU = cpu_time();
			printf("VM_BENCH mode=%s size=%zu round=%u allocate_us=%lld zero_read_us=%lld dirty_us=%lld delete_us=%lld allocate_cpu_us=%lld read_cpu_us=%lld dirty_cpu_us=%lld delete_cpu_us=%lld\n",
				mode, size, round, (long long)(allocated - start),
				(long long)(read - allocated), (long long)(dirty - read),
				(long long)(deleted - dirty), (long long)(allocatedCPU - startCPU),
				(long long)(readCPU - allocatedCPU), (long long)(dirtyCPU - readCPU),
				(long long)(deletedCPU - dirtyCPU));
		}
	}
	puts("VM_BENCH_PASS");
	return 0;
}
