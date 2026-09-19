/* Can we reach the Wi-Fi half of the card at all?
 *
 * The Bluetooth half of this MT7922 turned out to be reachable long before
 * anything resembling a driver existed, and knowing that early is what made
 * the rest tractable. This asks the same question of the Wi-Fi half, which
 * hangs off PCI rather than USB and which no driver on this system claims.
 *
 * Haiku's poke driver hands userland the two things needed: the PCI
 * configuration of every device, and a mapping of any physical address. So the
 * card's registers can be read from an ordinary program, and a wrong guess
 * costs a rerun instead of a reboot.
 *
 * usage: wifiprobe [offset-hex [count]]
 *        with no arguments, reports what the card looks like on the bus
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>
#include <PCI.h>

#include "poke.h"

#define MTK_VENDOR	0x14c3
#define MTK_DEVICE	0x7922


static int sPoke = -1;


static status_t
FindCard(pci_info* found)
{
	for (uint8 index = 0; index < 255; index++) {
		pci_info info;
		pci_info_args args;

		memset(&info, 0, sizeof(info));
		args.signature = POKE_SIGNATURE;
		args.index = index;
		args.info = &info;
		args.status = B_OK;

		if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) < 0)
			break;
		if (args.status != B_OK)
			break;

		if (info.vendor_id == MTK_VENDOR && info.device_id == MTK_DEVICE) {
			*found = info;
			return B_OK;
		}
	}

	return B_ENTRY_NOT_FOUND;
}


static uint32
ReadConfig(const pci_info& info, uint8 offset, uint8 size)
{
	pci_io_args args;
	args.signature = POKE_SIGNATURE;
	args.bus = info.bus;
	args.device = info.device;
	args.function = info.function;
	args.size = size;
	args.offset = offset;
	args.value = 0;

	if (ioctl(sPoke, POKE_PCI_READ_CONFIG, &args, sizeof(args)) < 0)
		return 0xffffffff;
	return args.value;
}


static void
WriteConfig(const pci_info& info, uint8 offset, uint8 size, uint32 value)
{
	pci_io_args args;
	args.signature = POKE_SIGNATURE;
	args.bus = info.bus;
	args.device = info.device;
	args.function = info.function;
	args.size = size;
	args.offset = offset;
	args.value = value;

	ioctl(sPoke, POKE_PCI_WRITE_CONFIG, &args, sizeof(args));
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	sPoke = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (sPoke < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", POKE_DEVICE_FULLNAME,
			strerror(errno));
		return 1;
	}

	pci_info info;
	if (FindCard(&info) != B_OK) {
		fprintf(stderr, "[!] no %04x:%04x on this machine\n", MTK_VENDOR,
			MTK_DEVICE);
		close(sPoke);
		return 1;
	}

	printf("MediaTek %04x:%04x at %d:%d:%d\n", info.vendor_id, info.device_id,
		info.bus, info.device, info.function);
	printf("  revision %#x, interrupt line %d\n", info.revision,
		info.u.h0.interrupt_line);

	/* A device only answers reads of its registers once memory space is
	 * switched on, and only moves data by itself once it is allowed to drive
	 * the bus. The firmware may have left either off.
	 */
	uint32 command = ReadConfig(info, PCI_command, 2);
	printf("  command register %#06x: memory %s, bus master %s\n",
		(unsigned)command,
		(command & PCI_command_memory) != 0 ? "on" : "OFF",
		(command & PCI_command_master) != 0 ? "on" : "OFF");

	if ((command & (PCI_command_memory | PCI_command_master))
			!= (PCI_command_memory | PCI_command_master)) {
		command |= PCI_command_memory | PCI_command_master;
		WriteConfig(info, PCI_command, 2, command);
		printf("  turned both on: now %#06x\n",
			(unsigned)ReadConfig(info, PCI_command, 2));
	}

	/* A card nobody has claimed may still be asleep. A sleeping device answers
	 * configuration cycles but not reads of its registers, which is exactly
	 * what a window full of zeroes looks like - so find the power management
	 * capability and wake it before believing anything the registers say.
	 */
	if ((ReadConfig(info, PCI_status, 2) & PCI_status_capabilities) != 0) {
		uint8 at = ReadConfig(info, PCI_capabilities_ptr, 1) & 0xfc;

		while (at != 0) {
			uint8 id = ReadConfig(info, at, 1);
			uint8 next = ReadConfig(info, at + 1, 1) & 0xfc;

			if (id == PCI_cap_id_pm) {
				uint16 control = ReadConfig(info, at + PCI_pm_status, 2);
				int state = control & PCI_pm_mask;

				printf("  power state D%d\n", state);
				if (state != 0) {
					WriteConfig(info, at + PCI_pm_status, 2,
						(control & ~PCI_pm_mask) | PCI_pm_state_d0);
					/* Coming out of D3 takes time, and reads before it is
					 * done come back as rubbish.
					 */
					snooze(20000);
					printf("  woke it: now D%d\n",
						ReadConfig(info, at + PCI_pm_status, 2) & PCI_pm_mask);
				}
				break;
			}
			at = next;
		}
	}

	/* Which window to look through; the first memory one unless told. */
	int bar = -1;
	const char* wanted = getenv("WIFIPROBE_BAR");
	int wantedBar = wanted != NULL ? atoi(wanted) : -1;

	for (int i = 0; i < 6; i++) {
		if (info.u.h0.base_register_sizes[i] == 0)
			continue;
		printf("  BAR %d: %#" B_PRIxPHYSADDR " for %#" B_PRIxSIZE " bytes%s\n",
			i, info.u.h0.base_registers[i],
			(size_t)info.u.h0.base_register_sizes[i],
			(info.u.h0.base_register_flags[i] & PCI_address_space) != 0
				? " (i/o)" : "");
		if ((info.u.h0.base_register_flags[i] & PCI_address_space) != 0)
			continue;
		if (wantedBar >= 0 ? i == wantedBar : bar < 0)
			bar = i;
	}

	if (bar < 0) {
		fprintf(stderr, "[!] the card exposes no memory window\n");
		close(sPoke);
		return 1;
	}

	/* Map the first memory window. Everything the chip says about itself is
	 * read through here.
	 */
	mem_map_args map;
	memset(&map, 0, sizeof(map));
	map.signature = POKE_SIGNATURE;
	map.name = "mt7922 registers";
	map.physical_address = info.u.h0.base_registers[bar];
	map.size = info.u.h0.base_register_sizes[bar];
	map.flags = B_ANY_ADDRESS;
	map.protection = B_READ_AREA | B_WRITE_AREA;
	map.address = NULL;

	/* A mapping that worked comes back as the area it was put in, which is a
	 * positive number rather than a zero.
	 */
	if (ioctl(sPoke, POKE_MAP_MEMORY, &map, sizeof(map)) < 0) {
		fprintf(stderr, "[!] cannot map BAR %d: %s\n", bar, strerror(errno));
		close(sPoke);
		return 1;
	}

	volatile uint32* registers = (volatile uint32*)map.address;
	printf("\nmapped BAR %d at %p\n", bar, map.address);

	/* Sweep the whole window for anything that is not zero. Reading a register
	 * is harmless where writing one is not, and knowing which parts of the
	 * window are alive says a great deal about what the card is doing.
	 */
	if (argc > 1 && strcmp(argv[1], "scan") == 0) {
		size_t live = 0;
		size_t shown = 0;

		for (size_t at = 0; at + 4 <= map.size; at += 4) {
			uint32 value = registers[at / 4];
			if (value == 0 || value == 0xffffffff)
				continue;
			live++;
			if (shown < 512) {
				printf("  %#08zx = %#010x\n", at, (unsigned)value);
				shown++;
			}
		}

		printf("\n%zu registers in the %#zx byte window are not empty\n",
			live, map.size);

		mem_map_args unmapScan;
		memset(&unmapScan, 0, sizeof(unmapScan));
		unmapScan.signature = POKE_SIGNATURE;
		unmapScan.area = map.area;
		ioctl(sPoke, POKE_UNMAP_MEMORY, &unmapScan, sizeof(unmapScan));
		close(sPoke);
		return 0;
	}

	size_t offset = argc > 1 ? strtoul(argv[1], NULL, 16) : 0;
	int count = argc > 2 ? atoi(argv[2]) : 8;

	printf("registers from %#zx:\n", offset);
	for (int i = 0; i < count; i++) {
		size_t at = offset + i * 4;
		if (at + 4 > map.size) {
			printf("  %#08zx is past the end of the window\n", at);
			break;
		}
		printf("  %#08zx = %#010x\n", at,
			(unsigned)registers[at / 4]);
	}

	/* All ones means nothing answered - the window is mapped but the card is
	 * not driving it, which is what a device held in reset looks like.
	 */
	if (registers[0] == 0xffffffff)
		printf("\nthe window reads all ones: nothing is answering\n");
	else
		printf("\nthe card answers reads of its registers\n");

	mem_map_args unmap;
	memset(&unmap, 0, sizeof(unmap));
	unmap.signature = POKE_SIGNATURE;
	unmap.area = map.area;
	ioctl(sPoke, POKE_UNMAP_MEMORY, &unmap, sizeof(unmap));

	close(sPoke);
	return 0;
}
