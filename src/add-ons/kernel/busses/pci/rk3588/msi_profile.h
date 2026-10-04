/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_PCIE_MSI_PROFILE_H
#define RK3588_PCIE_MSI_PROFILE_H

#include <stdint.h>

namespace RK3588Firmware {

struct MsiProfile {
	uint64_t controller;
	uint32_t firmwareRequesterBase;
	uint32_t requesterBase;
	uint32_t requesterCount;
};

inline bool
FindMsiProfile(unsigned segment, MsiProfile& profile)
{
	switch (segment) {
		case 0: profile = {UINT64_C(0xfe660000), 0, 0, 0x200}; return true;
		// Linux assigns this domain buses 0x20..0x2f. EDK2 leaves the bridge
		// at buses 0..1, which Haiku preserves, so its runtime requester IDs
		// start at zero even though the firmware msi-map starts at 0x2000.
		case 2: profile = {UINT64_C(0xfe640000), 0x2000, 0, 0x200}; return true;
	}
	return false;
}

inline bool
MsiResourcesMatch(const MsiProfile& profile, const uint32_t* map, unsigned bytes,
	uint64_t controllerBase, uint64_t controllerSize)
{
	return map != nullptr && bytes == 16
		&& map[0] == profile.firmwareRequesterBase
		&& map[2] == profile.firmwareRequesterBase
		&& map[3] == 0x1000 && controllerBase == profile.controller
		&& controllerSize == 0x20000;
}

} // namespace RK3588Firmware

#endif
