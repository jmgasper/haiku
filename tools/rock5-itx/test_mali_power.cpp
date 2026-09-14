#include "CsfPower.h"

#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <map>
#include <vector>

using namespace MaliCSF;

enum Fault { None, ClockWrite, InitialIdleWrite, PowerUp, DeIdle, Mapping,
	WrongID, WrongFeatures, RestoreIdle, PowerDown, RestoreClock, PoweredClock,
	FrozenClock, BackwardsClock };

struct Model {
	Fault fault;
	std::map<uint32_t, uint32_t> clock{{0x578,0x1f80},{0x57c,0},{0x580,0},{0x908,4},{0x90c,0}};
	std::map<uint32_t, uint32_t> power{{0x10c,0},{0x118,0xfff},{0x120,0xfff},{0x14c,0xfff9},{0x290,0xffff8001}};
	unsigned writes = 0, maps = 0, gpuReads = 0, pauses = 0, onDelay = 0, offDelay = 0, idleDelay = 0;
	int64_t elapsed = 0, clockCalls = 0;
	bool powered = false, deidled = false, touchedPower = false;
	void* gpu = NULL;
	size_t page = sysconf(_SC_PAGESIZE);

	explicit Model(Fault fault = None) : fault(fault) {}
	~Model() { assert(gpu == NULL); }
	int64_t Now()
	{
		if (fault == FrozenClock) return 0;
		if (fault == BackwardsClock) return 100000 - clockCalls++;
		return elapsed;
	}
	uint32_t ReadClock(uint32_t offset) { return clock.at(offset); }
	uint32_t ReadPower(uint32_t offset) { return power.at(offset); }
	void WriteClock(uint32_t offset, uint32_t value)
	{
		assert(gpu == NULL && !powered);
		assert(offset == 0x578 && (value >> 16) == 31);
		assert((value & 0xffff) == 3 || (value & 0xffff) == 0);
		writes++;
		if ((fault == ClockWrite && (value & 31) == 3)
			|| (fault == RestoreClock && (value & 31) == 0)) return;
		clock[offset] = (clock[offset] & ~(value >> 16)) | (value & (value >> 16));
	}
	void WritePower(uint32_t offset, uint32_t value)
	{
		assert(gpu == NULL);
		assert((offset == 0x10c || offset == 0x14c) && (value >> 16) == 1);
		assert((value & 0xffff) <= 1);
		writes++;
		if (fault == InitialIdleWrite && offset == 0x10c && !touchedPower && (value & 1)) return;
		uint32_t old = power[offset];
		power[offset] = (old & ~1u) | (value & 1);
		if (offset == 0x14c && ((old ^ value) & 1)) {
			touchedPower = true;
			assert((clock[0x578] & 0xff) == 0x83);
			if ((value & 1) == 0) {
				assert((power[0x10c] & 1) && (power[0x118] & 1) && (power[0x120] & 1));
				if (fault != PowerUp && fault != FrozenClock) onDelay = 2;
			} else {
				assert(powered && (power[0x118] & 1) && (power[0x120] & 1));
				if (fault != PowerDown) offDelay = 2;
			}
		} else if (offset == 0x10c && powered && ((old ^ value) & 1)) {
			if (!((fault == DeIdle && !(value & 1)) || (fault == RestoreIdle && deidled && (value & 1))))
				idleDelay = 2;
		}
	}
	void Pause()
	{
		assert(++pauses <= 5000);
		elapsed += 10;
		if (onDelay && --onDelay == 0) {
			powered = true; power[0x290] |= 2;
		}
		if (idleDelay && --idleDelay == 0) {
			power[0x118] = (power[0x118] & ~1u) | (power[0x10c] & 1);
			power[0x120] = (power[0x120] & ~1u) | (power[0x10c] & 1);
			if ((power[0x10c] & 1) == 0) {
				deidled = true;
				if (fault == PoweredClock) clock[0x908] |= 16;
			}
		}
		if (offDelay && --offDelay == 0) {
			powered = false; power[0x290] &= ~2u;
		}
	}
	bool MapGpu()
	{
		assert(gpu == NULL && powered && deidled);
		assert((clock[0x578] & 0x40ff) == 0x83 && (clock[0x908] & 0x7fda) == 0);
		assert((power[0x290] & 2) && !(power[0x14c] & 1));
		assert(!(power[0x118] & 1) && !(power[0x120] & 1));
		maps++;
		if (fault == Mapping) return false;
		gpu = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(gpu != MAP_FAILED);
		uint32_t* regs = (uint32_t*)((char*)gpu + page);
		assert(mprotect(regs, page, PROT_READ | PROT_WRITE) == 0);
		regs[0] = fault == WrongID ? 0xffffffff : 0xa8670005;
		regs[0x14/4] = 0x2830; regs[0x18/4] = 0xff; regs[0x1c/4] = 0x040a0412;
		regs[0x100/4] = fault == WrongFeatures ? 0 : 0x50005;
		regs[0x104/4] = 0; regs[0x280/4] = 0;
		assert(mprotect(regs, page, PROT_READ) == 0);
		return true;
	}
	void UnmapGpu()
	{
		assert(gpu != NULL && powered && munmap(gpu, page * 3) == 0);
		gpu = NULL;
	}
	uint32_t ReadGpu(uint32_t offset)
	{
		assert(gpu != NULL && powered && !(power[0x118] & 1));
		assert(offset == 0 || offset == 0x14 || offset == 0x18 || offset == 0x1c
			|| offset == 0x100 || offset == 0x104 || offset == 0x280);
		gpuReads++;
		return ((const volatile uint32_t*)((char*)gpu + page))[offset / 4];
	}
};

int main()
{
	static_assert(sizeof(IdentityInfo) == 256, "Identity ABI changed");
	IdentityInfo info;
	Model model;
	auto clock = model.clock, power = model.power;
	for (int repeat = 0; repeat < 3; repeat++) {
		CycleIdentity(model, info);
		assert(info.result == kIdentityOK && info.restoreResult == kIdentityOK);
		assert(info.flags == (kIdentityRead | kIdentityRestored | kIdentityChangedRegisters));
		assert(model.clock == clock && model.power == power && !model.powered);
		assert(model.gpuReads == 7u * (repeat + 1) && model.maps == (unsigned)repeat + 1);
		assert(info.clockHertz == 175500000 && info.gpuID == 0xa8670005);
		assert(IdentityInitialStateMatches(info.before) && IdentityPoweredStateMatches(info.powered));
	}
	// Each unexpected gate/source/divider/idle/power state rejects before writes.
	for (auto item : std::vector<std::pair<bool, uint32_t>>{{true,0x578},{true,0x57c},{true,0x580},
		{true,0x908},{true,0x90c},{false,0x10c},{false,0x118},{false,0x120},{false,0x14c},{false,0x290}}) {
		Model invalid;
		uint32_t bit = item.second == 0x908 ? 16 : item.second == 0x90c ? 4 : item.second == 0x290 ? 2 : 1;
		(item.first ? invalid.clock : invalid.power)[item.second] ^= bit;
		CycleIdentity(invalid, info);
		assert(info.result == kInitialStateMismatch && invalid.writes == 0 && invalid.maps == 0);
	}
	for (Fault fault : {ClockWrite, InitialIdleWrite, PowerUp, DeIdle, Mapping,
			WrongID, WrongFeatures, RestoreIdle, PowerDown, RestoreClock, PoweredClock,
			FrozenClock, BackwardsClock}) {
		Model failed(fault);
		CycleIdentity(failed, info);
		assert(info.result != kIdentityOK || info.restoreResult != kIdentityOK);
		if (fault == PowerUp || fault == FrozenClock) {
			assert(info.result == kPowerUpFailed && info.restoreResult == kPowerStateUncertain);
			assert((failed.clock[0x578] & 31) == 3 && failed.maps == 0);
		}
		if (fault == ClockWrite || fault == InitialIdleWrite || fault == DeIdle
			|| fault == Mapping || fault == WrongID || fault == WrongFeatures) {
			assert(info.restoreResult == kIdentityOK && failed.clock == clock && failed.power == power);
			assert(info.flags & kIdentityRestored);
		} else {
			assert(info.flags & kIdentityNeedsRecovery);
			assert(!(info.flags & kIdentityRestored));
		}
		if (fault == WrongID) assert(failed.gpuReads == 1);
		if (fault == ClockWrite || fault == InitialIdleWrite || fault == DeIdle
			|| fault == Mapping || fault == PoweredClock || fault == FrozenClock
			|| fault == BackwardsClock) assert(failed.gpuReads == 0);
		assert(failed.gpu == NULL && failed.pauses <= 2002);
	}
	puts("MALI_CSF_POWER_TEST_PASS");
}
