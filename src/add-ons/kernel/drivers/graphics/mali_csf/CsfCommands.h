/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_COMMANDS_H
#define MALI_CSF_COMMANDS_H

#include "CsfRun.h"
#include "CsfCommandMemory.h"
#include "CsfShader.h"

namespace MaliCSF {

static const uint32_t kCycleCommands = 0x4d435305;
static const uint32_t kCycleShader = 0x4d435306;
static const uint32_t kCommandRunVersion = 1;
static const uint32_t kCommandConfigured = 1;
static const uint32_t kCommandMapped = 2;
static const uint32_t kCommandGroupStarted = 4;
static const uint32_t kCommandRound0 = 8;
static const uint32_t kCommandRound1 = 16;
static const uint32_t kCommandTerminated = 32;
static const uint32_t kCommandHalted = 64;
static const uint32_t kCommandUnmapped = 128;
static const uint32_t kCommandSnapshot = 256;
static const uint32_t kCommandShader = 512;

enum CommandResult {
	kCommandOK = 0, kCommandNotAttempted, kCommandInterfaceFailed,
	kCommandConfigFailed, kCommandMapFailed, kCommandStartFailed,
	kCommandFreshnessFailed, kCommandCompletionFailed, kCommandDataFailed,
	kCommandQueueFault, kCommandTerminateFailed, kCommandHaltFailed,
	kCommandCleanupFailed
};

struct CommandRoundInfo {
	uint64_t insert;
	uint64_t extract;
	uint64_t sequenceBefore;
	uint64_t sequenceAfter;
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t status;
	uint32_t irqBefore;
	uint32_t irqAfter;
	uint32_t syncBefore;
	uint32_t syncAfter;
	uint32_t mismatches;
	uint32_t before[2048];
	uint32_t after[2048];
};

// One system-owned arena backs both address spaces. The large diagnostic
// response must be heap allocated, never placed on the kernel stack. Complete
// buffers are retained for an independent host decoder, including all guards.
struct CommandRunInfo {
	uint32_t version;
	uint32_t firmwareBytes;
	uint32_t result;
	uint32_t cleanupResult;
	uint32_t flags;
	uint32_t roundsCompleted;
	uint64_t rootPhysical;
	uint32_t userTablePages;
	uint32_t userBytes;
	uint32_t arenaBytes;
	uint32_t globalRequest;
	uint32_t globalAck;
	uint32_t progressTimer;
	uint32_t poweroffTimer;
	uint32_t timerRate;
	uint64_t shaderMask;
	uint64_t tilerMask;
	uint32_t groupRequest;
	uint32_t groupAck;
	uint32_t streamRequest;
	uint32_t streamAck;
	uint32_t groupEvents;
	uint32_t syncEvents;
	uint32_t streamFault;
	uint32_t streamFatal;
	uint64_t streamFaultInfo;
	uint64_t streamFatalInfo;
	uint64_t commandPointer;
	uint32_t waitStatus;
	uint32_t blockedReason;
	uint32_t haltStatus;
	uint32_t asStatusAfter;
	uint64_t asConfigAfter;
	FirmwareCapture jobs;
	QueueInterfaceInfo interface;
	FirmwareRunInfo firmware;
	CommandRoundInfo rounds[kCommandRounds];
	uint8_t codeAfter[4096];
	uint8_t ringAfter[65536];
	uint8_t completionAfter[4096];
	uint8_t queueInterfaceAfter[8192];
};

static_assert(sizeof(CommandRoundInfo) == 16456, "Command round ABI changed");
static_assert(sizeof(QueueInterfaceInfo) == 32, "Queue interface ABI changed");
static_assert(sizeof(CommandRunInfo) == 116200, "Command run ABI changed");
static_assert(offsetof(CommandRunInfo, firmware) == 240, "Embedded firmware ABI moved");
static_assert(offsetof(CommandRunInfo, rounds) == 1368, "Command buffers ABI moved");

inline uint64_t ReadInterface64(const volatile uint32_t* words, unsigned byteOffset)
{
	return *(const volatile uint64_t*)(words + byteOffset / 4);
}

inline void WriteInterface64(volatile uint32_t* words, unsigned byteOffset, uint64_t value)
{
	*(volatile uint64_t*)(words + byteOffset / 4) = value;
}

// Uses the CSF 1.5 layout checked by InspectInterface and Linux 6.18.52's
// panthor_fw/sched handshake (MIT option). A single thread owns shared-input
// writes; no atomic RMW is used on the ARM64 Normal-NC shared mapping.
class CommandOperation {
public:
	CommandOperation(CommandMemory& memory, CommandRunInfo& info)
		: fMemory(memory), fInfo(info) {}
	uint32_t CompletedAddressSpaces() const { return 3u << 16; }

	template<typename IO>
	bool Run(IO& io, FirmwareMemory& memory, FirmwareRunInfo& firmware)
	{
		fInfo.result = kCommandInterfaceFailed;
		fInfo.cleanupResult = kCommandOK;
		InterfaceInfo global;
		io.MemoryBarrier();
		if (!InspectInterface(memory.SharedData(), memory.SharedBytes(), memory.SharedAddress(),
				global, &fInfo.interface)
			|| memcmp(&global, &firmware.interface, sizeof(global)) != 0
			|| fInfo.interface.suspendBytes > kSuspendCapacity
			|| fInfo.interface.protectedBytes > kSuspendCapacity)
			return false;
		volatile uint32_t* shared = (volatile uint32_t*)memory.SharedData();
		fGlobalIn = shared + global.inputOffset / 4;
		fGlobalOut = shared + global.outputOffset / 4;
		fGroupIn = shared + fInfo.interface.groupInputOffset / 4;
		fGroupOut = shared + fInfo.interface.groupOutputOffset / 4;
		fStreamIn = shared + fInfo.interface.streamInputOffset / 4;
		fStreamOut = shared + fInfo.interface.streamOutputOffset / 4;
		fQueueIn = (volatile uint32_t*)memory.WorkspaceData(0);
		fQueueOut = fQueueIn + 4096 / 4;
		fInfo.result = kCommandConfigFailed;
		fInfo.timerRate = io.TimerRate();
		fInfo.shaderMask = ReadGpu64(io, 0x100);
		fInfo.tilerMask = ReadGpu64(io, 0x110);
		if (fInfo.timerRate == 0 || fInfo.timerRate > 1000000000
			|| fInfo.shaderMask == 0 || fInfo.tilerMask == 0
			|| fInfo.tilerMask > 0xffffffff || (fGroupOut[0] & 7) != 0
			|| (fStreamOut[0] & 7) != 0 || !io.ArmCommandJob())
			return false;
		// Configure allocation and timers before making any group runnable.
		// MCU idle is disabled for this bounded diagnostic; core poweroff
		// hysteresis and the five-second progress timer match the reference.
		fInfo.progressTimer = (UINT64_C(5) * 500 * 1024 * 1024) >> 10;
		fInfo.poweroffTimer = (UINT64_C(10000) * fInfo.timerRate + 1023999999) / 1024000000;
		fGlobalIn[4] = fInfo.progressTimer;
		fGlobalIn[5] = fInfo.poweroffTimer;
		WriteInterface64(fGlobalIn, 24, fInfo.shaderMask);
		fGlobalIn[1] = 0x10e;
		fGlobalIn[0] = (fGlobalIn[0] & ~(0x40eu)) | ((fGlobalOut[0] ^ 14) & 14);
		fInfo.globalRequest = fGlobalIn[0];
		io.MemoryBarrier();
		io.RingFirmwareDoorbell();
		if (!WaitFirmware(io, 100000, [&]() {
			io.MemoryBarrier();
			return ((fGlobalOut[0] ^ fGlobalIn[0]) & 0x40e) == 0 || io.FirmwareFaulted();
		}) || io.FirmwareFaulted())
			return false;
		fInfo.globalAck = fGlobalOut[0];
		fInfo.flags |= kCommandConfigured;
		fInfo.result = kCommandMapFailed;
		if ((io.ReadGpu(0x18) & 2) == 0 || (io.ReadGpu(0x2468) & 1) != 0
			|| ReadGpu64(io, 0x2470) != 1)
			return false;
		fInfo.flags |= kCommandMapped; // exposure precedes the first AS write
		io.MemoryBarrier();
		WriteGpu64(io, 0x2440, fMemory.RootPhysical());
		WriteGpu64(io, 0x2448, FirmwareMemory::MemoryAttributes());
		WriteGpu64(io, 0x2470, FirmwareMemory::TranslationConfig());
		if (!FirmwareAsCommand(io, 1, 1)
			|| ReadGpu64(io, 0x2440) != fMemory.RootPhysical()
			|| ReadGpu64(io, 0x2448) != FirmwareMemory::MemoryAttributes()
			|| ReadGpu64(io, 0x2470) != FirmwareMemory::TranslationConfig())
			return false;
		fInfo.result = kCommandStartFailed;
		// Start an empty queue; no command is visible until the fresh fence and
		// complete initial buffer have been recorded for the first submission.
		WriteInterface64(fStreamIn, 16, kRingAddress);
		fStreamIn[6] = 65536;
		WriteInterface64(fStreamIn, 48, kQueueInterfaceAddress);
		WriteInterface64(fStreamIn, 56, kQueueInterfaceAddress + 4096);
		fStreamIn[1] = 1u << 8; // user doorbell 1, queue priority 0
		fStreamIn[3] = ~0u;
		fStreamIn[0] = (fStreamIn[0] & ~0x517u) | 0x511u;
		WriteInterface64(fGroupIn, 32, fInfo.shaderMask);
		WriteInterface64(fGroupIn, 40, fInfo.shaderMask);
		fGroupIn[12] = uint32_t(fInfo.tilerMask);
		unsigned shaders = BitCount(fInfo.shaderMask), tilers = BitCount(fInfo.tilerMask);
		if (tilers > 15)
			return false;
		fGroupIn[13] = shaders | (shaders << 8) | (tilers << 16);
		WriteInterface64(fGroupIn, 64, fInfo.interface.suspendBytes ? kSuspendAddress : 0);
		WriteInterface64(fGroupIn, 72, fInfo.interface.protectedBytes ? kProtectedSuspendAddress : 0);
		fGroupIn[20] = 1; // application address space
		fGroupIn[1] = ~0u;
		fGroupIn[3] = fGroupOut[3];
		fGroupIn[0] = (fGroupIn[0] & ~0xb0000017u) | (fGroupOut[0] & 0xb0000000u)
			| ((fGroupOut[0] ^ 16) & 16) | 1;
		fInfo.flags |= kCommandGroupStarted; // request may expose all queue memory
		RingGroup(io, true);
		if (!WaitFirmware(io, 100000, [&]() {
			Poll(io);
			return (((fGroupIn[0] ^ fGroupOut[0]) & 0x17) == 0
				&& ((fStreamIn[0] ^ fStreamOut[0]) & 7) == 0) || Faulted(io);
		}) || Faulted(io))
			return false;
		for (unsigned round = 0; round < kCommandRounds; round++) {
			if (!Submit(io, round))
				return false;
		}
		fInfo.result = kCommandOK;
		return true;
	}

	template<typename IO>
	bool BeforeStop(IO& io)
	{
		bool ok = true;
		if ((fInfo.flags & kCommandGroupStarted) != 0) {
			fGroupIn[0] &= ~7u;
			RingGroup(io, false);
			bool terminated = WaitFirmware(io, 100000, [&]() {
				Poll(io);
				return (fGroupOut[0] & 7) == 0;
			});
			CaptureQueue();
			if (Faulted(io)) {
				fInfo.result = kCommandQueueFault;
				ok = false;
			}
			if (terminated)
				fInfo.flags |= kCommandTerminated;
			else {
				fInfo.cleanupResult = kCommandTerminateFailed;
				ok = false;
			}
		}
		if (fGlobalIn != NULL) {
			fGlobalIn[0] |= 1; // GLB_HALT: drain firmware work before MCU disable
			io.MemoryBarrier();
			io.RingFirmwareDoorbell();
			bool halted = WaitFirmware(io, 100000, [&]() { return io.ReadGpu(0x704) == 2; });
			fInfo.haltStatus = io.ReadGpu(0x704);
			if (halted)
				fInfo.flags |= kCommandHalted;
			else {
				fInfo.cleanupResult = kCommandHaltFailed;
				ok = false;
			}
		}
		fInfo.jobs = io.ReadFirmwareCapture(0);
		return ok;
	}

	template<typename IO>
	bool Cleanup(IO& io, bool stopped)
	{
		if ((fInfo.flags & kCommandMapped) == 0)
			return true;
		bool ok = stopped && FlushFirmware(io, 1);
		if (ok) {
			io.MemoryBarrier();
			memcpy(fInfo.codeAfter, fMemory.Code(), sizeof(fInfo.codeAfter));
			memcpy(fInfo.ringAfter, fMemory.Ring(), sizeof(fInfo.ringAfter));
			memcpy(fInfo.completionAfter, fMemory.Completion(), sizeof(fInfo.completionAfter));
			memcpy(fInfo.queueInterfaceAfter, fMemory.Firmware().WorkspaceData(0),
				sizeof(fInfo.queueInterfaceAfter));
			fInfo.flags |= kCommandSnapshot;
			WriteGpu64(io, 0x2470, 1);
			WriteGpu64(io, 0x2440, 0);
			WriteGpu64(io, 0x2448, 0);
			ok = FirmwareAsCommand(io, 1, 1) && ReadGpu64(io, 0x2470) == 1
				&& ReadGpu64(io, 0x2440) == 0 && ReadGpu64(io, 0x2448) == 0;
		}
		fInfo.asStatusAfter = io.ReadGpu(0x2468);
		fInfo.asConfigAfter = ReadGpu64(io, 0x2470);
		if (ok)
			fInfo.flags |= kCommandUnmapped;
		else
			fInfo.cleanupResult = kCommandCleanupFailed;
		return ok;
	}

private:
	static unsigned BitCount(uint64_t mask)
	{
		unsigned result = 0;
		for (; mask != 0; mask >>= 1) result += mask & 1;
		return result;
	}
	template<typename IO> void RingGroup(IO& io, bool queue)
	{
		if (queue)
			fGroupIn[2] = (fGroupIn[2] & ~1u) | ((fGroupOut[2] ^ 1) & 1);
		fGlobalIn[2] = (fGlobalIn[2] & ~1u) | ((fGlobalOut[2] ^ 1) & 1);
		io.MemoryBarrier();
		io.RingFirmwareDoorbell();
	}
	void CaptureQueue()
	{
		fInfo.groupRequest = fGroupIn[0]; fInfo.groupAck = fGroupOut[0];
		fInfo.streamRequest = fStreamIn[0]; fInfo.streamAck = fStreamOut[0];
		fInfo.streamFault = fStreamOut[32]; fInfo.streamFatal = fStreamOut[33];
		fInfo.streamFaultInfo = ReadInterface64(fStreamOut, 136);
		fInfo.streamFatalInfo = ReadInterface64(fStreamOut, 144);
		fInfo.commandPointer = ReadInterface64(fStreamOut, 64);
		fInfo.waitStatus = fStreamOut[18]; fInfo.blockedReason = fStreamOut[24];
	}
	template<typename IO> bool Faulted(IO& io)
	{
		return io.FirmwareFaulted() || fQueueFault;
	}
	template<typename IO> void Poll(IO& io)
	{
		io.MemoryBarrier();
		uint32_t ack = fGroupOut[0], events = (fGroupIn[0] ^ ack) & 0xb0000000u;
		uint32_t irqs = fGroupIn[3] ^ fGroupOut[3];
		// Ack the stream IRQ bitmap before reading its request/ack interface.
		fGroupIn[3] = fGroupOut[3];
		io.MemoryBarrier();
		uint32_t streamEvents = (fStreamIn[0] ^ fStreamOut[0]) & 0xcc000000u;
		fInfo.groupEvents |= events;
		if ((events & (1u << 28)) != 0)
			fInfo.syncEvents++;
		if ((events & (1u << 31)) != 0 || streamEvents != 0 || (irqs & ~1u) != 0)
			fQueueFault = true;
		CaptureQueue();
		if (fInfo.streamFault != 0 || fInfo.streamFatal != 0)
			fQueueFault = true;
		fGroupIn[0] = (fGroupIn[0] & ~0xb0000000u) | (ack & 0xb0000000u);
		// A fatal/protected/OOM request is evidence, not permission to attempt
		// recovery in-place. The bounded outer teardown retains the arena.
		if (events != 0 || irqs != 0)
			RingGroup(io, false);
	}
	template<typename IO> bool Submit(IO& io, unsigned round)
	{
		CommandRoundInfo& result = fInfo.rounds[round];
		volatile uint32_t* completion = (volatile uint32_t*)fMemory.Completion() + round * 8;
		fInfo.result = kCommandFreshnessFailed;
		if (!fMemory.PrepareData(round))
			return false;
		io.MemoryBarrier();
		result.sequenceBefore = ReadInterface64(completion, 0);
		memcpy(result.before, fMemory.Data(), sizeof(result.before));
		if (result.sequenceBefore != 0 || completion[2] != 0 || completion[3] != 0
			|| ReadInterface64(fQueueIn, 0) != round * 128
			|| ReadInterface64(fQueueOut, 0) != round * 128)
			return false;
		result.irqBefore = io.CommandJobCount();
		result.syncBefore = fInfo.syncEvents;
		result.startedMicros = io.Now();
		result.insert = (round + 1) * 128;
		WriteInterface64(fQueueIn, 8, ReadInterface64(fQueueOut, 0));
		WriteInterface64(fQueueIn, 0, result.insert);
		RingGroup(io, true);
		fInfo.result = kCommandCompletionFailed;
		bool completed = WaitFirmware(io, 5000000, [&]() {
			Poll(io);
			return (ReadInterface64(completion, 0) == 1
				&& ReadInterface64(fQueueOut, 0) == result.insert
				&& io.CommandJobCount() > result.irqBefore
				&& fInfo.syncEvents > result.syncBefore) || Faulted(io);
		});
		io.MemoryBarrier();
		result.finishedMicros = io.Now();
		result.sequenceAfter = ReadInterface64(completion, 0);
		result.extract = ReadInterface64(fQueueOut, 0);
		result.status = completion[2];
		result.irqAfter = io.CommandJobCount();
		result.syncAfter = fInfo.syncEvents;
		memcpy(result.after, fMemory.Data(), sizeof(result.after));
		if (Faulted(io)) {
			fInfo.result = kCommandQueueFault;
			return false;
		}
		if (!completed || result.status != 0 || completion[3] != 0)
			return false;
		for (unsigned word = 0; word < 2048; word++) {
			uint32_t expected = CommandGuard(round, word);
			if (result.before[word] != expected)
				result.mismatches++;
			for (unsigned i = 0; i < 8; i++) {
				if (word == kStoreWordIndices[i])
					expected = CommandValue(round, i);
			}
			if (result.after[word] != expected)
				result.mismatches++;
		}
		if (result.mismatches != 0) {
			fInfo.result = kCommandDataFailed;
			return false;
		}
		fInfo.roundsCompleted++;
		fInfo.flags |= round == 0 ? kCommandRound0 : kCommandRound1;
		return true;
	}
	CommandMemory& fMemory;
	CommandRunInfo& fInfo;
	volatile uint32_t* fGlobalIn = NULL;
	const volatile uint32_t* fGlobalOut = NULL;
	volatile uint32_t* fGroupIn = NULL;
	const volatile uint32_t* fGroupOut = NULL;
	volatile uint32_t* fStreamIn = NULL;
	const volatile uint32_t* fStreamOut = NULL;
	volatile uint32_t* fQueueIn = NULL;
	const volatile uint32_t* fQueueOut = NULL;
	bool fQueueFault = false;
};

} // namespace MaliCSF
#endif
