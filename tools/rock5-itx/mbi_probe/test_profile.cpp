#include "profile.h"

#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace Rock5MbiProbe;

int
main(int argc, char** argv)
{
	if (argc > 2)
		return 2;
	std::vector<uint8_t> source(1024 * 1024);
	if (argc == 2) {
		FILE* input = fopen(argv[1], "rb");
		if (input == nullptr)
			return 2;
		size_t bytes = fread(source.data(), 1, source.size(), input);
		fclose(input);
		assert(bytes > 0 && bytes < source.size());
	} else {
		void* fdt = source.data();
		assert(fdt_create_empty_tree(fdt, source.size()) == 0);
		const char compatible[] = "radxa,rock-5-itx\0rockchip,rk3588";
		assert(fdt_setprop(fdt, 0, "compatible", compatible, sizeof(compatible)) == 0);
		assert(fdt_setprop_u32(fdt, 0, "#address-cells", 2) == 0);
		assert(fdt_setprop_u32(fdt, 0, "#size-cells", 2) == 0);
		int node = fdt_add_subnode(fdt, 0, "interrupt-controller@fe600000");
		assert(node >= 0);
		assert(fdt_setprop_string(fdt, node, "compatible", "arm,gic-v3") == 0);
		assert(fdt_setprop_u32(fdt, node, "#interrupt-cells", 4) == 0);
		assert(fdt_setprop_empty(fdt, node, "msi-controller") == 0);
		fdt32_t regs[] = {0, cpu_to_fdt32(0xfe600000), 0, cpu_to_fdt32(0x10000),
			0, cpu_to_fdt32(0xfe680000), 0, cpu_to_fdt32(0x100000)};
		fdt32_t alias[] = {0, cpu_to_fdt32(0xfe610000)};
		fdt32_t ranges[] = {cpu_to_fdt32(424), cpu_to_fdt32(56)};
		assert(fdt_setprop(fdt, node, "reg", regs, sizeof(regs)) == 0);
		assert(fdt_setprop(fdt, node, "mbi-alias", alias, sizeof(alias)) == 0);
		assert(fdt_setprop(fdt, node, "mbi-ranges", ranges, sizeof(ranges)) == 0);
	}
	const char* profile = "rock5-itx-edk2-v1.1-dt-mbi-test";
	assert(FirmwareMatches(source.data(), profile));
	assert(!FirmwareMatches(source.data(), nullptr));
	assert(!FirmwareMatches(source.data(), "rock5-itx-edk2-v1.1-dt-onboard"));
	assert(!FirmwareMatches(nullptr, profile));
	std::vector<uint8_t> changed(source.size() + 4096);
	const char* properties[] = {"compatible", "reg", "mbi-alias", "mbi-ranges",
		"#interrupt-cells", "msi-controller"};
	for (const char* property : properties) {
		assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
		int node = fdt_path_offset(changed.data(), "/interrupt-controller@fe600000");
		assert(node >= 0 && fdt_delprop(changed.data(), node, property) == 0);
		assert(!FirmwareMatches(changed.data(), profile));
	}
	assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
	assert(fdt_setprop_string(changed.data(), 0, "compatible", "radxa,rock-5b") == 0);
	assert(!FirmwareMatches(changed.data(), profile));
	for (uint32_t address : {0xfe600000u, 0xfe620000u, 0u}) {
		assert(fdt_open_into(source.data(), changed.data(), changed.size()) == 0);
		int node = fdt_path_offset(changed.data(), "/interrupt-controller@fe600000");
		fdt32_t alias[] = {cpu_to_fdt32(0), cpu_to_fdt32(address)};
		assert(fdt_setprop(changed.data(), node, "mbi-alias", alias, sizeof(alias)) == 0);
		assert(!FirmwareMatches(changed.data(), profile));
	}
	assert(RegistersMatch(0x7b040f, 0x3b, 0x13, 0, 0x80, 0, 0, 0));
	assert(RegistersMatch(0x7b040f, 0x3b, 0x53, kMask, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x7b040f, 0x3b, 0x53, 0, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x7b040f, 0x3b, 0x13, kMask, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x7b040f, 0x3b, 0x13, 0, 0, 0, 0, 0));
	for (uint32_t control : {0x80000013u, 0x3u, 0x11u, 0u})
		assert(!RegistersMatch(0x7b040f, 0x3b, control, 0, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x37a0008, 0x3b, 0x13, 0, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x7a040f, 0x3b, 0x13, 0, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x7b040f, 0x2b, 0x13, 0, 0x80, 0, 0, 0));
	assert(!RegistersMatch(0x7b040f, 0x3b, 0x13, 0, 0x80, kMask, 0, 0));
	assert(!RegistersMatch(0x7b040f, 0x3b, 0x13, 0, 0x80, 0, kMask, 0));
	assert(!RegistersMatch(0x7b040f, 0x3b, 0x13, 0, 0x80, 0, 0, kMask));
	assert(RegistersMatch(0x7b040f, 0x3b, 0x13, 0, 0x80, ~kMask, ~kMask, ~kMask));
	assert(kVector >= 454 && kVector <= 479 && kVector == 464);
	puts("ROCK5_MBI_PROFILE_TEST_PASS");
	return 0;
}
