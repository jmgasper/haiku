#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <inttypes.h>
#include <string>

#include "firmware_profile.h"

using namespace RK3588Firmware;
using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using bigtime_t = int64_t;
using status_t = int32_t;
static const status_t B_OK = 0, B_NOT_SUPPORTED = -1, B_TIMED_OUT = -2,
	B_INTERRUPTED = -3;
#define B_PRIx32 PRIx32
#define B_PRIdBIGTIME PRId64

static bigtime_t sNow, sOversleep;
static unsigned sSleeps, sSnapshots;
static status_t sSleepStatus;
static std::function<void()> sAdvance;
static std::string sLog;

static bigtime_t system_time() { return sNow; }
static void memory_full_barrier() { sSnapshots++; }
static status_t
snooze(bigtime_t delay)
{
	assert(delay > 0 && delay <= 1000);
	sSleeps++;
	sNow += delay + sOversleep;
	if (sAdvance)
		sAdvance();
	return sSleepStatus;
}

static void
Trace(const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	assert(length >= 0 && size_t(length) < sizeof(buffer));
	sLog += buffer;
}
#define dprintf Trace
#include "training.inc"
#undef dprintf

static void
Reset(uint32* root, const PortProfile& port)
{
	sNow = 123456;
	sOversleep = 0;
	sSleeps = sSnapshots = 0;
	sSleepStatus = B_OK;
	sAdvance = nullptr;
	sLog.clear();
	memset(root, 0, 256);
	root[0] = 0x35881d87;
	root[1] = 0x00100007;
	root[2] = 0x06040001;
	root[3] = 0x00010000;
	root[6] = 0x00010100;
	root[8] = uint32(port.memoryBase | (port.memoryBase >> 16));
	root[0x70 / 4] = 0x1042b010;
	root[0x80 / 4] = 0xb8230000;
}

int
main()
{
	uint32 root[64], snapshot[64], before[64];
	uint64 base, size;
	for (const auto& port : kPorts) {
		Reset(root, port);
		root[0x80 / 4] &= ~0x08000000u;
		memcpy(snapshot, root, sizeof(root));
		memcpy(before, root, sizeof(root));
		// A ready snapshot requires no further MMIO or sleep.
		assert(WaitForRootLink(nullptr, snapshot, base, size, port) == B_OK);
		assert(sSleeps == 0 && sSnapshots == 0 && sLog.empty());
		assert(base == port.memoryBase && size == 0x100000);
		assert(memcmp(root, before, sizeof(root)) == 0);

		Reset(root, port);
		memcpy(snapshot, root, sizeof(root));
		sAdvance = [&] {
			if (sSleeps == 3)
				root[0x80 / 4] &= ~0x08000000u;
		};
		assert(WaitForRootLink((uint8*)root, snapshot, base, size, port) == B_OK);
		assert(sSleeps == 3 && sSnapshots == 3 && sNow == 126456);
		assert(RootMatches(snapshot, base, size, port));
		assert(sLog.find("settled after 3000 us (3 polls)") != std::string::npos);
		assert(sLog.find("b8230000 -> b0230000") != std::string::npos);
		// Only the fake hardware transition modified the original config.
		assert(memcmp(root, before, sizeof(root)) == 0);
	}

	const auto& port = kPorts[1];
	// Identity, class, header, buses, capability, memory decode and window
	// violations reject immediately even when training is the observed state.
	for (unsigned field : {0u, 2u, 3u, 6u, 8u, 0x70u / 4}) {
		Reset(root, port);
		root[field] = 0xffffffff;
		assert(WaitForRootLink(nullptr, root, base, size, port) == B_NOT_SUPPORTED);
		assert(sSleeps == 0 && sSnapshots == 0);
	}
	Reset(root, port);
	root[1] &= ~2u;
	assert(WaitForRootLink(nullptr, root, base, size, port) == B_NOT_SUPPORTED);
	assert(sSleeps == 0 && sSnapshots == 0);
	for (uint32 link : {0x98230000u, 0xb8200000u, 0xb8030000u,
			0x10230000u, 0xffffffffu}) {
		Reset(root, port);
		root[0x80 / 4] = link;
		if (link == 0xffffffffu) {
			// Existing policy accepts nonzero speed/width encodings; permanence
			// of training must still bound the wait for this value.
			memcpy(snapshot, root, sizeof(root));
			assert(WaitForRootLink((uint8*)root, snapshot, base, size, port) == B_TIMED_OUT);
			assert(sSleeps == 1000 && sSnapshots == 1000);
		} else {
			assert(WaitForRootLink(nullptr, root, base, size, port) == B_NOT_SUPPORTED);
			assert(sSleeps == 0 && sSnapshots == 0);
		}
	}

	// Revalidate a changed root before every subsequent snapshot/downstream use.
	for (unsigned field : {1u, 2u, 3u, 6u, 8u, 0x70u / 4, 0x80u / 4}) {
		Reset(root, port);
		memcpy(snapshot, root, sizeof(root));
		sAdvance = [&] { root[field] = 0; };
		assert(WaitForRootLink((uint8*)root, snapshot, base, size, port) == B_NOT_SUPPORTED);
		assert(sSleeps == 1 && sSnapshots == 1);
		assert(sLog.find("settled") == std::string::npos);
	}
	Reset(root, port);
	uint32 invalidId = 0xffffffff;
	// ASan catches any attempt to read beyond the rejected identity word.
	assert(WaitForRootLink((uint8*)&invalidId, root, base, size, port) == B_NOT_SUPPORTED);
	assert(sSleeps == 1 && sSnapshots == 0 && root[0] == invalidId);
	assert(root[0x80 / 4] == 0);

	Reset(root, port);
	memcpy(snapshot, root, sizeof(root));
	memcpy(before, root, sizeof(root));
	assert(WaitForRootLink((uint8*)root, snapshot, base, size, port) == B_TIMED_OUT);
	assert(sNow == 1123456 && sSleeps == 1000 && sSnapshots == 1000);
	assert(memcmp(root, before, sizeof(root)) == 0);
	assert(sLog.find("settled") == std::string::npos);

	Reset(root, port);
	memcpy(snapshot, root, sizeof(root));
	sAdvance = [&] {
		if (sSleeps == 1000)
			root[0x80 / 4] &= ~0x08000000u;
	};
	assert(WaitForRootLink((uint8*)root, snapshot, base, size, port) == B_OK);
	assert(sNow == 1123456 && sSleeps == 1000 && sSnapshots == 1000);

	Reset(root, port);
	sOversleep = 1000000;
	// Scheduling past the deadline must not issue another register read.
	assert(WaitForRootLink(nullptr, root, base, size, port) == B_TIMED_OUT);
	assert(sSleeps == 1 && sSnapshots == 0);
	Reset(root, port);
	sSleepStatus = B_INTERRUPTED;
	assert(WaitForRootLink(nullptr, root, base, size, port) == B_INTERRUPTED);
	assert(sSleeps == 1 && sSnapshots == 0);
	puts("PCIe training transitions, profile rejection and bounded failures passed");
}
