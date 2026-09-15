/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_RUN_H
#define MALI_CSF_RUN_H

#include "CsfReset.h"
#include "CsfMemory.h"
#include "CsfInterface.h"

namespace MaliCSF {

static const uint32_t kCycleFirmware = 0x4d435304;
static const uint32_t kFirmwareRunVersion = 1;
static const uint32_t kGlobalInterrupt = 1u << 31;
static const uint32_t kFirmwarePing = 1u << 8;
// MMU IRQ low bits are faults; upper bits report AS request completion.
static const uint32_t kMmuFaultBits = 0xffff;
static const uint32_t kMmuAs0Completed = 1u << 16;
static const uint32_t kFirmwareAllocated = 1;
static const uint32_t kFirmwareAddressSpace = 2;
static const uint32_t kFirmwareMcuCommand = 4;
static const uint32_t kFirmwareBootObserved = 8;
static const uint32_t kFirmwareInterfaceChecked = 16;
static const uint32_t kFirmwarePingObserved = 32;
static const uint32_t kFirmwareCleaned = 64;
static const uint32_t kFirmwarePowerRestored = 128;
static const uint32_t kFirmwareMemoryRetained = 256;
static const uint32_t kFirmwareNeedsRecovery = 512;

enum FirmwareRunResult {
	kFirmwareRunOK = 0,
	kFirmwareRunNotAttempted,
	kFirmwareResetFailed,
	kFirmwareHandlerFailed,
	kFirmwareL2Failed,
	kFirmwareAddressSpaceFailed,
	kFirmwareBootFailed,
	kFirmwareInterfaceFailed,
	kFirmwarePingFailed,
	kFirmwareFault,
	kFirmwareStopFailed,
	kFirmwareFlushFailed,
	kFirmwareUnmapFailed,
	kFirmwareL2OffFailed,
	kFirmwareFinalStateFailed,
	kFirmwarePowerFailed,
	kFirmwareOperationFailed,
	kFirmwareOperationCleanupFailed
};

struct FirmwareCapture {
	uint32_t count;
	uint32_t status;
	uint32_t raw;
	uint32_t deviceStatus;
	uint64_t address;
	uint64_t extra;
	int32_t cpu;
	uint32_t reserved;
	int64_t whenMicros;
};

// Input: version and firmwareBytes, followed by exactly firmwareBytes bytes.
// Output: this structure replaces the header. No user pointer enters the ABI.
// Only an explicitly enabled diagnostic profile and uid 0 may use it.
struct FirmwareRunInfo {
	uint32_t version;
	uint32_t firmwareBytes;
	uint32_t result;
	uint32_t cleanupResult;
	uint32_t flags;
	uint32_t tablePages;
	uint32_t allocationBytes;
	uint32_t mcuBootStatus;
	uint64_t rootPhysical;
	uint64_t translationConfig;
	uint64_t memoryAttributes;
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t pingRequest;
	uint32_t pingAck;
	uint32_t jobRawAfter;
	uint32_t mmuRawAfter;
	uint32_t asStatusAfter;
	uint32_t reserved;
	uint64_t asConfigAfter;
	FirmwareCapture boot;
	FirmwareCapture ping;
	FirmwareCapture gpuFault;
	FirmwareCapture mmuFault;
	InterfaceInfo interface;
	ResetInfo reset;
	ResetSnapshot after;
	IdentityInfo power;
};

static_assert(sizeof(FirmwareCapture) == 48, "Firmware capture ABI changed");
static_assert(sizeof(InterfaceInfo) == 48, "Firmware interface ABI changed");
static_assert(sizeof(FirmwareRunInfo) == 1128, "Firmware run ABI changed");

template<typename IO, typename Condition>
bool
WaitFirmware(IO& io, int64_t timeout, Condition condition)
{
	int64_t start = io.Now();
	if (start < 0)
		return false;
	for (unsigned i = 0; i < unsigned(timeout / 100 + 2); i++) {
		int64_t now = io.Now();
		if (now < start || now - start > timeout)
			return false;
		if (condition())
			return true;
		io.PauseReset();
	}
	return false;
}

template<typename IO>
uint64_t
ReadGpu64(IO& io, uint32_t offset)
{
	uint32_t low = io.ReadGpu(offset);
	return low | (uint64_t(io.ReadGpu(offset + 4)) << 32);
}

template<typename IO>
void
WriteGpu64(IO& io, uint32_t offset, uint64_t value)
{
	io.WriteGpu(offset, uint32_t(value));
	io.WriteGpu(offset + 4, uint32_t(value >> 32));
}

template<typename IO>
bool
FirmwareAsCommand(IO& io, uint32_t command, unsigned as = 0)
{
	if (as > 1)
		return false;
	uint32_t offset = as * 0x40;
	if (!WaitFirmware(io, 100000, [&]() { return (io.ReadGpu(0x2428 + offset) & 1) == 0; }))
		return false;
	io.WriteGpu(0x2418 + offset, command);
	return WaitFirmware(io, 100000, [&]() { return (io.ReadGpu(0x2428 + offset) & 1) == 0; });
}

template<typename IO>
bool
FlushLockedFirmwareCaches(IO& io)
{
	io.WriteGpu(kGpuClear, 1u << 17);
	if ((io.ReadGpu(kGpuRaw) & (1u << 17)) != 0)
		return false;
	io.WriteGpu(kGpuCommand, 0x3304); // L2 and load/store clean + invalidate.
	bool completed = WaitFirmware(io, 100000, [&]() {
		return (io.ReadGpu(kGpuRaw) & (1u << 17)) != 0;
	});
	io.WriteGpu(kGpuClear, 1u << 17);
	return completed;
}

template<typename IO>
bool
FlushFirmware(IO& io, unsigned as = 0)
{
	if (as > 1)
		return false;
	// Lock the full 48-bit input range. CSF uses a GPU cache command and an
	// explicit AS unlock, rather than the legacy AS FLUSH_MEM command.
	WriteGpu64(io, 0x2410 + as * 0x40, 47);
	if (!FirmwareAsCommand(io, 2, as))
		return false;
	bool completed = FlushLockedFirmwareCaches(io);
	// Attempt unlock even when completion was lost; retain both failures.
	bool unlocked = FirmwareAsCommand(io, 3, as);
	return completed && unlocked;
}

inline bool
FirmwareEventMatches(const FirmwareCapture& event, int64_t start, int64_t end)
{
	return event.count == 1 && event.status == kGlobalInterrupt
		&& event.raw == kGlobalInterrupt && event.cpu >= 0 && event.cpu < 8
		&& event.reserved == 0 && start >= 0 && end >= start
		&& event.whenMicros >= start && event.whenMicros <= end;
}

struct NoFirmwareOperation {
	template<typename IO> bool Run(IO&, FirmwareMemory&, FirmwareRunInfo&) { return true; }
	template<typename IO> bool BeforeStop(IO&) { return true; }
	template<typename IO> bool Cleanup(IO&, bool) { return true; }
	uint32_t CompletedAddressSpaces() const { return kMmuAs0Completed; }
};

template<typename IO, typename Operation>
void
RunFirmwarePowered(IO& io, FirmwareMemory& memory, FirmwareRunInfo& info, Operation& operation)
{
	info.cleanupResult = kFirmwareRunOK;
	info.reset.version = kResetVersion;
	info.reset.interrupt = 126;
	ResetGpu(io, info.reset);
	if (info.reset.result != kResetOK || info.reset.cleanupResult != kResetOK) {
		info.result = kFirmwareResetFailed;
		return;
	}
	bool l2Requested = false;
	bool asExposed = false;
	bool mcuRequested = false;
	if (!io.InstallFirmwareHandlers()) {
		info.result = kFirmwareHandlerFailed;
		io.StopFirmwareHandlers();
		return;
	}
	do {
		// Pin the mainline reference's noncoherent mode before L2 power-on.
		if ((ReadGpu64(io, 0x120) & 1) == 0 || ReadGpu64(io, 0x220) != 0) {
			info.result = kFirmwareL2Failed;
			break;
		}
		io.WriteGpu(0x304, 31);
		if (io.ReadGpu(0x304) != 31) {
			info.result = kFirmwareL2Failed;
			break;
		}
		l2Requested = true;
		WriteGpu64(io, 0x1a0, 1);
		if (!WaitFirmware(io, 20000, [&]() {
			return ReadGpu64(io, 0x160) == 1 && ReadGpu64(io, 0x220) == 0;
		})) {
			info.result = kFirmwareL2Failed;
			break;
		}
		if (!FlushFirmware(io)) {
			info.result = kFirmwareFlushFailed;
			break;
		}
		io.MemoryBarrier();
		asExposed = true;
		info.flags |= kFirmwareAddressSpace;
		WriteGpu64(io, 0x2400, memory.RootPhysical());
		WriteGpu64(io, 0x2408, FirmwareMemory::MemoryAttributes());
		WriteGpu64(io, 0x2430, FirmwareMemory::TranslationConfig());
		if (!FirmwareAsCommand(io, 1)
			|| ReadGpu64(io, 0x2400) != memory.RootPhysical()
			|| ReadGpu64(io, 0x2408) != FirmwareMemory::MemoryAttributes()
			|| ReadGpu64(io, 0x2430) != FirmwareMemory::TranslationConfig()) {
			info.result = kFirmwareAddressSpaceFailed;
			break;
		}
		if (!io.ArmFirmwareInterrupts()) {
			info.result = kFirmwareHandlerFailed;
			break;
		}
		int64_t bootStart = io.Now();
		mcuRequested = true;
		info.flags |= kFirmwareMcuCommand;
		io.WriteGpu(0x700, 2);
		bool boot = WaitFirmware(io, 1000000, [&]() {
			return io.ReadFirmwareCapture(0).count != 0 || io.FirmwareFaulted();
		});
		info.boot = io.ReadFirmwareCapture(0);
		info.mcuBootStatus = io.ReadGpu(0x704);
		if (!boot || !FirmwareEventMatches(info.boot, bootStart, io.Now())
			|| info.mcuBootStatus != 1 || io.FirmwareFaulted()) {
			info.result = io.FirmwareFaulted() ? kFirmwareFault : kFirmwareBootFailed;
			break;
		}
		info.flags |= kFirmwareBootObserved;
		io.MemoryBarrier();
		if (!InspectInterface(memory.SharedData(), memory.SharedBytes(), memory.SharedAddress(),
				info.interface)) {
			info.result = kFirmwareInterfaceFailed;
			break;
		}
		info.flags |= kFirmwareInterfaceChecked;
		volatile uint32_t* shared = (volatile uint32_t*)memory.SharedData();
		volatile uint32_t* input = shared + info.interface.inputOffset / 4;
		const volatile uint32_t* output = shared + info.interface.outputOffset / 4;
		int64_t pingStart = io.Now();
		if (!io.ArmFirmwareJob()) {
			info.result = kFirmwarePingFailed;
			break;
		}
		input[1] = kFirmwarePing;
		info.pingRequest = (input[0] & ~kFirmwarePing) | ((output[0] ^ kFirmwarePing) & kFirmwarePing);
		input[0] = info.pingRequest;
		io.MemoryBarrier();
		io.RingFirmwareDoorbell();
		bool ping = WaitFirmware(io, 100000, [&]() {
			return io.ReadFirmwareCapture(0).count != 0 || io.FirmwareFaulted();
		});
		io.MemoryBarrier();
		info.pingAck = output[0];
		info.ping = io.ReadFirmwareCapture(0);
		if (!ping || !FirmwareEventMatches(info.ping, pingStart, io.Now())
			|| ((info.pingRequest ^ info.pingAck) & kFirmwarePing) != 0 || io.FirmwareFaulted()) {
			info.result = io.FirmwareFaulted() ? kFirmwareFault : kFirmwarePingFailed;
			break;
		}
		info.flags |= kFirmwarePingObserved;
		info.result = operation.Run(io, memory, info)
			? kFirmwareRunOK : kFirmwareOperationFailed;
	} while (false);

	// Keep memory, MMIO and handler cookies alive through stop, flush, AS
	// removal and IRQ teardown. The outer power cycle runs only afterwards.
	bool stopped = true;
	if (mcuRequested) {
		if (!operation.BeforeStop(io))
			info.cleanupResult = kFirmwareOperationCleanupFailed;
		io.WriteGpu(0x700, 0);
		stopped = WaitFirmware(io, 100000, [&]() { return io.ReadGpu(0x704) == 0; });
		if (!stopped)
			info.cleanupResult = kFirmwareStopFailed;
	}
	if (!operation.Cleanup(io, stopped))
		info.cleanupResult = kFirmwareOperationCleanupFailed;
	if (asExposed && stopped) {
		if (!FlushFirmware(io))
			info.cleanupResult = kFirmwareFlushFailed;
		else {
			WriteGpu64(io, 0x2430, 1);
			WriteGpu64(io, 0x2400, 0);
			WriteGpu64(io, 0x2408, 0);
			if (!FirmwareAsCommand(io, 1) || ReadGpu64(io, 0x2430) != 1
				|| ReadGpu64(io, 0x2400) != 0 || ReadGpu64(io, 0x2408) != 0)
				info.cleanupResult = kFirmwareUnmapFailed;
		}
	}
	if (l2Requested && stopped && info.cleanupResult == kFirmwareRunOK) {
		// Do not issue an opposite request during an unfinished power-up.
		if (ReadGpu64(io, 0x220) != 0)
			info.cleanupResult = kFirmwareL2OffFailed;
		else {
			WriteGpu64(io, 0x1e0, 1);
			if (!WaitFirmware(io, 20000, [&]() {
				return ReadGpu64(io, 0x160) == 0 && ReadGpu64(io, 0x220) == 0;
			}))
				info.cleanupResult = kFirmwareL2OffFailed;
		}
	}
	io.StopFirmwareHandlers();
	info.gpuFault = io.ReadFirmwareCapture(2);
	info.mmuFault = io.ReadFirmwareCapture(1);
	info.jobRawAfter = io.ReadGpu(0x1000);
	info.mmuRawAfter = io.ReadGpu(0x2000);
	info.asStatusAfter = io.ReadGpu(0x2428);
	info.asConfigAfter = ReadGpu64(io, 0x2430);
	SnapshotReset(io, info.after);
	if (info.gpuFault.count != 0 || info.mmuFault.count != 0
		|| (info.after.raw & 3) != 0 || (info.mmuRawAfter & kMmuFaultBits) != 0)
		info.result = kFirmwareFault;
	if (info.cleanupResult == kFirmwareRunOK
		&& (!ResetIdleMatches(info.after) || info.jobRawAfter != 0
			|| (info.mmuRawAfter & ~operation.CompletedAddressSpaces()) != 0
			|| (info.asStatusAfter & 1) != 0))
		info.cleanupResult = kFirmwareFinalStateFailed;
	if (info.cleanupResult == kFirmwareRunOK)
		info.flags |= kFirmwareCleaned;
}

template<typename IO, typename Operation>
void
CycleFirmware(IO& io, FirmwareMemory& memory, FirmwareRunInfo& info, Operation& operation)
{
	info.result = kFirmwareRunNotAttempted;
	info.cleanupResult = kFirmwareRunNotAttempted;
	info.startedMicros = io.Now();
	CycleIdentity(io, info.power, [&]() {
		RunFirmwarePowered(io, memory, info, operation);
		return info.result == kFirmwareRunOK && info.cleanupResult == kFirmwareRunOK;
	});
	if (info.result == kFirmwareRunNotAttempted)
		info.result = kFirmwarePowerFailed;
	if ((info.power.flags & kIdentityRestored) != 0)
		info.flags |= kFirmwarePowerRestored;
	if (info.result != kFirmwareRunOK || info.cleanupResult != kFirmwareRunOK
		|| (info.flags & kFirmwarePowerRestored) == 0)
		info.flags |= kFirmwareNeedsRecovery;
	info.finishedMicros = io.Now();
}

template<typename IO>
void
CycleFirmware(IO& io, FirmwareMemory& memory, FirmwareRunInfo& info)
{
	NoFirmwareOperation operation;
	CycleFirmware(io, memory, info, operation);
}

} // namespace MaliCSF
#endif
