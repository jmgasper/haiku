// SPDX-License-Identifier: MIT
// Read known PCI configuration functions through Haiku's physical mappings.
// The ROCK addresses match the installed EDK2 v1.1 PCI segment implementation.
#include <KernelExport.h>
#include <OS.h>
#include <poke.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct ConfigFunction {
	const char* name;
	phys_addr_t address;
	uint32 identity;
	uint32 classCode;
};

static const ConfigFunction kQemu[] = {
	{"host", 0x4010000000ULL, 0x00081b36, 0x060000},
	{"nvme", 0x4010018000ULL, 0x00101b36, 0x010802}
};

static const ConfigFunction kRock[] = {
	{"root", 0xa40000000ULL, 0x35881d87, 0x060400},
	{"nvme", 0x900100000ULL, 0xa802144d, 0x010802}
};

static bool
ReadFunction(int fd, const ConfigFunction& function)
{
	printf("PCI_MAP_BEGIN function=%s address=%#" B_PRIxPHYSADDR "\n",
		function.name, function.address);
	fflush(stdout);
	mem_map_args mapping = {};
	mapping.signature = POKE_SIGNATURE;
	mapping.name = "ROCK5 PCI configuration read";
	mapping.physical_address = function.address;
	mapping.size = B_PAGE_SIZE;
	mapping.flags = B_ANY_ADDRESS | B_UNCACHED_MEMORY;
	mapping.protection = B_READ_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mapping, sizeof(mapping)) < 0) {
		fprintf(stderr, "PCI_MAP_FAIL function=%s error=%s\n",
			function.name, strerror(errno));
		return false;
	}

	volatile const uint32* registers = static_cast<volatile const uint32*>(mapping.address);
	uint32 identity = registers[0];
	if (identity != function.identity) {
		fprintf(stderr, "PCI_ID_MISMATCH function=%s actual=%08" B_PRIx32
			" expected=%08" B_PRIx32 "\n", function.name, identity, function.identity);
		delete_area(mapping.area);
		return false;
	}

	uint32 words[64];
	// Volatile 32-bit loads match the firmware's PCI configuration accesses.
	// No configuration writes or controller address-window changes are made.
	for (size_t i = 0; i < 64; i++)
		words[i] = registers[i];
	status_t unmap = delete_area(mapping.area);
	if (unmap != B_OK || words[2] >> 8 != function.classCode) {
		fprintf(stderr, "PCI_CONFIG_FAIL function=%s class=%06" B_PRIx32
			" unmap=%" B_PRId32 "\n", function.name, words[2] >> 8, unmap);
		return false;
	}

	printf("PCI_CONFIG_BEGIN function=%s address=%#" B_PRIxPHYSADDR "\n",
		function.name, function.address);
	for (size_t i = 0; i < 64; i += 4) {
		printf("%03" B_PRIxSIZE ": %08" B_PRIx32 " %08" B_PRIx32
			" %08" B_PRIx32 " %08" B_PRIx32 "\n", i * sizeof(uint32),
			words[i], words[i + 1], words[i + 2], words[i + 3]);
	}
	printf("PCI_CONFIG_END function=%s\n", function.name);
	return true;
}

int
main(int argc, char** argv)
{
#if !defined(__aarch64__)
	fprintf(stderr, "This diagnostic requires ARM64 Haiku.\n");
	return 1;
#endif
	if (argc != 2 || (strcmp(argv[1], "qemu") != 0
		&& strcmp(argv[1], "rock5-efi-v1.1") != 0)) {
		fprintf(stderr, "Usage: %s qemu|rock5-efi-v1.1\n", argv[0]);
		return 2;
	}
	const ConfigFunction* functions = strcmp(argv[1], "qemu") == 0 ? kQemu : kRock;
	int fd = open(POKE_DEVICE_FULLNAME, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open PCI mapping interface: %s\n", strerror(errno));
		return 1;
	}
	bool passed = ReadFunction(fd, functions[0]) && ReadFunction(fd, functions[1]);
	close(fd);
	printf("ROCK5_PCI_CONFIG_%s profile=%s functions=%u\n",
		passed ? "PASS" : "FAIL", argv[1], passed ? 2 : 0);
	return passed ? 0 : 1;
}
