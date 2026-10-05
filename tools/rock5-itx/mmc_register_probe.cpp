/* Read-only diagnostics for the admitted ROCK 5 ITX v1.12 eMMC controller. */
#include <KernelExport.h>
#include <OS.h>
#include <poke.h>

#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../src/add-ons/kernel/busses/mmc/rk3588_profile.h"

static bool
Map(int fd, phys_addr_t address, mem_map_args& mapping)
{
	mapping = {};
	mapping.signature = POKE_SIGNATURE;
	mapping.name = "ROCK5 eMMC register read";
	mapping.physical_address = address;
	mapping.size = B_PAGE_SIZE;
	mapping.flags = B_ANY_ADDRESS | B_UNCACHED_MEMORY;
	mapping.protection = B_READ_AREA;
	if (ioctl(fd, POKE_MAP_MEMORY, &mapping, sizeof(mapping)) < 0) {
		perror("POKE_MAP_MEMORY");
		return false;
	}
	return true;
}

int
main(int argc, char** argv)
{
	if (argc != 2 || strcmp(argv[1], RK3588Mmc::kProfile) != 0)
		return 2;
	int fd = open(POKE_DEVICE_FULLNAME, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	mem_map_args mapping;
	if (!Map(fd, RK3588Mmc::kBase, mapping)) {
		close(fd);
		return 1;
	}
	volatile const uint32* words = (volatile const uint32*)mapping.address;
	if (!RK3588Mmc::Controller(words[0x40 / 4], words[0x44 / 4],
			(words[0xfc / 4] >> 16) & 255, words[0xe8 / 4])) {
		fprintf(stderr, "eMMC controller identity rejected\n");
		delete_area(mapping.area);
		close(fd);
		return 1;
	}
	static const unsigned kOffsets[] = {0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
		0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0xe8, 0xfc,
		0x800, 0x804, 0x808, 0x80c, 0x810};
	for (unsigned offset : kOffsets)
		printf("EMMC %03x=%08" B_PRIx32 "\n", offset, words[offset / 4]);
	volatile const uint8* bytes = (volatile const uint8*)mapping.address;
	printf("EMMC 508=%02x\n", bytes[0x508]);
	printf("EMMC 52c=%04x\n", *(volatile const uint16*)(bytes + 0x52c));
	bool pass = delete_area(mapping.area) == B_OK;
	if (pass && Map(fd, RK3588Mmc::kCru, mapping)) {
		words = (volatile const uint32*)mapping.address;
		printf("CRU 434=%08" B_PRIx32 "\n", words[RK3588Mmc::kClockOffset / 4]);
		pass = delete_area(mapping.area) == B_OK;
	} else
		pass = false;
	// TRM Part 1 BUS_IOC GPIO2A/GPIO2D mux, and EMMC_IOC pad controls.
	// Part 2's interface table calls the mux registers EMMC_IOC, but the
	// Part 1 register map and installed board firmware place them in BUS_IOC.
	if (pass && Map(fd, 0xfd5f8000, mapping)) {
		words = (volatile const uint32*)mapping.address;
		for (unsigned offset : {0x40u, 0x58u, 0x5cu})
			printf("BUS_IOC %03x=%08" B_PRIx32 "\n", offset, words[offset / 4]);
		pass = delete_area(mapping.area) == B_OK;
	} else
		pass = false;
	if (pass && Map(fd, 0xfd5fd000, mapping)) {
		words = (volatile const uint32*)mapping.address;
		static const unsigned kPadOffsets[] = {0x40, 0x58, 0x5c, 0x120, 0x12c,
			0x190, 0x19c, 0x220, 0x22c, 0x290};
		for (unsigned offset : kPadOffsets)
			printf("EMMC_IOC %03x=%08" B_PRIx32 "\n", offset, words[offset / 4]);
		pass = delete_area(mapping.area) == B_OK;
	} else
		pass = false;
	close(fd);
	printf("ROCK5_EMMC_REGISTER_READ_%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
