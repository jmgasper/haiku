/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "mali_heap_commands.h"
#include <algorithm>
#include <array>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdexcept>
#include <unordered_map>

// Independent CS interpreter: v10.xml defines a *signed* 16-bit memory offset.
// Sparse high GPU addresses and unique data expose bad sources, register pairs,
// half-word stores, address wraparound and incorrect batch destinations.
static const uint32_t kGuard = 0xda715bc3;
struct Machine {
	std::array<uint32_t, 96> registers{};
	std::unordered_map<uint64_t, uint32_t> memory;
	std::array<uint32_t, 16384> output;
	std::array<unsigned, 16384> writes{};
	unsigned loads = 0;
	bool pending = false;
	Machine() { output.fill(kGuard); }
	uint64_t Pair(unsigned r) const
	{
		assert(r % 2 == 0 && r + 1 < registers.size());
		return registers[r] | (uint64_t(registers[r + 1]) << 32);
	}
	void Execute(const uint64_t* code, unsigned bytes)
	{
		assert(bytes > 0 && bytes <= 32768 && bytes % 8 == 0);
		for (unsigned i = 0; i < bytes / 8; i++) {
			uint64_t instruction = code[i];
			unsigned opcode = instruction >> 56;
			unsigned base = (instruction >> 48) & 255;
			if (opcode == 1) {
				assert(base % 2 == 0 && base + 1 < registers.size());
				registers[base] = uint32_t(instruction);
				registers[base + 1] = (instruction >> 32) & 65535;
			} else if (opcode == 3) {
				assert(((instruction >> 16) & 1) != 0);
				pending = false;
			} else if (opcode == 23) {
				assert(instruction == UINT64_C(0x1700000000000002));
			} else {
				assert((opcode == 20 || opcode == 21) && !pending);
				assert(((instruction >> 16) & 65535) == 3);
				int offset = instruction & 65535;
				if (offset >= 32768) offset -= 65536;
				uint64_t address = Pair((instruction >> 40) & 255) + int64_t(offset);
				assert(address % 4 == 0 && base + 1 < registers.size());
				for (unsigned word = 0; word < 2; word++, address += 4) {
					if (opcode == 20) {
						auto found = memory.find(address);
						assert(found != memory.end());
						registers[base + word] = found->second;
						loads++;
					} else {
						if (address < kHeapReadbackAddress || address >= kHeapReadbackAddress + 65536)
							throw std::runtime_error("STORE outside readback buffer");
						unsigned at = (address - kHeapReadbackAddress) / 4;
						output[at] = registers[base + word]; writes[at]++;
					}
				}
				pending = true;
			}
		}
		assert(!pending);
	}
};

int main()
{
	std::array<HeapSample, 8192> samples;
	Machine original;
	for (unsigned i = 0; i < samples.size(); i++) {
		uint64_t address = UINT64_C(0x808000200000) + uint64_t(i) * 4096;
		uint64_t value = UINT64_C(0x7129d3c5816ae20b) ^ (uint64_t(i + 1) * UINT64_C(0x9e3779b97f4a7c15));
		samples[i] = {address, value};
		original.memory[address] = uint32_t(value);
		original.memory[address + 4] = value >> 32;
	}
	std::array<uint64_t, 4098> storage;
	const uint64_t codeGuard = UINT64_C(0x9b31ac527468ef0d);
	unsigned programs = 0;
	for (unsigned limit : {5159u, 8192u}) {
		Machine machine = original;
		for (unsigned first = 0; first < limit;) {
			unsigned count = std::min(limit - first, 512u);
			storage.fill(codeGuard);
			unsigned bytes = HeapCopyProgram(storage.data() + 1, samples.data(), first, count);
			assert(storage[0] == codeGuard && bytes > 0 && bytes <= 32768);
			for (unsigned i = bytes / 8 + 1; i < storage.size(); i++) assert(storage[i] == codeGuard);
			machine.Execute(storage.data() + 1, bytes); programs++;
			first += count;
			assert(machine.loads == first * 2);
			for (unsigned i = 0; i < machine.output.size(); i++) {
				uint32_t expected = samples[i / 2].expected >> ((i % 2) * 32);
				assert(machine.output[i] == (i < first * 2 ? expected : kGuard));
				assert(machine.writes[i] == (i < first * 2 ? 1u : 0u));
			}
		}
	}
	// Single-slot generations and batches straddling the signed-offset boundary.
	for (unsigned first : {0u, 1u, 4095u, 4096u, 7680u, 8191u}) {
		Machine machine = original;
		unsigned count = std::min(8192 - first, 512u);
		unsigned bytes = HeapCopyProgram(storage.data(), samples.data(), first, count);
		machine.Execute(storage.data(), bytes); programs++;
		for (unsigned i = 0; i < machine.output.size(); i++) {
			bool touched = i / 2 >= first && i / 2 < first + count;
			uint32_t expected = samples[i / 2].expected >> ((i % 2) * 32);
			assert(machine.output[i] == (touched ? expected : kGuard));
			assert(machine.writes[i] == (touched ? 1u : 0u));
		}
	}
	// Exact failed +206 encoding: the ninth batch writes to base - 32768.
	unsigned bytes = HeapCopyProgram(storage.data(), samples.data(), 4096, 512);
	storage[2] = UINT64_C(0x0104000000000000) | kHeapReadbackAddress;
	for (unsigned i = 0; i < 512; i++)
		storage[6 + i * 5] = UINT64_C(0x1502040000030000) | ((4096 + i) * 8);
	bool rejected = false;
	try { Machine machine = original; machine.Execute(storage.data(), bytes); }
	catch (const std::runtime_error&) { rejected = true; }
	assert(rejected);
	storage.fill(codeGuard);
	for (auto pair : {std::array<unsigned, 2>{8193, 1}, {8192, 1}, {8191, 2},
		{0, 0}, {0, 513}, {UINT_MAX, 2}, {0, UINT_MAX}}) {
		assert(HeapCopyProgram(storage.data(), samples.data(), pair[0], pair[1]) == 0);
		for (auto value : storage) assert(value == codeGuard);
	}
	printf("MALI_HEAP_COMMANDS_PASS programs=%u signed_offset_regression=1 invalid_inputs=7\n", programs);
	return 0;
}
