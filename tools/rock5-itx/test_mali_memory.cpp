/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfMemory.h"
#include "CsfInterface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <map>
#include <set>
#include <vector>

using namespace MaliCSF;
using std::vector;

static void put(vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) bytes.at(offset + i) = uint8_t(value >> (i * 8));
}

struct Region { uint32_t start, bytes, flags; };
static vector<uint8_t> container(const vector<Region>& regions)
{
	size_t table = 20 + 24 * regions.size();
	vector<uint8_t> result(table + regions.size(), 0);
	put(result, 0, 0xc3f13a6e); result[4] = 3;
	put(result, 8, 0x01050000); put(result, 16, table);
	for (size_t i = 0; i < regions.size(); i++) {
		size_t off = 20 + i * 24;
		put(result, off, 24 << 8); put(result, off + 4, regions[i].flags);
		put(result, off + 8, regions[i].start);
		put(result, off + 12, regions[i].start + regions[i].bytes);
		put(result, off + 16, table + i); put(result, off + 20, table + i + 1);
		result[table + i] = uint8_t(0x91 + i);
	}
	return result;
}

struct Guarded {
	uint8_t* allocation;
	uint8_t* data;
	size_t size;
	explicit Guarded(size_t bytes) : size(bytes)
	{
		allocation = (uint8_t*)mmap(NULL, bytes + 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(allocation != MAP_FAILED);
		data = allocation + 4096;
		assert(mprotect(data, bytes, PROT_READ | PROT_WRITE) == 0);
	}
	~Guarded() { assert(munmap(allocation, size + 8192) == 0); }
};

// Independent decoder: walk every valid descriptor, rather than asking the
// builder for translations. Also catch orphan/duplicate tables and mappings.
static void decode(const uint8_t* arena, uint64_t physical, size_t tableBytes,
	uint64_t table, unsigned level, uint64_t prefix, std::set<uint64_t>& visited,
	std::map<uint64_t, uint64_t>& mappings)
{
	assert((table & 4095) == 0 && table >= physical && table - physical < tableBytes);
	assert(visited.insert(table).second);
	const uint64_t* entries = (const uint64_t*)(arena + table - physical);
	for (unsigned i = 0; i < 512; i++) {
		uint64_t value = entries[i];
		if (value == 0) continue;
		assert((value & 3) == 3);
		uint64_t address = prefix | (uint64_t(i) << (39 - level * 9));
		if (level == 3) {
			assert(address < UINT64_C(0x100000000));
			assert(mappings.emplace(address, value).second);
		} else {
			assert((value & ~UINT64_C(0xfffffff003)) == 0);
			decode(arena, physical, tableBytes, value & UINT64_C(0xfffffff000),
				level + 1, address, visited, mappings);
		}
	}
}

static void check_layout(const vector<Region>& regions, unsigned tablePages)
{
	auto bytes = container(regions);
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory;
	assert(memory.Plan(image) && memory.TablePages() == tablePages);
	Guarded arena(memory.RequiredBytes());
	const uint64_t bases[] = {0x1000, UINT64_C(0x182300000),
		(UINT64_C(1) << 40) - memory.RequiredBytes()};
	for (uint64_t base : bases) {
		assert(memory.Build(arena.data, arena.size, base));
		std::map<uint64_t, uint64_t> mappings;
		std::set<uint64_t> tables;
		decode(arena.data, base, tablePages * 4096, base, 0, 0, tables, mappings);
		assert(tables.size() == tablePages);
		size_t payloadOffset = tablePages * 4096;
		for (size_t i = 0; i < regions.size(); i++) {
			const Region& region = regions[i];
			for (unsigned p = 0; p < region.bytes; p += 4096) {
				uint64_t virtualAddress = uint64_t(region.start) + p;
				assert(mappings.count(virtualAddress) == 1);
				uint64_t descriptor = mappings.at(virtualAddress);
				assert((descriptor & UINT64_C(0xfffffff000)) == base + payloadOffset + p);
				assert((descriptor & (UINT64_C(1) << 10)) != 0); // AF
				assert(((descriptor >> 6) & 3) == ((region.flags & 2) ? 1 : 3));
				assert(((descriptor >> 53) & 3) == ((region.flags & 4) ? 0 : 3));
				bool cached = (region.flags & 24) == 8;
				assert(((descriptor >> 2) & 7) == (cached ? 1 : 0));
				assert(((descriptor >> 8) & 3) == (cached ? 3 : 2));
				mappings.erase(virtualAddress);
			}
			for (unsigned n = 0; n < region.bytes; n++)
				assert(arena.data[payloadOffset + n] == (n == 0 ? uint8_t(0x91 + i) : 0));
			payloadOffset += region.bytes;
		}
		assert(mappings.empty() && payloadOffset == arena.size);
	}
	for (uint64_t base : {UINT64_C(1), UINT64_C(0x10000000000),
		(UINT64_C(1) << 40) - 4096, UINT64_MAX - 4095}) {
		memset(arena.data, 0x5a, arena.size);
		assert(!memory.Build(arena.data, arena.size, base));
		assert(memory.SharedData() == NULL && memory.RootPhysical() == 0);
		for (size_t n = 0; n < arena.size; n++) assert(arena.data[n] == 0x5a);
	}
	assert(!memory.Build(arena.data, arena.size - 1, 0x1000));
	assert(!memory.Build(arena.data + 1, arena.size, 0x1000));
	assert(!memory.Build(NULL, arena.size, 0x1000));
	FirmwareImage empty;
	assert(!memory.Plan(empty) && memory.RequiredBytes() == 0);
	assert(!memory.Build(arena.data, arena.size, 0x1000));
}

static vector<uint32_t> shared_fixture()
{
	vector<uint32_t> w(65536 / 4, 0);
	w[0] = 0x01050000; w[4] = 8; w[5] = 0xa0;
	unsigned cursor = 0x2000;
	auto allocate = [&](unsigned bytes) {
		uint32_t address = 0x04000000 + cursor;
		cursor += (bytes + 7) & ~7;
		return address;
	};
	w[2] = allocate(136); w[3] = allocate(28);
	for (unsigned g = 0; g < 8; g++) {
		unsigned group = (0x1000 + g * 0xa0) / 4;
		w[group + 1] = allocate(88); w[group + 2] = allocate(32);
		w[group + 5] = 8; w[group + 6] = 12;
		for (unsigned s = 0; s < 8; s++) {
			unsigned stream = group + 16 + s * 3;
			w[stream] = 0x7085f;
			w[stream + 1] = allocate(88); w[stream + 2] = allocate(216);
		}
	}
	assert(cursor < 65536);
	return w;
}

static void interface_checks()
{
	auto good = shared_fixture();
	InterfaceInfo info;
	assert(InspectInterface(good.data(), 65536, 0x04000000, info));
	assert(info.groupCount == 8 && info.streamCount == 8 && info.workRegisters == 96);
	for (unsigned word : {0u, 2u, 3u, 4u, 5u, 1025u, 1026u, 1029u, 1030u,
		1040u, 1041u, 1042u, 1340u, 1341u, 1342u}) {
		for (uint32_t value : {0u, 1u, 0xffffffffu, 0x04000000u, 0x0400fff8u}) {
			auto damaged = good;
			damaged[word] = value;
			assert(!InspectInterface(damaged.data(), 65536, 0x04000000, info));
			assert(info.version == 0);
		}
	}
	auto overlap = good; overlap[1041] = good[2];
	assert(!InspectInterface(overlap.data(), 65536, 0x04000000, info));
	overlap = good; overlap[1081] = good[1041];
	assert(!InspectInterface(overlap.data(), 65536, 0x04000000, info));
	Guarded guarded(65536);
	memcpy(guarded.data, good.data(), guarded.size);
	for (size_t size : {size_t(0), size_t(4), size_t(31), size_t(32), size_t(4096), size_t(16384)})
		assert(!InspectInterface(guarded.data, size, 0x04000000, info));
	assert(!InspectInterface(guarded.data + 1, guarded.size, 0x04000000, info));
	assert(!InspectInterface(guarded.data, guarded.size, 0xfffff000, info));
}

int main(int argc, char** argv)
{
	check_layout({{0x400000, 65536, 9}, {0x410000, 65536, 9}, {0, 65536, 9},
		{0x800000, 131072, 13}, {0x2000000, 262144, 0x8000000b},
		{0x1000000, 262144, 0x80000009}, {0x4000000, 65536, 0xc000001b}}, 9);
	check_layout({{0x3fffe000, 16384, 13}, {0xbffff000, 8192, 9},
		{0x4000000, 65536, 0xc000001b}}, 11);
	interface_checks();
	if (argc == 2) {
		FILE* file = fopen(argv[1], "rb"); assert(file);
		vector<uint8_t> bytes(282624);
		assert(fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
		assert(fgetc(file) == EOF && !ferror(file)); fclose(file);
		FirmwareImage image;
		assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
		FirmwareMemory memory;
		assert(memory.Plan(image) && memory.TablePages() == 9 && memory.RequiredBytes() == 954368);
		Guarded arena(memory.RequiredBytes());
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x123400000)));
		std::map<uint64_t, uint64_t> mappings; std::set<uint64_t> tables;
		decode(arena.data, memory.RootPhysical(), 9 * 4096, memory.RootPhysical(), 0, 0, tables, mappings);
		assert(mappings.size() == 224 && tables.size() == 9);
		puts("MALI_CSF_OFFICIAL_TABLES_PASS pages=224 tables=9 bytes=954368 gpu_started=0");
	}
	puts("MALI_CSF_MEMORY_TEST_PASS");
	return 0;
}
