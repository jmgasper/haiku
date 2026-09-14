#include "CsfReset.h"

// Reuse the independent delayed power-domain model and its protected GPU page.
#define main MaliPowerFixtureMain
#include "test_mali_power.cpp"
#undef main

enum ResetFault { ResetNone, InstallFailure, StaleUnclearable, EnableFailure,
	NoCompletion, NoDelivery, WrongInterrupt, FaultAtInterrupt, CleanupMask,
	CleanupAck, LateInterrupt, ResetFrozenClock, ResetBackwardsClock, AtRemoval };

struct ResetModel : Model {
	ResetFault resetFault;
	std::map<uint32_t, uint32_t> regs;
	ResetInterruptState state{};
	bool installed = false, command = false, stopping = false;
	unsigned resetPauses = 0, resetWrites = 0, installs = 0, removes = 0;
	unsigned lockDepth = 0;
	bool unchangedIdentity = false;

	explicit ResetModel(ResetFault fault = ResetNone, Fault powerFault = None)
		: Model(powerFault), resetFault(fault)
	{
		for (uint32_t offset : {0x20,0x24,0x28,0x2c,0x30,0x34,0x704,0x1008,0x2008,
				0x140,0x144,0x150,0x154,0x160,0x164,0x200,0x210,0x220}) regs[offset] = 0;
		regs[kGpuRaw] = kResetCompleted | (1u << 9); // Stale reset plus unrelated raw power event.
	}
	~ResetModel() { assert(!installed && lockDepth == 0); }
	int64_t Now()
	{
		if (command && !stopping && resetFault == ResetFrozenClock) return 80;
		if (command && !stopping && resetFault == ResetBackwardsClock) return -1;
		return Model::Now();
	}
	uint32_t ReadGpu(uint32_t offset)
	{
		assert(gpu != NULL && powered && !(power.at(0x118) & 1));
		if (offset == kGpuInterruptStatus) return regs[kGpuRaw] & regs[kGpuMask];
		if (regs.count(offset)) return regs.at(offset);
		return Model::ReadGpu(offset);
	}
	void WriteGpu(uint32_t offset, uint32_t value)
	{
		assert(gpu != NULL && powered && !(power.at(0x118) & 1));
		resetWrites++;
		if (offset == kGpuClear) {
			assert(value == 0 || value == kResetCompleted);
			if (resetFault != StaleUnclearable && !(command && resetFault == CleanupAck))
				regs[kGpuRaw] &= ~value;
		} else if (offset == kGpuMask) {
			assert(value == 0 || value == kResetCompleted);
			if ((resetFault == EnableFailure && value) || (resetFault == CleanupMask && !value && command)) return;
			regs[kGpuMask] = value;
		} else {
			assert(offset == kGpuCommand && value == kSoftReset);
			assert(lockDepth == 1 && installed && state.armed && !command);
			assert(regs[kGpuMask] == kResetCompleted && !(regs[kGpuRaw] & kResetCompleted));
			command = true;
		}
	}
	int32_t Cpu() { return 0; }
	bool Deliver()
	{
		assert(installed && lockDepth == 0);
		lockDepth++;
		bool handled = CaptureResetInterrupt(*this, state);
		assert(--lockDepth == 0);
		return handled;
	}
	bool InstallResetHandler()
	{
		assert(!installed && gpu != NULL && ReadGpu(kGpuMask) == 0);
		installs++;
		if (resetFault == InstallFailure) return false;
		installed = true;
		assert(!Deliver()); // An unrelated vector invocation cannot touch unarmed state.
		return true;
	}
	ResetResult BeginReset()
	{
		assert(++lockDepth == 1);
		auto result = ArmReset(*this, state);
		assert(--lockDepth == 0);
		return result;
	}
	ResetCapture ReadResetCapture() { return state.capture; }
	void Complete()
	{
		regs[kGpuRaw] |= kResetCompleted;
		if (resetFault == FaultAtInterrupt) regs[kGpuRaw] |= 1;
		if (resetFault == WrongInterrupt) {
			regs[kGpuMask] = 2;
			regs[kGpuRaw] = 2;
		}
	}
	void PauseReset()
	{
		assert(installed && ++resetPauses <= 1001);
		elapsed += 100;
		if (resetPauses == 1) assert(!Deliver()); // Armed, but no GPU source pending.
		if (resetPauses == 3 && resetFault != NoCompletion && resetFault != AtRemoval) {
			Complete();
			if (resetFault != NoDelivery && resetFault != ResetFrozenClock && resetFault != ResetBackwardsClock) {
				if (resetFault == LateInterrupt) elapsed += 100001;
				assert(Deliver());
				assert(!Deliver()); // The source is disarmed after one event.
			}
		}
	}
	void StopResetHandler()
	{
		assert(installed && gpu != NULL && lockDepth == 0);
		stopping = true;
		if (resetFault == AtRemoval) { Complete(); assert(Deliver()); }
		lockDepth++;
		WriteGpu(kGpuMask, 0);
		state.armed = false;
		lockDepth--;
		assert(!Deliver()); // A queued invocation cannot read MMIO after disarming.
		installed = false;
		removes++;
	}
	void UnmapGpu()
	{
		assert(!installed && lockDepth == 0);
		Model::UnmapGpu();
	}
};

int main()
{
	static_assert(sizeof(ResetSnapshot) == 64, "Reset register ABI changed");
	static_assert(sizeof(ResetCapture) == 32, "Reset capture ABI changed");
	static_assert(sizeof(ResetInfo) == 464, "Reset ABI changed");
	for (unsigned repeat = 0; repeat < 3; repeat++) {
		ResetModel model;
		auto clocks = model.clock, powers = model.power;
		ResetInfo info;
		CycleReset(model, info, 126);
		assert(info.version == 1 && info.result == kResetOK && info.cleanupResult == kResetOK);
		assert(info.flags == 7 && info.interrupt == 126 && model.command);
		assert(info.capture.count == 1 && info.capture.status == 256 && info.capture.raw == 768);
		assert(info.before.raw == 768 && info.after.raw == 512 && info.completionRaw == 512);
		assert(info.power.result == kIdentityOK && info.power.restoreResult == kIdentityOK && info.power.flags == 7);
		assert(model.clock == clocks && model.power == powers && !model.powered);
		assert(model.installs == 1 && model.removes == 1 && model.gpu == NULL);
	}
	for (uint32_t offset : {0x28,0x34,0x704,0x1008,0x2008,0x140,0x144,0x150,0x154,0x160,0x164,0x200,0x210,0x220}) {
		ResetModel model;
		model.regs[offset] = 1;
		ResetInfo info;
		CycleReset(model, info, 126);
		assert(info.result == kResetInitialStateMismatch && model.resetWrites == 0);
		assert(!model.command && !model.installs && !model.removes && model.gpu == NULL);
		assert(info.power.restoreResult == kIdentityOK && (info.flags & kResetNeedsRecovery));
	}
	for (auto pair : std::vector<std::pair<ResetFault, ResetResult>>{
		{InstallFailure,kResetHandlerInstallFailed}, {StaleUnclearable,kResetClearFailed},
		{EnableFailure,kResetMaskFailed}, {NoCompletion,kResetTimedOut},
		{NoDelivery,kResetInterruptMissing}, {WrongInterrupt,kResetInterruptMismatch},
		{FaultAtInterrupt,kResetInterruptMismatch}, {CleanupMask,kResetOK},
		{CleanupAck,kResetOK}, {LateInterrupt,kResetInterruptMismatch},
		{ResetFrozenClock,kResetInterruptMissing}, {ResetBackwardsClock,kResetClockFailed},
		{AtRemoval,kResetOK}}) {
		ResetModel model(pair.first);
		ResetInfo info;
		CycleReset(model, info, 126);
		if (info.result != pair.second) fprintf(stderr, "fault=%u result=%u expected=%u\n", pair.first, info.result, pair.second);
		assert(info.result == pair.second && !model.installed && model.gpu == NULL);
		assert(model.installs == 1 && model.removes == (pair.first == InstallFailure ? 0u : 1u));
		if (pair.first == CleanupMask || pair.first == CleanupAck || pair.first == FaultAtInterrupt
			|| pair.first == WrongInterrupt || pair.first == StaleUnclearable)
			assert(info.cleanupResult == kResetCleanupFailed);
		else assert(info.cleanupResult == kResetOK);
		if (pair.first != AtRemoval) assert(info.flags & kResetNeedsRecovery);
		assert(info.power.restoreResult == kIdentityOK && info.power.flags == 7);
		if (pair.first == NoDelivery) assert(info.capture.count == 0 && (info.completionRaw & 256));
		assert(model.resetPauses <= 1001);
	}
	for (Fault fault : {PowerUp, Mapping, WrongID, WrongFeatures}) {
		ResetModel model(ResetNone, fault);
		ResetInfo info;
		CycleReset(model, info, 126);
		assert(info.result == kResetPowerCycleFailed && !model.command && !model.installs);
		assert(info.flags == kResetNeedsRecovery && model.gpu == NULL);
	}
	// The original read-only identity operation never reaches reset methods.
	ResetModel identity;
	IdentityInfo info;
	CycleIdentity(identity, info);
	assert(info.result == kIdentityOK && info.flags == 7 && identity.resetWrites == 0 && !identity.installs);
	puts("MALI_CSF_RESET_TEST_PASS");
	return 0;
}
