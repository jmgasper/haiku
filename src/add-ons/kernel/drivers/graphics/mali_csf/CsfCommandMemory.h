/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_COMMAND_MEMORY_H
#define MALI_CSF_COMMAND_MEMORY_H

#include "CsfMemory.h"

namespace MaliCSF {

static const uint32_t kQueueInterfaceAddress = 0x04010000;
static const uint32_t kSuspendAddress = 0x04020000;
static const uint32_t kProtectedSuspendAddress = 0x04200000;
static const uint32_t kSuspendCapacity = 1024 * 1024;
static const uint64_t kCommandAddress = UINT64_C(0x100000000);
static const uint64_t kDataAddress = UINT64_C(0x100002000);
static const uint64_t kRingAddress = UINT64_C(0x100400000);
static const uint64_t kCompletionAddress = UINT64_C(0x100800000);
static const unsigned kCommandRounds = 2;
static const unsigned kStoreWordIndices[8] = {1, 31, 127, 511, 1023, 1024, 1535, 2046};

inline uint32_t CommandGuard(unsigned round, unsigned word)
{
	return 0xa5c3e271u ^ (round * 0x794a270du) ^ (word * 0x9e3779b9u);
}

inline uint32_t CommandValue(unsigned round, unsigned store)
{
	return 0x6d610000u ^ (round * 0x17213579u) ^ (store * 0x04030107u);
}

// Architecture-10 memory-store program qualified on Linux on this board.
// Host tests compare all 512 bytes with Mesa 25.3.6's generated packer output.
// Stores wait before reusing r2. The final clean and wait precede completion.
inline void BuildStoreCommands(uint64_t* code, unsigned round)
{
	unsigned n = 0;
	code[n++] = UINT64_C(0x0300000000ff0000); // WAIT all eight scoreboards
	code[n++] = UINT64_C(0x1700000000000002); // SET_SB_ENTRY endpoint 2, other 0
	code[n++] = UINT64_C(0x0100000000000000) | kDataAddress; // MOVE48 r0:r1
	for (unsigned i = 0; i < 8; i++) {
		code[n++] = UINT64_C(0x0202000000000000) | CommandValue(round, i);
		code[n++] = UINT64_C(0x1502000000010000) | (kStoreWordIndices[i] * 4);
		code[n++] = UINT64_C(0x0300000000010000);
	}
	code[n++] = UINT64_C(0x0204000000000000); // MOVE32 r4, latest flush ID 0
	code[n++] = UINT64_C(0x2400040000000211); // FLUSH_CACHE2 clean L2/LSC, other invalidate
	code[n++] = UINT64_C(0x0300000000010000);
	code[n++] = 0;
	code[n++] = 0;
}

// Linux 6.18.52 panthor_sched.c prepare_job_instrs(), MIT license option.
// Registers 92..95 are reserved by the admitted 96-register interface. Each
// round gets its own initially-zero 64-bit sync object, with status at +8.
inline void BuildCommandWrapper(uint64_t* ring, unsigned round)
{
	const uint64_t instructions[] = {
		UINT64_C(0x025e000000000000), // MOVE32 r94, latest flush ID 0
		UINT64_C(0x24005e0000000233), // FLUSH_CACHE2 clean/invalidate all, signal 0
		UINT64_C(0x015c000000000000) | (kCommandAddress + round * 256),
		UINT64_C(0x025e000000000100), // MOVE32 r94, CALL length 256
		UINT64_C(0x0300000000010000), // WAIT 0
		UINT64_C(0x20005c5e00000000), // CALL r92:r93, r94
		UINT64_C(0x015c000000000000) | (kCompletionAddress + round * 32),
		UINT64_C(0x015e000000000001), // MOVE48 r94:r95, 1
		UINT64_C(0x0300000000ff0000), // WAIT all
		UINT64_C(0x33005c5e00000001), // SYNC_ADD64 system scope, propagate error
		UINT64_C(0x2f00000000000000), // ERROR_BARRIER
		0, 0, 0, 0, 0
	};
	memcpy(ring, instructions, sizeof(instructions));
}

// A bounded first submission, not a userspace BO/VM allocator. Unlike the MCU
// image, the application address space exercises the level-1 entry above 4 GiB.
// Only the four advertised buffers are mapped; all holes remain invalid.
class CommandMemory {
public:
	bool Plan(const FirmwareImage& image)
	{
		fData = NULL;
		fPhysical = 0;
		const FirmwareWorkspace workspace[] = {
			{kQueueInterfaceAddress, 8192, false},
			{kSuspendAddress, kSuspendCapacity, true},
			{kProtectedSuspendAddress, kSuspendCapacity, true}
		};
		return fFirmware.Plan(image, workspace, 3);
	}
	size_t RequiredBytes() const
	{
		return fFirmware.RequiredBytes() == 0 ? 0 : fFirmware.RequiredBytes() + kUserBytes;
	}
	bool Build(void* data, size_t bytes, uint64_t physical)
	{
		fData = NULL;
		fPhysical = 0;
		fFirmware.InvalidateMapping();
		size_t required = RequiredBytes();
		if (required == 0 || data == NULL || (uintptr_t(data) & 4095) != 0
			|| bytes < required || (physical & 4095) != 0
			|| physical >= (UINT64_C(1) << 40)
			|| required > (UINT64_C(1) << 40) - physical)
			return false;
		if (!fFirmware.Build(data, fFirmware.RequiredBytes(), physical))
			return false;
		uint8_t* user = (uint8_t*)data + fFirmware.RequiredBytes();
		uint64_t base = physical + fFirmware.RequiredBytes();
		memset(user, 0, kUserBytes);
		((uint64_t*)user)[0] = (base + 4096) | 3;
		((uint64_t*)(user + 4096))[4] = (base + 8192) | 3;
		uint64_t* level2 = (uint64_t*)(user + 8192);
		level2[0] = (base + 3 * 4096) | 3;
		level2[2] = (base + 4 * 4096) | 3;
		level2[4] = (base + 5 * 4096) | 3;
		const uint64_t dataFlags = 3 | (1u << 10) | (1u << 6) | (UINT64_C(3) << 53);
		const uint64_t cached = (1u << 2) | (3u << 8), uncached = 2u << 8;
		uint64_t* leaves = (uint64_t*)(user + 3 * 4096);
		leaves[0] = (base + kCodeOffset) | 3 | (1u << 10) | (3u << 6) | cached;
		leaves[2] = (base + kDataOffset) | dataFlags | cached;
		leaves[3] = (base + kDataOffset + 4096) | dataFlags | cached;
		leaves = (uint64_t*)(user + 4 * 4096);
		for (unsigned i = 0; i < 16; i++)
			leaves[i] = (base + kRingOffset + i * 4096) | dataFlags | uncached;
		((uint64_t*)(user + 5 * 4096))[0] = (base + kCompletionOffset) | dataFlags | uncached;
		fData = user;
		fPhysical = base;
		for (unsigned round = 0; round < kCommandRounds; round++) {
			BuildStoreCommands((uint64_t*)Code() + round * 32, round);
			BuildCommandWrapper((uint64_t*)Ring() + round * 16, round);
		}
		PrepareData(0);
		return true;
	}
	FirmwareMemory& Firmware() { return fFirmware; }
	uint64_t RootPhysical() const { return fPhysical; }
	void* Code() const { return At(kCodeOffset); }
	void* Data() const { return At(kDataOffset); }
	void* Ring() const { return At(kRingOffset); }
	void* Completion() const { return At(kCompletionOffset); }
	bool PrepareData(unsigned round)
	{
		if (fData == NULL || round >= kCommandRounds)
			return false;
		uint32_t* words = (uint32_t*)Data();
		for (unsigned word = 0; word < 2048; word++)
			words[word] = CommandGuard(round, word);
		return true;
	}
	static const unsigned kUserTablePages = 6;
	static const unsigned kCodeOffset = kUserTablePages * 4096;
	static const unsigned kDataOffset = kCodeOffset + 4096;
	static const unsigned kRingOffset = kDataOffset + 8192;
	static const unsigned kCompletionOffset = kRingOffset + 65536;
	static const unsigned kUserBytes = kCompletionOffset + 4096;
private:
	void* At(unsigned offset) const { return fData == NULL ? NULL : fData + offset; }
	FirmwareMemory fFirmware;
	uint8_t* fData = NULL;
	uint64_t fPhysical = 0;
};

} // namespace MaliCSF
#endif
