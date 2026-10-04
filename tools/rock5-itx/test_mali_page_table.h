/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_PAGE_TABLE_TEST_H
#define MALI_PAGE_TABLE_TEST_H

#include <assert.h>
#include <stdint.h>
#include <map>
#include <set>

// Independent recursive decoder. Follow physical descriptors through the page
// inventory; catch duplicate/orphan tables, unexpected flags and extra leaves.
static void DecodeGpuTable(const void* data, const uint64_t* pages, unsigned count,
	uint64_t physical, unsigned level, uint64_t prefix, std::set<uint64_t>& visited,
	std::map<uint64_t, uint64_t>& leaves)
{
	assert(level < 4 && visited.insert(physical).second);
	unsigned index = 0;
	while (index < count && pages[index] != physical) index++;
	assert(index < count);
	const uint64_t* entries = (const uint64_t*)((const uint8_t*)data + index * 4096);
	for (unsigned i = 0; i < 512; i++) {
		uint64_t value = entries[i];
		if (value == 0) continue;
		assert((value & 3) == 3);
		uint64_t address = prefix | (uint64_t(i) << (39 - level * 9));
		if (level == 3) {
			assert((value & ~(UINT64_C(0xfffffff000) | (UINT64_C(3) << 53) | 0x7df)) == 0);
			assert(leaves.emplace(address, value).second);
		} else {
			assert((value & ~UINT64_C(0xfffffff003)) == 0);
			DecodeGpuTable(data, pages, count, value & UINT64_C(0xfffffff000),
				level + 1, address, visited, leaves);
		}
	}
}

static std::map<uint64_t, uint64_t> DecodeGpuTables(const void* data,
	const uint64_t* pages, unsigned count)
{
	assert(count != 0);
	std::set<uint64_t> visited;
	std::map<uint64_t, uint64_t> leaves;
	DecodeGpuTable(data, pages, count, pages[0], 0, 0, visited, leaves);
	assert(visited.size() == count);
	return leaves;
}
#endif
