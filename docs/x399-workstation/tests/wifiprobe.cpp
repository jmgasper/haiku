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

/* The chip keeps its register domain asleep until the host asks for it, and a
 * sleeping domain reads back as zeroes rather than as nothing at all. The
 * asking is done through one register that is always reachable, at a fixed
 * place in the window - no remapping needed, which is what makes it safe to do
 * before anything else.
 */
#define MT_CONN_ON_LPCTL	0x0e0010
#define LPCTL_HOST_SET_OWN	(1 << 0)
#define LPCTL_HOST_CLR_OWN	(1 << 1)
#define LPCTL_HOST_OWN_SYNC	(1 << 2)

/* Everything else in the part is reached through a 64 KiB window that the host
 * points at whatever it wants to see. The upper half of the target address is
 * written into the LOWER half of the control register - the one place this is
 * easy to get backwards.
 */
#define MT_HIF_REMAP_L1		0x0fe24c
#define MT_HIF_REMAP_BASE_L1	0x040000

/* The Wi-Fi subsystem's own reset. Held low, then released, then it says when
 * it has finished starting itself. This part is a connac2, so the register is
 * the 0x18000000 one rather than the older 0x7c000000.
 */
#define MT_WFSYS_SW_RST_B	0x18000140
#define WFSYS_SW_RST_B		(1 << 0)
#define WFSYS_SW_INIT_DONE	(1 << 4)

#define MT_HW_CHIPID		0x70010200
#define MT_HW_REV		0x70010204


static bool
PollRegister(volatile uint32* registers, size_t offset, uint32 mask,
	uint32 wanted, int milliseconds)
{
	for (int i = 0; i <= milliseconds; i++) {
		if ((registers[offset / 4] & mask) == wanted)
			return true;
		snooze(1000);
	}
	return false;
}


/* Take the register domain from the firmware. The chip is first pushed into
 * firmware-own whatever state it was left in, so that what follows is a clean
 * handover rather than a guess about where it started.
 */
static bool
TakeOwnership(volatile uint32* registers)
{
	const int kRetries = 10;

	for (int i = 0; i < kRetries; i++) {
		registers[MT_CONN_ON_LPCTL / 4] = LPCTL_HOST_SET_OWN;
		if (PollRegister(registers, MT_CONN_ON_LPCTL, LPCTL_HOST_OWN_SYNC,
				LPCTL_HOST_OWN_SYNC, 50)) {
			break;
		}
		if (i == kRetries - 1) {
			printf("  the firmware would not take the registers back\n");
			return false;
		}
	}
	printf("  firmware owns the registers\n");

	for (int i = 0; i < kRetries; i++) {
		registers[MT_CONN_ON_LPCTL / 4] = LPCTL_HOST_CLR_OWN;
		/* A link that can go to sleep needs a moment before it answers. */
		snooze(3000);
		if (PollRegister(registers, MT_CONN_ON_LPCTL, LPCTL_HOST_OWN_SYNC, 0,
				50)) {
			printf("  we own the registers\n");
			return true;
		}
	}

	printf("  the firmware would not hand the registers over\n");
	return false;
}


/* Point the window at an address and give back where to read it. The write
 * that moves the window is posted, so it is read back to make sure it has
 * landed before anything is read through it.
 */
static size_t
MapThroughWindow(volatile uint32* registers, uint32 address)
{
	uint32 base = address >> 16;
	uint32 offset = address & 0xffff;

	uint32 control = registers[MT_HIF_REMAP_L1 / 4];
	registers[MT_HIF_REMAP_L1 / 4] = (control & 0xffff0000) | base;
	(void)registers[MT_HIF_REMAP_L1 / 4];

	return MT_HIF_REMAP_BASE_L1 + offset;
}




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

	/* Where every device's windows sit. A window has to live in the hole the
	 * chipset leaves for them, above the memory; one that lands in the middle
	 * of RAM was never really assigned, and reads of it go to memory rather
	 * than to the card - which looks exactly like a card that answers with
	 * zeroes.
	 */
	if (argc > 1 && strcmp(argv[1], "bars") == 0) {
		for (uint8 index = 0; index < 255; index++) {
			pci_info one;
			pci_info_args args;

			memset(&one, 0, sizeof(one));
			args.signature = POKE_SIGNATURE;
			args.index = index;
			args.info = &one;
			args.status = B_OK;

			if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) < 0
				|| args.status != B_OK) {
				break;
			}
			/* A bridge forwards only the range it was given. A device window
			 * outside its bridge's range is never reached, however good the
			 * address looks.
			 */
			if ((one.header_type & PCI_header_type_mask)
					== PCI_header_type_PCI_to_PCI_bridge) {
				/* The bottom four bits of each of these are not part of the
				 * address: they say whether the window can reach above four
				 * gigabytes. Reading them as address makes a window look
				 * shifted, and a device inside one look as though it is
				 * outside.
				 */
				uint64 memoryBase = ((uint64)(one.u.h1.memory_base & 0xfff0))
					<< 16;
				uint64 memoryLimit
					= (((uint64)(one.u.h1.memory_limit & 0xfff0)) << 16)
						| 0xfffff;
				uint64 fetchBase
					= ((uint64)(one.u.h1.prefetchable_memory_base & 0xfff0))
						<< 16;
				uint64 fetchLimit
					= (((uint64)(one.u.h1.prefetchable_memory_limit & 0xfff0))
						<< 16) | 0xfffff;

				if ((one.u.h1.prefetchable_memory_base & 0xf) == 1) {
					fetchBase |= ((uint64)one.u.h1
						.prefetchable_memory_base_upper32) << 32;
					fetchLimit |= ((uint64)one.u.h1
						.prefetchable_memory_limit_upper32) << 32;
				}

				printf("%04x:%04x  %2d:%d:%d  bridge to bus %d, memory "
					"%#012" B_PRIx64 "-%#012" B_PRIx64 ", prefetchable "
					"%#012" B_PRIx64 "-%#012" B_PRIx64 "\n",
					one.vendor_id, one.device_id, one.bus, one.device,
					one.function, one.u.h1.secondary_bus,
					memoryBase, memoryLimit, fetchBase, fetchLimit);
				continue;
			}

			if ((one.header_type & PCI_header_type_mask) != 0)
				continue;

			for (int i = 0; i < 6; i++) {
				if (one.u.h0.base_register_sizes[i] == 0)
					continue;
				if ((one.u.h0.base_register_flags[i] & PCI_address_space) != 0)
					continue;
				printf("%04x:%04x  %2d:%d:%d  BAR %d  %#012" B_PRIxPHYSADDR
					"  %#9" B_PRIxSIZE "\n", one.vendor_id, one.device_id,
					one.bus, one.device, one.function, i,
					one.u.h0.base_registers[i],
					(size_t)one.u.h0.base_register_sizes[i]);
			}
		}
		close(sPoke);
		return 0;
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

	/* What the card itself says its windows are. A window that can reach above
	 * four gigabytes is written as two registers, and reading only the first
	 * of them gives an address that looks plausible and points at memory
	 * instead of at the card.
	 */
	uint64 trueAddress[6] = { 0, 0, 0, 0, 0, 0 };

	printf("  base address registers, as the card has them:\n");
	for (int i = 0; i < 6; i++) {
		uint32 low = ReadConfig(info, PCI_base_registers + i * 4, 4);
		if (low == 0)
			continue;

		bool is64 = (low & PCI_address_space) == 0
			&& (low & PCI_address_type) == PCI_address_type_64;
		uint64 address = low & PCI_address_memory_32_mask;

		if (is64) {
			uint32 high = ReadConfig(info, PCI_base_registers + (i + 1) * 4, 4);
			address |= ((uint64)high) << 32;
			printf("    %d: %#012" B_PRIx64 " (64 bit, upper half %#x)\n",
				i, address, (unsigned)high);
			trueAddress[i] = address;
			i++;
		} else {
			printf("    %d: %#012" B_PRIx64 "\n", i, address);
			trueAddress[i] = address;
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
	/* What the card says, not what was remembered about it: a window that can
	 * reach above four gigabytes is two registers wide, and an upper half
	 * dropped somewhere along the way leaves an address that points at memory.
	 */
	uint64 remembered = info.u.h0.base_registers[bar];
	if ((info.u.h0.base_register_flags[bar] & PCI_address_type)
			== PCI_address_type_64 && bar < 5) {
		remembered |= ((uint64)info.u.h0.base_registers[bar + 1]) << 32;
	}

	map.physical_address = remembered;
	printf("  mapping %#012" B_PRIx64 "\n", remembered);
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

	/* Drive the Wi-Fi subsystem through its own reset. Reading tells us the
	 * part is there; this tells us it can be made to do something, which is a
	 * different question and the one a driver depends on.
	 */
	if (argc > 1 && strcmp(argv[1], "reset") == 0) {
		if (!TakeOwnership(registers)) {
			printf("\nwithout the registers there is nothing to drive.\n");
		} else {
			size_t at = MapThroughWindow(registers, MT_WFSYS_SW_RST_B);
			uint32 before = registers[at / 4];
			printf("  reset register starts at %#010x\n", (unsigned)before);

			registers[at / 4] = before & ~WFSYS_SW_RST_B;
			snooze(50000);

			at = MapThroughWindow(registers, MT_WFSYS_SW_RST_B);
			registers[at / 4] = registers[at / 4] | WFSYS_SW_RST_B;

			at = MapThroughWindow(registers, MT_WFSYS_SW_RST_B);
			bool done = PollRegister(registers, at, WFSYS_SW_INIT_DONE,
				WFSYS_SW_INIT_DONE, 500);

			printf("  reset register now %#010x\n",
				(unsigned)registers[at / 4]);
			printf("\nthe Wi-Fi subsystem %s\n", done
				? "reset and says it has started"
				: "never said it had started");
		}

		mem_map_args unmapReset;
		memset(&unmapReset, 0, sizeof(unmapReset));
		unmapReset.signature = POKE_SIGNATURE;
		unmapReset.area = map.area;
		ioctl(sPoke, POKE_UNMAP_MEMORY, &unmapReset, sizeof(unmapReset));
		close(sPoke);
		return 0;
	}

	/* Wake the chip and ask it who it is. This is the whole question: a part
	 * that names itself is a part a driver can be written for.
	 */
	if (argc > 1 && strcmp(argv[1], "chipid") == 0) {
		if (!TakeOwnership(registers)) {
			printf("\nwithout the registers there is nothing to ask.\n");
		} else {
			size_t at = MapThroughWindow(registers, MT_HW_CHIPID);
			uint32 chipId = registers[at / 4];

			at = MapThroughWindow(registers, MT_HW_REV);
			uint32 revision = registers[at / 4];

			printf("\n  chip id  %#010x\n", (unsigned)chipId);
			printf("  revision %#010x\n", (unsigned)revision);

			if ((chipId & 0xffff) == 0x7922) {
				printf("\nthe Wi-Fi side names itself MT%04x, revision %#x.\n",
					(unsigned)(chipId & 0xffff),
					(unsigned)(revision & 0xff));
			} else {
				printf("\nthat is not a part number we expected.\n");
			}
		}

		mem_map_args unmapId;
		memset(&unmapId, 0, sizeof(unmapId));
		unmapId.signature = POKE_SIGNATURE;
		unmapId.area = map.area;
		ioctl(sPoke, POKE_UNMAP_MEMORY, &unmapId, sizeof(unmapId));
		close(sPoke);
		return 0;
	}

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
