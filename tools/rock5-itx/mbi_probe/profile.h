/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ROCK5_MBI_PROBE_PROFILE_H
#define ROCK5_MBI_PROBE_PROFILE_H

#include <stdint.h>
#include <string.h>

extern "C" {
#include <libfdt.h>
}

namespace Rock5MbiProbe {

static const uint64_t kDistributor = UINT64_C(0xfe600000);
static const uint64_t kAlias = UINT64_C(0xfe610000);
// RK3588 TRM v1.0 Part 1, Table 1-3 reserves INTIDs 454..511.
// 464 is also inside the firmware DT's advertised MBI range 424..479.
static const unsigned kVector = 464;
static const uint32_t kMask = 1u << (kVector % 32);
static const unsigned kConfigOffset = 0xc00 + (kVector / 16) * 4;
static const uint32_t kEdgeMask = 2u << ((kVector % 16) * 2);

inline bool
CellsMatch(const void* fdt, int node, const char* name,
	const uint32_t* expected, unsigned count)
{
	int length = 0;
	const fdt32_t* cells = (const fdt32_t*)fdt_getprop(fdt, node, name, &length);
	if (cells == nullptr || length != int(count * sizeof(fdt32_t)))
		return false;
	for (unsigned i = 0; i < count; i++) {
		if (fdt32_to_cpu(cells[i]) != expected[i])
			return false;
	}
	return true;
}

inline bool
FirmwareMatches(const void* fdt, const char* profile)
{
	if (profile == nullptr
		|| strcmp(profile, "rock5-itx-edk2-v1.1-dt-mbi-test") != 0
		|| fdt == nullptr || fdt_check_header(fdt) != 0
		|| fdt_node_check_compatible(fdt, 0, "radxa,rock-5-itx") != 0
		|| fdt_node_check_compatible(fdt, 0, "rockchip,rk3588") != 0) {
		return false;
	}
	int node = fdt_path_offset(fdt, "/interrupt-controller@fe600000");
	if (node < 0 || fdt_node_check_compatible(fdt, node, "arm,gic-v3") != 0
		|| fdt_getprop(fdt, node, "msi-controller", nullptr) == nullptr)
		return false;
	const uint32_t rootCells[] = {2};
	const uint32_t regs[] = {0, 0xfe600000, 0, 0x10000, 0, 0xfe680000, 0, 0x100000};
	const uint32_t alias[] = {0, 0xfe610000};
	const uint32_t ranges[] = {424, 56};
	const uint32_t interruptCells[] = {4};
	return CellsMatch(fdt, 0, "#address-cells", rootCells, 1)
		&& CellsMatch(fdt, 0, "#size-cells", rootCells, 1)
		&& CellsMatch(fdt, node, "reg", regs, 8)
		&& CellsMatch(fdt, node, "mbi-alias", alias, 2)
		&& CellsMatch(fdt, node, "mbi-ranges", ranges, 2)
		&& CellsMatch(fdt, node, "#interrupt-cells", interruptCells, 1);
}

inline bool
RegistersMatch(uint32_t typer, uint32_t pidr2, uint32_t control, uint32_t group,
	uint32_t priority, uint32_t enabled, uint32_t pending, uint32_t active)
{
	// This is the measured controller, with MBIS and 512 total interrupt IDs.
	// With DS=0, IGROUPR is RAZ/WI from this non-secure OS. With DS=1,
	// its Group 1 bit is visible. Priority is readable for an accessible SPI
	// in either view; the kernel set its non-secure priority to 0x80.
	return typer == 0x7b040f && ((pidr2 >> 4) & 0xf) == 3
		&& (control & 0x80000012) == 0x12 && priority == 0x80
		&& ((control & 0x40) != 0 ? (group & kMask) != 0 : group == 0)
		&& ((enabled | pending | active) & kMask) == 0;
}

} // namespace Rock5MbiProbe
#endif
