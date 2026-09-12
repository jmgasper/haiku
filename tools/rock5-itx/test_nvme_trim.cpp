#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#include "trim_range.h"


static void
Check(uint64_t capacity, uint32_t sector, uint64_t offset, uint64_t size)
{
	uint64_t lba = UINT64_MAX;
	uint64_t blocks = UINT32_MAX;
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
	assert(blocks == count);
	assert(lba == (count == 0 ? 0 : first));
	if (blocks != 0) {
		assert(__uint128_t(lba) * sector >= offset);
		assert((__uint128_t(lba) + blocks) * sector <= end);
	}
}


static void
CheckBatches(uint16_t ranges, uint32_t perRange, uint64_t perCommand,
	std::initializer_list<uint64_t> inputs)
{
	NVMeTrimBatch batch(ranges, perRange, perCommand);
	const uint16_t rangeLimit = ranges == 0 || ranges > 256 ? 256 : ranges;
	const uint32_t blockLimit = perRange == 0 ? UINT32_MAX : perRange;
	const uint64_t commandLimit = perCommand == 0 ? UINT64_MAX : perCommand;
	std::vector<uint32_t> command;
	__uint128_t total = 0;
	__uint128_t expected = 0;
	auto complete = [&]() {
		assert(!command.empty());
		assert(command.size() <= rangeLimit);
		uint64_t sum = 0;
		for (uint32_t count : command) {
			assert(count != 0 && count <= blockLimit);
			sum += count;
		}
		assert(sum <= commandLimit);
		assert(sum == batch.Blocks() && command.size() == batch.Count());
		total += sum;
		command.clear();
		batch.Reset();
		assert(batch.Count() == 0 && batch.Blocks() == 0);
	};
	assert(batch.MaxRanges() == rangeLimit);
	assert(batch.Add(0) == 0 && batch.Count() == 0);
	for (uint64_t input : inputs) {
		expected += input;
		uint64_t remaining = input;
		while (remaining != 0) {
			uint32_t count = batch.Add(remaining);
			if (count == 0) {
				complete();
				continue;
			}
			assert(count <= remaining);
			remaining -= count;
			command.push_back(count);
		}
	}
	if (batch.Count() != 0)
		complete();
	assert(total == expected);
}


int
main()
{
	uint64_t lba;
	uint64_t blocks;
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
	for (uint16_t ranges : {1, 2, 255, 256, 257, 0}) {
		for (uint32_t perRange : {1U, 7U, 0U, UINT32_MAX}) {
			for (uint64_t perCommand : {uint64_t(1), uint64_t(11), uint64_t(0),
				UINT64_MAX}) {
				CheckBatches(ranges, perRange, perCommand, {0, 1, 3, 0, 1000, 9});
			}
		}
	}
	// More descriptors than one command and an interval beyond UINT32_MAX.
	CheckBatches(2, 100, 0, {601});
	CheckBatches(0, 0, 0, {uint64_t(UINT32_MAX) * 257 + 19, 0, 1});
	CheckBatches(1, 0, UINT64_MAX, {uint64_t(UINT32_MAX) + 1});
	CheckBatches(256, UINT32_MAX, UINT32_MAX, {uint64_t(UINT32_MAX) + 1});
	// Actual QEMU DMRSL and the BFS free-space intervals that exposed it.
	CheckBatches(0, 4194303, 0, {4160488, 524264, 3653632, 7335920});
	puts("NVMe trim interval and batch-limit checks passed");
}
