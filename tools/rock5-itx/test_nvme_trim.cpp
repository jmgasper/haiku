#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>

#include "trim_range.h"


static void
Check(uint64_t capacity, uint32_t sector, uint64_t offset, uint64_t size)
{
	uint64_t lba = UINT64_MAX;
	uint32_t blocks = UINT32_MAX;
	bool valid = nvme_normalize_trim_range(capacity, sector, offset, size,
		lba, blocks);
	bool expectedValid = sector != 0 && capacity % sector == 0 && offset < capacity;
	assert(valid == expectedValid);
	if (!valid) {
		assert(lba == 0 && blocks == 0);
		return;
	}

	// An independent interval calculation with wider arithmetic allows both
	// endpoint additions to exceed UINT64_MAX without wrapping.
	__uint128_t end = __uint128_t(offset) + size;
	if (end > capacity)
		end = capacity;
	__uint128_t first = (__uint128_t(offset) + sector - 1) / sector;
	__uint128_t last = end / sector;
	__uint128_t count = last > first ? last - first : 0;
	if (count > UINT32_MAX)
		count = UINT32_MAX;
	assert(blocks == count);
	assert(lba == (count == 0 ? 0 : first));
	if (blocks != 0) {
		assert(__uint128_t(lba) * sector >= offset);
		assert((__uint128_t(lba) + blocks) * sector <= end);
	}
}


int
main()
{
	uint64_t lba;
	uint32_t blocks;
	assert(nvme_normalize_trim_range(4096, 512, 17, 1, lba, blocks));
	assert(lba == 0 && blocks == 0);
	assert(nvme_normalize_trim_range(4096, 512, 17, 1007, lba, blocks));
	assert(lba == 1 && blocks == 1);

	for (uint64_t offset = 0; offset <= 1024; offset++) {
		for (uint64_t size = 0; size <= 2048; size++)
			Check(4096, 512, offset, size);
	}
	const uint64_t cases[] = {0, 1, 17, 511, 512, 513, 4095, 4096, 4097,
		UINT64_C(0x100000000) - 1, UINT64_C(0x100000000),
		UINT64_C(0x100000000) + 1, UINT64_C(0x8000000000000000),
		UINT64_MAX - 4096, UINT64_MAX - 511, UINT64_MAX - 1, UINT64_MAX};
	for (uint32_t sector : {512U, 4096U}) {
		for (uint64_t capacity : {uint64_t(8192), uint64_t(256060514304),
			UINT64_MAX - (sector - 1)}) {
			for (uint64_t offset : cases) {
				for (uint64_t size : cases)
					Check(capacity, sector, offset, size);
			}
			Check(capacity, sector, capacity - 1, UINT64_MAX);
			Check(capacity, sector, capacity, 0);
		}
	}
	Check(4096, 0, 0, 512);
	Check(4095, 512, 0, 512);
	Check(0, 512, 0, 0);
	puts("NVMe trim interval checks passed");
}
