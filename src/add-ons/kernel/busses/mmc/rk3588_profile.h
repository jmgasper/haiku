/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_MMC_PROFILE_H
#define RK3588_MMC_PROFILE_H

#include <stdint.h>
#include <string.h>

namespace RK3588Mmc {

static const char kProfile[] = "rock5-itx-edk2-v1.1-emmc-legacy";
static const uint64_t kBase = UINT64_C(0xfe2e0000);
static const uint64_t kCru = UINT64_C(0xfd7c0000);
static const uint32_t kClockOffset = 0x434;
static const uint32_t kClockMask = 0xff00;
static const uint32_t kOscillatorClock = 0x8000;

inline uint32_t Read32(const void* bytes)
{
	const uint8_t* p = (const uint8_t*)bytes;
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
		| (uint32_t(p[2]) << 8) | p[3];
}

inline bool HasString(const void* bytes, int length, const char* value)
{
	const char* p = (const char*)bytes;
	while (p != NULL && length > 0) {
		const char* end = (const char*)memchr(p, 0, length);
		if (end == NULL)
			return false;
		if (strcmp(p, value) == 0)
			return true;
		length -= end - p + 1;
		p = end + 1;
	}
	return false;
}

struct Resources {
	uint64_t base, size, interrupt, cruBase, cruSize;
	uint32_t width, maxFrequency, clockPhandle;
	bool board, nonRemovable, noSD, noSDIO, identity, levelHigh, cru;
};

inline bool Allows(const Resources& p)
{
	return p.base == kBase && p.size == 0x10000 && p.interrupt == 237
		&& p.cruBase == kCru && p.cruSize == 0x5c000
		&& p.width == 8 && p.maxFrequency == 150000000 && p.clockPhandle != 0
		&& p.board && p.nonRemovable && p.noSD && p.noSDIO
		&& p.identity && p.levelHigh && p.cru;
}

inline bool ClockReferences(const void* data, int length, const void* names,
	int namesLength, bool reset, uint32_t& phandle)
{
	static const char kNames[] = "core\0bus\0axi\0block\0timer";
	static const uint32_t kClocks[] = {0x12c, 0x12a, 0x12b, 0x12d, 0x12e};
	static const uint32_t kResets[] = {0x118, 0x116, 0x117, 0x119, 0x11a};
	if (data == NULL || length != 40 || names == NULL || namesLength != sizeof(kNames)
		|| memcmp(names, kNames, sizeof(kNames)) != 0)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t provider = Read32(p);
	if (provider == 0 || (reset && provider != phandle))
		return false;
	for (unsigned i = 0; i < 5; i++) {
		if (Read32(p + i * 8) != provider
			|| Read32(p + i * 8 + 4) != (reset ? kResets[i] : kClocks[i]))
			return false;
	}
	phandle = provider;
	return true;
}

inline bool Interrupt(const void* data, int length)
{
	if (data == NULL || (length != 12 && length != 16))
		return false;
	const uint8_t* p = (const uint8_t*)data;
	return Read32(p) == 0 && Read32(p + 4) == 205 && Read32(p + 8) == 4
		&& (length == 12 || Read32(p + 12) == 0);
}

inline bool Controller(uint32_t caps, uint32_t caps2, uint8_t version, uint32_t vendor)
{
	// Read-only values recorded on PCB v1.12. Reject before any writes if
	// another controller or incompatible firmware configuration is present.
	return caps == 0x226dc881 && caps2 == 0x08000007 && version == 5
		&& (vendor & 0xfff) == 0x500;
}

inline uint32_t ClockWrite(uint32_t value)
{
	return (kClockMask << 16) | (value & kClockMask);
}

template<class IO>
bool ConfigureLegacy(IO& io)
{
	// RK3588 TRM Part 1 CLKSEL_CON77: select the 24 MHz oscillator
	// without changing the adjacent NVM bus clock fields. SDCLK must be off.
	if ((io.Read16(0x2c) & 4) != 0)
		return false;
	io.WriteClock(ClockWrite(kOscillatorClock));
	io.Barrier();
	if ((io.ReadClock() & kClockMask) != kOscillatorClock)
		return false;
	// Part 2 eMMC: legacy SDR, reset deasserted, data CRC enabled.
	// Disable command-conflict detection, enhanced strobe and delay lines.
	io.Write8(0x508, io.Read8(0x508) & ~uint8_t(1));
	io.Write16(0x52c, (io.Read16(0x52c) & ~uint16_t(0x102)) | 5);
	io.Write32(0x800, 0x01000001); // DLL bypass + start
	io.Write32(0x804, 0x80000000); // original RX clock gating
	io.Write32(0x808, 0);
	io.Write32(0x80c, 0);
	io.Write32(0x810, 0);
	// Keep signaling on the board's fixed 1.8 V supply.
	io.Write16(0x3e, io.Read16(0x3e) | (1 << 3));
	io.Barrier();
	return true;
}

} // namespace RK3588Mmc
#endif
