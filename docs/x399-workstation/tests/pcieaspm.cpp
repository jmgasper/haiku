// pcieaspm: read-only - for each xHCI controller and the bridge above it,
// the PCIe link's ASPM support, exit latencies and what is enabled.
#include <Drivers.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

enum {
	POKE_PCI_READ_CONFIG = B_DEVICE_OP_CODES_END + 5,
	POKE_GET_NTH_PCI_INFO = B_DEVICE_OP_CODES_END + 7,
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; uint8 index; pci_info* info; status_t status; }
	pci_info_args;
typedef struct { uint32 signature; uint8 bus; uint8 device; uint8 function;
	uint8 size; uint8 offset; uint32 value; } pci_io_args;

static int sFd;
static uint32
config(uint8 bus, uint8 device, uint8 function, uint8 offset, uint8 size)
{
	pci_io_args args = { POKE_SIGNATURE, bus, device, function, size, offset, 0 };
	ioctl(sFd, POKE_PCI_READ_CONFIG, &args, sizeof(args));
	return args.value;
}

static void
link(const char* what, uint8 bus, uint8 device, uint8 function)
{
	for (uint8 cap = config(bus, device, function, 0x34, 1); cap != 0;
			cap = config(bus, device, function, cap + 1, 1)) {
		if (config(bus, device, function, cap, 1) != 0x10)
			continue;
		uint32 linkCap = config(bus, device, function, cap + 0x0c, 4);
		uint16 linkControl = config(bus, device, function, cap + 0x10, 2);
		uint16 linkStatus = config(bus, device, function, cap + 0x12, 2);
		uint32 deviceCap2 = config(bus, device, function, cap + 0x24, 4);
		uint16 deviceControl2 = config(bus, device, function, cap + 0x28, 2);
		static const char* kAspm[] = { "none", "L0s", "L1", "L0s+L1" };
		printf("   %-8s %02x:%02x.%x  supports %s (L0s exit %u, L1 exit %u)"
			"  ENABLED %s  clock pm %u  link x%u gen%u  LTR %s/%s\n", what,
			bus, device, function, kAspm[(linkCap >> 10) & 3],
			(linkCap >> 12) & 7, (linkCap >> 15) & 7,
			kAspm[linkControl & 3], (linkControl >> 8) & 1,
			(linkStatus >> 4) & 0x3f, linkStatus & 0xf,
			(deviceCap2 & (1 << 11)) ? "supported" : "no",
			(deviceControl2 & (1 << 10)) ? "on" : "off");
		return;
	}
	printf("   %-8s %02x:%02x.%x  no PCIe capability (integrated)\n", what, bus,
		device, function);
}

int
main()
{
	sFd = open("/dev/misc/poke", O_RDWR);
	pci_info all[256];
	int count = 0;
	for (; count < 256; count++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)count, &all[count], B_OK };
		if (ioctl(sFd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
	}
	for (int i = 0; i < count; i++) {
		const pci_info& info = all[i];
		if (info.class_base != 0x0c || info.class_sub != 0x03
			|| info.class_api != 0x30)
			continue;
		printf("== xHCI %04x:%04x\n", info.vendor_id, info.device_id);
		link("xhci", info.bus, info.device, info.function);
		// walk up: the bridge whose secondary bus is this bus, and so on
		uint8 bus = info.bus;
		for (int depth = 0; depth < 4 && bus != 0; depth++) {
			bool found = false;
			for (int j = 0; j < count; j++) {
				const pci_info& b = all[j];
				if ((b.header_type & 0x7f) != 1
					|| config(b.bus, b.device, b.function, 0x19, 1) != bus)
					continue;
				link("bridge", b.bus, b.device, b.function);
				bus = b.bus;
				found = true;
				break;
			}
			if (!found)
				break;
		}
	}
	return 0;
}
