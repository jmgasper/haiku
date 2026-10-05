#include <arch/arm64/rk3588_its.h>
#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace Rk3588Its;

static void Cells(void* fdt, int node, const char* key,
	std::initializer_list<uint32_t> values)
{
	std::vector<fdt32_t> cells;
	for (uint32_t v : values) cells.push_back(cpu_to_fdt32(v));
	assert(fdt_setprop(fdt, node, key, cells.data(), cells.size() * 4) == 0);
}

static void Synthetic(void* fdt, size_t size)
{
	assert(fdt_create_empty_tree(fdt, size) == 0);
	const char compatible[] = "radxa,rock-5-itx\0rockchip,rk3588";
	assert(fdt_setprop(fdt, 0, "compatible", compatible, sizeof(compatible)) == 0);
	Cells(fdt, 0, "#address-cells", {2});
	Cells(fdt, 0, "#size-cells", {2});
	int gic = fdt_add_subnode(fdt, 0, "interrupt-controller@fe600000");
	assert(gic >= 0);
	assert(fdt_setprop_string(fdt, gic, "compatible", "arm,gic-v3") == 0);
	assert(fdt_setprop_empty(fdt, gic, "msi-controller") == 0);
	assert(fdt_setprop_empty(fdt, gic, "ranges") == 0);
	Cells(fdt, gic, "#interrupt-cells", {4});
	Cells(fdt, gic, "#address-cells", {2});
	Cells(fdt, gic, "#size-cells", {2});
	Cells(fdt, gic, "reg", {0, 0xfe600000, 0, 0x10000, 0, 0xfe680000, 0, 0x100000});
	Cells(fdt, gic, "mbi-alias", {0, 0xfe610000});
	Cells(fdt, gic, "mbi-ranges", {424, 56});
	int its = fdt_add_subnode(fdt, gic, "msi-controller@fe660000");
	assert(its >= 0);
	assert(fdt_setprop_string(fdt, its, "compatible", "arm,gic-v3-its") == 0);
	assert(fdt_setprop_empty(fdt, its, "msi-controller") == 0);
	assert(fdt_setprop_empty(fdt, its, "dma-noncoherent") == 0);
	Cells(fdt, its, "#msi-cells", {1});
	Cells(fdt, its, "reg", {0, 0xfe660000, 0, 0x20000});
	Cells(fdt, its, "phandle", {17});
	int wifiIts = fdt_add_subnode(fdt, gic, "msi-controller@fe640000");
	assert(wifiIts >= 0);
	assert(fdt_setprop_string(fdt, wifiIts, "compatible", "arm,gic-v3-its") == 0);
	assert(fdt_setprop_empty(fdt, wifiIts, "msi-controller") == 0);
	assert(fdt_setprop_empty(fdt, wifiIts, "dma-noncoherent") == 0);
	Cells(fdt, wifiIts, "#msi-cells", {1});
	Cells(fdt, wifiIts, "reg", {0, 0xfe640000, 0, 0x20000});
	Cells(fdt, wifiIts, "phandle", {18});
	int pci = fdt_add_subnode(fdt, 0, "pcie@fe150000");
	assert(pci >= 0);
	assert(fdt_setprop_string(fdt, pci, "compatible", "rockchip,rk3588-pcie") == 0);
	Cells(fdt, pci, "linux,pci-domain", {0});
	Cells(fdt, pci, "bus-range", {0, 15});
	Cells(fdt, pci, "msi-map", {0, 17, 0, 0x1000});
	int wifiPci = fdt_add_subnode(fdt, 0, "pcie@fe170000");
	assert(wifiPci >= 0);
	assert(fdt_setprop_string(fdt, wifiPci, "compatible", "rockchip,rk3588-pcie") == 0);
	Cells(fdt, wifiPci, "linux,pci-domain", {2});
	Cells(fdt, wifiPci, "bus-range", {0x20, 0x2f});
	Cells(fdt, wifiPci, "msi-map", {0x2000, 18, 0x2000, 0x1000});
}

int main(int argc, char** argv)
{
	assert(argc == 1 || argc == 2);
	std::vector<uint8_t> source(1024 * 1024), changed(source.size() + 4096);
	if (argc == 1) Synthetic(source.data(), source.size());
	else {
		FILE* input = fopen(argv[1], "rb");
		assert(input != nullptr);
		size_t bytes = fread(source.data(), 1, source.size(), input);
		fclose(input);
		assert(bytes > 0 && bytes < source.size());
	}
	assert(FirmwareMatches(source.data()));
	assert(!FirmwareMatches(nullptr));
	const char* paths[] = {"/", "/pcie@fe150000", "/interrupt-controller@fe600000",
		"/interrupt-controller@fe600000/msi-controller@fe660000",
		"/pcie@fe170000",
		"/interrupt-controller@fe600000/msi-controller@fe640000"};
	for (const char* path : paths) {
		assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
		int node = fdt_path_offset(changed.data(), path);
		assert(node >= 0);
		assert(fdt_setprop_string(changed.data(), node, "compatible", "test,other") == 0);
		assert(!FirmwareMatches(changed.data()));
	}
	for (const char* path : {paths[1], paths[2], paths[3], paths[4], paths[5]}) {
		assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
		int node = fdt_path_offset(changed.data(), path);
		assert(fdt_setprop_string(changed.data(), node, "status", "disabled") == 0);
		assert(!FirmwareMatches(changed.data()));
	}
	for (const char* property : {"msi-map", "bus-range", "linux,pci-domain"}) {
		for (const char* path : {paths[1], paths[4]}) {
			assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
			int pci = fdt_path_offset(changed.data(), path);
			assert(fdt_delprop(changed.data(), pci, property) == 0);
			assert(!FirmwareMatches(changed.data()));
		}
	}
	for (const char* property : {"reg", "#msi-cells", "msi-controller", "dma-noncoherent"}) {
		for (const char* path : {paths[3], paths[5]}) {
			assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
			int its = fdt_path_offset(changed.data(), path);
			assert(fdt_delprop(changed.data(), its, property) == 0);
			assert(!FirmwareMatches(changed.data()));
		}
	}
	// A valid-looking map to the wrong ITS or a nonidentity translation is unsafe.
	for (unsigned field = 0; field < 4; field++) {
		for (const char* path : {paths[1], paths[4]}) {
			assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
			int pci = fdt_path_offset(changed.data(), path);
			int length;
			auto cells = (fdt32_t*)fdt_getprop_w(changed.data(), pci, "msi-map", &length);
			assert(cells != nullptr && length == 16);
			cells[field] = cpu_to_fdt32(fdt32_to_cpu(cells[field]) ^ 1);
			assert(!FirmwareMatches(changed.data()));
		}
	}
	assert(RequesterMatches(0xfe660000, 0x100));
	assert(RequesterMatches(0xfe640000, 0x100));
	for (uint32_t device : {0u, 0x101u, 0x1100u, 0x3100u, 0x4100u, UINT32_MAX})
		assert(!RequesterMatches(0xfe660000, device));
	assert(!RequesterMatches(0xfe640000, 0x2100));
	assert(!RequesterMatches(0xfe660000, 0x2100));
	assert(!RequesterMatches(0xfe670040, 0x100));
	puts("ROCK5_ITS_PROFILE_PASS");
}
