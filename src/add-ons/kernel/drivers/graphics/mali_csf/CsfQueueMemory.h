/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_QUEUE_MEMORY_H
#define MALI_CSF_QUEUE_MEMORY_H

#include "CsfMemory.h"
#include "CsfQueue.h"

namespace MaliCSF {

static const uint32_t kRuntimeInterfaceAddress = 0x04010000;
static const uint32_t kRuntimeSuspendAddress = 0x04100000;
static const uint32_t kRuntimeProtectedAddress = 0x05000000;
static const uint32_t kRuntimeSuspendBytes = 1024 * 1024;
static const uint64_t kPrivateRingAddress = UINT64_C(1) << 47;
static const uint64_t kPrivateCompletionAddress = kPrivateRingAddress + 65536;

inline bool PlanRuntimeFirmware(FirmwareMemory& memory, const FirmwareImage& image)
{
	const FirmwareWorkspace workspace[] = {
		{kRuntimeInterfaceAddress, kMaxRuntimeQueues * 8192, false},
		{kRuntimeSuspendAddress, kMaxRuntimeQueues * kRuntimeSuspendBytes, true},
		{kRuntimeProtectedAddress, kMaxRuntimeQueues * kRuntimeSuspendBytes, true}
	};
	return memory.Plan(image, workspace, 3);
}

// A queue's root owns only its upper-half kernel subtree. Its lower 256 entries
// point into a leased immutable application VM generation. Never replace those
// entries while this root is active; the scheduler must quiesce and unmap first.
class QueueMemory {
public:
	size_t RequiredBytes() const { return kBytes; }
	bool Build(void* data, size_t bytes, uint64_t physical)
	{
		fData = NULL;
		fPhysical = 0;
		if (data == NULL || (uintptr_t(data) & 4095) != 0 || bytes != kBytes
			|| (physical & 4095) != 0 || physical >= (UINT64_C(1) << 40)
			|| bytes > (UINT64_C(1) << 40) - physical)
			return false;
		memset(data, 0, bytes);
		uint64_t* root = (uint64_t*)data;
		root[256] = (physical + 4096) | 3;
		((uint64_t*)((uint8_t*)data + 4096))[0] = (physical + 8192) | 3;
		((uint64_t*)((uint8_t*)data + 8192))[0] = (physical + 12288) | 3;
		uint64_t* leaves = (uint64_t*)((uint8_t*)data + 12288);
		const uint64_t flags = 3 | (1u << 10) | (1u << 6) | (2u << 8)
			| (UINT64_C(3) << 53); // read/write, NX, GPU uncached
		for (unsigned i = 0; i < 17; i++)
			leaves[i] = (physical + kRingOffset + i * 4096) | flags;
		fData = (uint8_t*)data;
		fPhysical = physical;
		return true;
	}
	bool SetUserRoot(const uint64_t* root)
	{
		if (fData == NULL || root == NULL || (uintptr_t(root) & 4095) != 0)
			return false;
		for (unsigned i = 0; i < 512; i++) {
			uint64_t value = root[i];
			if (value != 0 && (i >= 256 || (value & 3) != 3
				|| (value & ~UINT64_C(0xfffffff003)) != 0))
				return false;
		}
		memcpy(fData, root, 256 * sizeof(uint64_t));
		return true;
	}
	uint64_t RootPhysical() const { return fPhysical; }
	void* Root() const { return fData; }
	void* Ring() const { return fData == NULL ? NULL : fData + kRingOffset; }
	void* Completion() const { return fData == NULL ? NULL : fData + kCompletionOffset; }
	static const unsigned kTablePages = 4;
	static const unsigned kRingOffset = kTablePages * 4096;
	static const unsigned kCompletionOffset = kRingOffset + 65536;
	static const unsigned kBytes = kCompletionOffset + 4096;
private:
	uint8_t* fData = NULL;
	uint64_t fPhysical = 0;
};

// Architecture-10 CSF wrappers, following Linux panthor_sched.c under its MIT
// option. Only reserved registers 92..95 are changed. A post-call cache flush
// publishes application writes before the uncached completion becomes visible.
// 128-byte slots divide the 64 KiB ring exactly, including at wraparound.
inline bool BuildQueueWrapper(uint64_t* output, uint64_t address, uint32_t bytes)
{
	if (output == NULL || (uintptr_t(output) & 7) != 0 || (address & 7) != 0
		|| (bytes & 7) != 0 || bytes > kMaxStreamBytes
		|| (bytes == 0 ? address != 0 : address >= kVmUserLimit || bytes > kVmUserLimit - address))
		return false;
	uint64_t instructions[16] = {};
	unsigned n = 0;
	instructions[n++] = UINT64_C(0x025e000000000000); // MOVE32 r94, flush ID 0
	instructions[n++] = UINT64_C(0x24005e0000000233); // FLUSH_CACHE2 clean/invalidate
	if (bytes != 0) {
		instructions[n++] = UINT64_C(0x015c000000000000) | address;
		instructions[n++] = UINT64_C(0x025e000000000000) | bytes;
	}
	instructions[n++] = UINT64_C(0x0300000000010000); // WAIT 0
	if (bytes != 0)
		instructions[n++] = UINT64_C(0x20005c5e00000000); // CALL r92:r93, r94
	instructions[n++] = UINT64_C(0x0300000000ff0000); // WAIT all scoreboards
	instructions[n++] = UINT64_C(0x025e000000000000);
	instructions[n++] = UINT64_C(0x24005e0000000233);
	instructions[n++] = UINT64_C(0x0300000000010000);
	instructions[n++] = UINT64_C(0x015c000000000000) | kPrivateCompletionAddress;
	instructions[n++] = UINT64_C(0x015e000000000001); // MOVE48 r94:r95, 1
	instructions[n++] = UINT64_C(0x33005c5e00000001); // SYNC_ADD64 system scope
	instructions[n++] = UINT64_C(0x2f00000000000000); // ERROR_BARRIER
	memcpy(output, instructions, sizeof(instructions));
	return true;
}

} // namespace MaliCSF
#endif
