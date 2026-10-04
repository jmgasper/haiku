/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ROCK5_MBI_PROBE_PROFILE_H
#define ROCK5_MBI_PROBE_PROFILE_H

#include <arch/arm64/gicv3_mbi_vectors.h>
#include <arch/arm64/rk3588_mbi.h>

namespace Rock5MbiProbe {

using Gicv3Mbi::kDistributor;
using Gicv3Mbi::kAlias;
static const unsigned kVector = Gicv3Mbi::kFirstVector;
static const uint32_t kMask = 1u << (kVector % 32);
static const unsigned kConfigOffset = 0xc00 + (kVector / 16) * 4;
static const uint32_t kEdgeMask = 2u << ((kVector % 16) * 2);

inline bool
FirmwareMatches(const void* fdt, const char* profile)
{
	return profile != nullptr
		&& strcmp(profile, "rock5-itx-edk2-v1.1-dt-mbi-test") == 0
		&& Gicv3Mbi::FirmwareMatches(fdt);
}

inline bool
RegistersMatch(uint32_t typer, uint32_t pidr2, uint32_t control, uint32_t group,
	uint32_t priority, uint32_t enabled, uint32_t pending, uint32_t active)
{
	return Gicv3Mbi::RegistersMatch(typer, pidr2, control, group, priority,
		enabled, pending, active, kMask);
}

} // namespace Rock5MbiProbe
#endif
