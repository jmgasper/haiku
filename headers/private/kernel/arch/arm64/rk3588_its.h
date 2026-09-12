/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARM64_RK3588_ITS_H
#define ARM64_RK3588_ITS_H

#include <arch/arm64/rk3588_mbi.h>

namespace Rk3588Its {

static const uint64_t kController = UINT64_C(0xfe660000);
static const uint32_t kDevice = 0x100;
static const uint64_t kTyper = UINT64_C(0x130001ef31);
static const uint32_t kIidr = 0x0201743b;

inline bool
EnabledNode(const void* fdt, int node)
{
	int length = 0;
	const char* status = (const char*)fdt_getprop(fdt, node, "status", &length);
	return status == nullptr || (length == 5 && memcmp(status, "okay", 5) == 0)
		|| (length == 3 && memcmp(status, "ok", 3) == 0);
}

inline bool
FirmwareMatches(const void* fdt)
{
	// This initial provider is confined to the captured EDK2 v1.1 DT profile.
	if (!Gicv3Mbi::FirmwareMatches(fdt))
		return false;
	int gic = fdt_path_offset(fdt, "/interrupt-controller@fe600000");
	int its = fdt_subnode_offset(fdt, gic, "msi-controller@fe660000");
	int pci = fdt_path_offset(fdt, "/pcie@fe150000");
	if (its < 0 || pci < 0 || !EnabledNode(fdt, gic) || !EnabledNode(fdt, its)
		|| !EnabledNode(fdt, pci)
		|| fdt_node_check_compatible(fdt, its, "arm,gic-v3-its") != 0
		|| fdt_node_check_compatible(fdt, pci, "rockchip,rk3588-pcie") != 0)
		return false;
	const uint32_t two[] = {2}, one[] = {1}, zero[] = {0};
	const uint32_t regs[] = {0, 0xfe660000, 0, 0x20000};
	const uint32_t buses[] = {0, 15};
	uint32_t phandle = fdt_get_phandle(fdt, its);
	const uint32_t map[] = {0, phandle, 0, 0x1000};
	int length = -1;
	if (phandle == 0 || fdt_getprop(fdt, gic, "ranges", &length) == nullptr || length != 0
		|| fdt_getprop(fdt, its, "msi-controller", nullptr) == nullptr
		|| fdt_getprop(fdt, its, "dma-noncoherent", nullptr) == nullptr
		|| fdt_getprop(fdt, gic, "dma-coherent", nullptr) != nullptr
		|| fdt_getprop(fdt, its, "dma-coherent", nullptr) != nullptr
		|| fdt_getprop(fdt, pci, "msi-map-mask", nullptr) != nullptr)
		return false;
	return Gicv3Mbi::CellsMatch(fdt, gic, "#address-cells", two, 1)
		&& Gicv3Mbi::CellsMatch(fdt, gic, "#size-cells", two, 1)
		&& Gicv3Mbi::CellsMatch(fdt, its, "#msi-cells", one, 1)
		&& Gicv3Mbi::CellsMatch(fdt, its, "reg", regs, 4)
		&& Gicv3Mbi::CellsMatch(fdt, pci, "linux,pci-domain", zero, 1)
		&& Gicv3Mbi::CellsMatch(fdt, pci, "bus-range", buses, 2)
		&& Gicv3Mbi::CellsMatch(fdt, pci, "msi-map", map, 4);
}

inline bool
RequesterMatches(uint64_t controller, uint32_t device)
{
	// EDK2 keeps segment-zero's endpoint at 01:00.0, as Linux does. Other
	// firmware roots also use local bus 1; do not invent a segment-ID prefix.
	return controller == kController && device == kDevice;
}

} // namespace Rk3588Its
#endif
