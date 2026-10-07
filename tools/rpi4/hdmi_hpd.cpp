/* Read-only HDMI hotplug diagnostics for the BCM2711 Raspberry Pi 4. */
#include <KernelExport.h>
#include <OS.h>
#include <poke.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char** argv)
{
	if (argc != 2 || strcmp(argv[1], "bcm2711") != 0) {
		fprintf(stderr, "usage: %s bcm2711\n", argv[0]);
		return 2;
	}
	int fd = open(POKE_DEVICE_FULLNAME, O_RDONLY);
	if (fd < 0) {
		perror("open poke");
		return 1;
	}
	mem_map_args maps[2] = {};
	for (unsigned i = 0; i < 2; i++) {
		mem_map_args& map = maps[i];
		map.signature = POKE_SIGNATURE;
		map.name = "BCM2711 HDMI HPD read";
		map.physical_address = 0xfef00000 + i * 0x5000;
		map.size = B_PAGE_SIZE;
		map.flags = B_ANY_ADDRESS | B_UNCACHED_MEMORY;
		map.protection = B_READ_AREA;
		if (ioctl(fd, POKE_MAP_MEMORY, &map, sizeof(map)) < 0) {
			perror("map HDMI");
			return 1;
		}
	}
	// bcm2711.dtsi HDMI0/1 register base + VC5 HDMI_HOTPLUG (0x1a8).
	for (unsigned sample = 0; sample < 100; sample++) {
		printf("HPD %u HDMI0=%08" B_PRIx32 " HDMI1=%08" B_PRIx32 "\n",
			sample, *(volatile const uint32*)((uint8*)maps[0].address + 0x8a8),
			*(volatile const uint32*)((uint8*)maps[1].address + 0x8a8));
		fflush(stdout);
		snooze(200000);
	}
	for (unsigned i = 0; i < 2; i++)
		delete_area(maps[i].area);
	close(fd);
	return 0;
}
