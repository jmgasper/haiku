/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_RESOURCES_H
#define MALI_CSF_RESOURCES_H


#include <stddef.h>
#include <stdint.h>
#include <string.h>


namespace MaliCSF {

static const uint32_t kGetResources = 0x4d435300;
static const uint32_t kResourceVersion = 1;
static const uint32_t kDescriptionValidated = 1;

// Pointer-free diagnostic ABI. These values describe firmware resources;
// they do not report GPU power, MMIO access, firmware execution or rendering.
struct ResourceInfo {
	uint32_t version;
	uint32_t flags;
	uint64_t gpuBase;
	uint64_t gpuSize;
	uint64_t clockBase;
	uint64_t clockSize;
	uint64_t powerBase;
	uint64_t powerSize;
	uint64_t interruptBase;
	uint32_t interrupts[3]; // job, mmu, gpu
	uint32_t clockIds[3]; // core, coregroup, stacks
	uint32_t powerDomain;
	uint32_t supplyPhandle;
	uint32_t supplyMinMicrovolt;
	uint32_t supplyMaxMicrovolt;
	char supplyName[32];
	char boardCompatible[32];
};


inline bool
ReadCells(const void* data, int length, uint32_t* cells, size_t count)
{
	if (data == NULL || cells == NULL || length < 0
		|| count > SIZE_MAX / 4 || (size_t)length != count * 4) {
		return false;
	}
	const uint8_t* bytes = (const uint8_t*)data;
	for (size_t i = 0; i < count; i++) {
		cells[i] = (uint32_t)bytes[i * 4] << 24
			| (uint32_t)bytes[i * 4 + 1] << 16
			| (uint32_t)bytes[i * 4 + 2] << 8
			| bytes[i * 4 + 3];
	}
	return true;
}


inline int
StringIndex(const void* data, int length, const char* wanted)
{
	if (data == NULL || wanted == NULL || length <= 0)
		return -1;
	const char* cursor = (const char*)data;
	int result = -1;
	for (int index = 0; length > 0; index++) {
		const char* end = (const char*)memchr(cursor, 0, length);
		if (end == NULL || end == cursor)
			return -1;
		if (strcmp(cursor, wanted) == 0) {
			if (result >= 0)
				return -1;
			result = index;
		}
		length -= end - cursor + 1;
		cursor = end + 1;
	}
	return result;
}


inline bool
ResourcesMatch(const ResourceInfo& info)
{
	// The owner's ROCK 5 ITX v1.12, with the installed EDK2 v1.1 mainline DT.
	// Keep admission centralized before adding hardware access to this driver.
	return info.version == kResourceVersion
		&& info.flags == kDescriptionValidated
		&& info.gpuBase == 0xfb000000 && info.gpuSize == 0x200000
		&& info.clockBase == 0xfd7c0000 && info.clockSize == 0x5c000
		&& info.powerBase == 0xfd8d8000 && info.powerSize == 0x400
		&& info.interruptBase == 0xfe600000
		&& info.interrupts[0] == 124 && info.interrupts[1] == 125
		&& info.interrupts[2] == 126
		&& info.clockIds[0] == 262 && info.clockIds[1] == 263
		&& info.clockIds[2] == 264
		&& info.powerDomain == 12 && info.supplyPhandle != 0
		&& info.supplyPhandle != UINT32_MAX
		&& info.supplyMinMicrovolt == 550000
		&& info.supplyMaxMicrovolt == 950000
		&& memcmp(info.supplyName, "vdd_gpu_s0", 11) == 0
		&& memcmp(info.boardCompatible, "radxa,rock-5-itx", 17) == 0;
}

} // namespace MaliCSF

#endif // MALI_CSF_RESOURCES_H
