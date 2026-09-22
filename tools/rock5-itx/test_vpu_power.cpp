/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "../../src/add-ons/kernel/drivers/video/rk3588_vpu/VpuPower.h"

#include <assert.h>
#include <stdio.h>

using namespace RK3588Vpu;

struct FakeHardware {
	Snapshot state = {};
	bool failOn = false;
	bool failIdle = false;
	bool failOff = false;
	bool delayStatus = false;
	bool statusPending = false;
	unsigned writes = 0;

	FakeHardware()
	{
		state.version = kSnapshotVersion;
		state.flags = 1;
		state.powerGate = kVdpuGate;
		state.powerStatus = kVdpuGate;
		state.idleAck = kVdpuIdle;
		state.idleStatus = kVdpuIdle;
		state.clockSelect98 = 0x21;
	}

	Snapshot SnapshotNow() { return state; }
	void WritePower(uint32_t offset, uint32_t value)
	{
		writes++;
		if (offset == kPowerGateOffset) {
			assert((value & (kVdpuGate << 16)) != 0);
			if ((value & kVdpuGate) != 0) {
				state.powerGate |= kVdpuGate;
				if (!failOff) {
					if (delayStatus)
						statusPending = true;
					else
						state.powerStatus |= kVdpuGate;
					state.repairStatus &= ~kVdpuRepair;
					state.idleAck |= kVdpuIdle;
					state.idleStatus |= kVdpuIdle;
				}
			} else {
				state.powerGate &= ~kVdpuGate;
				if (!failOn) {
					state.powerStatus &= ~kVdpuGate;
					state.repairStatus |= kVdpuRepair;
				}
			}
		} else {
			assert(offset == kIdleRequestOffset);
			assert((value & (kVdpuIdle << 16)) != 0);
			if ((value & kVdpuIdle) != 0)
				state.idleRequest |= kVdpuIdle;
			else
				state.idleRequest &= ~kVdpuIdle;
			if ((state.repairStatus & kVdpuRepair) && !failIdle) {
				state.idleAck = (state.idleAck & ~kVdpuIdle)
					| (state.idleRequest & kVdpuIdle);
				state.idleStatus = (state.idleStatus & ~kVdpuIdle)
					| (state.idleRequest & kVdpuIdle);
			}
		}
	}
	bool PowerIsOn(uint32_t mask) { return (state.repairStatus & mask) != 0; }
	bool WaitPower(uint32_t mask, bool on)
	{
		if (statusPending) {
			state.powerStatus |= kVdpuGate;
			statusPending = false;
		}
		return PowerIsOn(mask) == on
			&& ((state.powerStatus & kVdpuGate) != 0) != on;
	}
	bool WaitIdle(uint32_t mask, bool idle)
	{
		return ((state.idleAck & mask) != 0) == idle
			&& ((state.idleStatus & mask) != 0) == idle;
	}
};

struct FakeRkvdec0Hardware {
	Snapshot state = {};
	bool failOn = false;
	bool failOff = false;
	unsigned writes = 0;

	FakeRkvdec0Hardware()
	{
		state.version = kSnapshotVersion;
		state.flags = 1;
		state.powerGate = 0xfff9 & ~kVdpuGate;
		state.powerStatus = 0xfff9 & ~kVdpuGate;
		state.repairStatus = 0xffff8001 | kVdpuRepair;
		state.memoryStatus = 0x00fff900;
		state.idleAck = 0xfff;
		state.idleStatus = 0xfff;
		state.clockSelect89 = 0x8100;
		state.clockSelect90 = 0x1801;
		state.clockSelect91 = 1;
	}

	Snapshot SnapshotNow() { return state; }
	void WritePower(uint32_t offset, uint32_t value)
	{
		writes++;
		if (offset == kPowerGateOffset) {
			assert((value & (kRkvdec0Gate << 16)) != 0);
			if ((value & kRkvdec0Gate) != 0) {
				state.powerGate |= kRkvdec0Gate;
				state.memoryStatus |= kRkvdec0Memory;
				if (!failOff) {
					state.powerStatus |= kRkvdec0Gate;
					state.repairStatus &= ~kRkvdec0Repair;
					state.idleAck |= kRkvdec0Idle;
					state.idleStatus |= kRkvdec0Idle;
				}
			} else {
				state.powerGate &= ~kRkvdec0Gate;
				if (!failOn) {
					state.powerStatus &= ~kRkvdec0Gate;
					state.repairStatus |= kRkvdec0Repair;
					state.memoryStatus &= ~kRkvdec0Memory;
				}
			}
		} else {
			assert(offset == kIdleRequestOffset);
			assert((value & (kRkvdec0Idle << 16)) != 0);
			if ((value & kRkvdec0Idle) != 0)
				state.idleRequest |= kRkvdec0Idle;
			else
				state.idleRequest &= ~kRkvdec0Idle;
			if ((state.repairStatus & kRkvdec0Repair) != 0) {
				state.idleAck = (state.idleAck & ~kRkvdec0Idle)
					| (state.idleRequest & kRkvdec0Idle);
				state.idleStatus = (state.idleStatus & ~kRkvdec0Idle)
					| (state.idleRequest & kRkvdec0Idle);
			}
		}
	}
	bool PowerIsOn(uint32_t mask) { return (state.repairStatus & mask) != 0; }
	bool WaitRkvdec0Power(bool on)
	{
		return PowerIsOn(kRkvdec0Repair) == on
			&& ((state.powerStatus & kRkvdec0Gate) != 0) != on;
	}
	bool WaitRkvdec0Memory(bool on)
	{
		return ((state.memoryStatus & kRkvdec0Memory) != 0) != on;
	}
	bool WaitIdle(uint32_t mask, bool idle)
	{
		return ((state.idleAck & mask) != 0) == idle
			&& ((state.idleStatus & mask) != 0) == idle;
	}
};

struct FakeParentChildHardware : FakeRkvdec0Hardware {
	FakeParentChildHardware()
	{
		state.powerGate |= kVdpuGate;
		state.powerStatus |= kVdpuGate;
		state.repairStatus &= ~kVdpuRepair;
		state.idleAck |= kVdpuIdle;
		state.idleStatus |= kVdpuIdle;
		state.clockSelect98 = 0x21;
	}

	void WritePower(uint32_t offset, uint32_t value)
	{
		bool parent = offset == kPowerGateOffset
			? (value & (kVdpuGate << 16)) != 0
			: (value & (kVdpuIdle << 16)) != 0;
		if (!parent) {
			FakeRkvdec0Hardware::WritePower(offset, value);
			return;
		}
		writes++;
		if (offset == kPowerGateOffset) {
			assert((value & (kVdpuGate << 16)) != 0);
			if ((value & kVdpuGate) != 0) {
				state.powerGate |= kVdpuGate;
				state.powerStatus |= kVdpuGate;
				state.repairStatus &= ~kVdpuRepair;
				state.idleAck |= kVdpuIdle;
				state.idleStatus |= kVdpuIdle;
			} else {
				state.powerGate &= ~kVdpuGate;
				state.powerStatus &= ~kVdpuGate;
				state.repairStatus |= kVdpuRepair;
			}
		} else {
			assert(offset == kIdleRequestOffset);
			assert((value & (kVdpuIdle << 16)) != 0);
			if ((value & kVdpuIdle) != 0)
				state.idleRequest |= kVdpuIdle;
			else
				state.idleRequest &= ~kVdpuIdle;
			if ((state.repairStatus & kVdpuRepair) != 0) {
				state.idleAck = (state.idleAck & ~kVdpuIdle)
					| (state.idleRequest & kVdpuIdle);
				state.idleStatus = (state.idleStatus & ~kVdpuIdle)
					| (state.idleRequest & kVdpuIdle);
			}
		}
	}

	bool WaitPower(uint32_t mask, bool on)
	{
		return PowerIsOn(mask) == on
			&& ((state.powerStatus & kVdpuGate) != 0) != on;
	}
};

struct FakeAv1Hardware {
	Snapshot state = {};
	bool failOn = false;
	bool failOff = false;
	unsigned writes = 0;

	FakeAv1Hardware()
	{
		state.version = kSnapshotVersion;
		state.flags = 1;
		state.powerGate = 0xfff9 & ~kVdpuGate;
		state.powerStatus = 0xfff9 & ~kVdpuGate;
		state.repairStatus = 0xffff8001 | kVdpuRepair;
		state.memoryStatus = 0x00fff900;
		state.idleAck = 0xfff;
		state.idleStatus = 0xfff;
		state.clockSelect163 = 2;
	}

	Snapshot SnapshotNow() { return state; }
	void WritePower(uint32_t offset, uint32_t value)
	{
		writes++;
		if (offset == kPowerGateOffset) {
			assert((value & (kAv1Gate << 16)) != 0);
			if ((value & kAv1Gate) != 0) {
				state.powerGate |= kAv1Gate;
				state.memoryStatus |= kAv1Memory;
				if (!failOff) {
					state.powerStatus |= kAv1Gate;
					state.repairStatus &= ~kAv1Repair;
					state.idleAck |= kAv1Idle;
					state.idleStatus |= kAv1Idle;
				}
			} else {
				state.powerGate &= ~kAv1Gate;
				if (!failOn) {
					state.powerStatus &= ~kAv1Gate;
					state.repairStatus |= kAv1Repair;
					state.memoryStatus &= ~kAv1Memory;
				}
			}
		} else {
			assert(offset == kIdleRequestOffset);
			assert((value & (kAv1Idle << 16)) != 0);
			if ((value & kAv1Idle) != 0)
				state.idleRequest |= kAv1Idle;
			else
				state.idleRequest &= ~kAv1Idle;
			if ((state.repairStatus & kAv1Repair) != 0) {
				state.idleAck = (state.idleAck & ~kAv1Idle)
					| (state.idleRequest & kAv1Idle);
				state.idleStatus = (state.idleStatus & ~kAv1Idle)
					| (state.idleRequest & kAv1Idle);
			}
		}
	}
	bool PowerIsOn(uint32_t mask) { return (state.repairStatus & mask) != 0; }
	bool WaitAv1Power(bool on)
	{
		return PowerIsOn(kAv1Repair) == on
			&& ((state.powerStatus & kAv1Gate) != 0) != on;
	}
	bool WaitAv1Memory(bool on)
	{
		return ((state.memoryStatus & kAv1Memory) != 0) != on;
	}
	bool WaitIdle(uint32_t mask, bool idle)
	{
		return ((state.idleAck & mask) != 0) == idle
			&& ((state.idleStatus & mask) != 0) == idle;
	}
};

int
main()
{
	FakeHardware good;
	PowerCycle result;
	CycleVdpuPower(good, result);
	assert(result.result == kPowerOK && result.restoreResult == kPowerOK);
	assert(result.flags == 7 && good.writes == 5);
	assert((result.powered.repairStatus & kVdpuRepair) != 0);
	assert(RestoredStateMatches(result.before, result.after));
	bool probed = false;
	FakeHardware identity;
	CycleVdpuPower(identity, result, [&] {
		probed = true;
		assert((identity.state.repairStatus & kVdpuRepair) != 0);
		assert((identity.state.idleStatus & kVdpuIdle) == 0);
	});
	assert(probed && result.result == kPowerOK
		&& result.restoreResult == kPowerOK);

	FakeHardware delayed;
	delayed.delayStatus = true;
	CycleVdpuPower(delayed, result);
	assert(result.result == kPowerOK && result.restoreResult == kPowerOK);
	assert(RestoredStateMatches(result.before, result.after));

	FakeHardware rejected;
	rejected.state.clockGate44 = 1;
	CycleVdpuPower(rejected, result);
	assert(result.result == kPowerInvalidState && rejected.writes == 0);

	FakeHardware powerTimeout;
	powerTimeout.failOn = true;
	CycleVdpuPower(powerTimeout, result);
	assert(result.result == kPowerOnTimeout && result.restoreResult == kPowerOK);
	assert(RestoredStateMatches(result.before, result.after));

	FakeHardware idleTimeout;
	idleTimeout.failIdle = true;
	CycleVdpuPower(idleTimeout, result);
	assert(result.result == kPowerIdleTimeout && result.restoreResult == kPowerOK);
	assert(RestoredStateMatches(result.before, result.after));

	FakeHardware restoreTimeout;
	restoreTimeout.failOff = true;
	CycleVdpuPower(restoreTimeout, result);
	assert(result.result == kPowerOK && result.restoreResult == kPowerOffTimeout);

	FakeRkvdec0Hardware rkvGood;
	CycleRkvdec0Power(rkvGood, result);
	assert(result.result == kPowerOK && result.restoreResult == kPowerOK);
	assert(result.flags == 7 && rkvGood.writes == 5);
	assert((result.powered.repairStatus & kRkvdec0Repair) != 0);
	assert(RestoredRkvdec0StateMatches(result.before, result.after));

	FakeRkvdec0Hardware rkvRejected;
	rkvRejected.state.clockGate40 = 1;
	CycleRkvdec0Power(rkvRejected, result);
	assert(result.result == kPowerInvalidState && rkvRejected.writes == 0);

	FakeRkvdec0Hardware missingParent;
	missingParent.state.repairStatus &= ~kVdpuRepair;
	CycleRkvdec0Power(missingParent, result);
	assert(result.result == kPowerInvalidState && missingParent.writes == 0);

	FakeRkvdec0Hardware rkvTimeout;
	rkvTimeout.failOn = true;
	CycleRkvdec0Power(rkvTimeout, result);
	assert(result.result == kPowerOnTimeout && result.restoreResult == kPowerOK);
	assert(RestoredRkvdec0StateMatches(result.before, result.after));

	FakeParentChildHardware combined;
	PowerCycle parent;
	PowerCycle child;
	CycleVdpuPower(combined, parent, [&] {
		CycleRkvdec0Power(combined, child);
	});
	assert(parent.result == kPowerOK && parent.restoreResult == kPowerOK);
	assert(child.result == kPowerOK && child.restoreResult == kPowerOK);
	assert(parent.flags == 7 && child.flags == 7 && combined.writes == 10);
	assert(RestoredStateMatches(parent.before, parent.after));
	assert(RestoredRkvdec0StateMatches(child.before, child.after));

	FakeParentChildHardware observed;
	bool readRegisters = false;
	CycleVdpuPower(observed, parent, [&] {
		CycleRkvdec0Power(observed, child, [&] {
			readRegisters = true;
			assert((observed.state.repairStatus
				& (kVdpuRepair | kRkvdec0Repair))
				== (kVdpuRepair | kRkvdec0Repair));
		});
	});
	assert(readRegisters && parent.result == kPowerOK
		&& child.result == kPowerOK);

	FakeAv1Hardware av1Good;
	CycleAv1Power(av1Good, result);
	assert(result.result == kPowerOK && result.restoreResult == kPowerOK);
	assert(result.flags == 7 && av1Good.writes == 5);
	assert((result.powered.repairStatus & kAv1Repair) != 0);
	assert(RestoredAv1StateMatches(result.before, result.after));

	FakeAv1Hardware av1Rejected;
	av1Rejected.state.clockSelect163 = 3;
	CycleAv1Power(av1Rejected, result);
	assert(result.result == kPowerInvalidState && av1Rejected.writes == 0);

	FakeAv1Hardware av1MissingParent;
	av1MissingParent.state.repairStatus &= ~kVdpuRepair;
	CycleAv1Power(av1MissingParent, result);
	assert(result.result == kPowerInvalidState && av1MissingParent.writes == 0);

	FakeAv1Hardware av1Timeout;
	av1Timeout.failOn = true;
	CycleAv1Power(av1Timeout, result);
	assert(result.result == kPowerOnTimeout && result.restoreResult == kPowerOK);
	assert(RestoredAv1StateMatches(result.before, result.after));

	puts("ROCK5_VPU_POWER_TEST_PASS");
}
