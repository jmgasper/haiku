/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfCommands.h"
#define MALI_RUN_MODEL_ONLY
#include "test_mali_run.cpp"
#undef MALI_RUN_MODEL_ONLY

enum CommandFault { CommandNone, LargeSuspend, NoTimer, ConfigTimeout, UserASBusy,
	UserMapTimeout, GroupStartTimeout, DirtyFence, MissingFence, MissingJobIRQ,
	MissingSyncEvent, BadGuard, StreamFault, GroupProgress, TerminateTimeout,
	HaltTimeout, McuStopTimeout, UserFlushTimeout, UserUnmapTimeout,
	SecondRoundFence, BadExtract, CommandClockStopped };

// Firmware/interrupt model, independent of the operation's waits. It delays
// acknowledgements until time advances, interprets the supplied instruction
// bytes, and separately publishes fences, extract pointers and IRQs. This is
// a host lifecycle test; it is never evidence of native GPU execution.
struct CommandModel : FirmwareModel {
	CommandMemory& commandMemory;
	CommandFault commandFault;
	bool commandMode = false, pending = false, userMapped = false, userLocked = false;
	bool groupRunning = false, globalConfigured = false;
	unsigned groupIRQs = 0, commandPauses = 0, executed = 0;
	CommandModel(CommandMemory& memory, CommandFault fault)
		: FirmwareModel(&memory.Firmware()), commandMemory(memory), commandFault(fault) {}
	uint32_t TimerRate() { return commandFault == NoTimer ? 0 : 24000000; }
	int64_t Now()
	{
		return commandMode && commandFault == CommandClockStopped ? 200000 : FirmwareModel::Now();
	}
	bool InstallFirmwareHandlers()
	{
		bool result = FirmwareModel::InstallFirmwareHandlers();
		regs[0x18] = 0xffff;
		regs[0x100] = 0x50005;
		regs[0x110] = 1;
		regs[0x2470] = 1;
		return result;
	}
	bool ArmCommandJob()
	{
		assert(handlers && !jobArmed && mcuRunning && captures[0].count == 1);
		commandMode = true; captures[0] = {};
		regs[0x1008] = 0x80000001;
		if (commandFault == DirtyFence) ((uint64_t*)commandMemory.Completion())[0] = 1;
		if (commandFault == UserASBusy) regs[0x2468] = 1;
		return true;
	}
	uint32_t CommandJobCount() { return groupIRQs; }
	void Event(bool group)
	{
		if (commandFault == MissingJobIRQ && group && executed != 0) return;
		auto& event = captures[0]; event.count++;
		event.status |= group ? 1 : 0x80000000;
		event.raw |= group ? 1 : 0x80000000;
		event.cpu = 3; event.whenMicros = Now();
		if (group) groupIRQs++;
	}
	uint32_t* In(unsigned control) { return (uint32_t*)memory->SharedData() + (Words()[control] - 0x4000000) / 4; }
	uint32_t* Words() { return (uint32_t*)memory->SharedData(); }
	void RingFirmwareDoorbell()
	{
		if (!commandMode && !(In(2)[0] & 1)) FirmwareModel::RingFirmwareDoorbell();
		else pending = true;
	}
	void WriteGpu(uint32_t offset, uint32_t value)
	{
		if (firmwareMode && offset >= 0x2440 && offset < 0x2480) {
			if (offset == 0x2458) {
				assert(l2On);
				regs[0x2000] |= 1u << 17;
				if (value == 2) { assert(!mcuRunning); userLocked = true; }
				else if (value == 3) { assert(userLocked); userLocked = false; }
				else {
					assert(value == 1 && !userLocked);
					if (regs[0x2470] == 1) {
						assert(!mcuRunning);
						if (commandFault == UserUnmapTimeout) regs[0x2468] = 1;
						else userMapped = false;
					} else {
						assert(globalConfigured && !userMapped && !groupRunning);
						assert(ReadGpu64(*this, 0x2440) == commandMemory.RootPhysical());
						userMapped = true;
						if (commandFault == UserMapTimeout) regs[0x2468] = 1;
					}
				}
			}
			regs[offset] = value;
			return;
		}
		if (offset == kGpuCommand && userLocked) {
			assert(value == 0x3304 && l2On && !mcuRunning);
			if (commandFault != UserFlushTimeout) regs[kGpuRaw] |= 1u << 17;
			return;
		}
		if (offset == 0x700 && value == 0 && commandFault == McuStopTimeout) return;
		FirmwareModel::WriteGpu(offset, value);
	}
	void ExecuteStream(const uint64_t* code)
	{
		uint32_t r[96] = {};
		unsigned stores = 0;
		for (unsigned i = 0; i < 32; i++) {
			uint64_t ins = code[i]; unsigned op = ins >> 56, reg = (ins >> 48) & 255;
			if (op == 1) { r[reg] = uint32_t(ins); r[reg + 1] = (ins >> 32) & 65535; }
			else if (op == 2) r[reg] = uint32_t(ins);
			else if (op == 21) {
				unsigned addressReg = (ins >> 40) & 255;
				uint64_t addr = r[addressReg] | (uint64_t(r[addressReg + 1]) << 32);
				addr += ins & 65535;
				assert(addr >= 0x100002000 && addr < 0x100004000 && (addr & 3) == 0);
				assert(((ins >> 16) & 65535) == 1);
				((uint32_t*)commandMemory.Data())[(addr - 0x100002000) / 4] = r[reg];
				stores++;
			} else assert(op == 0 || op == 3 || op == 23 || op == 36);
		}
		assert(stores == 8);
	}
	void Execute(unsigned round)
	{
		assert(userMapped && groupRunning && globalConfigured && round == executed);
		const uint64_t* ring = (uint64_t*)commandMemory.Ring() + round * 16;
		uint32_t r[96] = {};
		for (unsigned i = 0; i < 16; i++) {
			uint64_t ins = ring[i]; unsigned op = ins >> 56, reg = (ins >> 48) & 255;
			if (op == 1) { r[reg] = uint32_t(ins); r[reg + 1] = (ins >> 32) & 65535; }
			else if (op == 2) r[reg] = uint32_t(ins);
			else if (op == 32 || op == 51) {
				unsigned ar = (ins >> 40) & 255, vr = (ins >> 32) & 255;
				uint64_t addr = r[ar] | (uint64_t(r[ar + 1]) << 32);
				if (op == 32) {
					assert(addr == UINT64_C(0x100000000) + round * 256 && r[vr] == 256);
					ExecuteStream((uint64_t*)commandMemory.Code() + (addr - 0x100000000) / 8);
				} else {
					assert(addr == UINT64_C(0x100800000) + round * 32 && r[vr] == 1 && r[vr + 1] == 0);
					if (commandFault != MissingFence && commandFault != CommandClockStopped
						&& !(commandFault == SecondRoundFence && round == 1))
						((uint64_t*)commandMemory.Completion())[(addr - 0x100800000) / 8] += 1;
				}
			} else assert(op == 0 || op == 3 || op == 36 || op == 47);
		}
		executed++;
		uint32_t* go = In(1026);
		if (commandFault != MissingSyncEvent) go[0] ^= 1u << 28;
		if (commandFault == BadGuard) ((uint32_t*)commandMemory.Data())[2047] ^= 1;
		if (commandFault == StreamFault) { In(1042)[32] = 0x45; In(1042)[0] ^= 1u << 31; go[3] ^= 1; }
		if (commandFault == GroupProgress) go[0] ^= 1u << 31;
		uint64_t* queue = (uint64_t*)memory->WorkspaceData(0);
		queue[512] = commandFault == BadExtract ? queue[0] + 8 : queue[0];
		Event(true);
	}
	void PauseReset()
	{
		if (!commandMode && !pending) {
			FirmwareModel::PauseReset();
			if (!pendingBoot && mcuRunning && commandFault == LargeSuspend) {
				for (unsigned g = 0; g < 8; g++) Words()[(0x1000 + g * 0xa0) / 4 + 3] = 1048577;
			}
			return;
		}
		assert(++commandPauses < 160000);
		elapsed += 100;
		if (!pending) return;
		pending = false;
		uint32_t* gi = In(2); uint32_t* go = In(3);
		uint32_t* cgi = In(1025); uint32_t* cgo = In(1026);
		uint32_t* csi = In(1041); uint32_t* cso = In(1042);
		if (gi[0] & 1) {
			if (commandFault != HaltTimeout) {
				regs[0x704] = 2; regs[0x140] = regs[0x150] = 0;
			}
			return;
		}
		if (((gi[0] ^ go[0]) & 14) != 0) {
			assert(!groupRunning && !userMapped && gi[4] == 2560000 && gi[5] == 235);
			assert(ReadInterface64(gi, 24) == 0x50005 && !(gi[0] & 0x400));
			if (commandFault == ConfigTimeout) return;
			go[0] = gi[0]; globalConfigured = true; Event(false);
		}
		if (((gi[2] ^ go[2]) & 1) == 0) return;
		go[2] = gi[2]; cgo[2] = cgi[2];
		if ((cgi[0] & 7) == 1 && !groupRunning) {
			assert(userMapped && ReadInterface64(csi, 16) == 0x100400000 && csi[6] == 65536);
			assert(ReadInterface64(csi, 48) == 0x4010000 && ReadInterface64(csi, 56) == 0x4011000);
			assert(cgi[20] == 1 && cgi[13] == 0x10404);
			assert(((uint64_t*)memory->WorkspaceData(0))[0] == 0);
			if (commandFault == GroupStartTimeout) return;
			cgo[0] = cgi[0]; cso[0] = csi[0]; groupRunning = true;
			regs[0x140] = 0x50005; regs[0x150] = 1; Event(true);
		} else if ((cgi[0] & 7) == 0 && groupRunning) {
			if (commandFault != TerminateTimeout) {
				cgo[0] &= ~7u; groupRunning = false; Event(true);
			}
		}
		uint64_t* queue = (uint64_t*)memory->WorkspaceData(0);
		if (groupRunning && queue[0] > executed * 128) {
			assert(queue[0] == (executed + 1) * 128);
			Execute(executed);
		}
	}
};

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	CommandMemory memory;
	assert(memory.Plan(image));
	Guarded arena(memory.RequiredBytes());
	for (unsigned fault = CommandNone; fault <= CommandClockStopped; fault++) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		CommandRunInfo* info = new CommandRunInfo{};
		CommandModel model(memory, CommandFault(fault));
		CommandOperation operation(memory, *info);
		info->firmware.flags = kFirmwareAllocated;
		CycleFirmware(model, memory.Firmware(), info->firmware, operation);
		printf("commands fault=%u result=%u cleanup=%u flags=%x rounds=%u fw=%u/%u/%x pauses=%u\n",
			fault, info->result, info->cleanupResult, info->flags, info->roundsCompleted,
			info->firmware.result, info->firmware.cleanupResult, info->firmware.flags, model.commandPauses);
		assert(!model.handlers && model.gpu == NULL);
		if (fault == CommandNone) {
			assert(info->result == kCommandOK && info->cleanupResult == kCommandOK);
			assert(info->flags == 511 && info->roundsCompleted == 2 && model.executed == 2);
			assert(info->firmware.result == kFirmwareRunOK && info->firmware.cleanupResult == kFirmwareRunOK);
			assert(info->firmware.flags == 255 && info->firmware.mmuRawAfter == (3u << 16));
			assert(!model.userMapped && !model.groupRunning && !model.mcuRunning && !model.l2On);
			assert(info->jobs.count >= 4 && (info->jobs.status & 1) != 0 && info->asConfigAfter == 1);
			for (unsigned r = 0; r < 2; r++) {
				assert(info->rounds[r].sequenceBefore == 0 && info->rounds[r].sequenceAfter == 1);
				assert(info->rounds[r].irqAfter > info->rounds[r].irqBefore);
				assert(info->rounds[r].syncAfter > info->rounds[r].syncBefore);
			}
		} else {
			assert(info->result != 0 || info->cleanupResult != 0);
			assert((info->firmware.flags & kFirmwareNeedsRecovery) != 0);
		}
		if (fault == MissingFence || fault == MissingJobIRQ || fault == MissingSyncEvent
			|| fault == BadExtract || fault == CommandClockStopped) assert(info->roundsCompleted == 0);
		if (fault == SecondRoundFence) assert(info->roundsCompleted == 1);
		if (fault == McuStopTimeout || fault == UserFlushTimeout || fault == UserUnmapTimeout)
			assert((info->flags & kCommandUnmapped) == 0 && model.userMapped);
		delete info;
	}
	puts("MALI_CSF_COMMANDS_TEST_PASS");
}
