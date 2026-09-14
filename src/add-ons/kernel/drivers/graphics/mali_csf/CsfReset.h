/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_RESET_H
#define MALI_CSF_RESET_H


#include "CsfPower.h"


namespace MaliCSF {

static const uint32_t kCycleReset = 0x4d435303;
static const uint32_t kResetVersion = 1;
static const uint32_t kResetCommandIssued = 1;
static const uint32_t kResetInterruptObserved = 2;
static const uint32_t kResetCleaned = 4;
static const uint32_t kResetNeedsRecovery = 8;

// Register facts: Panthor v6.12 panthor_regs.h, under its MIT option.
static const uint32_t kGpuRaw = 0x20;
static const uint32_t kGpuClear = 0x24;
static const uint32_t kGpuMask = 0x28;
static const uint32_t kGpuInterruptStatus = 0x2c;
static const uint32_t kGpuCommand = 0x30;
static const uint32_t kGpuStatus = 0x34;
static const uint32_t kResetCompleted = 1u << 8;
static const uint32_t kSoftReset = 0x101;
static const int64_t kResetTimeoutMicros = 100000;

enum ResetResult {
	kResetOK = 0,
	kResetNotAttempted,
	kResetInitialStateMismatch,
	kResetHandlerInstallFailed,
	kResetClearFailed,
	kResetMaskFailed,
	kResetTimedOut,
	kResetInterruptMissing,
	kResetInterruptMismatch,
	kResetClockFailed,
	kResetCleanupFailed,
	kResetPowerCycleFailed
};

struct ResetSnapshot {
	uint32_t mask;
	uint32_t raw;
	uint32_t interruptStatus;
	uint32_t status;
	uint32_t mcu;
	uint32_t jobMask;
	uint32_t mmuMask;
	uint32_t shaderReady[2];
	uint32_t tilerReady[2];
	uint32_t l2Ready[2];
	uint32_t shaderTransition;
	uint32_t tilerTransition;
	uint32_t l2Transition;
};

struct ResetCapture {
	uint32_t count;
	uint32_t status;
	uint32_t raw;
	uint32_t gpuStatus;
	int32_t cpu;
	uint32_t reserved;
	int64_t whenMicros;
};

struct ResetInfo {
	uint32_t version;
	uint32_t result;
	uint32_t cleanupResult;
	uint32_t flags;
	uint32_t interrupt;
	uint32_t completionRaw;
	uint32_t reserved[2];
	int64_t startedMicros;
	int64_t finishedMicros;
	ResetCapture capture;
	ResetSnapshot before;
	ResetSnapshot after;
	IdentityInfo power;
};

// Kernel-private state. All access is protected by the adapter's IRQ spinlock.
struct ResetInterruptState {
	bool armed;
	ResetCapture capture;
};


template<typename IO>
void
SnapshotReset(IO& io, ResetSnapshot& value)
{
	value = {};
	value.mask = io.ReadGpu(kGpuMask);
	value.raw = io.ReadGpu(kGpuRaw);
	value.interruptStatus = io.ReadGpu(kGpuInterruptStatus);
	value.status = io.ReadGpu(kGpuStatus);
	value.mcu = io.ReadGpu(0x704);
	value.jobMask = io.ReadGpu(0x1008);
	value.mmuMask = io.ReadGpu(0x2008);
	for (unsigned i = 0; i < 2; i++) {
		value.shaderReady[i] = io.ReadGpu(0x140 + i * 4);
		value.tilerReady[i] = io.ReadGpu(0x150 + i * 4);
		value.l2Ready[i] = io.ReadGpu(0x160 + i * 4);
	}
	value.shaderTransition = io.ReadGpu(0x200);
	value.tilerTransition = io.ReadGpu(0x210);
	value.l2Transition = io.ReadGpu(0x220);
}


inline bool
ResetIdleMatches(const ResetSnapshot& value)
{
	return value.mask == 0 && value.interruptStatus == 0
		&& (value.raw & 3) == 0 && (value.status & 0x93) == 0
		&& value.mcu == 0 && value.jobMask == 0 && value.mmuMask == 0
		&& value.shaderReady[0] == 0 && value.shaderReady[1] == 0
		&& value.tilerReady[0] == 0 && value.tilerReady[1] == 0
		&& value.l2Ready[0] == 0 && value.l2Ready[1] == 0
		&& value.shaderTransition == 0 && value.tilerTransition == 0
		&& value.l2Transition == 0;
}


// Called with local interrupts disabled and the capture lock held. The source
// cannot run its handler between arming and issuing the command, on any CPU.
template<typename IO>
ResetResult
ArmReset(IO& io, ResetInterruptState& state)
{
	state = {};
	io.WriteGpu(kGpuClear, kResetCompleted);
	if ((io.ReadGpu(kGpuRaw) & kResetCompleted) != 0)
		return kResetClearFailed;
	state.armed = true;
	io.WriteGpu(kGpuMask, kResetCompleted);
	if (io.ReadGpu(kGpuMask) != kResetCompleted) {
		state.armed = false;
		return kResetMaskFailed;
	}
	io.WriteGpu(kGpuCommand, kSoftReset);
	return kResetOK;
}


// Also called under the capture lock. Mask after one event, retain unexpected
// device status, and acknowledge only the reset bit owned by this diagnostic.
template<typename IO>
bool
CaptureResetInterrupt(IO& io, ResetInterruptState& state)
{
	if (!state.armed)
		return false;
	uint32_t status = io.ReadGpu(kGpuInterruptStatus);
	if (status == 0)
		return false;
	state.capture.count++;
	state.capture.status = status;
	state.capture.raw = io.ReadGpu(kGpuRaw);
	state.capture.gpuStatus = io.ReadGpu(kGpuStatus);
	state.capture.cpu = io.Cpu();
	state.capture.whenMicros = io.Now();
	io.WriteGpu(kGpuMask, 0);
	io.WriteGpu(kGpuClear, status & kResetCompleted);
	state.armed = false;
	return true;
}


inline bool
ResetCaptureMatches(const ResetCapture& capture, int64_t start)
{
	return capture.count == 1 && capture.status == kResetCompleted
		&& (capture.raw & (kResetCompleted | 3)) == kResetCompleted
		&& (capture.gpuStatus & 0x90) == 0
		&& capture.cpu >= 0 && capture.cpu < 8 && capture.reserved == 0
		&& start >= 0 && capture.whenMicros >= start
		&& capture.whenMicros - start <= kResetTimeoutMicros;
}


template<typename IO>
void
ResetGpu(IO& io, ResetInfo& info)
{
	info.startedMicros = io.Now();
	info.cleanupResult = kResetOK;
	SnapshotReset(io, info.before);
	info.after = info.before;
	if (info.startedMicros < 0 || !ResetIdleMatches(info.before)) {
		info.result = info.startedMicros < 0 ? kResetClockFailed : kResetInitialStateMismatch;
		info.finishedMicros = io.Now();
		return;
	}
	if (!io.InstallResetHandler()) {
		info.result = kResetHandlerInstallFailed;
		info.finishedMicros = io.Now();
		return;
	}
	info.result = io.BeginReset();
	if (info.result == kResetOK) {
		info.flags |= kResetCommandIssued;
		info.result = kResetTimedOut;
		for (unsigned i = 0; i < 1001; i++) {
			ResetCapture capture = io.ReadResetCapture();
			int64_t now = io.Now();
			if (now < info.startedMicros) {
				info.result = kResetClockFailed;
				break;
			}
			if (capture.count != 0) {
				info.result = ResetCaptureMatches(capture, info.startedMicros)
					? kResetOK : kResetInterruptMismatch;
				break;
			}
			if (now - info.startedMicros >= kResetTimeoutMicros)
				break;
			io.PauseReset();
		}
	}
	// This masks under the IRQ lock and synchronously removes the exact handler.
	// Its cookie and every mapped GPU page remain alive through that operation.
	io.StopResetHandler();
	info.capture = io.ReadResetCapture();
	info.completionRaw = io.ReadGpu(kGpuRaw);
	if (info.capture.count != 0)
		info.flags |= kResetInterruptObserved;
	if (info.result == kResetTimedOut) {
		if (ResetCaptureMatches(info.capture, info.startedMicros))
			info.result = kResetOK;
		else if (info.capture.count != 0)
			info.result = kResetInterruptMismatch;
		else if ((info.completionRaw & kResetCompleted) != 0)
			info.result = kResetInterruptMissing;
	}
	io.WriteGpu(kGpuClear, kResetCompleted);
	SnapshotReset(io, info.after);
	if (!ResetIdleMatches(info.after) || (info.after.raw & kResetCompleted) != 0)
		info.cleanupResult = kResetCleanupFailed;
	else
		info.flags |= kResetCleaned;
	info.finishedMicros = io.Now();
}


template<typename IO>
void
CycleReset(IO& io, ResetInfo& info, uint32_t interrupt)
{
	info = {};
	info.version = kResetVersion;
	info.result = kResetNotAttempted;
	info.cleanupResult = kResetNotAttempted;
	info.interrupt = interrupt;
	CycleIdentity(io, info.power, [&]() {
		ResetGpu(io, info);
		return info.result == kResetOK && info.cleanupResult == kResetOK;
	});
	if (info.result == kResetNotAttempted)
		info.result = kResetPowerCycleFailed;
	if (info.result != kResetOK || info.cleanupResult != kResetOK
		|| (info.power.flags & kIdentityNeedsRecovery) != 0) {
		info.flags |= kResetNeedsRecovery;
	}
}

} // namespace MaliCSF

#endif // MALI_CSF_RESET_H
