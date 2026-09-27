/*
 * What the PCIe link to a device and the bridge above it are set to: which
 * ASPM states each end supports and has enabled, and the completion timeout
 * the bridge applies to reads the device never answers.
 *
 * A read the device never completes stops the processor that issued it, and
 * soon every other processor waiting on that one; whether the root port gives
 * up on it (and after how long), and whether the link can sleep underneath the
 * driver, are what decide how bad that gets.
 *
 * Usage: pcielink [<vendor> <device>]   (default 14c3 7922, the MT7922)
 * Reads through /dev/misc/poke; only the first 256 bytes of configuration
 * space are reachable that way, so L1 substates are not shown.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <OS.h>
#include <PCI.h>

#include "poke.h"


static int sPoke = -1;


static bool
NthDevice(uint8 index, pci_info* info)
{
	pci_info_args args;
	memset(info, 0, sizeof(*info));
	args.signature = POKE_SIGNATURE;
	args.index = index;
	args.info = info;
	args.status = B_OK;
	return ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) >= 0
		&& args.status == B_OK;
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


static uint8
PcieCapability(const pci_info& info)
{
	if ((ReadConfig(info, PCI_status, 2) & PCI_status_capabilities) == 0)
		return 0;
	uint8 at = ReadConfig(info, PCI_capabilities_ptr, 1) & 0xfc;
	for (int guard = 0; at != 0 && guard < 48; guard++) {
		if ((ReadConfig(info, at, 1) & 0xff) == 0x10)
			return at;
		at = ReadConfig(info, at + 1, 1) & 0xfc;
	}
	return 0;
}


static const char*
AspmName(uint32 bits)
{
	static const char* const kNames[] = { "none", "L0s", "L1", "L0s+L1" };
	return kNames[bits & 3];
}


static void
Show(const char* what, const pci_info& info)
{
	uint8 cap = PcieCapability(info);
	printf("%s %02x:%02x.%x %04x:%04x", what, info.bus, info.device,
		info.function, info.vendor_id, info.device_id);
	if (cap == 0) {
		printf(": no PCIe capability\n");
		return;
	}

	uint32 linkCap = ReadConfig(info, cap + 0x0c, 4);
	uint32 linkControl = ReadConfig(info, cap + 0x10, 2);
	uint32 linkStatus = ReadConfig(info, cap + 0x12, 2);
	uint32 deviceControl2 = ReadConfig(info, cap + 0x28, 2);
	uint32 deviceCap2 = ReadConfig(info, cap + 0x24, 4);

	printf("\n  ASPM supported %s, enabled %s, clock PM %s\n",
		AspmName(linkCap >> 10), AspmName(linkControl),
		(linkControl & (1 << 8)) != 0 ? "on" : "off");
	printf("  link Gen%u x%u%s\n", linkStatus & 0xf, (linkStatus >> 4) & 0x3f,
		(linkStatus & (1 << 11)) != 0 ? ", training" : "");
	printf("  completion timeout: ranges supported %#x, value %#x, %s\n",
		deviceCap2 & 0xf, deviceControl2 & 0xf,
		(deviceControl2 & (1 << 4)) != 0 ? "DISABLED" : "enabled");
}


int
main(int argc, char** argv)
{
	uint16 vendor = argc > 2 ? strtoul(argv[1], NULL, 16) : 0x14c3;
	uint16 device = argc > 2 ? strtoul(argv[2], NULL, 16) : 0x7922;

	sPoke = open("/dev/misc/poke", O_RDWR);
	if (sPoke < 0) {
		perror("/dev/misc/poke");
		return 1;
	}

	pci_info target;
	bool found = false;
	for (uint8 i = 0; i < 255 && NthDevice(i, &target); i++) {
		if (target.vendor_id == vendor && target.device_id == device) {
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "no %04x:%04x\n", vendor, device);
		return 1;
	}
	Show("device", target);

	// The bridge whose secondary bus the device is on.
	pci_info bridge;
	for (uint8 i = 0; i < 255 && NthDevice(i, &bridge); i++) {
		if ((bridge.header_type & PCI_header_type_mask) != PCI_header_type_PCI_to_PCI_bridge)
			continue;
		if (bridge.u.h1.secondary_bus == target.bus) {
			Show("bridge", bridge);
			break;
		}
	}

	close(sPoke);
	return 0;
}
