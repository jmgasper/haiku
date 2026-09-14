/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_POWER_H
#define MALI_CSF_POWER_H


#include "CsfPlatform.h"


namespace MaliCSF {

static const uint32_t kCycleIdentity = 0x4d435302;
static const uint32_t kIdentityVersion = 1;
static const uint32_t kIdentityRead = 1;
static const uint32_t kIdentityRestored = 2;
static const uint32_t kIdentityChangedRegisters = 4;
static const uint32_t kIdentityNeedsRecovery = 8;

enum IdentityResult {
	kIdentityOK = 0,
	kInitialStateMismatch,
	kClockReadbackFailed,
	kInitialIdleFailed,
	kPowerUpFailed,
	kDeIdleFailed,
	kPoweredStateMismatch,
	kGpuMappingFailed,
	kGpuIdentityMismatch,
	kRestoreIdleFailed,
	kPowerDownFailed,
	kRestoreRegistersFailed,
	kPowerStateUncertain,
	kRestoreNotAttempted
};

// All fields are output. Clock frequency is the admitted firmware description
// divided by four, not a measurement. A failed operation retains its result
// independently of restoration. No firmware, DMA or GPU command is issued.
struct IdentityInfo {
	uint32_t version;
	uint32_t result;
	uint32_t restoreResult;
	uint32_t flags;
	uint32_t clockHertz;
	uint32_t gpuID;
	uint32_t csfID;
	uint32_t mmuFeatures;
	uint32_t addressSpaces;
	uint32_t shaderPresentLow;
	uint32_t shaderPresentHigh;
	uint32_t gpuRevision;
	int64_t startedMicros;
	int64_t finishedMicros;
	PlatformSnapshot before;
	PlatformSnapshot powered;
	PlatformSnapshot after;
};


template<typename IO>
void
SnapshotPlatform(IO& io, PlatformSnapshot& snapshot)
{
	snapshot = {};
	snapshot.version = kPlatformVersion;
	snapshot.flags = kPlatformReadOnly;
	snapshot.startedMicros = io.Now();
	for (unsigned i = 0; i < 3; i++)
		snapshot.clockSelect[i] = io.ReadClock(kClockSelectOffsets[i]);
	for (unsigned i = 0; i < 2; i++)
		snapshot.clockGate[i] = io.ReadClock(kClockGateOffsets[i]);
	snapshot.idleRequest = io.ReadPower(kIdleRequestOffset);
	snapshot.idleAck = io.ReadPower(kIdleAckOffset);
	snapshot.idleStatus = io.ReadPower(kIdleStatusOffset);
	snapshot.powerRequest = io.ReadPower(kPowerRequestOffset);
	snapshot.powerRepair = io.ReadPower(kPowerRepairOffset);
	snapshot.finishedMicros = io.Now();
}


inline bool
IdentityClockMatches(const PlatformSnapshot& snapshot, uint32_t divider)
{
	return snapshot.version == kPlatformVersion && snapshot.flags == kPlatformReadOnly
		&& (snapshot.clockSelect[0] & 0x40ff) == (0x80 | divider)
		&& (snapshot.clockSelect[1] & 0x7fff) == 0
		&& (snapshot.clockSelect[2] & 0x7fff) == 0
		&& (snapshot.clockGate[0] & 0x7fda) == 0
		&& (snapshot.clockGate[1] & 4) == 0;
}


inline bool
IdentityInitialStateMatches(const PlatformSnapshot& snapshot)
{
	return IdentityClockMatches(snapshot, 0)
		&& (snapshot.idleRequest & 1) == 0 && (snapshot.idleAck & 1) == 1
		&& (snapshot.idleStatus & 1) == 1 && (snapshot.powerRequest & 1) == 1
		&& (snapshot.powerRepair & 2) == 0;
}


inline bool
IdentityPoweredStateMatches(const PlatformSnapshot& snapshot)
{
	return IdentityClockMatches(snapshot, 3)
		&& (snapshot.idleRequest & 1) == 0 && (snapshot.idleAck & 1) == 0
		&& (snapshot.idleStatus & 1) == 0 && (snapshot.powerRequest & 1) == 0
		&& (snapshot.powerRepair & 2) == 2;
}


template<typename IO, typename Condition>
bool
WaitForPower(IO& io, Condition condition)
{
	const int64_t start = io.Now();
	if (start < 0)
		return false;
	// The iteration bound also terminates a frozen clock. A backwards clock
	// fails immediately. PMU reads themselves use the already tested AO window.
	for (unsigned i = 0; i < 1001; i++) {
		const int64_t now = io.Now();
		if (now < start || now - start > 10000)
			return false;
		if (condition())
			return true;
		io.Pause();
	}
	return false;
}


template<typename IO>
bool
WaitIdle(IO& io, bool idle)
{
	return WaitForPower(io, [&]() {
		return (io.ReadPower(kIdleAckOffset) & 1) == (uint32_t)idle
			&& (io.ReadPower(kIdleStatusOffset) & 1) == (uint32_t)idle;
	});
}


template<typename IO>
bool
WaitRepair(IO& io, bool repaired)
{
	return WaitForPower(io, [&]() {
		return (io.ReadPower(kPowerRepairOffset) & 2) == (repaired ? 2u : 0u);
	});
}


template<typename IO>
void
CycleIdentity(IO& io, IdentityInfo& info)
{
	info = {};
	info.version = kIdentityVersion;
	info.restoreResult = kRestoreNotAttempted;
	info.startedMicros = io.Now();
	SnapshotPlatform(io, info.before);
	if (!IdentityInitialStateMatches(info.before)) {
		info.result = kInitialStateMismatch;
		info.after = info.before;
		info.finishedMicros = io.Now();
		return;
	}

	bool powerRequested = false;
	bool powerReady = false;
	info.flags = kIdentityChangedRegisters;
	info.clockHertz = 702000000 / 4;
	do {
		// Only the GPU divisor changes. SPLL and the existing gates stay intact.
		io.WriteClock(kClockSelectOffsets[0], 0x001f0003);
		if (io.ReadClock(kClockSelectOffsets[0]) != (info.before.clockSelect[0] | 3)) {
			info.result = kClockReadbackFailed;
			break;
		}
		io.WritePower(kIdleRequestOffset, 0x00010001);
		if ((io.ReadPower(kIdleRequestOffset) & 1) != 1 || !WaitIdle(io, true)) {
			info.result = kInitialIdleFailed;
			break;
		}
		io.WritePower(kPowerRequestOffset, 0x00010000);
		powerRequested = true;
		if ((io.ReadPower(kPowerRequestOffset) & 1) != 0 || !WaitRepair(io, true)) {
			info.result = kPowerUpFailed;
			break;
		}
		powerReady = true;
		io.WritePower(kIdleRequestOffset, 0x00010000);
		if ((io.ReadPower(kIdleRequestOffset) & 1) != 0 || !WaitIdle(io, false)) {
			info.result = kDeIdleFailed;
			break;
		}
		SnapshotPlatform(io, info.powered);
		if (!IdentityPoweredStateMatches(info.powered)) {
			info.result = kPoweredStateMismatch;
			break;
		}
		if (!io.MapGpu()) {
			info.result = kGpuMappingFailed;
			break;
		}
		// Static CSF register layout: Panthor v6.12 panthor_regs.h (MIT option).
		info.flags |= kIdentityRead;
		info.gpuID = io.ReadGpu(0);
		if (info.gpuID == 0xa8670005) {
			info.mmuFeatures = io.ReadGpu(0x14);
			info.addressSpaces = io.ReadGpu(0x18);
			info.csfID = io.ReadGpu(0x1c);
			info.shaderPresentLow = io.ReadGpu(0x100);
			info.shaderPresentHigh = io.ReadGpu(0x104);
			info.gpuRevision = io.ReadGpu(0x280);
		}
		io.UnmapGpu();
		info.result = info.gpuID == 0xa8670005 && info.mmuFeatures == 0x2830
			&& info.addressSpaces == 0xff && info.csfID == 0x040a0412
			&& info.shaderPresentLow == 0x50005 && info.shaderPresentHigh == 0
			&& info.gpuRevision == 0 ? kIdentityOK : kGpuIdentityMismatch;
	} while (false);

	bool safeToRestore = !powerRequested;
	if (powerRequested && !powerReady) {
		// Repair=0 after a failed power-up does not prove that an opposite
		// power-down transition completed. Leave the conservative clock and
		// retain this failure for controller recovery; never access the GPU.
		info.restoreResult = kPowerStateUncertain;
	} else if (powerReady) {
		io.WritePower(kIdleRequestOffset, 0x00010001);
		if ((io.ReadPower(kPowerRepairOffset) & 2) != 2
			|| (io.ReadPower(kIdleRequestOffset) & 1) != 1 || !WaitIdle(io, true)) {
			info.restoreResult = kRestoreIdleFailed;
		} else {
			io.WritePower(kPowerRequestOffset, 0x00010001);
			if ((io.ReadPower(kPowerRequestOffset) & 1) != 1 || !WaitRepair(io, false))
				info.restoreResult = kPowerDownFailed;
			else
				safeToRestore = true;
		}
	}
	if (safeToRestore) {
		// Verify the off state again before raising the original clock rate.
		if ((io.ReadPower(kPowerRequestOffset) & 1) != 1
			|| (io.ReadPower(kPowerRepairOffset) & 2) != 0 || !WaitIdle(io, true)) {
			info.restoreResult = kPowerStateUncertain;
			safeToRestore = false;
		} else {
			io.WritePower(kIdleRequestOffset, 0x00010000 | (info.before.idleRequest & 1));
			io.WriteClock(kClockSelectOffsets[0], 0x001f0000 | (info.before.clockSelect[0] & 31));
			info.restoreResult = kIdentityOK;
		}
	}
	SnapshotPlatform(io, info.after);
	// These ten registers stayed identical across the native baseline's boots.
	// Comparing all their bits also detects unexpected changes outside our mask.
	if (safeToRestore && info.restoreResult == kIdentityOK) {
		if (memcmp((const uint8_t*)&info.after + offsetof(PlatformSnapshot, clockSelect),
				(const uint8_t*)&info.before + offsetof(PlatformSnapshot, clockSelect),
				sizeof(PlatformSnapshot) - offsetof(PlatformSnapshot, clockSelect)) == 0) {
			info.flags |= kIdentityRestored;
		} else
			info.restoreResult = kRestoreRegistersFailed;
	}
	if ((info.flags & kIdentityRestored) == 0)
		info.flags |= kIdentityNeedsRecovery;
	info.finishedMicros = io.Now();
}

} // namespace MaliCSF

#endif // MALI_CSF_POWER_H
