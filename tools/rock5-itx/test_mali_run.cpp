/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfRun.h"

#define MALI_RESET_MODEL_ONLY
#include "test_mali_reset.cpp"
#undef MALI_RESET_MODEL_ONLY
#define main MaliMemoryFixtureMain
#include "test_mali_memory.cpp"
#undef main

enum RunFault { RunNone, HandlerFailure, L2Failure, FirstFlushFailure,
	AsUpdateFailure, MissingBoot, LostBootInterrupt, BadInterface, MissingPing,
	WrongPingAck, MmuFaultOnBoot, GpuFaultOnPing, StopFailure, FinalFlushFailure,
	UnmapFailure, L2OffFailure, FinalMaskFailure, FrozenFirmwareClock, BackwardsFirmwareClock,
	L2TransitionFailure };

struct FirmwareModel : ResetModel {
	RunFault fault;
	FirmwareMemory* memory;
	bool firmwareMode = false, handlers = false, jobArmed = false;
	bool mcuRunning = false, asMapped = false, l2On = false, asLocked = false;
	bool pendingBoot = false, pendingPing = false;
	unsigned flushes = 0, firmwarePauses = 0, barriers = 0;
	FirmwareCapture captures[3] = {};
	vector<std::pair<uint32_t, uint32_t>> writes;

	FirmwareModel(FirmwareMemory* value, RunFault runFault = RunNone, Fault powerFault = None)
		: ResetModel(ResetNone, powerFault), fault(runFault), memory(value) {}
	~FirmwareModel() { assert(!handlers); }
	int64_t Now()
	{
		if (firmwareMode && fault == FrozenFirmwareClock) return 10000;
		if (firmwareMode && fault == BackwardsFirmwareClock && firmwarePauses) return -1;
		return ResetModel::Now();
	}
	uint32_t ReadGpu(uint32_t offset)
	{
		if (!firmwareMode) return ResetModel::ReadGpu(offset);
		assert(gpu != NULL && powered);
		if (offset == 0x120) return 1;
		if (offset == kGpuInterruptStatus) return regs[kGpuRaw] & regs[kGpuMask];
		return regs[offset];
	}
	void WriteGpu(uint32_t offset, uint32_t value)
	{
		if (!firmwareMode) { ResetModel::WriteGpu(offset, value); return; }
		assert(gpu != NULL && powered);
		writes.push_back({offset, value});
		if (offset == kGpuClear) {
			regs[kGpuRaw] &= ~value;
			return;
		}
		if (offset == kGpuCommand) {
			assert(value == 0x3304 && l2On && asLocked && !mcuRunning);
			flushes++;
			if (!((fault == FirstFlushFailure && flushes == 1)
				|| (fault == FinalFlushFailure && flushes == 2)))
				regs[kGpuRaw] |= 1u << 17;
			return;
		}
		if (offset == 0x1a0 && value) {
			assert(!l2On && !asMapped && !mcuRunning && value == 1 && regs[0x304] == 31);
			if (fault == L2TransitionFailure) regs[0x220] = 1;
			else if (fault != L2Failure && fault != FrozenFirmwareClock && fault != BackwardsFirmwareClock) {
				l2On = true; regs[0x160] = 1;
			}
		} else if (offset == 0x1e0 && value) {
			assert(!asMapped && !mcuRunning && value == 1 && regs[0x220] == 0);
			if (fault != L2OffFailure) { l2On = false; regs[0x160] = 0; }
		} else if (offset == 0x2418) {
			assert(l2On && !mcuRunning);
			regs[0x2000] |= 1u << 16; // AS0 request completion is not a fault.
			if (value == 2) asLocked = true;
			else if (value == 3) asLocked = false;
			else {
				assert(value == 1 && !asLocked);
				if (regs[0x2430] == 1) {
					if (fault == UnmapFailure) regs[0x2428] = 1;
					else asMapped = false;
				} else {
					assert(regs[0x2430] == 0x420001c6 && regs[0x2434] == 0);
					assert((uint64_t(regs[0x2404]) << 32 | regs[0x2400]) == memory->RootPhysical());
					assert(regs[0x2408] == 0xc0c08f4c && regs[0x240c] == 0xc0c0c0c0);
					assert(barriers > 0);
					asMapped = true;
					if (fault == AsUpdateFailure) regs[0x2428] = 1;
				}
			}
		} else if (offset == 0x700) {
			if (value == 2) {
				assert(asMapped && l2On && handlers && jobArmed && !mcuRunning);
				mcuRunning = true; regs[0x704] = 1; pendingBoot = true;
			} else {
				assert(value == 0);
				if (fault != StopFailure) { mcuRunning = false; regs[0x704] = 0; }
			}
		}
		regs[offset] = value;
	}
	bool InstallFirmwareHandlers()
	{
		assert(!installed && !handlers);
		firmwareMode = true;
		if (fault == HandlerFailure) return false;
		handlers = true;
		return true;
	}
	void StopFirmwareHandlers()
	{
		if (handlers) {
			regs[0x1008] = regs[0x2008] = regs[kGpuMask] = 0;
			if (fault == FinalMaskFailure) regs[0x1008] = 0x80000000;
		}
		handlers = jobArmed = false;
	}
	bool ArmFirmwareInterrupts()
	{
		assert(handlers && asMapped && !mcuRunning);
		regs[0x1008] = 0x80000000; regs[0x2008] = 1; regs[kGpuMask] = 3;
		jobArmed = true;
		return true;
	}
	bool ArmFirmwareJob()
	{
		assert(!jobArmed && handlers && mcuRunning && captures[0].count == 1);
		captures[0] = {};
		jobArmed = true;
		return true;
	}
	FirmwareCapture ReadFirmwareCapture(unsigned index) { return captures[index]; }
	bool FirmwareFaulted() { return captures[1].count || captures[2].count; }
	void MemoryBarrier() { barriers++; }
	void RingFirmwareDoorbell()
	{
		assert(jobArmed && mcuRunning && barriers >= 3);
		pendingPing = true;
	}
	void Event(unsigned index)
	{
		captures[index] = {};
		captures[index].count = 1;
		captures[index].status = captures[index].raw = index == 0 ? 0x80000000 : 1;
		captures[index].cpu = 2;
		captures[index].whenMicros = Now();
		if (index == 0) { jobArmed = false; regs[0x1008] = 0; }
	}
	void PauseReset()
	{
		if (!firmwareMode) { ResetModel::PauseReset(); return; }
		assert(++firmwarePauses < 25000);
		elapsed += 100;
		if (pendingBoot && fault != MissingBoot) {
			pendingBoot = false;
			if (fault == MmuFaultOnBoot) { Event(1); return; }
			auto shared = shared_fixture();
			if (fault == BadInterface) shared[1041] = shared[2];
			memcpy(memory->SharedData(), shared.data(), 65536);
			if (fault == LostBootInterrupt) { regs[0x1000] = 0x80000000; return; }
			Event(0);
		}
		if (pendingPing && fault != MissingPing) {
			pendingPing = false;
			if (fault == GpuFaultOnPing) { Event(2); return; }
			uint32_t* w = (uint32_t*)memory->SharedData();
			unsigned input = (w[2] - 0x04000000) / 4, output = (w[3] - 0x04000000) / 4;
			if (fault != WrongPingAck) w[output] = w[input];
			Event(0);
		}
	}
	void UnmapGpu()
	{
		assert(!handlers);
		ResetModel::UnmapGpu();
	}
};

#ifndef MALI_RUN_MODEL_ONLY
int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory;
	assert(memory.Plan(image));
	Guarded arena(memory.RequiredBytes());
	for (unsigned fault = RunNone; fault <= L2TransitionFailure; fault++) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		FirmwareModel model(&memory, RunFault(fault));
		FirmwareRunInfo info = {};
		info.flags = kFirmwareAllocated;
		CycleFirmware(model, memory, info);
		printf("firmware lifecycle fault=%u result=%u cleanup=%u flags=%x pauses=%u\n",
			fault, info.result, info.cleanupResult, info.flags, model.firmwarePauses);
		assert(model.gpu == NULL && !model.handlers);
		if (fault != BackwardsFirmwareClock) {
			assert(!model.powered && (info.flags & kFirmwarePowerRestored) != 0);
		} else
			assert((info.flags & kFirmwarePowerRestored) == 0);
		if (fault == RunNone) {
			assert(info.result == 0 && info.cleanupResult == 0 && info.flags == 255);
			assert(info.boot.count == 1 && info.ping.count == 1 && model.flushes == 2);
			assert(info.mmuRawAfter == 65536);
			assert(info.interface.version == 0x01050000 && info.interface.groupCount == 8);
			assert(!model.asMapped && !model.l2On && !model.mcuRunning && !model.asLocked);
		} else {
			assert(info.result != 0 || info.cleanupResult != 0);
			assert((info.flags & kFirmwareNeedsRecovery) != 0);
		}
		if (fault == StopFailure || fault == FinalFlushFailure || fault == UnmapFailure
			|| fault == L2OffFailure || fault == FinalMaskFailure || fault == L2TransitionFailure)
			assert((info.flags & kFirmwareCleaned) == 0);
		if (fault == StopFailure) assert(model.flushes == 1 && model.asMapped);
		if (fault == FinalFlushFailure) assert(model.asMapped);
		if (fault == MmuFaultOnBoot) assert(info.result == kFirmwareFault && info.mmuFault.count == 1);
		if (fault == GpuFaultOnPing) assert(info.result == kFirmwareFault && info.gpuFault.count == 1);
	}
	FirmwareModel restore(&memory, RunNone, RestoreClock);
	FirmwareRunInfo info = {};
	CycleFirmware(restore, memory, info);
	assert(info.result == kFirmwareRunOK && (info.flags & kFirmwareNeedsRecovery) != 0
		&& (info.flags & kFirmwarePowerRestored) == 0);
	puts("MALI_CSF_RUN_TEST_PASS");
}
#endif
