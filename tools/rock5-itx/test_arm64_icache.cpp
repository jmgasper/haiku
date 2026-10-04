/* Instruction aliases on mixed PIPT/VIPT CPUs. Distributed under the MIT License. */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "cache_line_size.h"

#define ROUNDDOWN(value, alignment) ((value) & ~((alignment) - 1))

static constexpr uint64_t kWriter = 0x10000000;
static constexpr size_t kBytes = 8192, kLine = 64;
using Line = std::array<uint8_t, kLine>;
struct CPU {
	bool aliasing;
	std::map<std::pair<size_t, unsigned>, Line> instructions;
};
static std::array<CPU, 8> sCPUs;
static std::array<uint8_t, kBytes> sData, sPoU;
static std::set<size_t> sPendingData;
static std::vector<uint64_t> sAliases;
static bool sPendingInstruction, sSynchronized;
static unsigned sEmitter;

static size_t Offset(uint64_t address)
{
	if (address >= kWriter && address < kWriter + kBytes)
		return address - kWriter;
	for (uint64_t alias : sAliases)
		if (address >= alias && address < alias + kBytes) return address - alias;
	assert(!"unmapped test address");
	return 0;
}
static unsigned Color(const CPU& cpu, uint64_t address)
{
	return cpu.aliasing ? (address >> 12) & 3 : 0;
}
static uint8_t Fetch(CPU& cpu, uint64_t address)
{
	size_t offset = Offset(address), line = offset / kLine;
	auto key = std::make_pair(line, Color(cpu, address));
	auto found = cpu.instructions.find(key);
	if (found == cpu.instructions.end()) {
		Line contents;
		for (size_t i = 0; i < kLine; ++i) contents[i] = sPoU[line * kLine + i];
		found = cpu.instructions.emplace(key, contents).first;
	}
	return found->second[offset % kLine];
}
static uint64_t ReadCTR()
{
	return (4 << 16) | (sCPUs[sEmitter].aliasing ? 2 << 14 : 3 << 14) | 4;
}
static void CleanData(uint64_t address) { sPendingData.insert(Offset(address) / kLine); }
[[maybe_unused]] static void InvalidateAddress(uint64_t address)
{
	assert(sPendingData.empty());
	for (CPU& cpu : sCPUs)
		cpu.instructions.erase(std::make_pair(Offset(address) / kLine, Color(cpu, address)));
	sPendingInstruction = true;
}
[[maybe_unused]] static void InvalidateAll()
{
	assert(sPendingData.empty());
	for (CPU& cpu : sCPUs) cpu.instructions.clear();
	sPendingInstruction = true;
}
static void DSB()
{
	for (size_t line : sPendingData)
		for (size_t i = 0; i < kLine; ++i) sPoU[line * kLine + i] = sData[line * kLine + i];
	sPendingData.clear();
	sPendingInstruction = false;
}
static void ISB()
{
	assert(sPendingData.empty() && !sPendingInstruction);
	sSynchronized = true;
}

#include "icache_production.inc"

static bool Check(bool mixed, unsigned emitter)
{
	sEmitter = emitter;
	sPendingData.clear();
	sPendingInstruction = sSynchronized = false;
	sData.fill(0x11); sPoU = sData;
	sAliases.clear();
	for (unsigned i = 0; i < 4; ++i)
		sAliases.push_back(0x20000000 + i * 0x100000 + ((i + 1) % 4) * 4096);
	for (unsigned i = 0; i < sCPUs.size(); ++i) {
		CPU& cpu = sCPUs[i];
		cpu.aliasing = mixed && i < 4;
		cpu.instructions.clear();
		for (uint64_t alias : sAliases)
			for (size_t j = 0; j < kBytes; j += 4) assert(Fetch(cpu, alias + j) == 0x11);
	}
	// Cover unaligned ends, both page colors and the final partial line.
	for (size_t j = 60; j < 4164; ++j) sData[j] = uint8_t(0x40 + j % 31);
	arch_cpu_sync_icache((void*)(kWriter + 60), 4104);
	assert(sSynchronized);
	size_t stale = 0;
	for (CPU& cpu : sCPUs)
		for (uint64_t alias : sAliases)
			for (size_t j = 0; j < kBytes; ++j)
				if (Fetch(cpu, alias + j) != sData[j]) stale++;
	printf("ICACHE mixed=%u emitter=%u stale_bytes=%zu\n", mixed, emitter, stale);
	return stale == 0;
}

int main()
{
	bool passed = Check(false, 0);
	passed &= Check(true, 0);
	passed &= Check(true, 6);
	if (!passed) return 1;
	puts("ARM64 instruction aliases passed on all eight modeled CPUs");
	return 0;
}
