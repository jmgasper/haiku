/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_PCIE_INTX_PROFILE_H
#define RK3588_PCIE_INTX_PROFILE_H

#include <stdint.h>

namespace RK3588Firmware {

// RK3588 TRM Part 2, PCIe client registers; the board's retained EDK2 v1.1
// device tree supplies the named "legacy" level-high GIC SPI for each root.
struct IntxProfile {
	uint64_t apb;
	uint32_t irq;
};

inline bool
FindIntxProfile(unsigned segment, IntxProfile& profile)
{
	switch (segment) {
		case 3: profile = {0xfe180000, 277}; return true;
		case 4: profile = {0xfe190000, 282}; return true;
	}
	return false;
}

inline bool
ValidIntxEndpoint(unsigned bus, unsigned device, unsigned function, unsigned pin)
{
	// A direct parent IRQ is safe only for this one endpoint with INTA alone
	// unmasked. Bridges, multifunction devices and the other three pins need a
	// separate interrupt domain and swizzling; this profile does not expose them.
	return bus == 1 && device == 0 && function == 0 && pin == 1;
}

inline bool
IntxResourcesMatch(const IntxProfile& profile, uint64_t apb, uint64_t size,
	const uint32_t* specifier, unsigned cells, uint64_t decodedIRQ, uint64_t gicBase)
{
	return apb == profile.apb && size == 0x10000 && cells == 4
		&& specifier != nullptr && specifier[0] == 0
		&& specifier[1] == profile.irq - 32 && specifier[2] == 4
		&& specifier[3] == 0 && decodedIRQ == profile.irq
		&& gicBase == 0xfe600000;
}

} // namespace RK3588Firmware
#endif
