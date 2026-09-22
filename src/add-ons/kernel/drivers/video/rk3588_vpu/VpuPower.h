/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_VPU_POWER_H
#define RK3588_VPU_POWER_H

#include "VpuInterface.h"

namespace RK3588Vpu {

static const uint32_t kVdpuGate = 1u << 10;
static const uint32_t kVdpuRepair = 1u << 9;
static const uint32_t kVdpuIdle = 1u << 8;
static const uint32_t kRkvdec0Gate = 1u << 8;
static const uint32_t kRkvdec0Repair = 1u << 7;
static const uint32_t kRkvdec0Idle = 1u << 6;
static const uint32_t kRkvdec0Memory = 1u << 16;
static const uint32_t kAv1Gate = 1u << 12;
static const uint32_t kAv1Repair = 1u << 11;
static const uint32_t kAv1Idle = 1u << 9;
static const uint32_t kAv1Memory = 1u << 20;

// PMU hiword write-mask format. Sources: RK3588 TRM v1.0 Part 1 and
// Linux 6.18.52 drivers/pmdomain/rockchip/pm-domains.c.
static const uint32_t kPowerGateOffset = 0x14c;
static const uint32_t kIdleRequestOffset = 0x10c;

inline bool
InitialStateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kVdpuGate) != 0
		&& (state.powerStatus & kVdpuGate) != 0
		&& (state.repairStatus & kVdpuRepair) == 0
		&& (state.repairStatus & ((1u << 7) | (1u << 8) | (1u << 11))) == 0
		&& (state.idleRequest & kVdpuIdle) == 0
		&& (state.idleAck & kVdpuIdle) != 0
		&& (state.idleStatus & kVdpuIdle) != 0
		&& state.clockSelect98 == 0x21
		&& state.clockGate44 == 0 && state.clockGate45 == 0;
}

inline bool
RestoredStateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kVdpuGate) == (before.powerGate & kVdpuGate)
		&& (after.powerStatus & kVdpuGate) == (before.powerStatus & kVdpuGate)
		&& (after.repairStatus & kVdpuRepair) == (before.repairStatus & kVdpuRepair)
		&& (after.idleRequest & kVdpuIdle) == (before.idleRequest & kVdpuIdle)
		&& (after.idleAck & kVdpuIdle) == (before.idleAck & kVdpuIdle)
		&& (after.idleStatus & kVdpuIdle) == (before.idleStatus & kVdpuIdle)
		&& after.clockSelect98 == before.clockSelect98
		&& after.clockSelect89 == before.clockSelect89
		&& after.clockSelect90 == before.clockSelect90
		&& after.clockSelect91 == before.clockSelect91
		&& after.clockSelect163 == before.clockSelect163
		&& after.clockGate40 == before.clockGate40
		&& after.clockGate41 == before.clockGate41
		&& after.clockGate44 == before.clockGate44
		&& after.clockGate45 == before.clockGate45
		&& after.clockGate68 == before.clockGate68
		&& after.softReset68 == before.softReset68;
}

// Hardware supplies SnapshotNow(), WritePower(offset, hiwordValue), and
// WaitPower(mask, set) / WaitIdle(mask, set). Each wait is time bounded.
template<class Hardware, class Probe>
void
CycleVdpuPower(Hardware& hardware, PowerCycle& output, Probe probe)
{
	PowerCycle result = {};
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialStateMatches(result.before)) {
		result.after = result.before;
		output = result;
		return;
	}

	// All VDPU parent-domain clocks are ungated in the admitted firmware state.
	// No CRU word is written. Clear bit 10 to request power on.
	hardware.WritePower(kPowerGateOffset, kVdpuGate << 16);
	result.flags |= 1;
	if (!hardware.WaitPower(kVdpuRepair, true)) {
		result.result = kPowerOnTimeout;
		goto restore;
	}

	// Leave NIU idle after the domain reports power and memory repair.
	hardware.WritePower(kIdleRequestOffset, kVdpuIdle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kVdpuIdle, false)) {
		result.result = kPowerIdleTimeout;
		goto restore;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	probe();

restore:
	// A timeout may still leave the domain powered. Request NIU idle before
	// switching it off, then restore the original idle-request bit.
	if (hardware.PowerIsOn(kVdpuRepair)) {
		hardware.WritePower(kIdleRequestOffset,
			(kVdpuIdle << 16) | kVdpuIdle);
		if (!hardware.WaitIdle(kVdpuIdle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset, (kVdpuGate << 16) | kVdpuGate);
	if (!hardware.WaitPower(kVdpuRepair, false))
		result.restoreResult = kPowerOffTimeout;
	hardware.WritePower(kIdleRequestOffset, kVdpuIdle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredStateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
	output = result;
}

template<class Hardware>
void
CycleVdpuPower(Hardware& hardware, PowerCycle& output)
{
	CycleVdpuPower(hardware, output, [] {});
}

inline bool
InitialRkvdec0StateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kRkvdec0Gate) != 0
		&& (state.powerStatus & kRkvdec0Gate) != 0
		&& (state.repairStatus & kRkvdec0Repair) == 0
		&& (state.memoryStatus & kRkvdec0Memory) != 0
		&& (state.idleRequest & kRkvdec0Idle) == 0
		&& (state.idleAck & kRkvdec0Idle) != 0
		&& (state.idleStatus & kRkvdec0Idle) != 0
		&& (state.powerGate & (1u << 2)) == 0
		&& (state.powerStatus & (1u << 2)) == 0
		// Linux models RKVDEC0 under both VCODEC and VDPU. VCODEC has
		// no repair-status bit; its power gate/status above indicate on.
		&& (state.repairStatus & kVdpuRepair) != 0
		&& (state.powerStatus & kVdpuGate) == 0
		&& state.clockGate40 == 0
		&& state.clockSelect89 == 0x8100
		&& state.clockSelect90 == 0x1801
		&& state.clockSelect91 == 0x1;
}

inline bool
RestoredRkvdec0StateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kRkvdec0Gate)
			== (before.powerGate & kRkvdec0Gate)
		&& (after.powerStatus & kRkvdec0Gate)
			== (before.powerStatus & kRkvdec0Gate)
		&& (after.repairStatus & kRkvdec0Repair)
			== (before.repairStatus & kRkvdec0Repair)
		&& (after.memoryStatus & kRkvdec0Memory)
			== (before.memoryStatus & kRkvdec0Memory)
		&& (after.idleRequest & kRkvdec0Idle)
			== (before.idleRequest & kRkvdec0Idle)
		&& (after.idleAck & kRkvdec0Idle)
			== (before.idleAck & kRkvdec0Idle)
		&& (after.idleStatus & kRkvdec0Idle)
			== (before.idleStatus & kRkvdec0Idle)
		&& after.clockGate40 == before.clockGate40
		&& after.clockSelect89 == before.clockSelect89
		&& after.clockSelect90 == before.clockSelect90
		&& after.clockSelect91 == before.clockSelect91
		&& after.clockSelect163 == before.clockSelect163
		&& after.softReset68 == before.softReset68;
}

// Core 0 is missing from this EDK2 FDT. This diagnostic is restricted to the
// validated Rock 5 ITX board, exact observed clock state and an opt-in setting.
// The register and PMU layout is from Linux mainline 75f2c0b36907.
template<class Hardware, class Probe>
void
CycleRkvdec0Power(Hardware& hardware, PowerCycle& output, Probe probe)
{
	PowerCycle result = {};
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialRkvdec0StateMatches(result.before)) {
		result.after = result.before;
		output = result;
		return;
	}

	hardware.WritePower(kPowerGateOffset, kRkvdec0Gate << 16);
	result.flags |= 1;
	if (!hardware.WaitRkvdec0Power(true)) {
		result.result = kPowerOnTimeout;
		goto restore;
	}
	if (!hardware.WaitRkvdec0Memory(true)) {
		result.result = kPowerMemoryTimeout;
		goto restore;
	}
	hardware.WritePower(kIdleRequestOffset, kRkvdec0Idle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kRkvdec0Idle, false)) {
		result.result = kPowerIdleTimeout;
		goto restore;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	probe();

restore:
	if (hardware.PowerIsOn(kRkvdec0Repair)) {
		hardware.WritePower(kIdleRequestOffset,
			(kRkvdec0Idle << 16) | kRkvdec0Idle);
		if (!hardware.WaitIdle(kRkvdec0Idle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset,
		(kRkvdec0Gate << 16) | kRkvdec0Gate);
	if (!hardware.WaitRkvdec0Power(false))
		result.restoreResult = kPowerOffTimeout;
	if (!hardware.WaitRkvdec0Memory(false))
		result.restoreResult = kPowerMemoryTimeout;
	hardware.WritePower(kIdleRequestOffset, kRkvdec0Idle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredRkvdec0StateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
	output = result;
}

template<class Hardware>
void
CycleRkvdec0Power(Hardware& hardware, PowerCycle& output)
{
	CycleRkvdec0Power(hardware, output, [] {});
}

inline bool
InitialAv1StateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kAv1Gate) != 0
		&& (state.powerStatus & kAv1Gate) != 0
		&& (state.repairStatus & kAv1Repair) == 0
		&& (state.memoryStatus & kAv1Memory) != 0
		&& (state.idleRequest & kAv1Idle) == 0
		&& (state.idleAck & kAv1Idle) != 0
		&& (state.idleStatus & kAv1Idle) != 0
		&& (state.repairStatus & kVdpuRepair) != 0
		&& (state.powerStatus & kVdpuGate) == 0
		&& state.clockSelect163 == 2
		&& state.clockGate68 == 0
		&& (state.softReset68 & 0x36) == 0;
}

inline bool
RestoredAv1StateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kAv1Gate) == (before.powerGate & kAv1Gate)
		&& (after.powerStatus & kAv1Gate) == (before.powerStatus & kAv1Gate)
		&& (after.repairStatus & kAv1Repair)
			== (before.repairStatus & kAv1Repair)
		&& (after.memoryStatus & kAv1Memory)
			== (before.memoryStatus & kAv1Memory)
		&& (after.idleRequest & kAv1Idle) == (before.idleRequest & kAv1Idle)
		&& (after.idleAck & kAv1Idle) == (before.idleAck & kAv1Idle)
		&& (after.idleStatus & kAv1Idle) == (before.idleStatus & kAv1Idle)
		&& after.clockSelect163 == before.clockSelect163
		&& after.clockGate68 == before.clockGate68
		&& after.softReset68 == before.softReset68;
}

template<class Hardware, class Probe>
void
CycleAv1Power(Hardware& hardware, PowerCycle& output, Probe probe)
{
	PowerCycle result = {};
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialAv1StateMatches(result.before)) {
		result.after = result.before;
		output = result;
		return;
	}

	hardware.WritePower(kPowerGateOffset, kAv1Gate << 16);
	result.flags |= 1;
	if (!hardware.WaitAv1Power(true)) {
		result.result = kPowerOnTimeout;
		goto restore;
	}
	if (!hardware.WaitAv1Memory(true)) {
		result.result = kPowerMemoryTimeout;
		goto restore;
	}
	hardware.WritePower(kIdleRequestOffset, kAv1Idle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kAv1Idle, false)) {
		result.result = kPowerIdleTimeout;
		goto restore;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	probe();

restore:
	if (hardware.PowerIsOn(kAv1Repair)) {
		hardware.WritePower(kIdleRequestOffset, (kAv1Idle << 16) | kAv1Idle);
		if (!hardware.WaitIdle(kAv1Idle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset, (kAv1Gate << 16) | kAv1Gate);
	if (!hardware.WaitAv1Power(false))
		result.restoreResult = kPowerOffTimeout;
	if (!hardware.WaitAv1Memory(false))
		result.restoreResult = kPowerMemoryTimeout;
	hardware.WritePower(kIdleRequestOffset, kAv1Idle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredAv1StateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
	output = result;
}

template<class Hardware>
void
CycleAv1Power(Hardware& hardware, PowerCycle& output)
{
	CycleAv1Power(hardware, output, [] {});
}

} // namespace RK3588Vpu

#endif
