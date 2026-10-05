/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#define MALI_RUN_MODEL_ONLY
#include "test_mali_run.cpp"
#undef MALI_RUN_MODEL_ONLY

enum RecoveryFault { RecoverNone, RecoverConfig, RecoverL2On, RecoverFlush0,
	RecoverFlush1, RecoverAs0, RecoverAs1, RecoverL2Off, RecoverMask,
	RecoverJob, RecoverMmu, RecoverHighTransition, RecoverTable, RecoverAttributes,
	RecoverAddressConfig, RecoverActiveAs, RecoverResetTransition };

// Extend the independently timed firmware/reset models. Firmware can refuse
// its normal stop; only a completed reset changes the model's DMA ownership.
struct RecoveryModel : FirmwareModel {
	RecoveryFault recoveryFault;
	ResetFault recoveryReset;
	bool resetting = false, recovering = false, locked[2] = {};
	unsigned admissionStops = 0, recoveryCommands = 0, recoveryFlushes = 0;
	unsigned recoveryPauses = 0;

	RecoveryModel(FirmwareMemory* memory, RecoveryFault fault = RecoverNone,
		ResetFault reset = ResetNone, Fault power = None, RunFault run = StopFailure)
		: FirmwareModel(memory, run, power), recoveryFault(fault), recoveryReset(reset) {}
	static void StopAdmission(void* cookie)
	{
		RecoveryModel& model = *(RecoveryModel*)cookie;
		assert(++model.admissionStops == 1 && model.powered && model.gpu != NULL);
	}
	int64_t Now()
	{
		return resetting ? ResetModel::Now() : FirmwareModel::Now();
	}
	bool InstallResetHandler()
	{
		if (firmwareMode) {
			assert(!handlers && admissionStops == 1 && !recovering);
			resetting = true;
			resetFault = recoveryReset;
			command = stopping = false;
			resetPauses = 0;
			if (resetFault == StaleUnclearable) regs[kGpuRaw] |= kResetCompleted;
		}
		return ResetModel::InstallResetHandler();
	}
	void ResetDevice()
	{
		// Independent device behavior: active execution, raw fault sources,
		// powered execution units and old AS activity disappear on completion.
		// Deliberately keep old AS register contents; production must remove
		// them explicitly instead of trusting a guessed reset value.
		mcuRunning = asMapped = l2On = asLocked = false;
		for (uint32_t offset : {0x34,0x704,0x1000,0x1008,0x2000,0x2008,
			0x140,0x144,0x150,0x154,0x160,0x164,0x200,0x204,0x210,0x214,
			0x220,0x224,0x304,0x2428,0x2468}) regs[offset] = 0;
		regs[kGpuRaw] &= ~3u;
		if (recoveryFault == RecoverResetTransition) regs[0x204] = 1;
	}
	void PauseReset()
	{
		if (!resetting) {
			if (recovering) { assert(++recoveryPauses < 2500); elapsed += 100; }
			else FirmwareModel::PauseReset();
			return;
		}
		assert(installed && ++resetPauses <= 1001);
		elapsed += 100;
		if (resetPauses == 1) assert(!Deliver());
		if (resetPauses == 3 && resetFault != NoCompletion && resetFault != AtRemoval) {
			ResetDevice(); Complete();
			if (resetFault != NoDelivery && resetFault != ResetFrozenClock
				&& resetFault != ResetBackwardsClock) {
				if (resetFault == LateInterrupt) elapsed += 100001;
				assert(Deliver()); assert(!Deliver());
			}
		}
	}
	void StopResetHandler()
	{
		if (resetting && resetFault == AtRemoval) ResetDevice();
		ResetModel::StopResetHandler();
		if (resetting) { resetting = false; recovering = true; }
	}
	void WriteGpu(uint32_t offset, uint32_t value)
	{
		if (resetting) { ResetModel::WriteGpu(offset, value); return; }
		if (!recovering) { FirmwareModel::WriteGpu(offset, value); return; }
		assert(gpu != NULL && powered && !handlers && !installed);
		recoveryCommands++;
		if (offset == kGpuClear) {
			if (!(resetFault == CleanupAck && value == kResetCompleted)) regs[kGpuRaw] &= ~value;
			return;
		}
		if (offset == kGpuCommand) {
			assert(value == 0x3304 && l2On && !mcuRunning && (locked[0] || locked[1]));
			recoveryFlushes++;
			if (!((recoveryFault == RecoverFlush0 && recoveryFlushes == 1)
				|| (recoveryFault == RecoverFlush1 && recoveryFlushes == 2)))
				regs[kGpuRaw] |= 1u << 17;
			return;
		}
		if (offset == 0x304 && recoveryFault == RecoverConfig) return;
		if (offset == 0x1a0 && value) {
			assert(!mcuRunning && !l2On && value == 1);
			if (recoveryFault != RecoverL2On) { l2On = true; regs[0x160] = 1; }
		} else if (offset == 0x2418 || offset == 0x2458) {
			unsigned as = (offset - 0x2418) / 0x40;
			assert(l2On && !mcuRunning);
			regs[0x2000] |= 1u << (16 + as);
			if (value == 2) locked[as] = true;
			else if (value == 3) locked[as] = false;
			else {
				assert(value == 1 && !locked[as]);
				if ((as == 0 && recoveryFault == RecoverAs0)
					|| (as == 1 && recoveryFault == RecoverAs1)) regs[0x2428 + as * 0x40] = 1;
			}
		} else if (offset == 0x1e0 && value) {
			assert(!mcuRunning && l2On && !locked[0] && !locked[1] && value == 1);
			if (recoveryFault != RecoverL2Off) { l2On = false; regs[0x160] = 0; }
			if (recoveryFault == RecoverMask) regs[0x2008] = 1;
			if (recoveryFault == RecoverJob) regs[0x1000] = 1;
			if (recoveryFault == RecoverMmu) regs[0x2000] |= 1;
			if (recoveryFault == RecoverHighTransition) regs[0x204] = 1;
			if (recoveryFault == RecoverTable) regs[0x2444] = 1;
			if (recoveryFault == RecoverAttributes) regs[0x2448] = 1;
			if (recoveryFault == RecoverAddressConfig) regs[0x2470] = 6;
			if (recoveryFault == RecoverActiveAs) regs[0x2468] = 1;
		}
		regs[offset] = value;
	}
};

struct FaultOperation : NoFirmwareOperation {
	template<typename IO> bool Run(IO& io, FirmwareMemory&, FirmwareRunInfo&)
	{
		io.regs[0x140] = 0x45; // execution units are active at the fault
		io.regs[0x2440] = 0x12345000;
		io.regs[0x2448] = 0x4c;
		io.regs[0x2470] = 6;
		io.regs[0x2468] = 1; // a stuck application address space
		io.regs[kGpuRaw] |= 1;
		io.Event(2);
		return false;
	}
};

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory;
	assert(memory.Plan(image));
	Guarded arena(memory.RequiredBytes());
	unsigned cases = 0;
	for (unsigned fault = RecoverNone; fault <= RecoverResetTransition; fault++) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		RecoveryModel model(&memory, RecoveryFault(fault));
		FirmwareRecovery recovery(RecoveryModel::StopAdmission, &model);
		FirmwareRunInfo info = {};
		info.flags = kFirmwareAllocated;
		FaultOperation operation;
		CycleFirmware(model, memory, info, operation, &recovery);
		printf("recovery cleanup fault=%u original=%u/%u recovery=%u reset=%u/%u flags=%x\n",
			fault, info.result, info.cleanupResult, recovery.result,
			recovery.reset.result, recovery.reset.cleanupResult, info.flags);
		assert(model.admissionStops == 1 && recovery.attempted);
		assert(info.result == kFirmwareFault && info.gpuFault.count == 1);
		assert(info.cleanupResult == kFirmwareStopFailed);
		assert(info.after.mcu == 1 && info.after.shaderReady[0] == 0x45);
		assert(recovery.reset.before.mcu == 1 && recovery.reset.before.shaderReady[0] == 0x45);
		assert(recovery.reset.result == kResetOK && recovery.reset.cleanupResult == kResetOK);
		assert(model.installs == 2 && model.removes == 2 && !model.handlers && !model.installed);
		assert(model.gpu == NULL && !model.powered && (info.flags & kFirmwarePowerRestored));
		if (fault == RecoverNone) {
			assert(recovery.quiescent && recovery.recovered && recovery.result == kFirmwareRunOK);
			assert((info.flags & kFirmwareRecovered) && !(info.flags & kFirmwareNeedsRecovery));
			assert(recovery.reset.flags == 7 && recovery.reset.capture.count == 1);
			assert(recovery.jobRaw == 0 && recovery.mmuRaw == (3u << 16));
			assert(model.recoveryFlushes == 2 && !model.l2On && !model.mcuRunning);
			for (const auto& space : recovery.spaces)
				assert(space.table == 0 && space.attributes == 0 && space.config == 1 && space.status == 0);
		} else {
			assert(!recovery.quiescent && !recovery.recovered && recovery.result != kFirmwareRunOK);
			assert((info.flags & kFirmwareNeedsRecovery) && !(info.flags & kFirmwareRecovered));
		}
		cases++;
	}
	// A firmware IRQ that failed to mask prevents even issuing an active reset.
	assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
	RecoveryModel masked(&memory, RecoverNone, ResetNone, None, FinalMaskFailure);
	FirmwareRecovery maskedRecovery(RecoveryModel::StopAdmission, &masked);
	FirmwareRunInfo maskedInfo = {};
	FaultOperation maskedOperation;
	CycleFirmware(masked, memory, maskedInfo, maskedOperation, &maskedRecovery);
	assert(maskedRecovery.attempted && !maskedRecovery.recovered && masked.installs == 1);
	assert(maskedRecovery.reset.result == kResetInitialStateMismatch);
	assert(maskedInfo.flags & kFirmwareNeedsRecovery);
	cases++;
	for (unsigned reset = InstallFailure; reset <= AtRemoval; reset++) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		RecoveryModel model(&memory, RecoverNone, ResetFault(reset));
		FirmwareRecovery recovery(RecoveryModel::StopAdmission, &model);
		FirmwareRunInfo info = {};
		FaultOperation operation;
		CycleFirmware(model, memory, info, operation, &recovery);
		printf("recovery reset fault=%u result=%u/%u recovered=%u pauses=%u\n", reset,
			recovery.reset.result, recovery.reset.cleanupResult, recovery.recovered, model.resetPauses);
		assert(model.admissionStops == 1 && recovery.attempted && model.resetPauses <= 1001);
		assert(!model.handlers && !model.installed && model.gpu == NULL);
		assert(info.result == kFirmwareFault && info.gpuFault.count == 1);
		if (reset == AtRemoval) assert(recovery.recovered && !(info.flags & kFirmwareNeedsRecovery));
		else {
			assert(!recovery.quiescent && !recovery.recovered && (info.flags & kFirmwareNeedsRecovery));
			assert(recovery.reset.result != kResetOK || recovery.reset.cleanupResult != kResetOK);
			assert(model.recoveryFlushes == 0);
		}
		cases++;
	}
	// A successful GPU reset cannot compensate for failed platform restoration.
	assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
	RecoveryModel restore(&memory, RecoverNone, ResetNone, RestoreClock);
	FirmwareRecovery recovery(RecoveryModel::StopAdmission, &restore);
	FirmwareRunInfo info = {};
	FaultOperation operation;
	CycleFirmware(restore, memory, info, operation, &recovery);
	assert(recovery.quiescent && !recovery.recovered && (info.flags & kFirmwareNeedsRecovery));
	assert(!(info.flags & kFirmwarePowerRestored) && !(info.flags & kFirmwareRecovered));
	cases++;
	// Firmware setup failures keep the original retention policy; no commands
	// have been admitted and the scheduler has not reached its runtime loop.
	for (RunFault run : {MissingBoot, BadInterface, MissingPing}) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		RecoveryModel model(&memory, RecoverNone, ResetNone, None, run);
		FirmwareRecovery recovery(RecoveryModel::StopAdmission, &model);
		FirmwareRunInfo info = {};
		CycleFirmware(model, memory, info, operation, &recovery);
		assert(!recovery.attempted && !recovery.recovered && model.admissionStops == 0);
		assert((info.flags & kFirmwareNeedsRecovery) && model.installs == 1);
		cases++;
	}
	printf("MALI_CSF_RECOVERY_TEST_PASS cases=%u\n", cases);
}
