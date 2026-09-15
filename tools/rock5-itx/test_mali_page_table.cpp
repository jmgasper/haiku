/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfPageTable.h"
#include "test_mali_page_table.h"
#include <stdio.h>
#include <sys/mman.h>
#include <vector>

using namespace MaliCSF;
using std::vector;

struct Arena {
	uint8_t* data;
	size_t bytes;
	explicit Arena(size_t size) : bytes(size)
	{
		void* guard = mmap(NULL, bytes + 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(guard != MAP_FAILED);
		data = (uint8_t*)guard + 4096;
		assert(mprotect(data, bytes, PROT_READ | PROT_WRITE) == 0);
	}
	~Arena() { assert(munmap(data - 4096, bytes + 8192) == 0); }
};

static void Check(const vector<PageTableRegion>& regions, unsigned expected = 0)
{
	unsigned count = GpuPageTable::CountPages(regions.data(), regions.size(), kVmUserLimit);
	assert(count != 0 && (expected == 0 || count == expected));
	vector<uint64_t> pages;
	for (unsigned i = 0; i < count; i++)
		pages.push_back(UINT64_C(0xffffff0000) - i * 8192); // descending, near 40-bit limit
	Arena arena(count * 4096);
	memset(arena.data, 0xff, arena.bytes);
	assert(GpuPageTable::Build(arena.data, arena.bytes, pages.data(), count,
		regions.data(), regions.size(), kVmUserLimit));
	auto leaves = DecodeGpuTables(arena.data, pages.data(), count);
	for (const auto& region : regions) {
		for (uint64_t offset = 0; offset < region.bytes; offset += 4096) {
			auto found = leaves.find(region.address + offset);
			assert(found != leaves.end());
			uint64_t value = found->second;
			assert((value & UINT64_C(0xfffffff000)) == region.pages[(region.offset + offset) / 4096]);
			assert((value & (1 << 10)) != 0);
			assert(((value >> 6) & 3) == ((region.flags & kVmReadOnly) ? 3 : 1));
			assert(((value >> 53) & 3) == ((region.flags & kVmNoExecute) ? 3 : 0));
			assert(((value >> 2) & 7) == ((region.flags & kVmUncached) ? 0 : 1));
			assert(((value >> 8) & 3) == ((region.flags & kVmUncached) ? 2 : 3));
			leaves.erase(found);
		}
	}
	assert(leaves.empty());
	// Every rejected build must leave the destination untouched.
	for (unsigned error = 0; error < 7; error++) {
		memset(arena.data, 0x59, arena.bytes);
		auto bad = pages;
		if (error == 0) bad.back()++;
		if (error == 1) bad.back() = UINT64_C(1) << 40;
		if (error == 2 && count > 1) bad.back() = bad.front();
		if (error == 2 && count == 1) continue;
		assert(!GpuPageTable::Build(error == 3 ? arena.data + 1 : arena.data,
			arena.bytes - (error == 4), error == 5 ? NULL : bad.data(),
			count + (error == 6), regions.data(), regions.size(), kVmUserLimit));
		for (size_t i = 0; i < arena.bytes; i++) assert(arena.data[i] == 0x59);
	}
}

int main()
{
	uint64_t pages[64];
	for (unsigned i = 0; i < 64; i++) pages[i] = UINT64_C(0x182345000) + i * 12288;
	PageTableRegion one{0, 4096, 0, sizeof(pages) / sizeof(pages[0]) * 4096, pages, kVmNoExecute};
	Check({}, 1);
	Check({one}, 4);
	for (uint64_t boundary : {UINT64_C(1) << 21, UINT64_C(1) << 30, UINT64_C(1) << 39}) {
		auto region = one; region.address = boundary - 4096; region.bytes = 8192;
		Check({region}, boundary == (UINT64_C(1) << 21) ? 5
			: boundary == (UINT64_C(1) << 30) ? 6 : 7);
	}
	for (uint32_t flags : {1u, 2u, 3u, 5u, 6u, 7u}) {
		auto region = one; region.address = kVmUserLimit - 8192;
		region.bytes = 8192; region.offset = 4096 * 7; region.flags = flags;
		Check({region}, 4);
	}
	// Sparse mappings exercise every level without allocating holes. Multiple
	// subranges share leaf tables and use independent permissions and offsets.
	vector<PageTableRegion> regions;
	uint64_t random = 0x378172ec;
	for (unsigned round = 0; round < 32; round++) {
		regions.clear();
		for (unsigned i = 0; i < 64; i++) {
			random = random * UINT64_C(6364136223846793005) + 1;
			auto region = one;
			region.address = (uint64_t(i) << 39) + ((random & 1023) * 4096);
			region.bytes = ((random >> 10) % 32 + 1) * 4096;
			region.offset = ((random >> 16) % 16) * 4096;
			region.flags = (random & 1 ? kVmReadOnly : kVmNoExecute) | (random & kVmUncached);
			regions.push_back(region);
		}
		Check(regions);
	}
	Arena arena(4 * 4096);
	uint64_t tablePages[] = {0x1000, 0x4000, 0xa000, 0x8000};
	for (unsigned error = 0; error < 20; error++) {
		auto bad = one; uint64_t limit = kVmUserLimit;
		if (error == 0) bad.bytes = 0;
		if (error == 1) bad.address++;
		if (error == 2) bad.bytes++;
		if (error == 3) bad.address = UINT64_MAX - 4095;
		if (error == 4) bad.bytes = kVmUserLimit + 4096;
		if (error == 5) bad.pages = NULL;
		if (error == 6) bad.offset++;
		if (error == 7) bad.offset = bad.bufferBytes;
		if (error == 8) bad.bufferBytes = 4095;
		if (error == 9) bad.flags = 0;
		if (error == 10) bad.flags = 4;
		if (error == 11) bad.flags = 8 | kVmNoExecute;
		if (error == 12) limit = 0;
		if (error == 13) limit--;
		if (error == 14) limit = (UINT64_C(1) << 48) + 4096;
		if (error == 15) pages[0]++;
		if (error == 16) pages[0] = UINT64_C(1) << 40;
		if (error == 17) { bad.bytes = kMaxVmMappedBytes + 4096; bad.bufferBytes = bad.bytes; }
		if (error == 18) bad.address = limit;
		if (error == 19) bad.bufferBytes = 4097;
		memset(arena.data, 0x32, arena.bytes);
		assert(GpuPageTable::CountPages(&bad, 1, limit) == 0);
		assert(!GpuPageTable::Build(arena.data, arena.bytes, tablePages, 4, &bad, 1, limit));
		for (size_t i = 0; i < arena.bytes; i++) assert(arena.data[i] == 0x32);
		pages[0] = UINT64_C(0x182345000);
	}
	auto second = one; second.address = 4096;
	PageTableRegion overlap[] = {one, one}, unsorted[] = {second, one};
	assert(GpuPageTable::CountPages(overlap, 2, kVmUserLimit) == 0);
	assert(GpuPageTable::CountPages(unsorted, 2, kVmUserLimit) == 0);
	assert(GpuPageTable::CountPages(NULL, 1, kVmUserLimit) == 0);
	assert(GpuPageTable::CountPages(&one, kMaxVmMappings + 1, kVmUserLimit) == 0);
	vector<uint64_t> largePages(1024);
	for (unsigned i = 0; i < largePages.size(); i++) largePages[i] = 0x2000000 + i * 8192;
	regions.clear();
	for (unsigned i = 0; i < kMaxVmMappings; i++)
		regions.push_back({uint64_t(i) << 39, 4 * 1024 * 1024, 0,
			4 * 1024 * 1024, largePages.data(), kVmNoExecute});
	assert(GpuPageTable::CountPages(regions.data(), regions.size(), kVmUserLimit) == 0);
	puts("MALI_CSF_PAGE_TABLE_TEST_PASS");
}
