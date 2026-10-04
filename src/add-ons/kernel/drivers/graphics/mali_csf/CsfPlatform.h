/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_PLATFORM_H
#define MALI_CSF_PLATFORM_H


#include "CsfResources.h"


namespace MaliCSF {

static const uint32_t kGetPlatformSnapshot = 0x4d435301;
static const uint32_t kPlatformVersion = 1;
static const uint32_t kPlatformReadOnly = 1;

// RK3588 TRM v1.0, Part 1 (2022-03-09), CRU and PMU chapters.
// PMU offsets are relative to the FDT window fd8d8000, not fd8d0000.
static const uint32_t kClockSelectOffsets[] = {0x578, 0x57c, 0x580};
static const uint32_t kClockGateOffsets[] = {0x908, 0x90c};
static const uint32_t kIdleRequestOffset = 0x10c;
static const uint32_t kIdleAckOffset = 0x118;
static const uint32_t kIdleStatusOffset = 0x120;
static const uint32_t kPowerRequestOffset = 0x14c;
static const uint32_t kPowerRepairOffset = 0x290;

// Fixed, pointer-free diagnostic ABI. A sequential observation, not an atomic
// snapshot or permission to access the GPU. No voltage or frequency is measured.
struct PlatformSnapshot {
	uint32_t version;
	uint32_t flags;
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t clockSelect[3];
	uint32_t clockGate[2];
	uint32_t idleRequest;
	uint32_t idleAck;
	uint32_t idleStatus;
	uint32_t powerRequest;
	uint32_t powerRepair;
};

} // namespace MaliCSF

#endif // MALI_CSF_PLATFORM_H
