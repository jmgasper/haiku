/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_PCIE_FIRMWARE_PROFILE_H
#define RK3588_PCIE_FIRMWARE_PROFILE_H

#include <stdint.h>

namespace RK3588Firmware {

// Explicit lab profile: edk2-rk3588 v1.1, ROCK 5 ITX, mainline DT only.
// Firmware commit 6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18.
// Rk3588Pcie.h and PciSegmentLib.c define these live mappings. The Linux
// DT config/ranges describe a different setup and must not replace them.
static const uint64_t kRootConfig = UINT64_C(0xa40000000);
static const uint64_t kEndpointConfig = UINT64_C(0x900100000);
static const uint64_t kMemoryBase = UINT64_C(0xf0000000);
static const uint64_t kMemorySize = UINT64_C(0x01000000);
static const unsigned kConfigSize = 4096;

inline bool
ValidAccess(unsigned bus, unsigned device, unsigned function, unsigned offset,
	unsigned size)
{
	// CFG0 has mirrored slots. Accessing 01:01.0 can hang this controller.
	// This profile supports only the root and the single-function Samsung SSD.
	return bus <= 1 && device == 0 && function == 0
		&& (size == 1 || size == 2 || size == 4)
		&& offset < kConfigSize && size <= kConfigSize - offset
		&& (offset & (size - 1)) == 0;
}

inline bool
RootMatches(const uint32_t* config, uint64_t& memoryBase, uint64_t& memorySize)
{
	if (config[0] != 0x35881d87 || (config[2] >> 8) != 0x060400
		|| ((config[3] >> 16) & 0xff) != 1
		|| (config[6] & 0x00ffffff) != 0x00010100
		|| (config[1] & 2) == 0) {
		return false;
	}
	// Export only the memory window currently forwarded by this root bridge.
	// The PCI core can then reserve the firmware BAR without moving it.
	uint64_t base = uint64_t(config[8] & 0xfff0) << 16;
	uint64_t limit = uint64_t(config[8] & 0xfff00000) | 0xfffff;
	if (base < kMemoryBase || limit < base
		|| limit >= kMemoryBase + kMemorySize) {
		return false;
	}
	memoryBase = base;
	memorySize = limit + 1 - base;
	return true;
}

inline bool
EndpointMatches(const uint32_t* config, uint64_t memoryBase, uint64_t memorySize)
{
	uint64_t bar = (uint64_t(config[5]) << 32) | (config[4] & ~UINT64_C(0xf));
	return config[0] == 0xa802144d && (config[2] >> 8) == 0x010802
		&& ((config[3] >> 16) & 0xff) == 0 && (config[1] & 2) != 0
		&& (config[4] & 0xf) == 4 && memorySize >= 0x4000
		&& bar >= memoryBase && bar - memoryBase <= memorySize - 0x4000;
}

} // namespace RK3588Firmware

#endif
