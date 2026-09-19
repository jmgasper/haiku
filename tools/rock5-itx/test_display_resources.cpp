/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Host fixture for the RK3588 display driver. The production device-tree
// traversal, ioctls, read-only observation, EDID transfer and scanout swap run
// against a modeled device manager and guarded register pages; nothing here
// touches hardware.

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

using int32 = int32_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using status_t = int32_t;
static const status_t B_OK = 0, B_BAD_VALUE = -1, B_BAD_ADDRESS = -2,
	B_DEV_INVALID_IOCTL = -3, B_NO_MEMORY = -4, B_NOT_SUPPORTED = -5,
	B_NOT_ALLOWED = -6, B_ENTRY_NOT_FOUND = -8, B_ERROR = -9, B_BUSY = -10, B_NO_INIT = -11,
	B_DEV_NOT_READY = -12;
using int64 = int64_t;
using area_id = int32_t;
using team_id = int32_t;
using addr_t = uintptr_t;
static const unsigned B_PAGE_SIZE = 4096, B_ANY_KERNEL_ADDRESS = 4, B_ANY_ADDRESS = 1,
	B_UNCACHED_MEMORY = 1u << 28, B_KERNEL_READ_AREA = 1u << 4,
	B_KERNEL_WRITE_AREA = 1u << 5, B_READ_AREA = 1, B_WRITE_AREA = 2,
	B_CLONEABLE_AREA = 1u << 8, B_FULL_LOCK = 1;
static const int32 B_CURRENT_TEAM = 0;
static const unsigned B_FILE_NAME_LENGTH = 256, B_PATH_NAME_LENGTH = 1024;
static const uint32 B_GET_ACCELERANT_SIGNATURE = 8300;
struct area_info { int32 area; size_t size; void* address; };
using sem_id = int32_t;
static const int32 B_UNHANDLED_INTERRUPT = 0, B_HANDLED_INTERRUPT = 1, B_INVOKE_SCHEDULER = 2;
static const uint32 B_DO_NOT_RESCHEDULE = 2;
typedef int32 (*interrupt_handler)(void*);
static int32 atomic_add(int32* value, int32 delta) { return __atomic_fetch_add(value, delta, __ATOMIC_SEQ_CST); }
static int32 atomic_get(int32* value) { return __atomic_load_n(value, __ATOMIC_SEQ_CST); }
static void atomic_set(int32* value, int32 newValue) { __atomic_store_n(value, newValue, __ATOMIC_SEQ_CST); }

#include "DisplayEdid.h"
#include "DisplayScanout.h"
#include "DisplayAccelerant.h"
#include "DisplayModeSet.h"
#include "DisplayCursor.h"
#include "DisplayPort.h"

using namespace RK3588Display;

static std::map<int, std::pair<void*, size_t> > sAreas;
static unsigned sMapAttempts, sFailMap;
static std::vector<uint64> sMappedBases;
static int64_t sTime;
static unsigned sLockDepth;
static uint32 sRepairStatus = (1u << 16) | (1u << 17) | (1u << 18); // VOP, VO0, VO1 on
static uint32 sGate52, sGate61;
static uint32 sGate2, sGate17, sGate56; // CLKGATE_CON(2), (17), (56): USBDP immortal, GPIO3, DP1
static uint32 sGpioPort; // GPIO3 external port: bit 29 is the DP1 hot-plug pin
static uint32 sIocMux = 0x50; // GPIO3D_IOMUX_SEL_H: D5 as dp1_hpdin_m0 (function 5)
// The DisplayPort path model: a sink behind the RA620 with its DPCD and EDID,
// the controller's hot-plug and AUX engine, the PHY PMA, and hiword-masked
// GRF, IOC and CRU words. Writes are found by diffing against a shadow.
static bool sAllowDp;
static uint32 sGpllCon1 = 0x42; // p = 2, s = 1: 1188 MHz with m = 198
static uint32* sDpModel;
static std::vector<uint32> sDpShadow;
static uint32* sPmaModel;
static std::vector<uint32> sPmaShadow;
static std::vector<std::pair<unsigned, uint32> > sPmaWrites, sDpWrites, sHiwordWrites;
static std::map<uint64, std::pair<uint32*, std::vector<uint32> > > sHiwordModels; // base -> (registers, shadow)
static uint8_t sDpcd[0x700];
static uint8_t sSinkEdid[128];
static bool sSinkPresent = true; // the RA620 drives hot-plug and answers AUX
static uint32 sAuxDeferReplies; // replies to answer with DEFER first
static bool sAuxNeverReplies, sLcpllNeverLocks;
static bool sRopllNeverLocks, sLinkNeverRecovers;
static uint32 sHighestRecoveringRate; // the sink only recovers the clock at or below this rate code
static std::vector<std::pair<unsigned, uint32> > sDpcdWrites;
static uint32 sRefclkSelect; // PMU CLKSEL_CON14
static unsigned sHotPlugDebounce; // model steps until the controller reports PLUG
static unsigned sAuxCommands;
static std::vector<uint32> sAuxLog;
static const uint32 kModelAuxMarker = 1u << 6; // a bit the driver never writes in AUX_CMD
static const uint32 kModelEventMarker = 1u << 31; // marks a raised event in GENERAL_INTERRUPT
static bool sAllowEdid;
static uint32 sHotPlug = (1u << 24) | (1u << 27);

// VOP2 model: firmware-like words for the registers the scanout swap reads,
// pseudo-random elsewhere. A writable mapping logs every changed word in
// order; only the located window's address and the commit word may change.
static std::map<unsigned, uint32> sVopOverrides;
static uint32* sVopModel;
static std::vector<uint32> sVopShadow;
static std::vector<std::pair<unsigned, uint32> > sVopWrites;
static bool sAllowScanout, sStickyAddress, sCommitNeverCompletes;
// Shadowed window registers: a written address waits in the shadow set until
// the port's frame start, modeled as a countdown of barrier/poll steps after
// the commit word; reads keep returning the active value until then.
static uint32 sVopPendingAddress;
static bool sVopAddressPending;
static int sVopCommitCountdown = -1;
// Mode-set models: PHY, HDMI TX packet words, HDPTX GRF (HIWORD control,
// modeled status) and the CRU reset words. Every changed word is logged.
static bool sAllowModeSet, sHoldNever;
static bool sAllowCursor;
static std::map<unsigned, uint32> sVopPendingWindow; // shadowed cursor window and mixer words
static bool sCursorStickyControl; // REGION0_CTRL of the cursor window never takes a write
static interrupt_handler sHandler;
static void* sHandlerData;
static uint32* sPhyModel;
static std::vector<uint32> sPhyShadow;
static std::vector<std::pair<unsigned, uint32> > sPhyWrites;
static std::vector<uint32> sHdmiShadow;
static std::vector<std::pair<unsigned, uint32> > sHdmiWrites;
static uint32* sGrfModel;
static std::vector<uint32> sGrfShadow;
static std::vector<std::pair<unsigned, uint32> > sGrfWrites;
static uint32 sPhyStatusModel = 0x0e;
static uint32 sGrfControl = 0xe0; // HDPTX GRF CON0: PLL, bias and bandgap enabled by firmware
static uint32* sCruModel;
static std::vector<uint32> sCruShadow;
static std::vector<std::pair<unsigned, uint32> > sCruWrites;
static int sVopHoldCountdown = -1;
static const unsigned kModelWindow = 2;
static const unsigned kModelAddressOffset = 0x1800 + kModelWindow * 0x200 + 0x14;
static const uint32 kModelFirmwareAddress = 0xed280000;
static const uint64 kModelPatternPhysical = 0x40100000;
static const uint64 kModelFramePhysical = 0x14c00000;
static const uint64 kModelCursorPhysical = 0x15400000;
static const unsigned kModelCursorWindowBase = 0x1800 + 3 * 0x200;
static const unsigned kModelCursorMixerBase = 0x650 + 6 * 0x10;

struct mutex {};
#define MUTEX_INITIALIZER(name) {}
class MutexLocker {
public:
	explicit MutexLocker(mutex&) { assert(sLockDepth++ == 0); }
	~MutexLocker() { assert(--sLockDepth == 0); }
};

static int64_t system_time() { return ++sTime; }
static void memory_read_barrier() {}
static void kernel_dprintf(const char*, ...) {}
#define dprintf kernel_dprintf
#define B_PRIu32 "u"
#define B_PRIx32 "x"
#define B_PRId32 "d"
#define B_PRId64 "lld"
#define B_PRIx64 "llx"

// I2C master model for the HDMI TX1 window: reacts synchronously to writes
// (the production write helper issues a barrier after each store) and to polls.
static uint32* sHdmiModel;
static uint8_t sEdid[512];
static int sNackAt = -1;
static bool sUnresponsive;
static unsigned sServed, sResets, sModelSpins;
static bool sServing;

static uint32 ModelRegister(uint64 base, unsigned offset);

static void
VopModelStep()
{
	if (sVopModel == NULL)
		return;
	for (unsigned i = 0; i < sVopShadow.size(); i++) {
		if (sVopModel[i] == sVopShadow[i])
			continue;
		unsigned offset = i * 4;
		uint32 value = sVopModel[i];
		sVopWrites.push_back(std::make_pair(offset, value));
		if (offset == 0x000) {
			// Commit: the port bit stays visible until its next frame start.
			assert((value & 0x8000) != 0 && (value >> 16) == (value & 0xf));
			sVopModel[i] = 0x8000 | (value & 0xf);
			sVopCommitCountdown = sCommitNeverCompletes ? -1 : 3;
		} else if (offset == kModelAddressOffset) {
			sVopPendingAddress = value;
			sVopAddressPending = true;
			sVopModel[i] = sVopShadow[i]; // the active address stays readable
		} else if (offset == 0xc0) {
			// VP2 interrupt enable: high half masks which low bits are written.
			uint32 mask = value >> 16;
			sVopModel[i] = (sVopShadow[i] & ~mask & 0xffff) | (value & mask);
			sVopOverrides[offset] = sVopModel[i];
		} else if (offset == 0xc4) {
			// VP2 interrupt clear: masked status bits drop; the word reads as zero.
			uint32 mask = value >> 16;
			sVopModel[0xc8 / 4] &= ~(value & mask & 0xffff);
			sVopShadow[0xc8 / 4] = sVopModel[0xc8 / 4];
			sVopOverrides[0xc8] = sVopModel[0xc8 / 4];
			sVopModel[i] = 0;
		} else if (sAllowCursor && (offset == 0x6f8 || offset == 0x008)) {
			// The window delays and the automatic clock gating are plain words.
			sVopOverrides[offset] = value;
		} else if (sAllowCursor && ((offset >= kModelCursorWindowBase && offset < kModelCursorWindowBase + 0x200)
				|| (offset >= kModelCursorMixerBase && offset < kModelCursorMixerBase + 0x10))) {
			// The cursor window and its mixer are shadowed like the scanout
			// address: the active word stays readable until the frame start.
			if (!(sCursorStickyControl && offset == kModelCursorWindowBase + 0x10))
				sVopPendingWindow[offset] = value;
			sVopModel[i] = sVopShadow[i];
		} else if (sAllowModeSet && (offset == 0xe00 || offset == 0xe04 || offset == 0xe2c
				|| offset == 0xe30 || offset == 0xe34 || offset == 0xe38 || offset == 0xe3c
				|| offset == 0xe40 || offset == 0xe48 || offset == 0xe4c || offset == 0xe50
				|| offset == 0xe54 || offset == 0x78 || offset == 0x6e8 || offset == 0x1c20
				|| offset == 0x1c24 || offset == 0x1c28)) {
			// Port timing, post-processing and window geometry are plain words;
			// standby reports DSP_HOLD_VALID once the frame ends.
			sVopOverrides[offset] = value;
			if (offset == 0xe00 && (value & 0x80000000u) != 0)
				sVopHoldCountdown = sHoldNever ? -1 : 3;
		} else if (sAllowDp && ((offset >= 0xd00 && offset <= 0xd54) || offset == 0x74 || offset == 0x6e4
				|| offset == 0x028 || offset == 0x030 || (offset >= 0x1800 && offset < 0x1900)
				|| (offset >= 0x650 && offset < 0x680) || offset == 0x6f8 || offset == 0x008
				|| (offset >= 0x1a00 && offset < 0x1b00)
				|| offset == 0xb0 || offset == 0xb4 || offset == 0x1c1c || offset == 0x1c20 || offset == 0x1c24)) {
			// Video port 1 for the DP probe: timing, background and the interface mux are plain words.
			sVopOverrides[offset] = value;
		} else
			assert(!"unexpected VOP2 register write");
		sVopShadow[i] = sVopModel[i];
	}
	if (sVopHoldCountdown > 0 && --sVopHoldCountdown == 0) {
		uint32 raised = 0x40 & sVopModel[0xc0 / 4];
		sVopModel[0xc8 / 4] |= raised;
		sVopShadow[0xc8 / 4] = sVopModel[0xc8 / 4];
		sVopOverrides[0xc8] = sVopModel[0xc8 / 4];
	}
	if (sVopCommitCountdown > 0 && --sVopCommitCountdown == 0) {
		// Frame start: the shadow set becomes active and the port bit clears.
		sVopModel[0] = ModelRegister(0xfdd90000, 0);
		sVopShadow[0] = sVopModel[0];
		if (sVopAddressPending && !sStickyAddress) {
			sVopOverrides[kModelAddressOffset] = sVopPendingAddress;
			sVopModel[kModelAddressOffset / 4] = sVopPendingAddress;
			sVopShadow[kModelAddressOffset / 4] = sVopPendingAddress;
		}
		sVopAddressPending = false;
		for (auto& pending : sVopPendingWindow) {
			sVopOverrides[pending.first] = pending.second;
			sVopModel[pending.first / 4] = pending.second;
			sVopShadow[pending.first / 4] = pending.second;
		}
		sVopPendingWindow.clear();
	}
}


static bool
PhyOffsetAccessible(unsigned offset)
{
	return offset <= 0x029c || (offset >= 0x0400 && offset <= 0x04a4)
		|| (offset >= 0x0800 && offset <= 0x08a4) || (offset >= 0x0c00 && offset <= 0x0cb4)
		|| (offset >= 0x1000 && offset <= 0x10b4) || (offset >= 0x1400 && offset <= 0x14b4)
		|| (offset >= 0x1800 && offset <= 0x18b4);
}


static void
ModeSetModelStep()
{
	if (sPhyModel != NULL) {
		for (unsigned i = 0; i < sPhyShadow.size(); i++) {
			if (sPhyModel[i] == sPhyShadow[i])
				continue;
			assert(PhyOffsetAccessible(i * 4) && sPhyModel[i] <= 0xff);
			sPhyWrites.push_back(std::make_pair(i * 4, sPhyModel[i]));
			sPhyShadow[i] = sPhyModel[i];
		}
	}
	if (sHdmiModel != NULL && !sHdmiShadow.empty()) {
		static const unsigned kPacketOffsets[] = {0x8e0, 0x968, 0xa9c, 0xaa8, 0xaac,
			0xbe0, 0xbe4, 0xbe8, 0xbec, 0xbf0};
		for (unsigned offset : kPacketOffsets) {
			if (sHdmiModel[offset / 4] == sHdmiShadow[offset / 4])
				continue;
			assert(sAllowModeSet);
			sHdmiWrites.push_back(std::make_pair(offset, sHdmiModel[offset / 4]));
			sHdmiShadow[offset / 4] = sHdmiModel[offset / 4];
		}
	}
	if (sGrfModel != NULL) {
		for (unsigned i = 0; i < sGrfShadow.size(); i++) {
			if (sGrfModel[i] == sGrfShadow[i])
				continue;
			assert(i == 0); // only HDPTX_CON0 is written, HIWORD-masked
			uint32 value = sGrfModel[i];
			sGrfWrites.push_back(std::make_pair(i * 4, value));
			uint32 mask = value >> 16;
			sGrfModel[i] = (sGrfShadow[i] & ~mask & 0xffff) | (value & mask);
			sGrfShadow[i] = sGrfModel[i];
			sGrfControl = sGrfModel[0];
		}
		sGrfModel[0x80 / 4] = sGrfShadow[0x80 / 4] = sPhyStatusModel;
	}
	if (sCruModel != NULL) {
		static const unsigned kResetOffsets[] = {0xb20, 0x30a0c, 0x30a10, 0xa08, 0xa0c, 0x4d4, 0x4bc, 0x4c0,
			0x8d0, 0x8d4};
		for (unsigned i = 0; i < sCruShadow.size(); i++) {
			if (sCruModel[i] == sCruShadow[i])
				continue;
			bool allowed = false;
			for (unsigned offset : kResetOffsets)
				allowed |= offset == i * 4;
			assert(allowed);
			uint32 value = sCruModel[i];
			sCruWrites.push_back(std::make_pair(i * 4, value));
			uint32 mask = value >> 16;
			sCruModel[i] = (sCruShadow[i] & ~mask & 0xffff) | (value & mask);
			sCruShadow[i] = sCruModel[i];
		}
	}
}


// The sink's link status: clock recovery needs swing 1 under pattern 1 (and
// a rate the sink recovers at), equalization pre-emphasis 1 under TPS2/3/4.
static void
SinkLinkStatus()
{
	uint8_t pattern = sDpcd[0x102] & 7;
	unsigned lanes = sDpcd[0x101] & 0x1f;
	bool rateOk = !sLinkNeverRecovers && sDpcd[0x100] <= sHighestRecoveringRate;
	uint8_t laneStatus[2] = {0, 0};
	uint8_t adjust = 0;
	bool aligned = true;
	for (unsigned lane = 0; lane < 2; lane++) {
		uint8_t set = sDpcd[0x103 + lane];
		uint8_t swing = set & 3, pre = (set >> 3) & 3;
		uint8_t bits = 0;
		if (lane < lanes && pattern != 0 && rateOk && swing >= 1) {
			bits |= 1;
			if (pattern >= 2 && pre >= 1)
				bits |= 6;
		}
		if ((bits & 6) != 6)
			aligned = false;
		laneStatus[lane] = bits;
		uint8_t want = (uint8_t)(1 | ((pattern >= 2 ? 1 : 0) << 2)); // swing 1, pre 1 once equalizing
		adjust |= want << (4 * lane);
	}
	sDpcd[0x202] = (uint8_t)(laneStatus[0] | (laneStatus[1] << 4));
	sDpcd[0x203] = 0;
	sDpcd[0x204] = aligned && pattern >= 2 ? 1 : 0;
	sDpcd[0x205] = 0;
	sDpcd[0x206] = adjust;
	sDpcd[0x207] = 0;
}


static void
DpModelStep()
{
	// Hiword-masked words: IOC, USBDP GRF, VO0 GRF.
	for (auto& model : sHiwordModels) {
		uint32* registers = model.second.first;
		std::vector<uint32>& shadow = model.second.second;
		for (unsigned i = 0; i < shadow.size(); i++) {
			if (registers[i] == shadow[i])
				continue;
			uint32 value = registers[i];
			sHiwordWrites.push_back(std::make_pair((unsigned)(model.first & 0xffffff) + i * 4, value));
			uint32 mask = value >> 16;
			registers[i] = (shadow[i] & ~mask & 0xffff) | (value & mask);
			shadow[i] = registers[i];
			if (model.first == 0xfd5f8000 && i * 4 == 0x7c)
				sIocMux = registers[i];
		}
	}
	if (sPmaModel != NULL) {
		for (unsigned i = 0; i < sPmaShadow.size(); i++) {
			if (sPmaModel[i] == sPmaShadow[i])
				continue;
			assert(i * 4 != 0x350 && i * 4 != 0x354);
			sPmaWrites.push_back(std::make_pair(i * 4, sPmaModel[i]));
			sPmaShadow[i] = sPmaModel[i];
		}
		uint32 lock = sLcpllNeverLocks ? 0 : 0xc0;
		sPmaModel[0x350 / 4] = sPmaShadow[0x350 / 4] = lock;
		// ROPLL locks while the DP common reset is released.
		uint32 ropll = !sRopllNeverLocks && (sPmaModel[0x38c / 4] & 4) != 0 ? 3 : 0;
		sPmaModel[0x354 / 4] = sPmaShadow[0x354 / 4] = ropll;
	}
	if (sDpModel == NULL)
		return;
	uint32* dp = sDpModel;
	for (unsigned i = 0; i < sDpShadow.size(); i++) {
		if (dp[i] == sDpShadow[i])
			continue;
		unsigned offset = i * 4;
		uint32 value = dp[i];
		sDpWrites.push_back(std::make_pair(offset, value));
		if (offset == 0xd00) {
			// Write one to clear.
			uint32 before = sDpShadow[i] & ~kModelEventMarker;
			dp[i] = before & ~(value & ~kModelEventMarker);
		} else if (offset == 0xb00) {
			sAuxCommands++;
			sAuxLog.push_back(value);
			uint32 type = value >> 28, address = (value >> 8) & 0xfffff;
			bool addressOnly = (value & (1u << 4)) != 0;
			uint32 size = addressOnly ? 0 : (value & 0xf) + 1;
			bool pinned = ((sIocMux >> 4) & 0xf) == 5;
			if (sAuxNeverReplies || !sSinkPresent || !pinned) {
				dp[0xb04 / 4] = 1u << 17; // the controller's own timeout, no reply event
				if (!sAuxNeverReplies)
					dp[0xd00 / 4] |= 2 | kModelEventMarker;
			} else if (sAuxDeferReplies > 0) {
				sAuxDeferReplies--;
				dp[0xb04 / 4] = 2u << 4; // DEFER
				dp[0xd00 / 4] |= 2 | kModelEventMarker;
			} else {
				uint8_t data[16] = {};
				static uint32 sEdidOffset;
				if (type == 0x9) {
					assert(address + size <= sizeof(sDpcd));
					if (address == 0x202)
						SinkLinkStatus();
					memcpy(data, sDpcd + address, size);
				} else if (type == 0x8) {
					assert(address + size <= sizeof(sDpcd) && size > 0);
					for (unsigned b = 0; b < size; b++) {
						uint8_t byte = (uint8_t)(dp[(0xb08 + (b / 4) * 4) / 4] >> (8 * (b % 4)));
						sDpcd[address + b] = byte;
						sDpcdWrites.push_back(std::make_pair(address + b, byte));
					}
				} else if ((type & ~0x4u) == 0x0) {
					assert(address == 0x50 && (size == 1 || size == 0));
					if (size == 1)
						sEdidOffset = dp[0xb08 / 4] & 0xff;
				} else if ((type & ~0x4u) == 0x1) {
					assert(address == 0x50);
					if (size > 0) {
						assert(sEdidOffset + size <= 128);
						memcpy(data, sSinkEdid + sEdidOffset, size);
						sEdidOffset += size;
					}
				} else
					assert(!"unexpected AUX request");
				for (unsigned w = 0; w < 4; w++) {
					uint32 word = 0;
					for (unsigned b = 0; b < 4; b++)
						word |= (uint32)data[w * 4 + b] << (b * 8);
					if ((type & 1) != 0)
						dp[(0xb08 + w * 4) / 4] = word;
				}
				dp[0xb04 / 4] = (type & 1) != 0 && size > 0 ? (size + 1) << 19 : 0; // ACK
				dp[0xd00 / 4] |= 2 | kModelEventMarker;
			}
			// The next identical command must still show as a write.
			dp[i] = value ^ kModelAuxMarker;
			sDpShadow[0xb04 / 4] = dp[0xb04 / 4];
			sDpShadow[0xd00 / 4] = dp[0xd00 / 4];
			for (unsigned w = 0; w < 4; w++)
				sDpShadow[(0xb08 + w * 4) / 4] = dp[(0xb08 + w * 4) / 4];
		} else {
			assert(offset == 0x200 || offset == 0xa00 || offset == 0xd04 || offset == 0xd0c
				|| (offset >= 0xb08 && offset <= 0xb14) || (offset >= 0x300 && offset <= 0x330));
		}
		sDpShadow[i] = dp[i];
	}
	sDpShadow[0xd00 / 4] = dp[0xd00 / 4];
	// Hot-plug: the pin, muxed to the controller, raises PLUG after a debounce.
	bool pinned = ((sIocMux >> 4) & 0xf) == 5;
	if (pinned && sSinkPresent) {
		if (sHotPlugDebounce > 0)
			sHotPlugDebounce--;
		else
			dp[0xd08 / 4] = (7u << 9) | (1u << 8) | (1u << 1);
	}
	sDpShadow[0xd08 / 4] = dp[0xd08 / 4];
}


static void
ModelStep()
{
	VopModelStep();
	ModeSetModelStep();
	DpModelStep();
	// The VOP interrupt line: an installed handler runs while enabled status is pending.
	if (sHandler != NULL && sVopModel != NULL && (sVopModel[0xc8 / 4] & 0xffff) != 0)
		sHandler(sHandlerData);
	if (sHdmiModel == NULL)
		return;
	uint32* regs = sHdmiModel;
	if (regs[0x3028 / 4] != 0) {
		regs[0x3020 / 4] &= ~regs[0x3028 / 4];
		regs[0x3028 / 4] = 0;
	}
	if ((regs[0xec / 4] & 1) != 0) {
		regs[0xec / 4] = 0;
		regs[0xf4 / 4] &= ~0x1eu;
		sServing = false;
		sResets++;
	}
	uint32 control = regs[0xf4 / 4];
	if ((control & 0x1e) == 0) {
		sServing = false;
		return;
	}
	if (sServing || sUnresponsive)
		return;
	sServing = true;
	assert((control & 0x1e) == 0x04 || (control & 0x1e) == 0x10);
	assert(((control >> 5) & 0x7f) == 0x50);
	unsigned address = (control >> 12) & 0xff;
	unsigned segment = 0;
	if ((control & 0x10) != 0) {
		assert((regs[0xf8 / 4] & 0x7f) == 0x30);
		segment = (regs[0xf8 / 4] >> 7) & 0x7f;
	}
	assert((regs[0x3024 / 4] & 0x5) == 0x5); // done/error status unmasked during transfers
	if (sNackAt >= 0 && (int)sServed == sNackAt) {
		regs[0x3020 / 4] |= 0x4;
	} else {
		regs[0x10c / 4] = 0xa5000000u | sEdid[segment * 256 + address];
		regs[0x3020 / 4] |= 0x1;
	}
	sServed++;
}

static void memory_write_barrier() { ModelStep(); }
static void spin(unsigned micros) { assert(micros >= 15 && micros <= 1000); sModelSpins++; ModelStep(); }


static uint32
ModelRegister(uint64 base, unsigned offset)
{
	if (base == 0xfd8d8000 && offset == 0x290)
		return sRepairStatus;
	if (base == 0xfd7c0000 && offset == 0x8d0)
		return sGate52;
	if (base == 0xfd7c0000 && offset == 0x8f4)
		return sGate61;
	if (base == 0xfd7c0000 && offset == 0x808)
		return sGate2;
	if (base == 0xfd7c0000 && offset == 0x844)
		return sGate17;
	if (base == 0xfd7c0000 && offset == 0x8e0)
		return sGate56;
	if (base == 0xfec40000 && offset == 0x70)
		return sGpioPort;
	if (base == 0xfec40000 && offset == 0x78)
		return 0x0101157c; // GPIO version id
	if (base == 0xfd5f8000 && offset == 0x7c)
		return sIocMux;
	if (base == 0xfde60000 && offset == 0x000)
		return 0x14110600; // DW DP version 1.41
	if (base == 0xfde60000 && offset == 0xd08)
		return 0x0; // hot-plug status: nothing plugged
	if (base == 0xfde60000 && (offset == 0x200 || offset == 0x204 || offset == 0xb00 || offset == 0xb04
			|| (offset >= 0xb08 && offset <= 0xb14) || offset == 0xd00 || offset == 0xd04 || offset == 0xd0c))
		return offset == 0x200 ? 0x4 : 0;
	if (base == 0xfd7c0000 && offset == 0x30338)
		return sRefclkSelect;
	if (base == 0xfd7c0000 && offset == 0x1c0)
		return 0xc6; // GPLL m = 198
	if (base == 0xfd7c0000 && offset == 0x1c4)
		return sGpllCon1;
	if (base == 0xfd7c0000 && offset == 0x1c8)
		return 0; // integer mode
	if (base == 0xfd7c0000 && offset == 0x4bc)
		return 0x201; // dclk_vop1_src: GPLL / 2 as the firmware leaves it
	if (base == 0xfde60000 && offset >= 0x300 && offset <= 0x330)
		return 0; // video stream off
	if (base == 0xfd7c0000 && offset == 0x4d4)
		return 0; // clk_aux16m_0/1 dividers as the firmware leaves them (unknown; zero here)
	if (base == 0xfd7c0000 && (offset == 0xa08 || offset == 0xa0c || offset == 0xae0 || offset == 0xb20))
		return 0; // every reset of the path deasserted by the firmware
	if (base == 0xfed98000 && offset == 0x350)
		return 0xc0; // LCPLL locked (EDK2 runs USB3 on the PHY)
	if (base == 0xfed98000 && (offset == 0x288 || offset == 0x38c))
		return 0;
	if (base == 0xfd5cc000 && offset == 0x4)
		return 0x6000;
	if (base == 0xfd5a6000 && offset == 0x8)
		return 0xe4;
	if (base == 0xfd58c000 && offset == 0x384)
		return sHotPlug;
	if (base == 0xfd5e4000 && offset == 0x00)
		return sGrfControl;
	if (base == 0xfd5e4000 && offset == 0x80)
		return sPhyStatusModel;
	if (base == 0xfdd90000) {
		auto found = sVopOverrides.find(offset);
		if (found != sVopOverrides.end())
			return found->second;
	}
	return (uint32)(base >> 4) ^ (offset * 0x01010101u);
}


static int
map_physical_memory(const char*, uint64 base, size_t bytes, uint32 spec,
	uint32 protection, void** address)
{
	assert(sLockDepth == 1);
	static const uint64 kControl[] = {0xfd8d8000, 0xfd7c0000, 0xfd58c000,
		0xfd5a4000, 0xfd5a8000, 0xfd5e4000, 0xfd5cc000, 0xfd5a6000, 0xfd5f8000};
	static const uint64 kDpWritable[] = {0xfd5cc000, 0xfd5a6000, 0xfd5f8000};
	bool control = false;
	for (uint64 candidate : kControl)
		control |= candidate == base;
	bool writable = false;
	if (control) {
		// The mode set maps the HDPTX GRF page and the whole CRU writable.
		writable = (protection & B_KERNEL_WRITE_AREA) != 0;
		bool dpWritable = false;
		for (uint64 candidate : kDpWritable)
			dpWritable |= candidate == base;
		if (base == 0xfd7c0000 && bytes == 0x5c000)
			assert(writable && (sAllowModeSet || sAllowDp));
		else
			assert(bytes == B_PAGE_SIZE);
		assert(!writable || (sAllowModeSet && (base == 0xfd5e4000 || base == 0xfd7c0000))
			|| (sAllowDp && (dpWritable || base == 0xfd7c0000)));
	} else if (base == 0xfed70000) {
		assert(sAllowModeSet && bytes == kPhyMapSize && (protection & B_KERNEL_WRITE_AREA) != 0);
		writable = true;
	} else if (base == 0xfdd90000) {
		assert(bytes == kVopMapSize && (sRepairStatus & (1u << 16)) != 0 && (sGate52 & 0x300) == 0);
		// Only the opt-in scanout swap or restore maps VOP2 writable; queries do not.
		writable = (protection & B_KERNEL_WRITE_AREA) != 0;
		assert(!writable || sAllowScanout || sAllowDp);
	} else if (base == 0xfdea0000) {
		assert((sRepairStatus & (1u << 18)) != 0 && (sGate61 & 4) == 0);
		// Only the opt-in EDID path maps HDMI TX1 writable, after its own gating.
		writable = (protection & B_KERNEL_WRITE_AREA) != 0;
		assert(bytes == (writable ? kHdmiEdidMapSize : kHdmiMapSize));
		assert(!writable || sAllowEdid || sAllowModeSet);
	} else if (base == 0xfec40000) {
		// GPIO3 only while its APB clock is ungated, read-only.
		assert(bytes == B_PAGE_SIZE && (sGate17 & 4) == 0);
	} else if (base == 0xfde60000) {
		// DisplayPort TX1 only with VO0 on and its APB clock ungated; writable only for the probe.
		assert(bytes == kDpMapSize && (sRepairStatus & (1u << 17)) != 0 && (sGate56 & 0x20) == 0);
		writable = (protection & B_KERNEL_WRITE_AREA) != 0;
		assert(!writable || sAllowDp);
	} else if (base == 0xfed98000) {
		// The USBDP PHY1 PMA window, for the probe only.
		assert(sAllowDp && bytes == 0x3000 && (protection & B_KERNEL_WRITE_AREA) != 0);
		writable = true;
	} else
		assert(false);
	assert(spec == (B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY));
	assert(protection == (B_KERNEL_READ_AREA | (writable ? B_KERNEL_WRITE_AREA : 0)));
	sMappedBases.push_back(base);
	if (++sMapAttempts == sFailMap)
		return B_NO_MEMORY;
	void* allocation = mmap(NULL, bytes + 2 * B_PAGE_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(allocation != MAP_FAILED);
	uint32* registers = (uint32*)((char*)allocation + B_PAGE_SIZE);
	assert(mprotect(registers, bytes, PROT_READ | PROT_WRITE) == 0);
	for (unsigned offset = 0; offset < bytes; offset += 4)
		registers[offset / 4] = ModelRegister(base, offset);
	if (writable && base == 0xfdd90000) {
		assert(sVopModel == NULL);
		sVopModel = registers;
		sVopShadow.assign(registers, registers + bytes / 4);
		// The driver always writes the address before committing, so a
		// commit without an observed write applies the active value again.
		sVopAddressPending = false;
		sVopCommitCountdown = -1;
	} else if (writable && base == 0xfed70000) {
		assert(sPhyModel == NULL);
		// A byte no sequence writes, so every programmed value shows as a change.
		for (unsigned offset = 0; offset < bytes; offset += 4)
			registers[offset / 4] = 0xa5;
		sPhyModel = registers;
		sPhyShadow.assign(registers, registers + bytes / 4);
	} else if (writable && base == 0xfd5e4000) {
		assert(sGrfModel == NULL);
		sGrfModel = registers;
		sGrfShadow.assign(registers, registers + bytes / 4);
	} else if (writable && base == 0xfd7c0000) {
		assert(sCruModel == NULL);
		sCruModel = registers;
		sCruShadow.assign(registers, registers + bytes / 4);
	} else if (writable && base == 0xfde60000) {
		assert(sDpModel == NULL);
		sDpModel = registers;
		sDpShadow.assign(registers, registers + bytes / 4);
	} else if (writable && base == 0xfed98000) {
		assert(sPmaModel == NULL);
		sPmaModel = registers;
		sPmaShadow.assign(registers, registers + bytes / 4);
	} else if (writable && (base == 0xfd5cc000 || base == 0xfd5a6000 || base == 0xfd5f8000)) {
		assert(sHiwordModels.count(base) == 0);
		sHiwordModels[base] = std::make_pair(registers, std::vector<uint32>(registers, registers + bytes / 4));
	} else if (writable) {
		registers[0xf4 / 4] = 0x00000a00; // idle master, slave 0x50 set by firmware
		registers[0x3020 / 4] = 0;
		registers[0x3024 / 4] = 0;
		registers[0x3028 / 4] = 0;
		registers[0xec / 4] = 0;
		registers[0x8e0 / 4] = 0x1; // HDCP2 bypassed, TMDS HDMI, AVI and GCP scheduled
		registers[0x968 / 4] = 0x0;
		registers[0xa9c / 4] = 0x6f00;
		registers[0xaa8 / 4] = 0x2008;
		sHdmiModel = registers;
		sHdmiShadow.assign(registers, registers + bytes / 4);
		sServing = false;
	} else {
		// Any production write faults immediately, as would either guard page.
		assert(mprotect(registers, bytes, PROT_READ) == 0);
	}
	int area = 17 + sMapAttempts;
	sAreas[area] = {allocation, bytes + 2 * B_PAGE_SIZE};
	*address = registers;
	return area;
}


class AreaDeleter {
public:
	explicit AreaDeleter(int area = -1) : fArea(area) {}
	~AreaDeleter() { SetTo(-1); }
	void SetTo(int area)
	{
		if (fArea >= 0) {
			assert(sAreas.count(fArea) == 1);
			if (sHdmiModel != NULL && (char*)sHdmiModel == (char*)sAreas.at(fArea).first + B_PAGE_SIZE)
				sHdmiModel = NULL;
			if (sVopModel != NULL && (char*)sVopModel == (char*)sAreas.at(fArea).first + B_PAGE_SIZE) {
				VopModelStep();
				sVopModel = NULL;
			}
			char* page = (char*)sAreas.at(fArea).first + B_PAGE_SIZE;
			ModeSetModelStep();
			if ((char*)sPhyModel == page) sPhyModel = NULL;
			if ((char*)sGrfModel == page) sGrfModel = NULL;
			if ((char*)sCruModel == page) sCruModel = NULL;
			if ((char*)sHdmiModel == page) sHdmiShadow.clear();
			DpModelStep();
			if ((char*)sDpModel == page) sDpModel = NULL;
			if ((char*)sPmaModel == page) sPmaModel = NULL;
			for (auto it = sHiwordModels.begin(); it != sHiwordModels.end(); ) {
				if ((char*)it->second.first == page)
					it = sHiwordModels.erase(it);
				else
					++it;
			}
			assert(munmap(sAreas.at(fArea).first, sAreas.at(fArea).second) == 0);
			sAreas.erase(fArea);
		}
		fArea = area;
	}
	int Get() const { return fArea; }
private:
	int fArea;
};


struct module_info { const char* name; };
struct driver_module_info { module_info info; };
struct device_node;
struct fdt_device { device_node* node; };
struct fdt_bus {};
struct fdt_device_module_info {
	driver_module_info info;
	device_node* (*get_bus)(fdt_device*);
	const char* (*get_name)(fdt_device*);
	const void* (*get_prop)(fdt_device*, const char*, int*);
	bool (*get_reg)(fdt_device*, uint32, uint64*, uint64*);
	bool (*get_interrupt)(fdt_device*, uint32, device_node**, uint64*);
};
struct fdt_bus_module_info {
	driver_module_info info;
	device_node* (*node_by_phandle)(fdt_bus*, int);
};
static const int B_STRING_TYPE = 1;
struct device_attr {
	const char* name;
	int type;
	union { const char* string; } value;
};
struct device_manager_info {
	status_t (*get_driver)(device_node*, driver_module_info**, void**);
	device_node* (*get_parent_node)(device_node*);
	void (*put_node)(device_node*);
	status_t (*find_child_node)(device_node*, const device_attr*, device_node**);
};
struct device_node {
	std::string name;
	device_node* parent = NULL;
	std::map<std::string, std::vector<uint8_t> > properties;
	fdt_device device{this};
	uint64 base = 0, size = 0, base1 = 0, size1 = 0;
	std::vector<uint64> irqs;
	device_node* irqController = NULL;
	bool wrongModule = false;
	int held = 0;
};

static device_node sBusNode, sRoot, sVop, sPorts, sPort1, sEndpoint8, sHdmi,
	sHdmiPorts, sHdmiPort0, sHdmiEndpoint, sPhy0, sPhy1, sHdptxGrf, sSysGrf,
	sVopGrf, sVo1Grf, sPmu, sPower, sClock, sGic, sOther, sUsbdpPhy1, sUsbdpGrf, sVo0Grf,
	sIoc, sPinctrl, sGpio3;
static fdt_bus sBus;
static std::map<int, device_node*> sPhandles;
static std::vector<device_node*> sAllNodes = {&sBusNode, &sRoot, &sVop, &sPorts,
	&sPort1, &sEndpoint8, &sHdmi, &sHdmiPorts, &sHdmiPort0, &sHdmiEndpoint, &sPhy0,
	&sPhy1, &sHdptxGrf, &sSysGrf, &sVopGrf, &sVo1Grf, &sPmu, &sPower, &sClock, &sGic,
	&sOther, &sUsbdpPhy1, &sUsbdpGrf, &sVo0Grf, &sIoc, &sPinctrl, &sGpio3};


static const void*
GetProperty(fdt_device* dev, const char* property, int* length)
{
	auto it = dev->node->properties.find(property);
	if (it == dev->node->properties.end()) {
		if (length != NULL) *length = -1;
		return NULL;
	}
	if (length != NULL) *length = it->second.size();
	return it->second.empty() ? (const void*)"" : it->second.data();
}


static bool
GetReg(fdt_device* dev, uint32 index, uint64* base, uint64* size)
{
	assert(index <= 1);
	*base = index == 0 ? dev->node->base : dev->node->base1;
	*size = index == 0 ? dev->node->size : dev->node->size1;
	return *size != 0;
}


static bool
GetInterrupt(fdt_device* dev, uint32 index, device_node** node, uint64* irq)
{
	assert(dev->node == &sVop || dev->node == &sHdmi);
	assert(index < dev->node->irqs.size());
	*node = dev->node->irqController;
	*irq = dev->node->irqs[index];
	return true;
}


static fdt_device_module_info sFdt = {
	{{"bus_managers/fdt/driver_v1"}},
	[](fdt_device*) { return &sBusNode; },
	[](fdt_device* dev) { return dev->node->name.c_str(); },
	GetProperty, GetReg, GetInterrupt
};
static fdt_bus_module_info sFdtBus = {
	{{"bus_managers/fdt/root/driver_v1"}},
	[](fdt_bus*, int phandle) -> device_node* {
		auto it = sPhandles.find(phandle);
		return it == sPhandles.end() ? NULL : it->second;
	}
};
static driver_module_info sWrongModule = {{"unrelated/driver_v1"}};

static device_manager_info sManager = {
	[](device_node* node, driver_module_info** module, void** cookie) -> status_t {
		if (node->wrongModule) {
			*module = &sWrongModule;
			*cookie = NULL;
		} else if (node == &sBusNode) {
			*module = &sFdtBus.info;
			*cookie = &sBus;
		} else {
			*module = &sFdt.info;
			*cookie = &node->device;
		}
		return B_OK;
	},
	[](device_node* node) -> device_node* {
		if (node->parent != NULL) node->parent->held++;
		return node->parent;
	},
	[](device_node* node) { assert(node->held > 0); node->held--; },
	[](device_node* parent, const device_attr* attributes, device_node** output) -> status_t {
		assert(*output == NULL && strcmp(attributes[0].name, "fdt/name") == 0);
		assert(attributes[0].type == B_STRING_TYPE && attributes[1].name == NULL);
		if (parent == NULL)
			return B_ENTRY_NOT_FOUND;
		// The real lookup searches all descendants; the caller must verify
		// the immediate parent of the returned node.
		for (device_node* node : sAllNodes) {
			if (node->name != attributes[0].value.string) continue;
			for (device_node* ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent) {
				if (ancestor != parent) continue;
				node->held++; *output = node; return B_OK;
			}
		}
		return B_ENTRY_NOT_FOUND;
	}
};


static status_t
user_memcpy(void* output, const void* input, size_t bytes)
{
	if (output == NULL) return B_BAD_ADDRESS;
	memcpy(output, input, bytes);
	return B_OK;
}


// Pattern buffer services: contiguous allocation below 4 GiB, its physical
// address, the cache eviction/retyping step and the framebuffer boot item.
struct virtual_address_restrictions { void* address; uint32 address_specification; size_t alignment; };
struct physical_address_restrictions { uint64 low_address, high_address, alignment, boundary; };
struct physical_entry { uint64 address; uint64 size; };
static const uint32 B_CONTIGUOUS = 3;
static const team_id B_SYSTEM_TEAM = 1;
static void* sPatternAllocation;
static void* sFrameAllocation;
static void* sCursorAllocation;
static size_t sFrameModelBytes = 0;
static int sPatternArea = -1, sFrameArea = -1, sCursorArea = -1, sSharedModelArea = -1;
static unsigned sCursorNoncacheable;
static void* sSharedPage;
static unsigned sPatternAllocations, sFailPattern, sNoncacheableCalls, sFrameNoncacheable;
static unsigned sClones, sNullClones, sConsoleUpdates;
static bool sPatternHighPhysical;
struct ConsoleState { addr_t address; int32 width, height, depth, bytesPerRow; };
static ConsoleState sConsole;

static area_id
create_area_etc(team_id team, const char* name, size_t size, uint32 lock, uint32 protection,
	uint32 flags, size_t guardSize, const virtual_address_restrictions* virtualRestrictions,
	const physical_address_restrictions* physicalRestrictions, void** address)
{
	bool pattern = strcmp(name, "RK3588 display pattern") == 0;
	bool cursor = strcmp(name, "RK3588 display cursor") == 0;
	assert(sLockDepth == 1 && team == B_SYSTEM_TEAM);
	assert(pattern || cursor || strcmp(name, "RK3588 display frame buffer") == 0);
	assert((size == (pattern ? kPatternBytes : cursor ? kCursorBufferBytes : kFrameBytes)
		|| (!pattern && !cursor && size == 2 * kFrameBytes)) && lock == B_CONTIGUOUS);
	assert(flags == 0 && guardSize == 0);
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA));
	assert(virtualRestrictions->address == NULL && virtualRestrictions->address_specification == 0);
	assert(physicalRestrictions->low_address == 0 && physicalRestrictions->high_address == 0x100000000ull);
	assert(physicalRestrictions->alignment == B_PAGE_SIZE && physicalRestrictions->boundary == 0);
	assert((pattern ? sPatternArea : cursor ? sCursorArea : sFrameArea) < 0);
	if (++sPatternAllocations == sFailPattern)
		return B_NO_MEMORY;
	void* allocation = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(allocation != MAP_FAILED);
	// A fresh mapping is zero; the driver must clear the frame buffer itself.
	memset(allocation, 0x5a, 64);
	int area = (pattern ? 900 : cursor ? 1200 : 1000) + (int)sPatternAllocations;
	(pattern ? sPatternAllocation : cursor ? sCursorAllocation : sFrameAllocation) = allocation;
	(pattern ? sPatternArea : cursor ? sCursorArea : sFrameArea) = area;
	if (!pattern && !cursor)
		sFrameModelBytes = size;
	*address = allocation;
	return area;
}


static status_t
get_memory_map(const void* address, size_t bytes, physical_entry* table, int32 count)
{
	assert(count == 1);
	if (address == sPatternAllocation && sPatternAllocation != NULL) {
		assert(bytes == kPatternBytes);
		table->address = sPatternHighPhysical ? 0xffc00000ull : kModelPatternPhysical;
	} else if (address == sCursorAllocation && sCursorAllocation != NULL) {
		assert(bytes == kCursorBufferBytes);
		table->address = kModelCursorPhysical;
	} else {
		assert(address == sFrameAllocation && sFrameAllocation != NULL
			&& (bytes == kFrameBytes || bytes == 2 * kFrameBytes));
		table->address = kModelFramePhysical;
	}
	table->size = bytes;
	return B_OK;
}


static status_t
delete_area(area_id area)
{
	if (sAreas.count(area) == 1) {
		// A persistent register mapping released directly by the driver.
		if (sVopModel != NULL && (char*)sVopModel == (char*)sAreas.at(area).first + B_PAGE_SIZE) {
			VopModelStep();
			sVopModel = NULL;
		}
		assert(munmap(sAreas.at(area).first, sAreas.at(area).second) == 0);
		sAreas.erase(area);
		return B_OK;
	}
	if (area == sPatternArea) {
		assert(munmap(sPatternAllocation, kPatternBytes) == 0);
		sPatternAllocation = NULL;
		sPatternArea = -1;
	} else if (area == sFrameArea) {
		assert(munmap(sFrameAllocation, sFrameModelBytes) == 0);
		sFrameAllocation = NULL;
		sFrameArea = -1;
	} else if (area == sCursorArea) {
		assert(munmap(sCursorAllocation, kCursorBufferBytes) == 0);
		sCursorAllocation = NULL;
		sCursorArea = -1;
	} else {
		assert(area == sSharedModelArea && sSharedPage != NULL);
		assert(munmap(sSharedPage, B_PAGE_SIZE) == 0);
		sSharedPage = NULL;
		sSharedModelArea = -1;
	}
	return B_OK;
}


static status_t
MakeBufferNoncacheable(area_id area, void* address, size_t bytes)
{
	const uint32* pixels = (const uint32*)address;
	if (area == sFrameArea) {
		// The frame buffer starts black; nothing of the allocation leaks through.
		assert(address == sFrameAllocation && bytes == sFrameModelBytes);
		for (unsigned i = 0; i < sFrameModelBytes / 4; i += 4093)
			assert(pixels[i] == 0);
		assert(pixels[0] == 0 && pixels[sFrameModelBytes / 4 - 1] == 0);
		sFrameNoncacheable++;
		return B_OK;
	}
	if (area == sCursorArea) {
		assert(address == sCursorAllocation && bytes == kCursorBufferBytes);
		for (unsigned i = 0; i < kCursorBufferBytes / 4; i++)
			assert(pixels[i] == 0);
		sCursorNoncacheable++;
		return B_OK;
	}
	assert(area == sPatternArea && address == sPatternAllocation && bytes == kPatternBytes);
	// The whole pattern is filled through the cached alias before retyping.
	assert(pixels[0] == kPatternBorderColor && pixels[1079 * 1920 + 1919] == kPatternBorderColor);
	assert(pixels[540 * 1920 + 31] == kPatternBorderColor && pixels[31 * 1920 + 960] == kPatternBorderColor);
	assert(pixels[540 * 1920 + 32] == 0xffffffff && pixels[540 * 1920 + 1887] == 0xff000000);
	for (unsigned bar = 0; bar < kPatternBars; bar++) {
		for (unsigned y = 32; y < 1048; y += 127)
			assert(pixels[y * 1920 + 32 + bar * 232 + 116] == kPatternColors[bar]);
	}
	sNoncacheableCalls++;
	return B_OK;
}


static area_id
create_area(const char* name, void** address, uint32 spec, size_t size, uint32 lock, uint32 protection)
{
	assert(sLockDepth == 1 && strcmp(name, "RK3588 display shared") == 0 && sSharedPage == NULL);
	assert(spec == B_ANY_KERNEL_ADDRESS && size == B_PAGE_SIZE && lock == B_FULL_LOCK);
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA));
	sSharedPage = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(sSharedPage != MAP_FAILED);
	sSharedModelArea = 1100 + (int)++sPatternAllocations;
	*address = sSharedPage;
	return sSharedModelArea;
}


static area_id
vm_clone_area(team_id team, const char* name, void** address, uint32 spec, uint32 protection,
	uint32 mapping, area_id source, bool kernel)
{
	assert(sLockDepth == 1 && team == B_CURRENT_TEAM && spec == B_ANY_ADDRESS && kernel);
	assert(strcmp(name, "RK3588 display frame buffer clone") == 0 && mapping == 0);
	assert(protection == (B_READ_AREA | B_WRITE_AREA) && source == sFrameArea && sFrameArea >= 0);
	*address = sFrameAllocation;
	return 1200 + (int)++sClones;
}


static status_t
_user_get_area_info(area_id area, area_info* info)
{
	assert(area == 1200 + (int)sClones && info != NULL);
	info->area = area;
	info->size = sFrameModelBytes;
	info->address = sFrameAllocation;
	return B_OK;
}


static status_t
vm_change_clones_to_null_areas(area_id area)
{
	assert(sLockDepth == 1 && area == sFrameArea && sFrameArea >= 0);
	sNullClones++;
	return B_OK;
}


static ssize_t
user_strlcpy(char* to, const char* from, size_t size)
{
	if (to == NULL)
		return B_BAD_ADDRESS;
	snprintf(to, size, "%s", from);
	return (ssize_t)strlen(from);
}


static status_t
frame_buffer_update(addr_t address, int32 width, int32 height, int32 depth, int32 bytesPerRow)
{
	assert(sLockDepth == 1);
	sConsole = ConsoleState{address, width, height, depth, bytesPerRow};
	sConsoleUpdates++;
	return B_OK;
}


// Interrupt line and semaphore model: one handler on the VOP interrupt, one
// retrace semaphore whose waiter count the test sets.
static int32 sHandlerInterrupt = -1;
static unsigned sHandlerInstalls, sHandlerRemovals, sFailInstall;
static sem_id sModelSemaphore = -1;
static int32 sSemaphoreWaiters, sSemaphoreReleases, sSemaphoreReleased;
static unsigned sSemaphoreCreations, sSemaphoreDeletions, sFailSemaphore;

static status_t
install_io_interrupt_handler(int32 interrupt, interrupt_handler handler, void* data, uint32 flags)
{
	assert(sLockDepth == 1 && interrupt == 188 && handler != NULL && flags == 0 && sHandler == NULL);
	if (++sHandlerInstalls == sFailInstall)
		return B_ERROR;
	sHandler = handler;
	sHandlerData = data;
	sHandlerInterrupt = interrupt;
	return B_OK;
}


static status_t
remove_io_interrupt_handler(int32 interrupt, interrupt_handler handler, void* data)
{
	assert(interrupt == sHandlerInterrupt && handler == sHandler && data == sHandlerData);
	sHandler = NULL;
	sHandlerInterrupt = -1;
	sHandlerRemovals++;
	return B_OK;
}


static const team_id kModelTeam = 4242;
static team_id team_get_current_team_id() { return kModelTeam; }
static team_id sSemaphoreOwner = -1;

static status_t
set_sem_owner(sem_id id, team_id team)
{
	assert(id == sModelSemaphore && id >= 0 && team == kModelTeam);
	sSemaphoreOwner = team;
	return B_OK;
}


static sem_id
create_sem(int32 count, const char* name)
{
	assert(sLockDepth == 1 && count == 0 && strcmp(name, "RK3588 display retrace") == 0);
	assert(sModelSemaphore < 0);
	if (++sSemaphoreCreations == sFailSemaphore)
		return B_NO_MEMORY;
	sModelSemaphore = 1300 + (sem_id)sSemaphoreCreations;
	sSemaphoreWaiters = 0;
	return sModelSemaphore;
}


static status_t
delete_sem(sem_id id)
{
	assert(id == sModelSemaphore && id >= 0);
	sModelSemaphore = -1;
	sSemaphoreOwner = -1;
	sSemaphoreDeletions++;
	return B_OK;
}


static status_t
get_sem_count(sem_id id, int32* count)
{
	assert(id == sModelSemaphore);
	*count = sSemaphoreWaiters;
	return B_OK;
}


static status_t
release_sem_etc(sem_id id, int32 count, uint32 flags)
{
	assert(id == sModelSemaphore && count > 0 && flags == B_DO_NOT_RESCHEDULE);
	assert(count == -sSemaphoreWaiters);
	sSemaphoreReleases++;
	sSemaphoreReleased += count;
	sSemaphoreWaiters += count;
	return B_OK;
}


#define FRAME_BUFFER_BOOT_INFO "frame_buffer/v1"
struct frame_buffer_boot_info {
	area_id area;
	addr_t physical_frame_buffer;
	addr_t frame_buffer;
	int32 width, height, depth, bytes_per_row;
	uint8_t vesa_capabilities;
};
static frame_buffer_boot_info sBootInfo;
static bool sBootInfoPresent;

static void*
get_boot_item(const char* name, size_t* size)
{
	assert(strcmp(name, FRAME_BUFFER_BOOT_INFO) == 0 && size == NULL);
	return sBootInfoPresent ? &sBootInfo : NULL;
}


#include "driver.inc"


static void
Cells(device_node& node, const char* property, std::initializer_list<uint32> cells)
{
	auto& bytes = node.properties[property];
	bytes.clear();
	for (uint32 value : cells) {
		for (int shift = 24; shift >= 0; shift -= 8)
			bytes.push_back(value >> shift);
	}
}


static void
Strings(device_node& node, const char* property, std::initializer_list<const char*> strings)
{
	auto& bytes = node.properties[property];
	bytes.clear();
	for (const char* value : strings)
		bytes.insert(bytes.end(), value, value + strlen(value) + 1);
}


static void
Prepare()
{
	for (device_node* node : sAllNodes) {
		assert(node->held == 0);
		node->properties.clear();
		node->wrongModule = false;
		node->parent = &sRoot;
		node->base1 = node->size1 = 0;
		node->irqs.clear();
		node->irqController = &sGic;
	}
	sRoot.parent = &sBusNode;
	sBusNode.parent = NULL;
	sRoot.name = "";
	sVop.name = "vop@fdd90000";
	sPorts.name = "ports"; sPorts.parent = &sVop;
	sPort1.name = "port@1"; sPort1.parent = &sPorts;
	sEndpoint8.name = "endpoint@8"; sEndpoint8.parent = &sPort1;
	sHdmi.name = "hdmi@fdea0000";
	sHdmiPorts.name = "ports"; sHdmiPorts.parent = &sHdmi;
	sHdmiPort0.name = "port@0"; sHdmiPort0.parent = &sHdmiPorts;
	sHdmiEndpoint.name = "endpoint"; sHdmiEndpoint.parent = &sHdmiPort0;
	sPhy0.name = "phy@fed60000";
	sPhy1.name = "phy@fed70000";
	sHdptxGrf.name = "syscon@fd5e4000";
	sSysGrf.name = "syscon@fd58c000";
	sVopGrf.name = "syscon@fd5a4000";
	sVo1Grf.name = "syscon@fd5a8000";
	sPmu.name = "power-management@fd8d8000";
	sPower.name = "power-controller"; sPower.parent = &sPmu;
	sClock.name = "clock-controller@fd7c0000";
	sGic.name = "interrupt-controller@fe600000";
	sOther.name = "other";
	// The second connector's path as the firmware tree has it.
	sUsbdpPhy1.name = "phy@fed90000";
	sUsbdpGrf.name = "syscon@fd5cc000";
	sVo0Grf.name = "syscon@fd5a6000";
	sIoc.name = "syscon@fd5f0000";
	sPinctrl.name = "pinctrl";
	sGpio3.name = "gpio@fec40000"; sGpio3.parent = &sPinctrl;
	sPhandles = {{0x21, &sClock}, {0x22, &sPower}, {0x6b, &sPhy0}, {0x6c, &sPhy1},
		{0x6e, &sSysGrf}, {0x6f, &sVopGrf}, {0x70, &sVo1Grf}, {0x71, &sPmu},
		{0x72, &sHdmiEndpoint}, {0x117, &sEndpoint8}, {0x127, &sHdptxGrf},
		{0x12a, &sUsbdpGrf}, {0xfd, &sVo0Grf}};
	sUsbdpPhy1.base = 0xfed90000; sUsbdpPhy1.size = 0x10000;
	sUsbdpGrf.base = 0xfd5cc000; sUsbdpGrf.size = 0x4000;
	sVo0Grf.base = 0xfd5a6000; sVo0Grf.size = 0x2000;
	sIoc.base = 0xfd5f0000; sIoc.size = 0x10000;
	sGpio3.base = 0xfec40000; sGpio3.size = 0x100;
	Strings(sUsbdpPhy1, "compatible", {"rockchip,rk3588-usbdp-phy"});
	Strings(sUsbdpPhy1, "status", {"okay"});
	Strings(sUsbdpPhy1, "clock-names", {"refclk", "immortal", "pclk", "utmi"});
	Cells(sUsbdpPhy1, "clocks", {0x21, 0x2a1, 0x21, 0x26d, 0x21, 0x257, 0x128});
	Strings(sUsbdpPhy1, "reset-names", {"init", "cmn", "lane", "pcs_apb", "pma_apb"});
	Cells(sUsbdpPhy1, "resets", {0x21, 0x0f, 0x21, 0x10, 0x21, 0x11, 0x21, 0x12, 0x21, 0x219});
	Cells(sUsbdpPhy1, "rockchip,u2phy-grf", {0x129});
	Cells(sUsbdpPhy1, "rockchip,usb-grf", {0xfb});
	Cells(sUsbdpPhy1, "rockchip,usbdpphy-grf", {0x12a});
	Cells(sUsbdpPhy1, "rockchip,vo-grf", {0xfd});
	Strings(sUsbdpGrf, "compatible", {"rockchip,rk3588-usbdpphy-grf", "syscon"});
	Strings(sVo0Grf, "compatible", {"rockchip,rk3588-vo0-grf", "syscon"});
	Strings(sIoc, "compatible", {"rockchip,rk3588-ioc", "syscon"});
	Strings(sGpio3, "compatible", {"rockchip,gpio-bank"});
	Cells(sGpio3, "clocks", {0x21, 0x77, 0x21, 0x78});
	sVop.base = 0xfdd90000; sVop.size = 0x4200; sVop.base1 = 0xfdd95000; sVop.size1 = 0x1000;
	sVop.irqs = {188};
	sHdmi.base = 0xfdea0000; sHdmi.size = 0x20000;
	sHdmi.irqs = {205, 206, 207, 208, 393};
	sPhy0.base = 0xfed60000; sPhy0.size = 0x2000;
	sPhy1.base = 0xfed70000; sPhy1.size = 0x2000;
	sHdptxGrf.base = 0xfd5e4000; sHdptxGrf.size = 0x100;
	sSysGrf.base = 0xfd58c000; sSysGrf.size = 0x1000;
	sVopGrf.base = 0xfd5a4000; sVopGrf.size = 0x2000;
	sVo1Grf.base = 0xfd5a8000; sVo1Grf.size = 0x4000;
	sPmu.base = 0xfd8d8000; sPmu.size = 0x400;
	sClock.base = 0xfd7c0000; sClock.size = 0x5c000;
	sGic.base = 0xfe600000; sGic.size = 0x10000;
	Strings(sRoot, "compatible", {"radxa,rock-5-itx", "rockchip,rk3588"});
	Strings(sVop, "compatible", {"rockchip,rk3588-vop"});
	Strings(sVop, "status", {"okay"});
	Strings(sVop, "reg-names", {"vop", "gamma-lut"});
	Strings(sVop, "clock-names", {"aclk", "hclk", "dclk_vp0", "dclk_vp1", "dclk_vp2",
		"dclk_vp3", "pclk_vop", "pll_hdmiphy0", "pll_hdmiphy1"});
	Cells(sVop, "clocks", {0x21, 0x25d, 0x21, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
		0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6c});
	Cells(sVop, "interrupts", {0, 156, 4, 0});
	Cells(sVop, "power-domains", {0x22, 24});
	Cells(sVop, "rockchip,grf", {0x6e});
	Cells(sVop, "rockchip,vop-grf", {0x6f});
	Cells(sVop, "rockchip,vo1-grf", {0x70});
	Cells(sVop, "rockchip,pmu", {0x71});
	Cells(sPort1, "reg", {1});
	Cells(sEndpoint8, "reg", {8});
	Cells(sEndpoint8, "remote-endpoint", {0x72});
	Cells(sHdmiPort0, "reg", {0});
	Cells(sHdmiEndpoint, "remote-endpoint", {0x117});
	Strings(sHdmi, "compatible", {"rockchip,rk3588-dw-hdmi-qp"});
	Strings(sHdmi, "status", {"okay"});
	Strings(sHdmi, "clock-names", {"pclk", "earc", "ref", "aud", "hdp", "hclk_vo1"});
	Cells(sHdmi, "clocks", {0x21, 0x213, 0x21, 0x214, 0x21, 0x215, 0x21, 0x239, 0x21, 0x253,
		0x21, 0x2cd});
	Strings(sHdmi, "interrupt-names", {"avp", "cec", "earc", "main", "hpd"});
	Cells(sHdmi, "interrupts", {0, 173, 4, 0, 0, 174, 4, 0, 0, 175, 4, 0, 0, 176, 4, 0,
		0, 361, 4, 0});
	Strings(sHdmi, "reset-names", {"ref", "hdp"});
	Cells(sHdmi, "resets", {0x21, 0x1d0, 0x21, 0x231});
	Cells(sHdmi, "power-domains", {0x22, 26});
	Cells(sHdmi, "rockchip,grf", {0x6e});
	Cells(sHdmi, "rockchip,vo-grf", {0x70});
	Cells(sHdmi, "phys", {0x6c});
	Strings(sPhy0, "compatible", {"rockchip,rk3588-hdptx-phy"});
	Strings(sPhy1, "compatible", {"rockchip,rk3588-hdptx-phy"});
	Strings(sPhy1, "status", {"okay"});
	Strings(sPhy1, "clock-names", {"ref", "apb"});
	Cells(sPhy1, "rockchip,grf", {0x127});
	Strings(sHdptxGrf, "compatible", {"rockchip,rk3588-hdptxphy-grf", "syscon"});
	Strings(sSysGrf, "compatible", {"rockchip,rk3588-sys-grf", "syscon"});
	Strings(sVopGrf, "compatible", {"rockchip,rk3588-vop-grf", "syscon"});
	Strings(sVo1Grf, "compatible", {"rockchip,rk3588-vo1-grf", "syscon"});
	Strings(sPmu, "compatible", {"rockchip,rk3588-pmu", "syscon", "simple-mfd"});
	Strings(sPower, "compatible", {"rockchip,rk3588-power-controller"});
	Cells(sPower, "#power-domain-cells", {1});
	Strings(sClock, "compatible", {"rockchip,rk3588-cru"});
	Cells(sClock, "#clock-cells", {1});
	Strings(sGic, "compatible", {"arm,gic-v3"});
	Cells(sGic, "#interrupt-cells", {4});
	sRepairStatus = (1u << 16) | (1u << 17) | (1u << 18);
	sGate52 = 0;
	sGate61 = 0;
	sGate2 = sGate17 = sGate56 = 0;
	sGpioPort = 0;
	sIocMux = 0x50;
	sAllowDp = false;
	assert(sDpModel == NULL && sPmaModel == NULL && sHiwordModels.empty());
	sDpShadow.clear(); sPmaShadow.clear();
	sPmaWrites.clear(); sDpWrites.clear(); sHiwordWrites.clear();
	sSinkPresent = true;
	sAuxDeferReplies = 0;
	sAuxNeverReplies = sLcpllNeverLocks = false;
	sRefclkSelect = 0;
	sHotPlugDebounce = 20;
	sRopllNeverLocks = sLinkNeverRecovers = false;
	sHighestRecoveringRate = 0x1e;
	sDpcdWrites.clear();
	sAuxCommands = 0;
	sAuxLog.clear();
	memset(sDpcd, 0, sizeof(sDpcd));
	// DPCD of a DP 1.2 branch device (the RA620): 2.7 Gb/s, two lanes, enhanced framing.
	static const uint8_t kReceiverCaps[16] = {0x12, 0x0a, 0x82, 0x01, 0x00, 0x15, 0x01, 0x81,
		0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00};
	memcpy(sDpcd, kReceiverCaps, 16);
	sDpcd[0x200] = 0x41;
	memset(sSinkEdid, 0, sizeof(sSinkEdid));
	static const uint8_t kEdidHeader[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	memcpy(sSinkEdid, kEdidHeader, 8);
	sSinkEdid[8] = 0x5a; sSinkEdid[9] = 0x63; // a manufacturer id
	for (unsigned i = 10; i < 127; i++)
		sSinkEdid[i] = (uint8_t)(i * 13);
	unsigned edidSum = 0;
	for (unsigned i = 0; i < 127; i++)
		edidSum += sSinkEdid[i];
	sSinkEdid[127] = (uint8_t)(0x100 - (edidSum & 0xff));
	sMapAttempts = 0;
	sFailMap = 0;
	sMappedBases.clear();
	sAllowEdid = false;
	sHotPlug = (1u << 24) | (1u << 27);
	sNackAt = -1;
	sUnresponsive = false;
	sServed = sResets = sModelSpins = 0;
	sHdmiModel = NULL;
	sVopModel = NULL;
	sVopShadow.clear();
	sVopWrites.clear();
	sAllowScanout = false;
	sAllowModeSet = false;
	sHoldNever = false;
	sAllowCursor = false;
	sVopPendingWindow.clear();
	sCursorStickyControl = false;
	sCursorNoncacheable = 0;
	sPhyModel = NULL; sPhyShadow.clear(); sPhyWrites.clear();
	sHdmiShadow.clear(); sHdmiWrites.clear();
	sGrfModel = NULL; sGrfShadow.clear(); sGrfWrites.clear(); sGrfControl = 0xe0;
	sPhyStatusModel = 0x0e;
	sCruModel = NULL; sCruShadow.clear(); sCruWrites.clear();
	sVopHoldCountdown = -1;
	sHoldValid = 0;
	memset(&sCurrentMode, 0, sizeof(sCurrentMode));
	sStickyAddress = false;
	sCommitNeverCompletes = false;
	sVopAddressPending = false;
	sVopCommitCountdown = -1;
	sVopOverrides.clear();
	// Firmware state from the qualified +263 observation: HDMI1 fed by video
	// port 2, ports 0/1/3 in standby, ESMART2 region 0 alone scanning the
	// 1920x1080 XRGB8888 framebuffer at 0xed280000.
	sVopOverrides[0x000] = 0x8000; // GLB_CFG_DONE_EN stays set, port bits clear
	sVopOverrides[0x028] = 0x00080020;
	sVopOverrides[0xc00] = 0x8000000f;
	sVopOverrides[0xd00] = 0x8000000f;
	sVopOverrides[0xe00] = 0x0000000f;
	sVopOverrides[0xf00] = 0x8000000f;
	for (unsigned window = 0; window < kVopEsmartCount; window++) {
		for (unsigned offset : kVopEsmartOffsets)
			sVopOverrides[kVopEsmartBase + window * kVopEsmartStride + offset] = 0;
	}
	sVopOverrides[0x1c00] = 4;
	sVopOverrides[0x1c10] = 1;
	// ESMART2 on AXI bus 1 with Linux' read ids and a 23-cycle delay; ESMART3
	// left mirrored with stale ids on bus 0; automatic clock gating enabled.
	sVopOverrides[0x1c04] = (0xau << 4) | (0xbu << 12);
	sVopOverrides[0x1c08] = 0x2;
	sVopOverrides[0x1e04] = 0x80000000u | (0x3u << 4) | (0x3u << 12);
	sVopOverrides[0x1e08] = 0x0;
	sVopOverrides[0x6f8] = 0x00170000;
	sVopOverrides[0x008] = 0x80000000u;
	sVopOverrides[kModelAddressOffset] = kModelFirmwareAddress;
	sVopOverrides[0x1c1c] = 1920;
	sVopOverrides[0x1c20] = 0x0437077f;
	sVopOverrides[0x1c24] = 0x0437077f;
	sVopOverrides[0x1c28] = 0;
	// No video-port interrupts enabled or pending, as observed.
	for (unsigned port = 0; port < 4; port++) {
		for (unsigned word = 0; word < 3; word++)
			sVopOverrides[0xa0 + port * 0x10 + word * 4] = 0;
	}
	// Video port 2 timing as observed: 2200x1125 total, sync 44/5, active 192-2112 / 41-1121.
	sVopOverrides[0xe48] = 0x0898002c;
	sVopOverrides[0xe4c] = 0x00c00840;
	sVopOverrides[0xe50] = 0x04650005;
	sVopOverrides[0xe54] = 0x00290461;
	sBootInfoPresent = true;
	sBootInfo = frame_buffer_boot_info{17, kModelFirmwareAddress, 0xffff000012340000ull, 1920, 1080, 32, 7680, 0};
	sScanoutSwapped = false;
	sFirmwareAddress = 0;
	assert(sOwner == NULL);
	ReleaseContiguous(sPattern);
	ReleaseContiguous(sFrame);
	ReleaseContiguous(sCursor);
	sCursorProgrammed = false;
	memset(&sCursorState, 0, sizeof(sCursorState));
	if (sSharedModelArea >= 0)
		delete_area(sSharedModelArea);
	sSharedArea = -1;
	sShared = NULL;
	memset(&sAccelerant, 0, sizeof(sAccelerant));
	sPatternAllocations = sFailPattern = sNoncacheableCalls = sFrameNoncacheable = 0;
	sClones = sNullClones = sConsoleUpdates = 0;
	assert(sVopRegisters == NULL && sVopArea < 0 && sRetraceSemaphore < 0 && !sInterruptInstalled);
	sHandler = NULL;
	sHandlerData = NULL;
	sHandlerInterrupt = -1;
	sHandlerInstalls = sHandlerRemovals = sFailInstall = 0;
	sModelSemaphore = -1;
	sSemaphoreWaiters = sSemaphoreReleases = sSemaphoreReleased = 0;
	sSemaphoreCreations = sSemaphoreDeletions = sFailSemaphore = 0;
	sRetraces = 0;
	sConsole = ConsoleState{};
	sPatternHighPhysical = false;
	for (unsigned i = 0; i < 512; i++)
		sEdid[i] = (uint8_t)(i * 7 + 3);
	static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	memcpy(sEdid, header, 8);
	sEdid[126] = 3;
	for (unsigned block = 0; block < 4; block++) {
		unsigned sum = 0;
		for (unsigned i = 0; i < 127; i++)
			sum += sEdid[block * 128 + i];
		sEdid[block * 128 + 127] = (uint8_t)(0x100 - (sum & 0xff));
	}
}


static void
CheckSnapshotValues(const DisplaySnapshot& snapshot, bool vop, bool hdmi)
{
	// The DisplayPort path follows the model's VO0 domain and gate words.
	bool dp = (sRepairStatus & (1u << 17)) != 0 && (sGate56 & 0x20) == 0;
	bool aux = dp && (sGate56 & 0x8) == 0;
	bool gpio = (sGate17 & 4) == 0;
	assert(snapshot.version == kSnapshotVersion && kSnapshotVersion == 2);
	assert((snapshot.flags & kSnapshotReadOnly) != 0);
	assert(((snapshot.flags & kSnapshotVopRead) != 0) == vop);
	assert(((snapshot.flags & kSnapshotVopSkipped) != 0) == !vop);
	assert(((snapshot.flags & kSnapshotHdmiRead) != 0) == hdmi);
	assert(((snapshot.flags & kSnapshotHdmiSkipped) != 0) == !hdmi);
	assert(((snapshot.flags & kSnapshotDpRead) != 0) == dp);
	assert(((snapshot.flags & kSnapshotDpSkipped) != 0) == !dp);
	assert(((snapshot.flags & kSnapshotDpAuxRead) != 0) == aux);
	assert(((snapshot.flags & kSnapshotGpioRead) != 0) == gpio);
	assert(((snapshot.flags & kSnapshotGpioSkipped) != 0) == !gpio);
	for (unsigned i = 0; i < kUsbdpGrfCount; i++)
		assert(snapshot.usbdpGrf[i] == ModelRegister(0xfd5cc000, kUsbdpGrfOffsets[i]));
	for (unsigned i = 0; i < kVo0GrfCount; i++)
		assert(snapshot.vo0Grf[i] == ModelRegister(0xfd5a6000, kVo0GrfOffsets[i]));
	for (unsigned i = 0; i < kIocCount; i++)
		assert(snapshot.ioc[i] == ModelRegister(0xfd5f8000, kIocOffsets[i]));
	for (unsigned i = 0; i < kGpioCount; i++)
		assert(snapshot.gpio[i] == (gpio ? ModelRegister(0xfec40000, kGpioOffsets[i]) : 0));
	for (unsigned i = 0; i < kDpCount; i++)
		assert(snapshot.dp[i] == (dp ? ModelRegister(0xfde60000, kDpOffsets[i]) : 0));
	for (unsigned i = 0; i < kDpAuxCount; i++)
		assert(snapshot.dpAux[i] == (aux ? ModelRegister(0xfde60000, kDpAuxOffsets[i]) : 0));
	assert(snapshot.finishedMicros > snapshot.startedMicros);
	for (unsigned i = 0; i < kPmuCount; i++)
		assert(snapshot.pmu[i] == ModelRegister(0xfd8d8000, kPmuOffsets[i]));
	for (unsigned i = 0; i < kClockSelectCount; i++)
		assert(snapshot.clockSelect[i] == ModelRegister(0xfd7c0000, kClockSelectOffsets[i]));
	for (unsigned i = 0; i < kClockGateCount; i++)
		assert(snapshot.clockGate[i] == ModelRegister(0xfd7c0000, kClockGateOffsets[i]));
	for (unsigned i = 0; i < kSysGrfCount; i++)
		assert(snapshot.sysGrf[i] == ModelRegister(0xfd58c000, kSysGrfOffsets[i]));
	assert(snapshot.vopGrf == ModelRegister(0xfd5a4000, kVopGrfOffset));
	for (unsigned i = 0; i < kVo1GrfCount; i++)
		assert(snapshot.vo1Grf[i] == ModelRegister(0xfd5a8000, kVo1GrfOffsets[i]));
	for (unsigned i = 0; i < kHdptxGrfCount; i++)
		assert(snapshot.hdptxGrf[i] == ModelRegister(0xfd5e4000, kHdptxGrfOffsets[i]));
	for (unsigned i = 0; i < kVopSystemCount; i++)
		assert(snapshot.vopSystem[i] == (vop ? ModelRegister(0xfdd90000, kVopSystemOffsets[i]) : 0));
	for (unsigned i = 0; i < kVopOverlayCount; i++)
		assert(snapshot.vopOverlay[i] == (vop ? ModelRegister(0xfdd90000, kVopOverlayOffsets[i]) : 0));
	for (unsigned port = 0; port < kVopPortCount; port++) {
		for (unsigned i = 0; i < kVopPortRegisterCount; i++) {
			assert(snapshot.vopPort[port][i] == (vop ? ModelRegister(0xfdd90000,
				kVopPortBase + port * kVopPortStride + kVopPortOffsets[i]) : 0));
		}
	}
	for (unsigned window = 0; window < kVopClusterCount; window++) {
		for (unsigned i = 0; i < kVopClusterRegisterCount; i++) {
			assert(snapshot.vopCluster[window][i] == (vop ? ModelRegister(0xfdd90000,
				kVopClusterBase + window * kVopClusterStride + kVopClusterOffsets[i]) : 0));
		}
	}
	for (unsigned window = 0; window < kVopEsmartCount; window++) {
		for (unsigned i = 0; i < kVopEsmartRegisterCount; i++) {
			assert(snapshot.vopEsmart[window][i] == (vop ? ModelRegister(0xfdd90000,
				kVopEsmartBase + window * kVopEsmartStride + kVopEsmartOffsets[i]) : 0));
		}
	}
	for (unsigned i = 0; i < kHdmiCount; i++)
		assert(snapshot.hdmi[i] == (hdmi ? ModelRegister(0xfdea0000, kHdmiOffsets[i]) : 0));
}


int
main()
{
	static_assert(sizeof(ResourceInfo) == 448, "Diagnostic ABI layout changed");
	static_assert(sizeof(EdidRequest) == 192, "EDID ABI layout changed");
	static_assert(sizeof(DisplaySnapshot) == 928, "Snapshot ABI layout changed");
	static_assert(sizeof(ScanoutRequest) == 88, "Scanout ABI layout changed");
	static_assert(kPatternBytes == 1920 * 1080 * 4, "Pattern is the firmware framebuffer size");
	for (unsigned offset : kVopSystemOffsets) assert(offset + 4 <= kVopMapSize);
	for (unsigned offset : kVopOverlayOffsets) assert(offset + 4 <= kVopMapSize);
	assert(kVopPortBase + 3 * kVopPortStride + 0x54 + 4 <= kVopMapSize);
	assert(kVopClusterBase + 3 * kVopClusterStride + 0x100 + 4 <= kVopMapSize);
	assert(kVopEsmartBase + 3 * kVopEsmartStride + 0x28 + 4 <= kVopMapSize);
	for (unsigned offset : kHdmiOffsets) assert(offset + 4 <= kHdmiMapSize);
	{
		// Registers Linux 6.18 dw-hdmi-qp.c or EDK2 DwHdmiQpLib.c read or read-modify-write.
		static const unsigned kReadableByReference[] = {0x044, 0x0f4, 0x0f8, 0x10c, 0x820, 0x8e0,
			0x968, 0xa9c, 0xaa8, 0x3020, 0x3024};
		for (unsigned offset : kHdmiOffsets) {
			bool listed = false;
			for (unsigned known : kReadableByReference) listed |= known == offset;
			assert(listed);
			assert(offset != 0x0ec); // write-only I2CM_CONTROL0 aborts on read (+259 panic)
		}
	}
	for (unsigned offset : kPmuOffsets) assert(offset + 4 <= 0x400);
	for (unsigned offset : kHdptxGrfOffsets) assert(offset + 4 <= 0x100);
	assert(sysconf(_SC_PAGESIZE) == B_PAGE_SIZE);
	sDeviceManager = &sManager;
	Prepare();
	ResourceInfo good = {};
	assert(ReadResources(&sVop, good) && ResourcesMatch(good));
	assert(good.hdmiPhyPhandle == 0x6c && good.vopInterrupt == 188 && good.hdmiInterrupts[4] == 393);
	assert(good.pmuBase == 0xfd8d8000 && good.clockBase == 0xfd7c0000 && good.hdptxGrfBase == 0xfd5e4000);
	assert(good.version == 2 && good.usbdpPhyBase == 0xfed90000 && good.usbdpGrfBase == 0xfd5cc000);
	assert(good.vo0GrfBase == 0xfd5a6000 && good.iocBase == 0xfd5f0000 && good.gpio3Base == 0xfec40000);
	assert(good.dpBase == 0xfde60000 && good.dpSize == 0x4000 && good.dpPowerDomain == 25);
	assert(good.usbdpPhyClockIds[2] == 0x257 && good.usbdpPhyResets[4] == 0x219 && good.gpio3ClockIds[1] == 0x78);
	Prepare();
	// Phandles are references, not fixed numerical board identifiers.
	sPhandles.erase(0x21); sPhandles[909] = &sClock;
	sPhandles.erase(0x6c); sPhandles[910] = &sPhy1;
	sPhandles.erase(0x72); sPhandles[911] = &sHdmiEndpoint;
	Cells(sVop, "clocks", {909, 0x25d, 909, 0x25c, 909, 0x261, 909, 0x262, 909, 0x263,
		909, 0x264, 909, 0x25b, 0x6b, 910});
	Cells(sHdmi, "clocks", {909, 0x213, 909, 0x214, 909, 0x215, 909, 0x239, 909, 0x253,
		909, 0x2cd});
	Cells(sHdmi, "resets", {909, 0x1d0, 909, 0x231});
	Cells(sHdmi, "phys", {910});
	Cells(sEndpoint8, "remote-endpoint", {911});
	Cells(sUsbdpPhy1, "clocks", {909, 0x2a1, 909, 0x26d, 909, 0x257, 0x128});
	Cells(sUsbdpPhy1, "resets", {909, 0x0f, 909, 0x10, 909, 0x11, 909, 0x12, 909, 0x219});
	Cells(sGpio3, "clocks", {909, 0x77, 909, 0x78});
	ResourceInfo moved = {};
	assert(ReadResources(&sVop, moved));
	good.hdmiPhyPhandle = 910;
	assert(memcmp(&moved, &good, sizeof(good)) == 0);
	good.hdmiPhyPhandle = 0x6c;

	std::vector<std::function<void()> > faults = {
		[] { sRoot.properties["compatible"].pop_back(); },
		[] { Strings(sRoot, "compatible", {"radxa,rock-5b", "rockchip,rk3588"}); },
		[] { Strings(sVop, "status", {"disabled"}); },
		[] { Strings(sVop, "compatible", {"rockchip,rk3568-vop"}); },
		[] { sVop.wrongModule = true; },
		[] { sBusNode.wrongModule = true; },
		[] { sVop.size = 0x4000; },
		[] { sVop.base += 0x1000; },
		[] { sVop.size1 = 0; },
		[] { Strings(sVop, "reg-names", {"vop"}); },
		[] { Strings(sVop, "clock-names", {"aclk", "hclk", "dclk_vp0", "dclk_vp1", "dclk_vp2",
			"dclk_vp3", "pclk_vop", "pll_hdmiphy1", "pll_hdmiphy0"}); },
		[] { sVop.properties["clocks"].pop_back(); },
		[] { Cells(sVop, "clocks", {0x21, 0x25d, 0x22, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6c}); },
		[] { Cells(sVop, "clocks", {0x21, 0x25d, 0x21, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6c, 0x6c}); },
		[] { Cells(sVop, "clocks", {0x21, 0x25d, 0x21, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6e}); },
		[] { Cells(sVop, "clocks", {0x21, 0x25c, 0x21, 0x25d, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6c}); },
		[] { Cells(sVop, "interrupts", {0, 157, 4, 0}); },
		[] { sVop.irqs = {189}; },
		[] { sUsbdpPhy1.name = "phy@fed80000"; },
		[] { Strings(sUsbdpPhy1, "status", {"disabled"}); },
		[] { Strings(sUsbdpPhy1, "compatible", {"rockchip,rk3588-hdptx-phy"}); },
		[] { Cells(sUsbdpPhy1, "rockchip,usbdpphy-grf", {0xfd}); },
		[] { Cells(sUsbdpPhy1, "rockchip,vo-grf", {0x70}); },
		[] { Cells(sUsbdpPhy1, "clocks", {0x22, 0x2a1, 0x21, 0x26d, 0x21, 0x257, 0x128}); },
		[] { Strings(sUsbdpPhy1, "reset-names", {"init", "cmn", "lane", "pcs", "pma_apb"}); },
		[] { sIoc.name = "syscon@fd5f1000"; },
		[] { Strings(sIoc, "compatible", {"rockchip,rk3588-ioc"}); },
		[] { sGpio3.parent = &sRoot; },
		[] { sGpio3.size = 0; },
		[] { Cells(sGpio3, "clocks", {0x21, 0x77, 0x22, 0x78}); },
		[] { Strings(sGpio3, "compatible", {"rockchip,gpio"}); },
		[] { sVop.irqController = &sClock; },
		[] { Cells(sVop, "interrupts-extended", {1}); },
		[] { Cells(sVop, "power-domains", {0x22, 25}); },
		[] { Cells(sPower, "#power-domain-cells", {2}); },
		[] { sPower.parent = &sClock; },
		[] { sPmu.wrongModule = true; },
		[] { sPmu.size = 0x100; },
		[] { Cells(sVop, "rockchip,pmu", {0x22}); },
		[] { Cells(sVop, "rockchip,grf", {0x6f}); },
		[] { Strings(sSysGrf, "compatible", {"rockchip,rk3588-sys-grf"}); },
		[] { Strings(sVopGrf, "compatible", {"rockchip,rk3588-vo0-grf", "syscon"}); },
		[] { sVo1Grf.base += 0x1000; },
		[] { Strings(sVo1Grf, "status", {"disabled"}); },
		[] { sClock.wrongModule = true; },
		[] { Cells(sClock, "#clock-cells", {2}); },
		[] { sClock.size = 0x1000; },
		[] { sPorts.name = "port"; },
		[] { sPort1.name = "port@2"; },
		[] { Cells(sPort1, "reg", {2}); },
		[] { sEndpoint8.name = "endpoint@9"; },
		[] { Cells(sEndpoint8, "reg", {9}); },
		[] { sEndpoint8.properties.erase("remote-endpoint"); },
		[] { sPhandles.erase(0x72); },
		[] { Cells(sEndpoint8, "remote-endpoint", {0x6c}); },
		[] { sHdmiPort0.name = "port@1"; Cells(sHdmiPort0, "reg", {1}); },
		[] { sHdmiEndpoint.parent = &sHdmiPorts; },
		[] { Strings(sHdmi, "status", {"disabled"}); },
		[] { Strings(sHdmi, "compatible", {"rockchip,rk3588-dw-hdmi"}); },
		[] { sHdmi.base = 0xfde80000; },
		[] { sHdmi.size = 0x10000; },
		[] { Strings(sHdmi, "clock-names", {"pclk", "earc", "ref", "aud", "hdp"}); },
		[] { sHdmi.properties["clocks"].pop_back(); },
		[] { Cells(sHdmi, "clocks", {0x22, 0x213, 0x21, 0x214, 0x21, 0x215, 0x21, 0x239, 0x21, 0x253,
			0x21, 0x2cd}); },
		[] { Cells(sHdmi, "clocks", {0x21, 0x210, 0x21, 0x211, 0x21, 0x212, 0x21, 0x234, 0x21, 0x252,
			0x21, 0x2cd}); },
		[] { Strings(sHdmi, "interrupt-names", {"avp", "cec", "earc", "hpd", "main"}); },
		[] { sHdmi.irqs = {205, 206, 207, 208, 394}; },
		[] { sHdmi.irqController = &sClock; },
		[] { sHdmi.properties["interrupts"][7] = 1; },
		[] { Cells(sHdmi, "interrupts", {0, 169, 4, 0, 0, 170, 4, 0, 0, 171, 4, 0, 0, 172, 4, 0,
			0, 360, 4, 0}); },
		[] { Strings(sHdmi, "reset-names", {"hdp", "ref"}); },
		[] { Cells(sHdmi, "resets", {0x22, 0x1d0, 0x21, 0x231}); },
		[] { Cells(sHdmi, "power-domains", {0x23, 26}); },
		[] { Cells(sHdmi, "power-domains", {0x22, 25}); },
		[] { Cells(sHdmi, "rockchip,grf", {0x6f}); },
		[] { Cells(sHdmi, "rockchip,vo-grf", {0x6e}); },
		[] { Cells(sHdmi, "phys", {0x6b}); },
		[] { sHdmi.properties.erase("phys"); },
		[] { Strings(sPhy1, "status", {"disabled"}); },
		[] { Strings(sPhy1, "compatible", {"rockchip,rk3588-usbdp-phy"}); },
		[] { sPhy1.base = 0xfed60000; },
		[] { Strings(sPhy1, "clock-names", {"apb", "ref"}); },
		[] { sPhy1.properties.erase("rockchip,grf"); },
		[] { Cells(sPhy1, "rockchip,grf", {0x6e}); },
		[] { sHdptxGrf.size = 0x1000; },
		[] { Strings(sPhy0, "compatible", {"rockchip,rk3588-usbdp-phy"}); },
		[] { Cells(sGic, "#interrupt-cells", {3}); },
		[] { Strings(sGic, "compatible", {"arm,gic-v2"}); },
		[] { sGic.size = 0x20000; },
	};
	for (auto& fault : faults) {
		Prepare(); fault();
		ResourceInfo invalid;
		memset(&invalid, 0xa5, sizeof(invalid));
		assert(!ReadResources(&sVop, invalid));
		for (unsigned char byte : std::vector<unsigned char>((unsigned char*)&invalid,
				(unsigned char*)&invalid + sizeof(invalid))) assert(byte == 0xa5);
	}
	Prepare(); // Also asserts that node references were released on every failure.

	Controller controller{};
	controller.resources = good;
	Handle handle{&controller, false};
	ResourceInfo copy;
	assert(Control(&handle, kGetResources, &copy, sizeof(copy)) == B_OK);
	assert(memcmp(&copy, &good, sizeof(good)) == 0);
	assert(Control(&handle, kGetResources, NULL, sizeof(copy)) == B_BAD_ADDRESS);
	assert(Control(&handle, kGetResources, &copy, sizeof(copy) - 1) == B_BAD_VALUE);
	assert(Control(&handle, kGetResources, &copy, sizeof(copy) + 1) == B_BAD_VALUE);
	assert(Control(&handle, kGetResources + 127, &copy, sizeof(copy)) == B_DEV_INVALID_IOCTL);
	assert(sMapAttempts == 0);

	DisplaySnapshot snapshot;
	memset(&snapshot, 0xa5, sizeof(snapshot));
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot) - 1) == B_BAD_VALUE);
	assert(Control(&handle, kGetSnapshot, NULL, sizeof(snapshot)) == B_BAD_ADDRESS);
	assert(sMapAttempts == 0);
	// Every block powered and clocked: nine control pages, VOP2, HDMI TX1,
	// GPIO3 and DP TX1 - thirteen read-only mappings, all released.
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 13 && sAreas.empty());
	assert(sMappedBases[9] == 0xfdd90000 && sMappedBases[10] == 0xfdea0000);
	assert(sMappedBases[11] == 0xfec40000 && sMappedBases.back() == 0xfde60000);
	CheckSnapshotValues(snapshot, true, true);
	assert(snapshot.dp[0] == 0x14110600 && snapshot.dpAux[2] == 0 && snapshot.ioc[1] == 0x50);
	// The hot-plug pin high and the AUX clock gated: the pin shows, the AUX words do not.
	Prepare(); sGpioPort = 1u << 29; sGate56 = 1u << 3;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 13 && sAreas.empty());
	CheckSnapshotValues(snapshot, true, true);
	assert(snapshot.gpio[4] == (1u << 29) && (snapshot.flags & kSnapshotDpAuxRead) == 0 && snapshot.dpAux[0] == 0);
	// DP APB clock gated, or the GPIO3 clock gated: those blocks are never mapped.
	Prepare(); sGate56 = 1u << 5;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 12 && sAreas.empty() && sMappedBases.back() == 0xfec40000);
	CheckSnapshotValues(snapshot, true, true);
	Prepare(); sGate17 = 1u << 2;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 12 && sAreas.empty() && sMappedBases.back() == 0xfde60000);
	CheckSnapshotValues(snapshot, true, true);
	// VOP power domain off: VOP2 is never mapped; HDMI still observed.
	Prepare(); sRepairStatus = (1u << 17) | (1u << 18);
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 12 && sAreas.empty());
	CheckSnapshotValues(snapshot, false, true);
	// VOP bus clock gated: same skip.
	Prepare(); sGate52 = 1u << 8;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 12 && sAreas.empty());
	CheckSnapshotValues(snapshot, false, true);
	// VO1 off or HDMI APB clock gated: HDMI TX1 is never mapped (VO0 off skips DP too).
	Prepare(); sRepairStatus = 1u << 16;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 11 && sAreas.empty());
	CheckSnapshotValues(snapshot, true, false);
	Prepare(); sGate61 = 1u << 2;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 12 && sAreas.empty());
	CheckSnapshotValues(snapshot, true, false);
	Prepare(); sRepairStatus = 0;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 10 && sAreas.empty());
	CheckSnapshotValues(snapshot, false, false);
	// Every mapping failure is reported and leaves nothing mapped.
	for (unsigned failing = 1; failing <= 13; failing++) {
		Prepare(); sFailMap = failing;
		memset(&snapshot, 0xa5, sizeof(snapshot));
		assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_NO_MEMORY);
		assert(sMapAttempts == failing && sAreas.empty());
		for (unsigned char byte : std::vector<unsigned char>((unsigned char*)&snapshot,
				(unsigned char*)&snapshot + sizeof(snapshot))) assert(byte == 0xa5);
	}
	// An altered description is refused before any mapping.
	Prepare();
	controller.resources.vopBase += 0x1000;
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_NOT_SUPPORTED);
	assert(sMapAttempts == 0);
	controller.resources = good;
	controller.resources.boardCompatible[16] = 'x';
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_NOT_SUPPORTED);
	assert(sMapAttempts == 0);
	assert(sLockDepth == 0);

	// EDID: request validation, opt-in gating, power/hot-plug gating, data and cleanup.
	Prepare();
	controller.resources = good;
	controller.edidEnabled = false;
	EdidRequest edid = {};
	edid.version = kEdidVersion;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid) - 1) == B_BAD_VALUE);
	assert(Control(&handle, kReadEdid, NULL, sizeof(edid)) == B_BAD_ADDRESS);
	edid.version = 2;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_BAD_VALUE);
	edid.version = kEdidVersion;
	edid.block = kEdidMaxBlocks;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_BAD_VALUE);
	edid.block = 0;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_NOT_ALLOWED);
	assert(sMapAttempts == 0);
	controller.edidEnabled = true;
	sAllowEdid = true;
	sRepairStatus = 1u << 16;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNotReady && sMapAttempts == 3 && sAreas.empty() && edid.bytesRead == 0);
	Prepare(); sAllowEdid = true; sGate61 = 1u << 2;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNotReady && sMapAttempts == 3 && sAreas.empty());
	Prepare(); sAllowEdid = true; sHotPlug = 1u << 27;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNoHotPlug && sMapAttempts == 3 && sAreas.empty() && edid.hotPlug == (1u << 27));
	for (unsigned block = 0; block < 4; block++) {
		Prepare(); sAllowEdid = true;
		memset(&edid, 0xa5, sizeof(edid));
		edid.version = kEdidVersion;
		edid.block = block;
		assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_OK);
		assert(edid.result == kEdidOK && edid.bytesRead == 128 && edid.block == block);
		assert(memcmp(edid.data, sEdid + block * 128, 128) == 0);
		assert(sMapAttempts == 4 && sAreas.empty() && sServed == 128 && sResets == 0);
		assert(edid.flags == (block >= 2 ? kEdidSegmentUsed : 0));
		assert((edid.controlAfter & kI2cmWriteMask) == 0 && (edid.statusAfter & 0x5) == 0);
		assert(edid.finishedMicros > edid.startedMicros && edid.polls == 0);
		assert(edid.hotPlug == ((1u << 24) | (1u << 27)));
	}
	// A NACK part-way through aborts the transfer, resets the master and clears requests.
	Prepare(); sAllowEdid = true; sNackAt = 17;
	memset(&edid, 0, sizeof(edid));
	edid.version = kEdidVersion;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNack && edid.bytesRead == 17 && sResets == 1 && sAreas.empty());
	assert((edid.flags & kEdidMasterReset) != 0 && (edid.controlAfter & kI2cmWriteMask) == 0);
	assert((edid.statusAfter & 0x5) == 0 && memcmp(edid.data, sEdid, 17) == 0 && edid.data[17] == 0);
	// A silent bus times out after the bounded poll count and leaves the master idle.
	Prepare(); sAllowEdid = true; sUnresponsive = true;
	memset(&edid, 0, sizeof(edid));
	edid.version = kEdidVersion;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidTimeout && edid.bytesRead == 0 && edid.polls == kEdidPollLimit);
	assert(sResets == 1 && sAreas.empty() && (edid.controlAfter & kI2cmWriteMask) == 0);
	// A failed HDMI mapping is reported and nothing stays mapped.
	Prepare(); sAllowEdid = true; sFailMap = 4;
	assert(Control(&handle, kReadEdid, &edid, sizeof(edid)) == B_NO_MEMORY);
	assert(sMapAttempts == 4 && sAreas.empty());
	assert(sLockDepth == 0);

	// Scanout swap: request validation, opt-in gating, power gating, window
	// location, the two permitted writes, restore, restore-on-close and failures.
	Prepare();
	controller.resources = good;
	controller.scanoutEnabled = false;
	ScanoutRequest scan = {};
	scan.version = kScanoutVersion;
	assert(Control(&handle, kSwapScanout, &scan, sizeof(scan) - 1) == B_BAD_VALUE);
	assert(Control(&handle, kSwapScanout, NULL, sizeof(scan)) == B_BAD_ADDRESS);
	scan.version = 2;
	assert(Control(&handle, kSwapScanout, &scan, sizeof(scan)) == B_BAD_VALUE);
	scan.version = kScanoutVersion;
	scan.action = kScanoutRestore + 1;
	assert(Control(&handle, kSwapScanout, &scan, sizeof(scan)) == B_BAD_VALUE);
	scan.action = kScanoutQuery;
	assert(Control(&handle, kSwapScanout, &scan, sizeof(scan)) == B_NOT_ALLOWED);
	assert(sMapAttempts == 0);
	controller.scanoutEnabled = true;
	auto scanout = [&](uint32 action, uint32 expected, bool writable) {
		sAllowScanout = writable;
		memset(&scan, 0xa5, sizeof(scan));
		scan.version = kScanoutVersion;
		scan.action = action;
		assert(Control(&handle, kSwapScanout, &scan, sizeof(scan)) == B_OK);
		assert(scan.result == expected && scan.action == action && scan.version == kScanoutVersion);
		assert(scan.finishedMicros > scan.startedMicros && sAreas.size() == (sOwner != NULL ? 1u : 0u));
		sAllowScanout = false;
	};
	// A query never maps VOP2 writable and needs the VOP domain and bus clocks.
	sRepairStatus = 1u << 18;
	scanout(kScanoutQuery, kScanoutNotReady, false);
	assert(sMapAttempts == 2 && scan.port == 0 && scan.addressBefore == 0);
	Prepare(); sGate52 = 1u << 9;
	scanout(kScanoutQuery, kScanoutNotReady, false);
	assert(sMapAttempts == 2);
	Prepare();
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.port == 2 && scan.window == 2 && scan.flags == 0 && sMapAttempts == 3);
	assert(scan.addressBefore == kModelFirmwareAddress && scan.addressAfter == 0);
	assert(scan.firmwareAddress == 0 && scan.patternAddress == 0 && scan.configDone == 0);
	assert(scan.regionControl == 1 && scan.virtualWidth == 1920 && scan.activeInfo == 0x0437077f);
	assert(scan.displayInfo == 0x0437077f && scan.displayStart == 0 && scan.interfaceEnable == 0x00080020);
	assert(sVopWrites.empty());
	// Window location rejects every deviation from the qualified firmware state.
	Prepare(); sVopOverrides[0x028] = 0x00080000; scanout(kScanoutQuery, kScanoutNoWindow, false); // HDMI1 off
	Prepare(); sVopOverrides[0x028] = 0x00040020; scanout(kScanoutQuery, kScanoutNoWindow, false); // port 1 (standby)
	Prepare(); sVopOverrides[0xe00] = 0x8000000f; scanout(kScanoutQuery, kScanoutNoWindow, false); // port 2 standby
	Prepare(); sVopOverrides[0x1810] = 1; scanout(kScanoutQuery, kScanoutNoWindow, false); // two windows
	Prepare(); sVopOverrides[0x1c10] = 0; scanout(kScanoutQuery, kScanoutNoWindow, false); // no window
	Prepare(); sVopOverrides[0x1c10] = 1 | (2 << 1); scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c1c] = 1921; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c20] = 0x0437077e; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c24] = 0x0433077f; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c28] = 0x00010000; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	assert(sVopWrites.empty() && sPatternArea < 0);
	// Showing the pattern requires the firmware framebuffer to match the boot item.
	Prepare(); sBootInfoPresent = false; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && scan.flags == 0 && sMapAttempts == 3);
	Prepare(); sBootInfo.physical_frame_buffer = 0xed281000; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	Prepare(); sBootInfo.width = 1280; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	Prepare(); sBootInfo.height = 1024; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	Prepare(); sBootInfo.bytes_per_row = 7684; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && sNoncacheableCalls == 0);
	Prepare(); sFailPattern = 1; scanout(kScanoutShowPattern, kScanoutNoBuffer, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && sNoncacheableCalls == 0 && scan.flags == 0);
	Prepare(); sPatternHighPhysical = true; scanout(kScanoutShowPattern, kScanoutNoBuffer, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && sNoncacheableCalls == 0 && sPatternAllocations == 1);
	Prepare(); sFailMap = 3; sAllowScanout = true;
	memset(&scan, 0, sizeof(scan));
	scan.version = kScanoutVersion;
	scan.action = kScanoutShowPattern;
	assert(Control(&handle, kSwapScanout, &scan, sizeof(scan)) == B_NO_MEMORY);
	assert(sMapAttempts == 3 && sAreas.empty() && sVopWrites.empty() && sPatternArea < 0);
	// The swap itself: exactly two writes, in order, with a verified read-back.
	Prepare();
	scanout(kScanoutShowPattern, kScanoutOK, true);
	assert(scan.flags == kScanoutSwapped && scan.port == 2 && scan.window == 2 && sMapAttempts == 3);
	assert(scan.addressBefore == kModelFirmwareAddress && scan.addressAfter == kModelPatternPhysical);
	assert(scan.firmwareAddress == kModelFirmwareAddress && scan.patternAddress == kModelPatternPhysical);
	assert(scan.configDone == (0x8000u | (1u << 2) | (1u << 18)) && scan.polls == 2);
	assert(sVopWrites.size() == 2);
	assert(sVopWrites[0] == std::make_pair(kModelAddressOffset, (uint32)kModelPatternPhysical));
	assert(sVopWrites[1] == std::make_pair(0x000u, 0x00048004u));
	assert(sNoncacheableCalls == 1 && sPatternArea >= 0 && sPatternAllocations == 1);
	assert(sModelSpins == 2); // one pause per poll that saw the port bit set
	// Observation reports the pattern address through its own read-only mapping.
	sVopWrites.clear();
	assert(Control(&handle, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	CheckSnapshotValues(snapshot, true, true);
	assert(snapshot.vopEsmart[2][2] == kModelPatternPhysical && sVopWrites.empty());
	// Showing again is idempotent; a foreign window address is refused.
	scanout(kScanoutShowPattern, kScanoutOK, true);
	assert(scan.flags == kScanoutSwapped && scan.addressBefore == kModelPatternPhysical && sVopWrites.empty());
	sVopOverrides[kModelAddressOffset] = 0x11111000;
	scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(scan.flags == kScanoutSwapped && sVopWrites.empty());
	sVopOverrides[kModelAddressOffset] = kModelPatternPhysical;
	// Restore writes the firmware address back and keeps the buffer for reuse.
	scanout(kScanoutRestore, kScanoutOK, true);
	assert(scan.flags == 0 && scan.addressBefore == kModelPatternPhysical && scan.addressAfter == kModelFirmwareAddress);
	assert(scan.firmwareAddress == kModelFirmwareAddress && scan.patternAddress == kModelPatternPhysical);
	assert(scan.polls == 2 && sModelSpins == 4);
	assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	assert(sVopWrites[1] == std::make_pair(0x000u, 0x00048004u) && sPatternArea >= 0 && sNoncacheableCalls == 1);
	sVopWrites.clear();
	scanout(kScanoutRestore, kScanoutNotSwapped, true);
	assert(sVopWrites.empty() && scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress && scan.polls == 0);
	// Close restores a pending swap and otherwise touches nothing.
	unsigned attempts = sMapAttempts;
	assert(Close(&handle) == B_OK && sMapAttempts == attempts && sVopWrites.empty());
	scanout(kScanoutShowPattern, kScanoutOK, true);
	assert(sNoncacheableCalls == 1 && sPatternAllocations == 1); // buffer reused, not refilled
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&handle) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	assert(sAreas.empty());
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	// A swap that cannot be read back stays pending and is retried on close.
	sVopWrites.clear();
	sStickyAddress = true;
	scanout(kScanoutShowPattern, kScanoutVerifyFailed, true);
	assert(scan.flags == kScanoutSwapped && scan.addressAfter == kModelFirmwareAddress && sVopWrites.size() == 2);
	assert(scan.polls == 2);
	sStickyAddress = false;
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&handle) == B_OK);
	sAllowScanout = false;
	assert(!sVopWrites.empty() && sVopWrites.back() == std::make_pair(0x000u, 0x00048004u));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	// A port that never takes the commit times out after the bounded polls;
	// the swap stays pending and restore succeeds once the port runs again.
	sVopWrites.clear();
	sCommitNeverCompletes = true;
	unsigned spins = sModelSpins;
	scanout(kScanoutShowPattern, kScanoutTimeout, true);
	assert(scan.flags == kScanoutSwapped && scan.polls == kScanoutPollLimit && sModelSpins == spins + kScanoutPollLimit);
	assert(scan.addressAfter == kModelFirmwareAddress && sVopWrites.size() == 2);
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&handle) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 1 && sVopWrites[0] == std::make_pair(0x000u, 0x00048004u));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == kScanoutSwapped);
	sCommitNeverCompletes = false;
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&handle) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 1 && sVopWrites[0] == std::make_pair(0x000u, 0x00048004u));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	// With the VOP domain off a pending swap waits for power to return.
	scanout(kScanoutShowPattern, kScanoutOK, true);
	sVopWrites.clear();
	sRepairStatus = 1u << 18;
	sAllowScanout = true;
	assert(Close(&handle) == B_OK);
	assert(sVopWrites.empty() && sAreas.empty());
	sRepairStatus = (1u << 16) | (1u << 18);
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == kScanoutSwapped && scan.addressBefore == kModelPatternPhysical);
	sAllowScanout = true;
	assert(Close(&handle) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	ReleaseContiguous(sPattern);
	assert(sPatternArea < 0 && sPatternAllocation == NULL);
	assert(sLockDepth == 0);

	// Accelerant profile: handles, signature and device name, acquiring and
	// releasing the frame buffer, clones, refusals and failure cleanup.
	static_assert(sizeof(AccelerantInfo) == 80, "Accelerant ABI layout changed");
	static_assert(sizeof(RetraceRearm) == 32, "Re-arm ABI layout changed");
	static_assert(sizeof(SharedInfo) == 244, "Shared info ABI layout changed");
	Prepare();
	controller.resources = good;
	controller.accelerantEnabled = false;
	void* opened = NULL;
	assert(Open(&controller, "", O_RDWR, &opened) == B_NOT_ALLOWED && opened == NULL);
	assert(Open(&controller, "", O_WRONLY, &opened) == B_NOT_ALLOWED);
	assert(Open(&controller, "", O_RDONLY, &opened) == B_OK);
	Handle* reader = (Handle*)opened;
	assert(!reader->writable && reader->controller == &controller);
	char signature[64];
	assert(Control(reader, B_GET_ACCELERANT_SIGNATURE, signature, sizeof(signature)) == B_DEV_INVALID_IOCTL);
	AccelerantInfo acc = {};
	acc.version = kAccelerantVersion;
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_DEV_INVALID_IOCTL);
	assert(Control(reader, kGetDeviceName, signature, sizeof(signature)) == B_DEV_INVALID_IOCTL);
	area_info cloneInfo = {};
	assert(Control(reader, kCloneFrameBuffer, &cloneInfo, sizeof(cloneInfo)) == B_DEV_INVALID_IOCTL);
	assert(Control(reader, kAcquireFrameBuffer, NULL, 0) == B_NOT_ALLOWED);
	assert(Close(reader) == B_OK && Free(reader) == B_OK && sMapAttempts == 0);
	controller.accelerantEnabled = true;
	assert(Open(&controller, "", O_RDONLY, &opened) == B_OK);
	reader = (Handle*)opened;
	assert(Control(reader, kAcquireFrameBuffer, NULL, 0) == B_NOT_ALLOWED); // read-only handle
	assert(Control(reader, B_GET_ACCELERANT_SIGNATURE, NULL, sizeof(signature)) == B_BAD_ADDRESS);
	assert(Control(reader, B_GET_ACCELERANT_SIGNATURE, signature, 8) == B_BAD_VALUE);
	assert(Control(reader, B_GET_ACCELERANT_SIGNATURE, signature, sizeof(signature)) == B_OK);
	assert(strcmp(signature, "rk3588_display.accelerant") == 0);
	assert(Control(reader, kGetDeviceName, signature, 8) == B_BAD_VALUE);
	assert(Control(reader, kGetDeviceName, signature, sizeof(signature)) == B_OK);
	assert(strcmp(signature, "graphics/rk3588_display/0") == 0);
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc) - 1) == B_BAD_VALUE);
	assert(Control(reader, kGetAccelerantInfo, NULL, sizeof(acc)) == B_BAD_ADDRESS);
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_NO_INIT);
	assert(Control(reader, kCloneFrameBuffer, &cloneInfo, sizeof(cloneInfo)) == B_NO_INIT);
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	Handle* primary = (Handle*)opened;
	assert(primary->writable && sMapAttempts == 0);
	// Refusals before anything is mapped or allocated.
	sBootInfoPresent = false;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sMapAttempts == 0);
	Prepare(); sBootInfo.bytes_per_row = 7684;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sMapAttempts == 0);
	Prepare(); sBootInfo.depth = 24;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sMapAttempts == 0);
	// The VOP domain must be on; the window must still scan the firmware buffer.
	Prepare(); sAllowEdid = true; sAllowScanout = true; sRepairStatus = 1u << 18;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_BUSY && sAreas.empty() && sFrameArea < 0);
	Prepare(); sAllowEdid = true; sAllowScanout = true; sVopOverrides[kModelAddressOffset] = 0x11111000;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sAreas.empty() && sFrameArea < 0);
	Prepare(); sAllowEdid = true; sAllowScanout = true; sVopOverrides[0x1c1c] = 1921;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sAreas.empty());
	Prepare(); sAllowEdid = true; sAllowScanout = true; sVopOverrides[0xe4c] = 0x00c00841; // 1921 active
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sAreas.empty());
	Prepare(); sAllowEdid = true; sAllowScanout = true; sVopOverrides[0xe48] = 0x0840002c; // total < active end
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NOT_SUPPORTED && sAreas.empty());
	assert(sVopWrites.empty() && sConsoleUpdates == 0 && sSharedPage == NULL);
	// Allocation failure leaves nothing behind and nothing written.
	Prepare(); sAllowEdid = true; sAllowScanout = true; sFailPattern = 1;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_NO_MEMORY);
	assert(sAreas.empty() && sFrameArea < 0 && sSharedPage == NULL && sVopWrites.empty() && sOwner == NULL);
	// A swap that never becomes active is undone and reported.
	Prepare(); sAllowEdid = true; sAllowScanout = true; sStickyAddress = true;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_ERROR);
	assert(sAreas.empty() && sFrameArea < 0 && sSharedPage == NULL && sOwner == NULL && sConsoleUpdates == 0);
	assert(sVopWrites.size() == 3 && sVopWrites[0] == std::make_pair(kModelAddressOffset, (uint32)kModelFramePhysical));
	assert(sVopWrites[1] == std::make_pair(0x000u, 0x00048004u) && sVopWrites[2] == std::make_pair(0x000u, 0x00048004u));
	// The real thing: EDID captured, timing decoded, buffer black, two writes, console moved.
	Prepare(); sAllowEdid = true; sAllowScanout = true;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK);
	assert(sOwner == primary && sMapAttempts == 8 && sAreas.size() == 1 && sVopModel != NULL);
	assert(sFrameArea >= 0 && sFrameNoncacheable == 1 && sSharedPage != NULL && sServed == 128);
	// The swap's two writes, then the port's interrupt clear and enable.
	assert(sVopWrites.size() == 4 && sVopWrites[0] == std::make_pair(kModelAddressOffset, (uint32)kModelFramePhysical));
	assert(sVopWrites[1] == std::make_pair(0x000u, 0x00048004u));
	assert(sVopWrites[2] == std::make_pair(0xc4u, 0xffffffffu) && sVopWrites[3] == std::make_pair(0xc0u, 0x00200020u));
	assert(sVopOverrides[0xc0] == 0x20 && sHandler != NULL && sHandlerInterrupt == 188 && sModelSemaphore >= 0);
	assert(sSemaphoreOwner == kModelTeam); // owned by app_server's team, never by the kernel
	assert(sConsoleUpdates == 1 && sConsole.address == (addr_t)sFrameAllocation && sConsole.width == 1920);
	assert(sConsole.height == 1080 && sConsole.depth == 32 && sConsole.bytesPerRow == 7680);
	const SharedInfo* shared = (const SharedInfo*)sSharedPage;
	assert(shared->version == kAccelerantVersion && shared->flags == kAccelerantEdid && shared->modeListArea == -1);
	assert(shared->width == 1920 && shared->height == 1080 && shared->bytesPerRow == 7680);
	assert(shared->hTotal == 2200 && shared->vTotal == 1125 && shared->pixelClockKHz == 148500);
	assert(shared->hSyncStart == 2008 && shared->hSyncEnd == 2052 && shared->vSyncStart == 1084 && shared->vSyncEnd == 1089);
	assert(shared->portTiming[0] == 0x0898002c && shared->portTiming[3] == 0x00290461);
	assert(shared->edidResult == kEdidOK && memcmp(shared->edid, sEdid, 128) == 0);
	assert(strcmp(shared->name, "RK3588 VOP2 HDMI TX1") == 0);
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_BUSY);
	memset(&acc, 0, sizeof(acc));
	acc.version = kAccelerantVersion;
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert(acc.flags == (kAccelerantAcquired | kAccelerantEdid | kAccelerantRetrace) && acc.sharedArea == sSharedModelArea);
	assert(acc.frameBufferPhysical == kModelFramePhysical && acc.firmwareAddress == kModelFirmwareAddress);
	assert(acc.port == 2 && acc.window == 2 && acc.polls == 2 && acc.width == 1920 && acc.height == 1080);
	assert(acc.bytesPerRow == 7680 && acc.retraces == 0 && acc.interruptCalls == 0);
	assert(acc.interruptSpurious == 0 && acc.firstRetraceMicros == 0 && acc.lastRetraceMicros == 0);
	assert((acc.flags & kAccelerantRetrace) != 0 && acc.retraceSemaphore == sModelSemaphore);
	// Frame-start interrupts: ignored while idle, acknowledged, and released
	// only towards waiting threads. Other status bits are acknowledged too.
	sVopWrites.clear();
	assert(sHandler(sHandlerData) == B_UNHANDLED_INTERRUPT && sVopWrites.empty());
	sVopModel[0xc8 / 4] = sVopShadow[0xc8 / 4] = 0x20;
	assert(sHandler(sHandlerData) == B_HANDLED_INTERRUPT);
	assert(sVopWrites.size() == 1 && sVopWrites[0] == std::make_pair(0xc4u, 0x00200020u) && sVopModel[0xc8 / 4] == 0);
	assert(sSemaphoreReleases == 0 && atomic_get(&sRetraces) == 1);
	sVopModel[0xc8 / 4] = sVopShadow[0xc8 / 4] = 0x21;
	sSemaphoreWaiters = -2;
	assert(sHandler(sHandlerData) == B_INVOKE_SCHEDULER);
	assert(sSemaphoreReleases == 1 && sSemaphoreReleased == 2 && sSemaphoreWaiters == 0 && atomic_get(&sRetraces) == 2);
	assert(sVopWrites.size() == 2 && sVopWrites[1] == std::make_pair(0xc4u, 0x00210021u) && sVopModel[0xc8 / 4] == 0);
	sVopModel[0xc8 / 4] = sVopShadow[0xc8 / 4] = 0x10; // not a frame start
	assert(sHandler(sHandlerData) == B_HANDLED_INTERRUPT && atomic_get(&sRetraces) == 2);
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK && acc.retraces == 2);
	assert(acc.interruptCalls == 4 && acc.interruptSpurious == 1);
	assert(acc.firstRetraceMicros > 0 && acc.lastRetraceMicros > acc.firstRetraceMicros);
	// Diagnostic re-arm: disable, reinstall the handler, clear and enable again.
	RetraceRearm rearm = {};
	rearm.version = kAccelerantVersion;
	assert(Control(reader, kRearmRetrace, &rearm, sizeof(rearm)) == B_NOT_ALLOWED);
	assert(Control(primary, kRearmRetrace, &rearm, sizeof(rearm) - 1) == B_BAD_VALUE);
	sVopWrites.clear();
	assert(Control(primary, kRearmRetrace, &rearm, sizeof(rearm)) == B_OK);
	assert(rearm.enableBefore == 0x20 && rearm.statusBefore == 0 && rearm.enableAfter == 0x20);
	assert(rearm.statusAfter == 0 && rearm.reinstall == B_OK && rearm.retraces == 2);
	assert(sVopWrites.size() == 3 && sVopWrites[0] == std::make_pair(0xc0u, 0x00200000u));
	assert(sVopWrites[1] == std::make_pair(0xc4u, 0xffffffffu) && sVopWrites[2] == std::make_pair(0xc0u, 0x00200020u));
	assert(sHandlerRemovals == 1 && sHandlerInstalls == 2 && sHandler != NULL);
	sVopWrites.clear();
	acc.version = 2;
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_BAD_VALUE);
	assert(Control(reader, kCloneFrameBuffer, &cloneInfo, sizeof(cloneInfo) - 1) == B_BAD_VALUE);
	assert(Control(reader, kCloneFrameBuffer, NULL, sizeof(cloneInfo)) == B_BAD_ADDRESS);
	assert(Control(reader, kCloneFrameBuffer, &cloneInfo, sizeof(cloneInfo)) == B_OK);
	assert(sClones == 1 && cloneInfo.area == 1201 && cloneInfo.size == kFrameBytes && cloneInfo.address == sFrameAllocation);
	assert(Control(primary, kCloneFrameBuffer, &cloneInfo, sizeof(cloneInfo)) == B_OK && sClones == 2);
	// Observation sees the new address; a pattern swap is refused while acquired.
	sVopWrites.clear();
	assert(Control(reader, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	CheckSnapshotValues(snapshot, true, true);
	assert(snapshot.vopEsmart[2][2] == kModelFramePhysical && sVopWrites.empty());
	scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(sVopWrites.empty() && sPatternArea < 0);
	// Closing another handle changes nothing; closing the owner restores everything.
	assert(Close(reader) == B_OK && sOwner == primary && sVopWrites.empty() && sConsoleUpdates == 1);
	assert(Free(reader) == B_OK);
	sAllowScanout = true;
	assert(Close(primary) == B_OK);
	sAllowScanout = false;
	// Release: interrupt off and handler gone first, then the swap back.
	assert(sOwner == NULL && sVopWrites.size() == 3 && sVopWrites[0] == std::make_pair(0xc0u, 0x00200000u));
	assert(sVopWrites[1] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	assert(sVopWrites[2] == std::make_pair(0x000u, 0x00048004u) && sVopOverrides[0xc0] == 0);
	assert(sHandler == NULL && sHandlerRemovals == 2 && sModelSemaphore < 0 && sSemaphoreDeletions == 1);
	assert(sConsoleUpdates == 2 && sConsole.address == 0xffff000012340000ull && sConsole.bytesPerRow == 7680);
	assert(sNullClones == 1 && sFrameArea < 0 && sSharedPage == NULL && sAreas.empty() && sVopModel == NULL);
	assert(Free(primary) == B_OK);
	assert(sVopOverrides[kModelAddressOffset] == kModelFirmwareAddress);
	// Without EDID (no hot-plug) the frame buffer is still acquired.
	Prepare(); sAllowEdid = true; sAllowScanout = true; sHotPlug = 1u << 27;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sMapAttempts == 7);
	shared = (const SharedInfo*)sSharedPage;
	assert(shared->flags == 0 && shared->edidResult == kEdidNoHotPlug && shared->hTotal == 2200);
	acc.version = kAccelerantVersion;
	assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert(acc.flags == (kAccelerantAcquired | kAccelerantRetrace));
	sAllowScanout = true;
	assert(Close(primary) == B_OK && Free(primary) == B_OK && sOwner == NULL && sNullClones == 1);
	sAllowScanout = false;
	// Without an interrupt handler or semaphore the frame buffer still works,
	// just without retrace; the release then skips the interrupt words.
	for (unsigned failure = 0; failure < 2; failure++) {
		Prepare(); sAllowEdid = true; sAllowScanout = true;
		if (failure == 0)
			sFailInstall = 1;
		else
			sFailSemaphore = 1;
		assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
		primary = (Handle*)opened;
		assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sOwner == primary);
		assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
		assert(acc.flags == (kAccelerantAcquired | kAccelerantEdid) && acc.retraceSemaphore == -1);
		assert(sHandler == NULL && sModelSemaphore < 0 && sVopWrites.size() == 2);
		sVopWrites.clear();
		sAllowScanout = true;
		assert(Close(primary) == B_OK && Free(primary) == B_OK && sOwner == NULL);
		sAllowScanout = false;
		assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
		assert(sAreas.empty() && sHandlerRemovals == 0);
	}
	assert(sLockDepth == 0);

	// Native mode set: gating, refusals, the full 720p60 sequence in order,
	// each timeout, and the way back to the firmware mode.
	static_assert(sizeof(ModeRequest) == 112, "Mode ABI layout changed");
	Prepare(); sAllowEdid = true; sAllowScanout = true;
	controller.modeSetEnabled = false;
	ModeRequest mode = {};
	mode.version = kModeVersion;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_DEV_INVALID_IOCTL);
	controller.modeSetEnabled = true;
	assert(Open(&controller, "", O_RDONLY, &opened) == B_OK);
	reader = (Handle*)opened;
	assert(Control(reader, kSetDisplayMode, &mode, sizeof(mode)) == B_NOT_ALLOWED);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode) - 1) == B_BAD_VALUE);
	assert(Control(primary, kSetDisplayMode, NULL, sizeof(mode)) == B_BAD_ADDRESS);
	mode.version = kModeVersion + 1;
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_BAD_VALUE);
	mode.version = kModeVersion;
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeNotAcquired);
	assert(sMapAttempts == 0);
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sOwner == primary);
	acc.version = kAccelerantVersion;
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert(acc.flags == (kAccelerantAcquired | kAccelerantEdid | kAccelerantRetrace | kAccelerantModeSet));
	auto fill = [&](unsigned w, unsigned h, unsigned clock, unsigned hss, unsigned hse, unsigned ht,
			unsigned vss, unsigned vse, unsigned vt, unsigned vic) {
		memset(&mode, 0xa5, sizeof(mode));
		mode.version = kModeVersion;
		mode.flags = kModePositiveHSync | kModePositiveVSync;
		mode.pixelClockKHz = clock;
		mode.hDisplay = w; mode.hSyncStart = hss; mode.hSyncEnd = hse; mode.hTotal = ht;
		mode.vDisplay = h; mode.vSyncStart = vss; mode.vSyncEnd = vse; mode.vTotal = vt;
		mode.vic = vic;
		sVopWrites.clear(); sPhyWrites.clear(); sGrfWrites.clear(); sCruWrites.clear(); sHdmiWrites.clear();
	};
	auto sequenceOf = [](const std::vector<std::pair<unsigned, uint32> >& log,
			std::initializer_list<std::pair<unsigned, uint32> > expected, unsigned from = 0) {
		// The expected writes appear in this order (not necessarily adjacent).
		unsigned at = from;
		for (auto item : expected) {
			while (at < log.size() && log[at] != item)
				at++;
			if (at == log.size())
				return false;
			at++;
		}
		return true;
	};
	auto adjacent = [](const std::vector<std::pair<unsigned, uint32> >& log,
			std::initializer_list<std::pair<unsigned, uint32> > expected) {
		for (unsigned start = 0; start + expected.size() <= log.size(); start++) {
			unsigned i = 0;
			for (auto item : expected) {
				if (log[start + i] != item)
					break;
				i++;
			}
			if (i == expected.size())
				return true;
		}
		return false;
	};
	sAllowModeSet = true;
	// Rates without a PLL configuration and modes beyond the buffer are refused untouched.
	fill(1280, 720, 74251, 1390, 1430, 1650, 725, 730, 750, 4);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeUnsupported);
	assert(mode.phase == 0 && sVopWrites.empty() && sPhyWrites.empty() && sCruWrites.empty() && sGrfWrites.empty());
	fill(2560, 1440, 148500, 2608, 2640, 2720, 1443, 1448, 1481, 0);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeUnsupported);
	fill(1280, 720, 74250, 1390, 1430, 1650, 725, 730, 750, 4);
	mode.flags = 4;
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeUnsupported);
	assert(sAreas.size() == 1 && sVopWrites.empty());
	// 1280x720@60 (CEA VIC 4): stop, PHY off, PLL, port, lanes, infoframe.
	fill(1280, 720, 74250, 1390, 1430, 1650, 725, 730, 750, 4);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK);
	assert(mode.result == kModeOK && mode.phase == kPhaseInfoframes);
	assert(mode.holdPolls >= 1 && mode.holdPolls <= 4 && mode.clockPolls == 0 && mode.lockPolls == 0);
	assert(mode.phyStatus == 0x0e && mode.interfaceEnable == 0x00080020);
	assert(mode.timing[0] == 0x06720028 && mode.timing[1] == 0x01040604);
	assert(mode.timing[2] == 0x02ee0005 && mode.timing[3] == 0x001902e9);
	assert(mode.finishedMicros > mode.startedMicros && sAreas.size() == 1);
	assert(sPhyModel == NULL && sGrfModel == NULL && sCruModel == NULL && sVopModel != NULL);
	// Stop: hold-valid armed, standby, the handler acknowledged it, hold-valid disarmed.
	assert(adjacent(sVopWrites, {{0xc4u, 0x00400040u}, {0xc0u, 0x00400040u}, {0xe00u, 0x80000000u}}));
	assert(sequenceOf(sVopWrites, {{0xe00u, 0x80000000u}, {0xc4u, 0x00400040u}, {0xc0u, 0x00400000u}}));
	assert(atomic_get(&sHoldValid) == 1 && sHandler != NULL);
	// Port programming in Linux order, then commit and the port out of standby.
	assert(adjacent(sVopWrites, {{0xe48u, 0x06720028u}, {0xe4cu, 0x01040604u}, {0xe54u, 0x001902e9u},
		{0x78u, 0x02e902e9u}, {0xe50u, 0x02ee0005u}, {0xe04u, 0u}, {0x6e8u, 0x34000000u},
		{0xe30u, 0x02b30028u}, {0xe34u, 0x01040604u}, {0xe38u, 0x001902e9u}, {0xe3cu, 0x10001000u},
		{0xe40u, 0u}, {0xe2cu, 0u}, {0x1c20u, 0x02cf04ffu}, {0x1c24u, 0x02cf04ffu},
		{0x000u, 0x00048004u}, {0xe00u, 0x0000000fu}})); // DSP_ST stays 0, so that write is invisible
	assert(sVopOverrides[0xe48] == 0x06720028 && sVopOverrides[0x1c20] == 0x02cf04ff);
	// PHY: power-off writes first, per-rate ROPLL words, PCG post-divider, lanes last.
	assert(adjacent(sPhyWrites, {{0xc00u, 0x82u}, {0x43cu, 0xc1u}, {0x440u, 0x01u}, {0xc04u, 0x80u},
		{0x1004u, 0x80u}, {0x1404u, 0x80u}, {0x1804u, 0x80u}}));
	assert(sequenceOf(sPhyWrites, {{0x1804u, 0x80u}, {0x024u, 0x0cu}, {0x020u, 0x00u}, {0x144u, 0x7cu},
		{0x154u, 0x7cu}, {0x164u, 0x11u}, {0x168u, 0x70u}, {0x180u, 0x3eu}, {0x194u, 0x10u},
		{0x1b0u, 0x00u}, {0x1c0u, 0x01u}, {0x218u, 0x71u}, {0x450u, 0x00u}, {0x800u, 0x06u},
		{0x804u, 0x07u}, {0x814u, 0x1fu}, {0x818u, 0x07u}, {0x81cu, 0x0fu}, {0xc0cu, 0x0cu},
		{0x1878u, 0x0au}}));
	for (auto write : sPhyWrites)
		assert(write.first != 0x808 || write.second == 0xc1); // the 1/10 clock table, not 1/40
	// GRF: everything off, bias/bandgap, PLL, TMDS mode, bias/bandgap for the lanes.
	assert(sGrfWrites.size() == 6 && sGrfWrites[0] == std::make_pair(0u, 0x00e00000u));
	assert(sGrfWrites[1] == std::make_pair(0u, 0x00e00000u) && sGrfWrites[2] == std::make_pair(0u, 0x00600060u));
	assert(sGrfWrites[3] == std::make_pair(0u, 0x00800080u) && sGrfWrites[4] == std::make_pair(0u, 0x00010000u));
	assert(sGrfWrites[5] == std::make_pair(0u, 0x00600060u));
	// CRU: only the three PHY resets, APB pulses first, deasserts in Linux order.
	assert(sCruWrites.size() == 13);
	assert(adjacent(sCruWrites, {{0xb20u, 0x00400040u}, {0xb20u, 0x00400000u}, {0x30a10u, 0x00020002u},
		{0x30a10u, 0x00010001u}, {0x30a0cu, 0x80008000u}}));
	assert(sequenceOf(sCruWrites, {{0x30a0cu, 0x80008000u}, {0x30a0cu, 0x80000000u}, {0x30a10u, 0x00010000u},
		{0x30a10u, 0x00020000u}}));
	assert(sCruWrites.back() == std::make_pair(0x30a10u, 0x00020000u));
	// HDMI TX: AVI infoframe for VIC 4 and the AVMUTE clear; RMW words kept their values.
	assert(sequenceOf(sHdmiWrites, {{0xbe0u, 0x000d0200u}, {0xbe4u, 0x00000269u}, {0xbe8u, 4u}, {0xaacu, 2u}}));
	for (auto write : sHdmiWrites)
		assert(write.first != 0x8e0 && write.first != 0x968 && write.first != 0xa9c && write.first != 0xaa8);
	// Shared information, description and console follow the new mode.
	shared = (const SharedInfo*)sSharedPage;
	assert(shared->width == 1280 && shared->height == 720 && shared->pixelClockKHz == 74250);
	assert(shared->hSyncStart == 1390 && shared->hSyncEnd == 1430 && shared->hTotal == 1650);
	assert(shared->vSyncStart == 725 && shared->vSyncEnd == 730 && shared->vTotal == 750);
	assert(shared->portTiming[0] == 0x06720028 && shared->bytesPerRow == 7680);
	assert(shared->flags == kAccelerantEdid && shared->syncFlags == (kModePositiveHSync | kModePositiveVSync));
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK && acc.width == 1280 && acc.height == 720);
	assert(sConsole.width == 1280 && sConsole.height == 720 && sConsole.bytesPerRow == 7680);
	// A port that never reports standby: nothing beyond the stop is touched.
	sHoldNever = true;
	fill(1920, 1080, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 16);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK);
	assert(mode.result == kModeHoldTimeout && mode.phase == kPhaseStopped && mode.holdPolls == 60);
	assert(sPhyWrites.empty() && sCruWrites.empty() && sGrfWrites.empty() && sHdmiWrites.empty());
	assert(sVopWrites.size() == 4 && sVopWrites[3] == std::make_pair(0xc0u, 0x00400000u));
	assert(shared->width == 1280 && sConsole.width == 1280);
	sHoldNever = false;
	// PHY clock never ready: the PLL was programmed, the port untouched.
	sPhyStatusModel = 0;
	fill(1920, 1080, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 16);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK);
	assert(mode.result == kModePllTimeout && mode.phase == kPhasePhyOff && mode.clockPolls == 100);
	assert(sequenceOf(sPhyWrites, {{0x144u, 0x7bu}, {0x168u, 0x30u}, {0x218u, 0x31u}}));
	assert(!sequenceOf(sPhyWrites, {{0x800u, 0x06u}}) && !sequenceOf(sVopWrites, {{0xe48u, 0x0898002cu}}));
	// Lanes never lock: the port was programmed for the new mode before.
	sPhyStatusModel = kPhyStatusClockReady;
	fill(1920, 1080, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 16);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK);
	assert(mode.result == kModeLaneTimeout && mode.phase == kPhasePortProgrammed && mode.lockPolls == 50);
	assert(sequenceOf(sPhyWrites, {{0x800u, 0x06u}, {0x81cu, 0x0fu}}) && sequenceOf(sVopWrites, {{0xe48u, 0x0898002cu}}));
	assert(shared->width == 1280);
	// Back to the firmware mode.
	sPhyStatusModel = 0x0e;
	fill(1920, 1080, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 16);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeOK);
	assert(mode.timing[0] == 0x0898002c && mode.timing[3] == 0x00290461);
	assert(sequenceOf(sHdmiWrites, {{0xbe4u, 0x0000025du}, {0xbe8u, 16u}}));
	assert(shared->width == 1920 && shared->height == 1080 && sConsole.width == 1920);

	// Power control: off stops the port and powers the PHY down, a second
	// off repeats only the PHY part, on runs the current mode's mode set.
	static_assert(sizeof(PowerRequest) == 56, "Power ABI layout changed");
	PowerRequest power = {};
	power.version = kPowerVersion;
	power.mode = kPowerOff;
	assert(Control(reader, kSetPowerMode, &power, sizeof(power)) == B_NOT_ALLOWED);
	assert(Control(primary, kSetPowerMode, &power, sizeof(power) - 1) == B_BAD_VALUE);
	assert(Control(primary, kSetPowerMode, NULL, sizeof(power)) == B_BAD_ADDRESS);
	power.version = kPowerVersion + 1;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_BAD_VALUE);
	power.version = kPowerVersion;
	power.mode = 2;
	sVopWrites.clear(); sPhyWrites.clear(); sGrfWrites.clear(); sCruWrites.clear(); sHdmiWrites.clear();
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeUnsupported);
	assert(power.previous == kPowerOn && sVopWrites.empty() && sPhyWrites.empty() && shared->powerMode == kPowerOn);
	power.mode = kPowerOff;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(power.phase == kPhasePhyOff && power.previous == kPowerOn && power.holdPolls >= 1 && power.holdPolls <= 4);
	assert(power.clockPolls == 0 && power.lockPolls == 0 && (power.portControl & kVopPortStandby) != 0);
	assert(power.finishedMicros > power.startedMicros && power.phyStatus == 0x0e);
	// Hold-valid armed, standby, the handler's acknowledgement, hold-valid disarmed.
	assert(sVopWrites.size() == 5 && sVopWrites[2] == std::make_pair(0xe00u, 0x80000000u)
		&& sVopWrites.back() == std::make_pair(0xc0u, 0x00400000u));
	assert(sPhyWrites.size() == 7 && adjacent(sPhyWrites, {{0xc00u, 0x82u}, {0x43cu, 0xc1u}, {0x440u, 0x01u},
		{0xc04u, 0x80u}, {0x1004u, 0x80u}, {0x1404u, 0x80u}, {0x1804u, 0x80u}}));
	assert(sCruWrites.size() == 5 && sCruWrites.back() == std::make_pair(0x30a0cu, 0x80008000u));
	assert(sGrfWrites.size() == 1 && sGrfWrites[0] == std::make_pair(0u, 0x00e00000u));
	assert(sHdmiWrites.empty() && shared->powerMode == kPowerOff && shared->width == 1920);
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK && acc.width == 1920);
	// With the PHY down the HDMI TX registers are unreachable: the observation
	// skips that block (the GRF shows the PLL off) and EDID requests are refused.
	DisplaySnapshot offSnapshot;
	memset(&offSnapshot, 0xa5, sizeof(offSnapshot));
	assert(Control(reader, kGetSnapshot, &offSnapshot, sizeof(offSnapshot)) == B_OK);
	assert((offSnapshot.flags & kSnapshotHdmiSkipped) != 0 && (offSnapshot.flags & kSnapshotHdmiRead) == 0);
	assert((offSnapshot.flags & kSnapshotVopRead) != 0 && offSnapshot.hdptxGrf[0] == 0 && offSnapshot.hdmi[0] == 0);
	assert(sMappedBases.back() == 0xfde60000);
	EdidRequest offEdid = {};
	offEdid.version = kEdidVersion;
	assert(Control(reader, kReadEdid, &offEdid, sizeof(offEdid)) == B_OK && offEdid.result == kEdidPoweredOff);
	assert(sMappedBases.back() == 0xfde60000);
	// Off again: nothing to do, nothing touched (app_server repeats DPMS on at every start).
	sVopWrites.clear(); sPhyWrites.clear(); sGrfWrites.clear(); sCruWrites.clear();
	unsigned mapsBefore = sMapAttempts;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(power.holdPolls == 0 && power.previous == kPowerOff && power.phase == 0 && power.startedMicros == 0);
	assert(sVopWrites.empty() && sPhyWrites.empty() && sCruWrites.empty() && sGrfWrites.empty() && sMapAttempts == mapsBefore);
	// A mode set while off works from the stopped state and powers on.
	fill(1280, 720, 74250, 1390, 1430, 1650, 725, 730, 750, 4);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeOK);
	assert(mode.holdPolls == 0 && shared->powerMode == kPowerOn && shared->width == 1280);
	power.mode = kPowerOff;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(power.previous == kPowerOn && power.holdPolls >= 1 && shared->powerMode == kPowerOff);
	// On: the current 720p60 mode again, from the stopped port.
	sVopWrites.clear(); sPhyWrites.clear(); sGrfWrites.clear(); sCruWrites.clear(); sHdmiWrites.clear();
	power.mode = kPowerOn;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(power.phase == kPhaseInfoframes && power.previous == kPowerOff && power.holdPolls == 0);
	assert(power.clockPolls == 0 && power.lockPolls == 0 && power.phyStatus == 0x0e);
	assert((power.portControl & kVopPortStandby) == 0 && (power.portControl & 0xf) == 0xf);
	// The timing words still hold the 720p values, so only the restart shows in the diff model.
	assert(sequenceOf(sVopWrites, {{0xe00u, 0x0000000fu}}) && sVopOverrides[0xe48] == 0x06720028);
	assert(sequenceOf(sPhyWrites, {{0xc00u, 0x82u}, {0x144u, 0x7cu}, {0x800u, 0x06u}, {0x81cu, 0x0fu}}));
	assert(sequenceOf(sHdmiWrites, {{0xbe8u, 4u}, {0xaacu, 2u}}) && sGrfWrites.size() == 6);
	assert(shared->powerMode == kPowerOn && shared->width == 1280 && sConsole.width == 1280);
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK && acc.width == 1280);
	memset(&offSnapshot, 0xa5, sizeof(offSnapshot));
	assert(Control(reader, kGetSnapshot, &offSnapshot, sizeof(offSnapshot)) == B_OK);
	assert((offSnapshot.flags & kSnapshotHdmiRead) != 0 && offSnapshot.hdptxGrf[0] == 0xe0);
	assert(sMappedBases.back() == 0xfde60000);
	// On again: already on, nothing touched.
	sVopWrites.clear(); sPhyWrites.clear();
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(power.previous == kPowerOn && power.phase == 0 && sVopWrites.empty() && sPhyWrites.empty());
	assert(Close(reader) == B_OK && Free(reader) == B_OK);
	// Releasing a powered-off port at 720p brings the firmware mode back for
	// the firmware frame buffer before the scanout returns to it.
	power.mode = kPowerOff;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	sVopWrites.clear(); sPhyWrites.clear(); sGrfWrites.clear(); sCruWrites.clear(); sHdmiWrites.clear();
	sAllowScanout = true;
	assert(Close(primary) == B_OK && Free(primary) == B_OK && sOwner == NULL);
	assert(sequenceOf(sVopWrites, {{0xe48u, 0x0898002cu}, {0x1c20u, 0x0437077fu}, {0xe00u, 0x0000000fu},
		{kModelAddressOffset, kModelFirmwareAddress}}));
	assert(sequenceOf(sHdmiWrites, {{0xbe8u, 16u}, {0xaacu, 2u}}) && sPowerMode == kPowerOn);
	assert(sCurrentMode.version == 0 && sConsole.width == 1920);
	sAllowScanout = false;
	sAllowModeSet = false;

	// Hardware cursor: gating, refusals, the buffer, the window and mixer
	// programming through the shadowed commit, clipping at every edge, the
	// mode change, the timeouts and the release.
	static_assert(sizeof(CursorBitmap) == 32 + 16384, "Cursor bitmap ABI layout changed");
	static_assert(sizeof(CursorMove) == 32 && sizeof(CursorShow) == 32, "Cursor ABI layout changed");
	static_assert(sizeof(CursorState) == 68 + 16384, "Cursor state ABI layout changed");
	Prepare(); sAllowEdid = true; sAllowScanout = true; sAllowCursor = true; sAllowModeSet = true;
	controller.modeSetEnabled = true;
	controller.cursorEnabled = false;
	CursorShow show = {};
	show.version = kCursorVersion;
	show.visible = 1;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_DEV_INVALID_IOCTL);
	controller.cursorEnabled = true;
	assert(Open(&controller, "", O_RDONLY, &opened) == B_OK);
	reader = (Handle*)opened;
	static CursorState cursorState;
	static CursorBitmap cursorBitmap;
	CursorMove move = {};
	move.version = kCursorVersion;
	assert(Control(reader, kShowCursor, &show, sizeof(show)) == B_NOT_ALLOWED);
	assert(Control(reader, kMoveCursor, &move, sizeof(move)) == B_NOT_ALLOWED);
	assert(Control(reader, kSetCursorBitmap, &cursorBitmap, sizeof(cursorBitmap)) == B_NOT_ALLOWED);
	assert(Control(primary, kShowCursor, &show, sizeof(show) - 1) == B_BAD_VALUE);
	assert(Control(primary, kMoveCursor, &move, sizeof(move) + 1) == B_BAD_VALUE);
	assert(Control(primary, kSetCursorBitmap, &cursorBitmap, sizeof(cursorBitmap) - 4) == B_BAD_VALUE);
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState) - 1) == B_BAD_VALUE);
	assert(Control(primary, kShowCursor, NULL, sizeof(show)) == B_BAD_ADDRESS);
	show.version = kCursorVersion + 1;
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_BAD_VALUE);
	show.version = kCursorVersion;
	move.version = 0;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_BAD_VALUE);
	move.version = kCursorVersion;
	cursorBitmap.version = 7;
	assert(Control(primary, kSetCursorBitmap, &cursorBitmap, sizeof(cursorBitmap)) == B_BAD_VALUE);
	// Nothing acquired yet: every request reports that, the hardware is untouched.
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_OK && show.result == kCursorNotAcquired);
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorNotAcquired);
	memset(&cursorBitmap, 0, sizeof(cursorBitmap));
	cursorBitmap.version = kCursorVersion;
	cursorBitmap.width = cursorBitmap.height = 16;
	cursorBitmap.bytesPerRow = 64;
	assert(Control(primary, kSetCursorBitmap, &cursorBitmap, sizeof(cursorBitmap)) == B_OK);
	assert(cursorBitmap.result == kCursorNotAcquired && sMapAttempts == 0 && sCursorArea < 0);
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK && cursorState.width == 0);
	// Acquisition allocates the cursor buffer (zeroed, non-cacheable) and
	// advertises the cursor; nothing is programmed until the pointer shows.
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sOwner == primary);
	assert(sCursorArea >= 0 && sCursorNoncacheable == 1 && sCursor.physical == kModelCursorPhysical);
	acc.version = kAccelerantVersion;
	assert(Control(reader, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert(acc.flags == (kAccelerantAcquired | kAccelerantEdid | kAccelerantRetrace | kAccelerantModeSet
		| kAccelerantCursor));
	static_assert(kAccelerantCursorHooks == 32, "Cursor hooks flag changed");
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.version == kCursorVersion && cursorState.window == 3 && cursorState.mixer == 6);
	assert(cursorState.width == 0 && cursorState.visible == 0 && cursorState.data[0] == 0);
	sVopWrites.clear();
	// The bitmap: rejected sizes and strides, then a 16x16 pointer with hot spot 2,3.
	auto bitmap = [&](unsigned w, unsigned h, unsigned hx, unsigned hy, unsigned stride) {
		memset(&cursorBitmap, 0, sizeof(cursorBitmap));
		cursorBitmap.version = kCursorVersion;
		cursorBitmap.width = w; cursorBitmap.height = h;
		cursorBitmap.hotX = hx; cursorBitmap.hotY = hy;
		cursorBitmap.bytesPerRow = stride;
		for (unsigned y = 0; y < h && y * stride < kCursorBufferBytes; y++) {
			for (unsigned x = 0; x < w; x++) {
				uint32 pixel = 0x80000000u | (y << 8) | x;
				memcpy(cursorBitmap.data + y * stride + x * 4, &pixel, 4);
			}
		}
		assert(Control(primary, kSetCursorBitmap, &cursorBitmap, sizeof(cursorBitmap)) == B_OK);
		return cursorBitmap.result;
	};
	assert(bitmap(0, 16, 0, 0, 64) == kCursorUnsupported && bitmap(16, 0, 0, 0, 64) == kCursorUnsupported);
	assert(bitmap(0, 0, 1, 0, 0) == kCursorUnsupported && bitmap(0, 0, 0, 0, 0) == kCursorOK && sVopWrites.empty());
	assert(bitmap(65, 16, 0, 0, 260) == kCursorUnsupported && bitmap(16, 65, 0, 0, 64) == kCursorUnsupported);
	assert(bitmap(16, 16, 16, 0, 64) == kCursorUnsupported && bitmap(16, 16, 0, 16, 64) == kCursorUnsupported);
	assert(bitmap(16, 16, 0, 0, 60) == kCursorUnsupported && bitmap(16, 16, 0, 0, 257) == kCursorUnsupported);
	assert(sVopWrites.empty() && cursorBitmap.polls == 0);
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK && cursorState.width == 0);
	assert(bitmap(16, 16, 2, 3, 64) == kCursorOK && cursorBitmap.polls == 0 && sVopWrites.empty());
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.width == 16 && cursorState.height == 16 && cursorState.hotX == 2 && cursorState.hotY == 3);
	uint32 pixel = 0;
	memcpy(&pixel, cursorState.data + 5 * kCursorBytesPerRow + 7 * 4, 4);
	assert(pixel == (0x80000000u | (5 << 8) | 7));
	memcpy(&pixel, cursorState.data + 15 * kCursorBytesPerRow + 15 * 4, 4);
	assert(pixel == (0x80000000u | (15 << 8) | 15));
	memcpy(&pixel, cursorState.data + 5 * kCursorBytesPerRow + 16 * 4, 4);
	assert(pixel == 0 && cursorState.data[16 * kCursorBytesPerRow] == 0 && cursorState.data[kCursorBufferBytes - 1] == 0);
	// A hidden pointer only remembers its position.
	move.x = 100; move.y = 200;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.polls == 0 && move.displayStart == 0 && move.address == 0 && sVopWrites.empty());
	move.x = 40000;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorUnsupported);
	move.x = 100; move.y = -40000;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorUnsupported);
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.x == 100 && cursorState.y == 200 && cursorState.visible == 0 && sVopWrites.empty());
	// Showing programs ESMART3 (bus ids, no scaling, no colour key, 64-pixel
	// rows, the buffer, 16x16 at 98,197), mixer 6 and commits port 2; the
	// read-backs wait for the frame start.
	show.visible = 1;
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_OK && show.result == kCursorOK);
	assert(show.polls >= 1 && show.polls <= 4 && show.regionControl == 1);
	assert(sequenceOf(sVopWrites, {{0x1e04u, (0xcu << 4) | (0xdu << 12)}, {0x1e08u, 0x2u}, {0x6f8u, 0x17170000u},
		{0x008u, 0u}, {0x1ed0u, 0u}, {0x1e30u, 0u}, {0x1e34u, 0u},
		{0x6b0u, 0x00ff0125u}, {0x6b4u, 0x00ff0060u}, {0x6b8u, 0x00000024u}, {0x6bcu, 0x00000074u},
		{0x1e1cu, 64u}, {0x1e14u, (uint32)kModelCursorPhysical}, {0x1e20u, 0x000f000fu},
		{0x1e24u, 0x000f000fu}, {0x1e28u, 0x00c50062u}, {0x1e10u, 1u}, {0x000u, 0x00048004u}}));
	// Ids two above the desktop window's on its bus, mirroring cleared, the
	// desktop delay copied, gating off; the desktop window itself untouched.
	assert(sVopOverrides[0x1e04] == ((0xcu << 4) | (0xdu << 12)) && sVopOverrides[0x1e08] == 2);
	assert(sVopOverrides[0x6f8] == 0x17170000 && sVopOverrides[0x008] == 0 && sVopOverrides[0x1c04] == ((0xau << 4) | (0xbu << 12)));
	assert(sVopOverrides[0x1e10] == 1 && sVopOverrides[0x1e28] == 0x00c50062);
	assert(sVopWrites.size() == 18 && sMappedBases.back() == 0xfdd90000);
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.visible == 1 && cursorState.regionControl == 1 && cursorState.displayStart == 0x00c50062);
	assert(cursorState.address == kModelCursorPhysical && cursorState.mixWords[0] == 0x00ff0125
		&& cursorState.mixWords[3] == 0x00000074);
	// Clipped at the top left: the window shrinks and the address skips the
	// cropped rows and columns.
	sVopWrites.clear();
	move.x = -5; move.y = -7;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.polls >= 1 && move.displayStart == 0 && move.address == kModelCursorPhysical + 10 * 256 + 7 * 4);
	assert(sequenceOf(sVopWrites, {{0x1e14u, (uint32)(kModelCursorPhysical + 10 * 256 + 7 * 4)}, {0x1e20u, 0x00050008u},
		{0x1e24u, 0x00050008u}, {0x1e28u, 0u}, {0x000u, 0x00048004u}}));
	assert(sVopWrites.size() == 5); // the settled words are not rewritten
	// Clipped at the bottom right.
	sVopWrites.clear();
	move.x = 1915; move.y = 1075;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.displayStart == ((1072u << 16) | 1913u) && move.address == kModelCursorPhysical);
	assert(sequenceOf(sVopWrites, {{0x1e14u, (uint32)kModelCursorPhysical}, {0x1e20u, 0x00070006u},
		{0x1e24u, 0x00070006u}, {0x1e28u, (1072u << 16) | 1913u}, {0x000u, 0x00048004u}}));
	// Entirely off the frame: the window is disabled but the pointer stays visible.
	sVopWrites.clear();
	move.x = 3000; move.y = 3000;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.displayStart == 0 && sequenceOf(sVopWrites, {{0x1e20u, 0u}, {0x1e24u, 0u}, {0x1e28u, 0u}, {0x1e10u, 0u},
		{0x000u, 0x00048004u}}));
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.visible == 1 && cursorState.regionControl == 0 && cursorState.x == 3000);
	sVopWrites.clear();
	move.x = 500; move.y = 400;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.displayStart == ((397u << 16) | 498u) && sequenceOf(sVopWrites, {{0x1e10u, 1u}, {0x000u, 0x00048004u}}));
	// A cleared bitmap (0x0, no hot spot) disables the window while visible;
	// the next bitmap brings it back. A new bitmap while visible is programmed
	// at once; the hot spot moves the window.
	sVopWrites.clear();
	assert(bitmap(0, 0, 0, 0, 0) == kCursorOK && cursorBitmap.polls >= 1);
	assert(sequenceOf(sVopWrites, {{0x1e20u, 0u}, {0x1e10u, 0u}, {0x000u, 0x00048004u}}));
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.width == 0 && cursorState.visible == 1 && cursorState.regionControl == 0 && cursorState.data[5 * kCursorBytesPerRow + 7 * 4] == 0);
	sVopWrites.clear();
	assert(bitmap(64, 32, 63, 31, 256) == kCursorOK && cursorBitmap.polls >= 1);
	assert(sequenceOf(sVopWrites, {{0x1e20u, 0x001f003fu}, {0x1e28u, (369u << 16) | 437u}, {0x000u, 0x00048004u}}));
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	memcpy(&pixel, cursorState.data + 31 * kCursorBytesPerRow + 63 * 4, 4);
	assert(pixel == (0x80000000u | (31 << 8) | 63) && cursorState.data[32 * kCursorBytesPerRow] == 0);
	// The port never takes the commit: a timeout after the poll limit.
	sVopWrites.clear();
	sCommitNeverCompletes = true;
	move.x = 600;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorTimeout);
	assert(move.polls == kScanoutPollLimit);
	sCommitNeverCompletes = false;
	// The region control never takes: the read-back differs.
	sCursorStickyControl = true;
	show.visible = 0;
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_OK && show.result == kCursorVerifyFailed);
	assert(show.regionControl == 1);
	sCursorStickyControl = false;
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_OK && show.result == kCursorOK);
	assert(show.regionControl == 0 && sequenceOf(sVopWrites, {{0x1e10u, 0u}, {0x000u, 0x00048004u}}));
	// Hidden: moves and bitmaps touch nothing; showing brings the window back.
	sVopWrites.clear();
	move.x = 700; move.y = 300;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK && move.polls == 0);
	assert(sVopWrites.empty() && bitmap(16, 16, 0, 0, 64) == kCursorOK && cursorBitmap.polls == 0);
	assert(sVopWrites.empty() && sVopOverrides[0x1e10] == 0);
	show.visible = 1;
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_OK && show.result == kCursorOK);
	assert(show.regionControl == 1 && sequenceOf(sVopWrites, {{0x1e20u, 0x000f000fu}, {0x1e28u, (300u << 16) | 700u},
		{0x1e10u, 1u}, {0x000u, 0x00048004u}}));
	// A mode change re-clips the pointer to the new frame: 1500,300 is off a
	// 720p frame and back on after the next move.
	move.x = 1500; move.y = 300;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.displayStart == ((300u << 16) | 1500u));
	fill(1280, 720, 74250, 1390, 1430, 1650, 725, 730, 750, 4);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeOK);
	assert(sequenceOf(sVopWrites, {{0xe48u, 0x06720028u}, {0x1e10u, 0u}, {0x000u, 0x00048004u}}));
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK);
	assert(cursorState.visible == 1 && cursorState.regionControl == 0 && cursorState.displayStart == 0);
	sVopWrites.clear();
	move.x = 1275; move.y = 100;
	assert(Control(primary, kMoveCursor, &move, sizeof(move)) == B_OK && move.result == kCursorOK);
	assert(move.displayStart == ((100u << 16) | 1275u) && sequenceOf(sVopWrites, {{0x1e20u, 0x000f0004u},
		{0x1e10u, 1u}, {0x000u, 0x00048004u}}));
	fill(1920, 1080, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 16);
	assert(Control(primary, kSetDisplayMode, &mode, sizeof(mode)) == B_OK && mode.result == kModeOK);
	assert(sequenceOf(sVopWrites, {{0xe48u, 0x0898002cu}, {0x1e20u, 0x000f000fu}, {0x000u, 0x00048004u}}));
	// Power off keeps the cursor state; the window comes back with the mode.
	power.mode = kPowerOff;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(Control(reader, kGetCursor, &cursorState, sizeof(cursorState)) == B_OK && cursorState.visible == 1);
	power.mode = kPowerOn;
	assert(Control(primary, kSetPowerMode, &power, sizeof(power)) == B_OK && power.result == kModeOK);
	assert(Close(reader) == B_OK && Free(reader) == B_OK);
	// Release disables the window before the firmware frame buffer returns,
	// and frees the buffer.
	sVopWrites.clear();
	assert(Close(primary) == B_OK && Free(primary) == B_OK && sOwner == NULL);
	assert(sequenceOf(sVopWrites, {{0x1e10u, 0u}, {0x000u, 0x00048004u}, {0x008u, 0x80000000u}, {kModelAddressOffset, kModelFirmwareAddress}}));
	assert(sCursorArea < 0 && sCursor.area < 0 && !sCursorProgrammed && sVopOverrides[0x1e10] == 0);
	assert(sVopOverrides[0x008] == 0x80000000u && sVopOverrides[0x6f8] == 0x17170000);
	// The desktop cursor profile also hands app_server's pointer to the window.
	Prepare(); sAllowEdid = true; sAllowScanout = true; sAllowCursor = true;
	controller.cursorHooksEnabled = true;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sOwner == primary && sCursorArea >= 0);
	assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert((acc.flags & (kAccelerantCursor | kAccelerantCursorHooks)) == (kAccelerantCursor | kAccelerantCursorHooks));
	assert(Close(primary) == B_OK && Free(primary) == B_OK && sOwner == NULL);
	controller.cursorHooksEnabled = false;
	// Without the cursor buffer the accelerant does without a hardware cursor.
	Prepare(); sAllowEdid = true; sAllowScanout = true; sAllowCursor = true; sFailPattern = 3;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sOwner == primary && sCursorArea < 0);
	assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK && (acc.flags & kAccelerantCursor) == 0);
	assert((acc.flags & kAccelerantAcquired) != 0 && sPatternAllocations == 3);
	sVopWrites.clear();
	assert(Control(primary, kShowCursor, &show, sizeof(show)) == B_OK && show.result == kCursorNotAcquired);
	assert(bitmap(16, 16, 0, 0, 64) == kCursorNotAcquired && sVopWrites.empty());
	assert(Close(primary) == B_OK && Free(primary) == B_OK && sOwner == NULL);
	assert(!sequenceOf(sVopWrites, {{0x1e10u, 0u}}) && sequenceOf(sVopWrites, {{kModelAddressOffset, kModelFirmwareAddress}}));
	sAllowScanout = false;
	sAllowModeSet = false;
	sAllowCursor = false;
	controller.cursorEnabled = false;

	// DisplayPort probe: gating and refusals, the full path up to the AUX
	// channel with DPCD and EDID, and every way it stops early.
	static_assert(sizeof(DpProbeRequest) <= 768, "DP request must stay small on the kernel stack");
	Prepare();
	Controller dpController = controller;
	controller.dpAuxEnabled = false;
	controller.accelerantEnabled = controller.modeSetEnabled = controller.cursorEnabled = false;
	controller.cursorHooksEnabled = false;
	DpProbeRequest dp = {};
	dp.version = kDpVersion;
	assert(Open(&controller, "", O_RDWR, &opened) == B_NOT_ALLOWED);
	controller.accelerantEnabled = true;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_DEV_INVALID_IOCTL);
	assert(Close(primary) == B_OK && Free(primary) == B_OK);
	controller.accelerantEnabled = false;
	controller.dpAuxEnabled = true;
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	assert(Open(&controller, "", O_RDONLY, &opened) == B_OK);
	reader = (Handle*)opened;
	assert(Control(reader, kDpProbe, &dp, sizeof(dp)) == B_NOT_ALLOWED);
	assert(Control(primary, kDpProbe, &dp, sizeof(dp) - 1) == B_BAD_VALUE);
	assert(Control(primary, kDpProbe, NULL, sizeof(dp)) == B_BAD_ADDRESS);
	dp.version = kDpVersion + 1;
	assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_BAD_VALUE);
	dp.version = kDpVersion;
	dp.flags = 16;
	assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_BAD_VALUE);
	dp.flags = kDpProbeVideo; // video needs a trained link
	assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_BAD_VALUE);
	dp.flags = kDpProbeTrain | kDpProbeWindow; // a window needs video
	assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_BAD_VALUE);
	dp.flags = 32;
	assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_BAD_VALUE);
	assert(sMapAttempts == 0);
	auto probe = [&](uint32 flags) {
		memset(&dp, 0xa5, sizeof(dp));
		dp.version = kDpVersion;
		dp.flags = flags;
		sDpWrites.clear(); sPmaWrites.clear(); sHiwordWrites.clear(); sCruWrites.clear(); sAuxLog.clear(); sVopWrites.clear();
		assert(Control(primary, kDpProbe, &dp, sizeof(dp)) == B_OK);
		assert(sAreas.empty() && sDpModel == NULL && sPmaModel == NULL && sHiwordModels.empty());
		return dp.result;
	};
	auto logged = [](const std::vector<std::pair<unsigned, uint32> >& log, unsigned offset, uint32 value) {
		for (auto& item : log) {
			if (item.first == offset && item.second == value)
				return true;
		}
		return false;
	};
	// VO0 off or a clock gated: nothing of the path is mapped.
	sAllowDp = true; sRepairStatus = (1u << 16) | (1u << 18);
	assert(probe(kDpProbeEdid) == kDpNotReady && sMapAttempts == 2 && dp.phase == 0 && sCruWrites.empty());
	sRepairStatus = (1u << 16) | (1u << 17) | (1u << 18);
	for (uint32* gate : {&sGate56, &sGate17, &sGate2}) {
		Prepare(); sAllowDp = true; sRepairStatus = (1u << 16) | (1u << 17) | (1u << 18);
		*gate = gate == &sGate56 ? (1u << 3) : gate == &sGate17 ? (1u << 2) : (1u << 15);
		assert(probe(0) == kDpNotReady && sMapAttempts == 2);
	}
	// The full path with the pin muxed as a GPIO by the firmware.
	Prepare(); sAllowDp = true; sIocMux = 0; sGpioPort = 1u << 29;
	assert(probe(kDpProbeEdid) == kDpOK && dp.phase == kDpPhaseEdid);
	assert(sMapAttempts == 8 && sMappedBases[2] == 0xfde60000 && sMappedBases[3] == 0xfed98000);
	assert(dp.pinMuxBefore == 0 && dp.pinMuxAfter == 0x50 && dp.gpioLevel == 1);
	assert(logged(sHiwordWrites, 0x5f807c, 0x00f00050));
	assert(dp.hpdStatusBefore == 0 && ((dp.hpdStatusAfter >> 9) & 7) == 7 && dp.hpdPolls >= 1 && dp.hpdPolls < 1000);
	assert(dp.cctlBefore == 0x4 && dp.cctlAfter == 0x0);
	assert(dp.refclkSelect == 0 && dp.resetsBefore[3] == 0 && dp.pmaBefore[0] == 0 && dp.pmaBefore[3] == 0xc0);
	// Resets asserted, the PMA APB and PCS released, then init, cmn and lane.
	assert(sCruWrites[0] == std::make_pair(0x4d4u, 0xff004a00u) && dp.auxClockBefore == 0 && dp.auxClockAfter == 0x4a00);
	assert(sequenceOf(sCruWrites, {{0xa08u, 0x80008000u}, {0xa0cu, 0x00070007u}, {0xb20u, 0x00100010u},
		{0xb20u, 0x00100000u}, {0xa0cu, 0x00040000u}, {0xa08u, 0x80000000u}, {0xa0cu, 0x00030000u}}));
	assert(sequenceOf(sHiwordWrites, {{0x5cc004u, 0x40004000u}, {0x5cc004u, 0x20002000u}, {0x5a6008u, 0x03ff0040u}}));
	assert(sPmaWrites.size() == 65 + 72 + 3 && sPmaWrites[0] == std::make_pair(0x104u, 0x44u));
	assert(sequenceOf(sPmaWrites, {{0x024u, 0x6eu}, {0x090u, 0x68u}, {0x1a64u, 0xa8u}, {0x288u, 0xc0u},
		{0x38cu, 0x08u}, {0x288u, 0xccu}}));
	assert(dp.lcpllPolls == 0 && dp.pmaAfter[0] == 0xcc && dp.pmaAfter[5] == 0x08);
	assert(dp.usbdpGrfAfter == 0x6000 && dp.vo0GrfAfter == 0x40);
	assert(memcmp(dp.dpcd, sDpcd, 16) == 0 && dp.dpcdCount == 16 && dp.sinkCount == 0x41);
	assert(memcmp(dp.edid, sSinkEdid, 128) == 0 && dp.edidBytes == 128);
	assert(sAuxLog.size() == 12 && sAuxLog[0] == 0x9000000fu && sAuxLog[1] == 0x90020000u);
	assert(sAuxLog[2] == 0x40005000u && sAuxLog[3] == 0x5000500fu && sAuxLog[10] == 0x5000500fu);
	assert(sAuxLog[11] == 0x10005010u && dp.auxTransfers == 12 && dp.auxRetries == 0);
	assert(dp.resetsAfter[0] == 0 && dp.resetsAfter[1] == 0 && dp.resetsAfter[3] == 0);
	assert(dp.finishedMicros > dp.startedMicros && sLockDepth == 0);
	// Without EDID only the DPCD is read.
	Prepare(); sAllowDp = true;
	assert(probe(0) == kDpOK && dp.phase == kDpPhaseDpcd && sAuxLog.size() == 2 && dp.edidBytes == 0);
	assert(dp.pinMuxBefore == 0x50 && !logged(sHiwordWrites, 0x5f807c, 0x00f00050));
	// Deferred replies are retried.
	Prepare(); sAllowDp = true; sAuxDeferReplies = 3;
	assert(probe(kDpProbeEdid) == kDpOK && dp.auxRetries == 3 && dp.auxTransfers == 15);
	Prepare(); sAllowDp = true; sAuxDeferReplies = 8;
	assert(probe(0) == kDpAuxNack && dp.phase == kDpPhaseDpcd && dp.auxTransfers == 8);
	// No sink: no hot-plug after 200 ms and nothing of the PHY is touched.
	Prepare(); sAllowDp = true; sSinkPresent = false;
	assert(probe(kDpProbeEdid) == kDpNoHotPlug && dp.phase == kDpPhaseHotPlug && dp.hpdPolls == 2500);
	assert(sCruWrites.size() == 1 && sPmaWrites.empty() && sAuxLog.empty());
	// Past a missing hot-plug on request: the controller's AUX timeout.
	Prepare(); sAllowDp = true; sSinkPresent = false;
	assert(probe(kDpProbeIgnoreHotPlug) == kDpAuxTimeout && dp.phase == kDpPhaseDpcd && sAuxLog.size() == 1);
	assert((dp.auxStatus & (1u << 17)) != 0 && !sCruWrites.empty());
	// No reply event at all.
	Prepare(); sAllowDp = true; sAuxNeverReplies = true;
	assert(probe(0) == kDpAuxTimeout && dp.auxPolls == 200);
	// The PHY PLL never locks; the reference clock is not 24 MHz.
	Prepare(); sAllowDp = true; sLcpllNeverLocks = true;
	assert(probe(0) == kDpLcpllTimeout && dp.phase == kDpPhasePhy && dp.lcpllPolls == 500 && sAuxLog.empty());
	Prepare(); sAllowDp = true; sRefclkSelect = 1u << 7;
	assert(probe(0) == kDpRefclkUnsupported && dp.refclkSelect == (1u << 7) && sCruWrites.size() == 1);
	// A corrupted EDID.
	Prepare(); sAllowDp = true; sSinkEdid[20] ^= 1;
	assert(probe(kDpProbeEdid) == kDpEdidInvalid && dp.edidBytes == 128);
	// Link training at the sink's 2.7 Gb/s over the two lanes: the power-up,
	// the link words, TPS1 until the swing request is met, TPS2 until the
	// pre-emphasis request is met, the pattern off; the drive tables applied
	// to PHY lanes 2 and 3.
	Prepare(); sAllowDp = true;
	assert(probe(kDpProbeEdid | kDpProbeTrain) == kDpOK && dp.phase == kDpPhaseTrain);
	assert(dp.linkRate == 0x0a && dp.laneCount == 2 && dp.enhancedFraming == 1 && dp.spreadSpectrum == 1);
	assert(dp.trainingPattern == 2 && dp.attempts == 1 && dp.clockRecoveryLoops == 2 && dp.equalizationLoops == 2);
	assert(dp.swing[0] == 1 && dp.swing[1] == 1 && dp.preEmphasis[0] == 1 && dp.preEmphasis[1] == 1);
	assert(dp.linkStatus[0] == 0x77 && dp.linkStatus[2] == 1 && dp.ropllPolls == 0);
	assert(sDpcd[0x600] == 1 && sDpcd[0x100] == 0x0a && sDpcd[0x101] == 0x82 && sDpcd[0x107] == 0x10 && sDpcd[0x108] == 1);
	assert(sDpcd[0x102] == 0 && sDpcd[0x103] == 0x09 && sDpcd[0x104] == 0x09);
	assert(sequenceOf(sDpcdWrites, {{0x600u, 1u}, {0x100u, 0x0au}, {0x101u, 0x82u}, {0x102u, 0x21u}, {0x103u, 0u},
		{0x103u, 1u}, {0x102u, 0x22u}, {0x103u, 1u}, {0x103u, 9u}, {0x102u, 0u}}));
	assert(sequenceOf(sPmaWrites, {{0x28cu, 0x38u}, {0x38cu, 0x0cu}}));
	// Swing 1 / pre-emphasis 1 at HBR on PHY lanes 2 and 3 (0x810 + 0x800 * lane), nothing on lanes 0 and 1.
	assert(logged(sPmaWrites, 0x1810, 0x2a) && logged(sPmaWrites, 0x2010, 0x2a) && logged(sPmaWrites, 0x2014, 0x17));
	assert(!logged(sPmaWrites, 0x1010, 0x2a) && !logged(sPmaWrites, 0x0810, 0x2a));
	assert((dp.phyifAfter & 0x1e0000) == 0 && ((dp.phyifAfter >> 8) & 0xf) == 3 && ((dp.phyifAfter >> 6) & 3) == 1);
	assert((dp.phyifAfter & 0xf) == 0 && (dp.cctlTrained & 3) == 2);
	// The sink only recovers at RBR: one downgrade from its 2.7 Gb/s.
	Prepare(); sAllowDp = true; sHighestRecoveringRate = 0x06;
	assert(probe(kDpProbeTrain) == kDpOK && dp.linkRate == 0x06 && dp.attempts == 2);
	assert(logged(sPmaWrites, 0x28c, 0x18) && sDpcd[0x100] == 0x06);
	// Never recovers: every rate fails, the pattern ends disabled.
	Prepare(); sAllowDp = true; sLinkNeverRecovers = true;
	assert(probe(kDpProbeTrain) == kDpTrainingFailed && dp.attempts == 2 && sDpcd[0x102] == 0);
	// ROPLL never locks.
	Prepare(); sAllowDp = true; sRopllNeverLocks = true;
	assert(probe(kDpProbeTrain) == kDpRopllTimeout && dp.ropllPolls == 50);
	// Video: VP1 at 1080p60 from GPLL / 8 with a magenta background, DP1 muxed
	// to it with positive syncs, the stream configured for 2 x 2.7 Gb/s.
	Prepare(); sAllowDp = true;
	assert(probe(kDpProbeTrain | kDpProbeVideo) == kDpOK && dp.phase == kDpPhaseVideo);
	assert(dp.gpll[0] == 0xc6 && dp.gpll[1] == 0x42 && dp.dclkSelectBefore == 0x201);
	assert(logged(sCruWrites, 0x4bc, 0xfe000e00u) && dp.dclkSelectAfter == 0x0e01);
	assert(logged(sCruWrites, 0x4c0, 0x06000000u) && logged(sCruWrites, 0x8d0, 0x08000000u)
		&& logged(sCruWrites, 0x8d4, 0x00010000u));
	assert(dp.dclkGatesAfter[0] == (sGate52 & ~0x800u) && (dp.dclkMuxAfter & 0x600) == 0);
	assert(dp.portControlBefore == 0x8000000f && dp.portControlAfter == 0xf && sVopOverrides[0xd00] == 0xf);
	assert(sVopOverrides[0xd0c] == 0xa && sVopOverrides[0xd48] == ((2200u << 16) | 44));
	assert(sVopOverrides[0xd4c] == ((192u << 16) | 2112) && sVopOverrides[0xd54] == ((41u << 16) | 1121));
	assert(sVopOverrides[0xd50] == ((1125u << 16) | 5) && sVopOverrides[0x74] == ((1121u << 16) | 1121));
	assert(sVopOverrides[0x6e4] == (54u << 24) && sVopOverrides[0xd2c] == kDpBackground);
	assert(sVopOverrides[0xd30] == (((54u + 959) << 16) | 44));
	assert(dp.interfaceEnableBefore == 0x00080020 && dp.interfaceEnableAfter == (0x00080020u | 2 | (1u << 14)));
	assert(((dp.interfacePolarityAfter >> 12) & 7) == 3 && (dp.interfacePolarityAfter & (1u << 28)) != 0);
	assert(logged(sVopWrites, 0x000, 0x8000u | 2 | (2u << 16)) && dp.commitPolls >= 1);
	// The DP port runs RGB 8 bpc quad pixel: HSTART 192, VSTART 41, TU 52.8, threshold 40, hblank 127.
	assert(dp.msa[0] == ((41u << 16) | 192) && dp.msa[1] == 0x20000000u && dp.msa[2] == 0);
	assert(dp.videoConfig[0] == ((1920u << 16) | (280u << 2)) && dp.videoConfig[1] == ((45u << 16) | 1080));
	assert(dp.videoConfig[2] == ((44u << 16) | 88) && dp.videoConfig[3] == ((5u << 16) | 4));
	assert(dp.videoConfig[4] == ((8u << 16) | (40u << 7) | 52) && dp.hblankInterval == 0x1007f);
	assert(logged(sDpWrites, 0x30c, 3) && logged(sDpWrites, 0x320, dp.videoConfig[4]));
	assert(dp.vsampleAfter == ((2u << 21) | (1u << 16) | (1u << 5)));
	assert(sequenceOf(sDpWrites, {{0x300u, (2u << 21) | (1u << 16)}, {0x330u, 0x1007fu},
		{0x300u, (2u << 21) | (1u << 16) | (1u << 5)}}));
	// The window: ESMART0 clones the desktop window ESMART2 on video port 1.
	Prepare(); sAllowDp = true;
	sVopOverrides[0x1c04] = (0xcu << 4) | (0xdu << 12); // the board's ids for ESMART2
	sVopOverrides[0x6f8] = 0x00170000;
	for (unsigned w = 0; w < 4; w++) {
		sVopOverrides[0x650 + 0x40 + w * 4] = 0x100 + w; // MIX4
		sVopOverrides[0x650 + 0x50 + w * 4] = 0x200 + w; // MIX5
	}
	assert(probe(kDpProbeTrain | kDpProbeVideo | kDpProbeWindow) == kDpOK && dp.phase == kDpPhaseWindow);
	assert(dp.desktopWindow == 2 && dp.windowAddress == kModelFirmwareAddress && dp.windowVirtual == 1920);
	assert(dp.windowActive == 0x0437077f && dp.windowRegionControl == 1);
	assert(sVopOverrides[0x1810] == 1 && sVopOverrides[0x1814] == kModelFirmwareAddress && sVopOverrides[0x181c] == 1920);
	assert(sVopOverrides[0x1820] == 0x0437077f && sVopOverrides[0x1824] == 0x0437077f && sVopOverrides[0x1828] == 0);
	assert(((sVopOverrides[0x1804] >> 4) & 0x1f) == 0x10 && ((sVopOverrides[0x1804] >> 12) & 0x1f) == 0x11);
	assert((sVopOverrides[0x1808] & 2) == 2 && sVopOverrides[0x6f8] == 0x00170017 && dp.smartDelay[1] == 0x00170017);
	assert((sVopOverrides[0x008] & 0x80000000u) == 0 && dp.autoGating[0] == 0x80000000u);
	for (unsigned w = 0; w < 4; w++)
		assert(sVopOverrides[0x650 + w * 4] == 0x100 + w && sVopOverrides[0x660 + w * 4] == 0x200 + w);
	assert(dp.mixers[1][0] == 0x100 && dp.mixers[3][3] == 0x203);
	assert(logged(sVopWrites, 0x000, 0x8000u | 2 | (2u << 16)) && dp.windowPolls >= 1);
	// The cursor window above the desktop still leaves ESMART2 as the source.
	Prepare(); sAllowDp = true; sVopOverrides[0x1e10] = 1;
	assert(probe(kDpProbeTrain | kDpProbeVideo | kDpProbeWindow) == kDpOK && dp.desktopWindow == 2);
	// No desktop window, or ESMART0 already in use: nothing of the window is written.
	Prepare(); sAllowDp = true; sVopOverrides[0x1c10] = 0;
	assert(probe(kDpProbeTrain | kDpProbeVideo | kDpProbeWindow) == kDpNoDesktopWindow && !logged(sVopWrites, 0x1810, 1));
	Prepare(); sAllowDp = true; sVopOverrides[0x1810] = 1;
	assert(probe(kDpProbeTrain | kDpProbeVideo | kDpProbeWindow) == kDpWindowBusy);
	// A GPLL that is not 1188 MHz: nothing of the port is touched.
	Prepare(); sAllowDp = true; sGpllCon1 = 0x43;
	assert(probe(kDpProbeTrain | kDpProbeVideo) == kDpGpllUnexpected && sVopWrites.empty());
	assert(!logged(sCruWrites, 0x4bc, 0xfe000e00u));
	sGpllCon1 = 0x42;
	// Video port 1 already running.
	Prepare(); sAllowDp = true; sVopOverrides[0xd00] = 0xf;
	assert(probe(kDpProbeTrain | kDpProbeVideo) == kDpPortBusy && sVopWrites.empty() && sCruWrites.size() > 0);
	// The port never takes its configuration.
	Prepare(); sAllowDp = true; sCommitNeverCompletes = true;
	assert(probe(kDpProbeTrain | kDpProbeVideo) == kDpPortTimeout && dp.commitPolls == kScanoutPollLimit);
	// The DP desktop profile: the accelerant's frame buffer on DP1. The first
	// acquisition runs the whole bring-up with the cloned window, then swaps
	// ESMART0 to the driver's 1080p buffer over black, moves the console and
	// arms video port 1's frame-start interrupt; release clones the firmware
	// desktop again.
	controller.dpDesktopEnabled = true;
	controller.accelerantEnabled = true;
	Prepare(); sAllowDp = true; sBootInfo = frame_buffer_boot_info{17, 0xed940000, 0xffff000012340000ull, 640, 480, 32, 2560, 0};
	sVopOverrides[0x1c14] = 0xed940000; sVopOverrides[0x1c1c] = 640; sVopOverrides[0x1c20] = 0x01df027f;
	sVopWrites.clear();
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sOwner == primary && sDpDesktop && sDpLinkUp);
	assert(sVopOverrides[0x1814] == kModelFramePhysical && sVopOverrides[0x181c] == 1920);
	assert(sVopOverrides[0x1820] == 0x0437077f && sVopOverrides[0x1824] == 0x0437077f && sVopOverrides[0x1810] == 1);
	assert(sVopOverrides[0xd2c] == 0 && sVopOverrides[0xd00] == 0xf && sVopOverrides[0x1c14] == 0xed940000);
	assert(logged(sVopWrites, 0x1814, 0xed940000) && logged(sVopWrites, 0x1814, kModelFramePhysical));
	assert(sConsole.address != 0 && sConsole.width == 1920 && sConsole.height == 1080 && sConsole.bytesPerRow == 7680);
	assert(sHandler != NULL && (sVopOverrides[0xb0] & 0x20) != 0);
	acc = {}; acc.version = kAccelerantVersion;
	assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert(acc.port == 1 && acc.window == 0 && acc.width == 1920 && acc.firmwareAddress == 0xed940000);
	assert((acc.flags & (kAccelerantAcquired | kAccelerantEdid | kAccelerantRetrace)) == (kAccelerantAcquired | kAccelerantEdid | kAccelerantRetrace));
	assert((acc.flags & (kAccelerantModeSet | kAccelerantCursor)) == 0 && acc.frameBufferPhysical == kModelFramePhysical);
	assert(sShared != NULL && strcmp(sShared->name, "RK3588 VOP2 DP TX1") == 0 && sShared->pixelClockKHz == 148500);
	assert(sShared->hTotal == 2200 && sShared->vSyncStart == 1084 && memcmp(sShared->edid, sSinkEdid, 128) == 0);
	assert(sShared->portTiming[0] == ((2200u << 16) | 44));
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_BUSY);
	// Release: the firmware desktop again, the console back on the boot buffer, the interrupt off.
	assert(Close(primary) == B_OK && sOwner == NULL && !sDpDesktop);
	assert(sVopOverrides[0x1814] == 0xed940000 && sVopOverrides[0x181c] == 640 && sVopOverrides[0x1820] == 0x01df027f);
	assert(sConsole.width == 640 && sHandler == NULL && (sVopOverrides[0xb0] & 0x20) == 0 && sFrameArea < 0);
	assert(Free(primary) == B_OK && sAreas.empty());
	// A second acquisition (app_server restarted) only moves the window.
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	sDpWrites.clear(); sVopWrites.clear();
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sDpWrites.empty());
	assert(sVopOverrides[0x1814] == kModelFramePhysical && !logged(sVopWrites, 0xd00, 0xf));
	assert(Close(primary) == B_OK && sVopOverrides[0x1814] == 0xed940000);
	controller.dpDesktopEnabled = false;
	// The spanning desktop: one 3840x1080 buffer, the left half on HDMI1 (its
	// port raised from the firmware's sink-less 640x480 to 1080p by the mode
	// set), the right half on DP1; release gives HDMI1 its firmware mode,
	// window and pitch back.
	controller.dpDesktopEnabled = controller.dpSpanEnabled = true;
	assert(Free(primary) == B_OK);
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	Prepare(); sAllowDp = true; sAllowModeSet = true;
	sBootInfo = frame_buffer_boot_info{17, 0xed940000, 0xffff000012340000ull, 640, 480, 32, 2560, 0};
	sVopOverrides[0x1c14] = 0xed940000; sVopOverrides[0x1c1c] = 640; sVopOverrides[0x1c20] = 0x01df027f;
	sVopOverrides[0x1c24] = 0x01df027f;
	sVopOverrides[0xe48] = (800u << 16) | 96; sVopOverrides[0xe4c] = (144u << 16) | 784;
	sVopOverrides[0xe50] = (525u << 16) | 2; sVopOverrides[0xe54] = (35u << 16) | 515;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sSpanHdmi && sSpanModeResult == kModeOK);
	assert(sSpanPort == 2 && sSpanWindow == 2 && sFirmwareMode.pixelClockKHz == 25175 && sFirmwareMode.vic == 1);
	assert(sVopOverrides[0x1814] == kModelFramePhysical + 7680 && sVopOverrides[0x181c] == 3840);
	assert(sVopOverrides[0x1c14] == kModelFramePhysical && sVopOverrides[0x1c1c] == 3840);
	assert(sVopOverrides[0x1c20] == 0x0437077f && sVopOverrides[0xe48] == ((2200u << 16) | 44));
	assert(sVopOverrides[0xe54] == ((41u << 16) | 1121) && (sVopOverrides[0xe00] & 0x80000000u) == 0);
	assert(sConsole.width == 3840 && sConsole.bytesPerRow == 15360 && sHandler != NULL);
	acc = {}; acc.version = kAccelerantVersion;
	assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert(acc.width == 3840 && acc.bytesPerRow == 15360 && acc.port == 1 && acc.window == 0);
	assert(sShared->width == 3840 && sShared->hTotal == 4400 && sShared->hSyncStart == 4016
		&& sShared->pixelClockKHz == 297000 && sShared->vTotal == 1125);
	assert(strcmp(sShared->name, "RK3588 VOP2 HDMI TX1 + DP TX1") == 0);
	assert(Close(primary) == B_OK && !sSpanHdmi && !sDpDesktop);
	assert(sVopOverrides[0x1c14] == 0xed940000 && sVopOverrides[0x1c1c] == 640 && sVopOverrides[0x1c20] == 0x01df027f);
	assert(sVopOverrides[0xe48] == ((800u << 16) | 96) && sVopOverrides[0xe54] == ((35u << 16) | 515));
	assert(sVopOverrides[0x1814] == 0xed940000 && sConsole.width == 640 && sHandler == NULL);
	assert(sAreas.empty());
	// The span cursor: app_server's pointer on one window per port, each
	// clipped to its own screen; hidden on both at release.
	controller.cursorEnabled = controller.cursorHooksEnabled = true;
	assert(Free(primary) == B_OK);
	assert(Open(&controller, "", O_RDWR, &opened) == B_OK);
	primary = (Handle*)opened;
	Prepare(); sAllowDp = true; sAllowModeSet = true; sAllowCursor = true;
	sBootInfo = frame_buffer_boot_info{17, 0xed940000, 0xffff000012340000ull, 640, 480, 32, 2560, 0};
	sVopOverrides[0x1c14] = 0xed940000; sVopOverrides[0x1c1c] = 640; sVopOverrides[0x1c20] = 0x01df027f;
	sVopOverrides[0x1c24] = 0x01df027f;
	sVopOverrides[0xe48] = (800u << 16) | 96; sVopOverrides[0xe4c] = (144u << 16) | 784;
	sVopOverrides[0xe50] = (525u << 16) | 2; sVopOverrides[0xe54] = (35u << 16) | 515;
	assert(Control(primary, kAcquireFrameBuffer, NULL, 0) == B_OK && sSpanHdmi);
	acc = {}; acc.version = kAccelerantVersion;
	assert(Control(primary, kGetAccelerantInfo, &acc, sizeof(acc)) == B_OK);
	assert((acc.flags & (kAccelerantCursor | kAccelerantCursorHooks)) == (kAccelerantCursor | kAccelerantCursorHooks));
	assert((acc.flags & kAccelerantModeSet) == 0 && sCursorState.window == 3 && sCursorRight.window == 1);
	CursorBitmap* spanBitmap = new CursorBitmap();
	spanBitmap->version = kCursorVersion; spanBitmap->width = spanBitmap->height = 22; spanBitmap->bytesPerRow = 88;
	memset(spanBitmap->data, 0xff, sizeof(spanBitmap->data));
	assert(Control(primary, kSetCursorBitmap, spanBitmap, sizeof(*spanBitmap)) == B_OK && spanBitmap->result == kCursorOK);
	delete spanBitmap;
	CursorMove spanMove = {}; spanMove.version = kCursorVersion; spanMove.x = 1910; spanMove.y = 500;
	assert(Control(primary, kMoveCursor, &spanMove, sizeof(spanMove)) == B_OK && spanMove.result == kCursorOK);
	CursorShow spanShow = {}; spanShow.version = kCursorVersion; spanShow.visible = 1;
	assert(Control(primary, kShowCursor, &spanShow, sizeof(spanShow)) == B_OK && spanShow.result == kCursorOK);
	// Straddling the seam: ten columns on HDMI1, the other twelve on DP1.
	assert(sVopOverrides[0x1e10] == 1 && sVopOverrides[0x1e28] == ((500u << 16) | 1910));
	assert(sVopOverrides[0x1e20] == ((21u << 16) | 9));
	assert(sVopOverrides[0x1a10] == 1 && sVopOverrides[0x1a28] == (500u << 16));
	assert(sVopOverrides[0x1a20] == ((21u << 16) | 11) && sVopOverrides[0x1a14] == sCursorRight.address);
	assert(sCursorRight.address == sCursorState.address + 40 && sCursorRight.x == -10);
	assert(sVopOverrides[0x670] == kVopMixerSourceColor && sVopOverrides[0x6b0] == kVopMixerSourceColor);
	assert(((sVopOverrides[0x1a04] >> 4) & 0x1f) == ((((sVopOverrides[0x1804] >> 4) & 0x1f) + 2) & 0x1f));
	// All on DP1, then hidden.
	spanMove.x = 3000;
	assert(Control(primary, kMoveCursor, &spanMove, sizeof(spanMove)) == B_OK && spanMove.result == kCursorOK);
	assert(sVopOverrides[0x1e10] == 0 && sVopOverrides[0x1a10] == 1 && sVopOverrides[0x1a28] == ((500u << 16) | 1080));
	spanShow.visible = 0;
	assert(Control(primary, kShowCursor, &spanShow, sizeof(spanShow)) == B_OK && spanShow.result == kCursorOK);
	assert(sVopOverrides[0x1e10] == 0 && sVopOverrides[0x1a10] == 0);
	spanShow.visible = 1;
	assert(Control(primary, kShowCursor, &spanShow, sizeof(spanShow)) == B_OK && sVopOverrides[0x1a10] == 1);
	assert(Close(primary) == B_OK && sVopOverrides[0x1a10] == 0 && sVopOverrides[0x1e10] == 0 && !sSpanHdmi);
	assert(sAreas.empty());
	controller.cursorEnabled = controller.cursorHooksEnabled = false;
	sAllowCursor = false;
	sAllowModeSet = false;
	controller.dpDesktopEnabled = controller.dpSpanEnabled = false;
	sDpLinkUp = false;
	assert(Close(reader) == B_OK && Free(reader) == B_OK && Free(primary) == B_OK);
	controller.dpAuxEnabled = false;
	sAllowDp = false;
	controller = dpController;
	assert(sAreas.empty() && sLockDepth == 0);
	printf("RK3588_DISPLAY_RESOURCES_TEST_PASS faults=%zu\n", faults.size());
	return 0;
}
