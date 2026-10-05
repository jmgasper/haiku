// xhcimsi: read-only dump of each xHCI controller's interrupt setup from PCI
// configuration space (legacy line, MSI and MSI-X capabilities).
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
config(const pci_info& info, uint8 offset, uint8 size)
{
	pci_io_args args = { POKE_SIGNATURE, info.bus, info.device, info.function,
		size, offset, 0 };
	ioctl(sFd, POKE_PCI_READ_CONFIG, &args, sizeof(args));
	return args.value;
}

int
main()
{
	sFd = open("/dev/misc/poke", O_RDWR);
	for (int index = 0; index < 256; index++) {
		pci_info info;
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sFd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		if (info.class_base != 0x0c || info.class_sub != 0x03
			|| info.class_api != 0x30)
			continue;
		printf("== %04x:%04x at %02x:%02x.%x  interrupt line %u pin %u"
			"  command %#x\n", info.vendor_id, info.device_id, info.bus,
			info.device, info.function, info.u.h0.interrupt_line,
			info.u.h0.interrupt_pin, config(info, 0x04, 2));
		for (uint8 cap = config(info, 0x34, 1); cap != 0;
				cap = config(info, cap + 1, 1)) {
			uint8 id = config(info, cap, 1);
			if (id == 0x05) {
				uint16 control = config(info, cap + 2, 2);
				bool is64 = control & (1 << 7);
				uint32 addressLow = config(info, cap + 4, 4);
				uint32 addressHigh = is64 ? config(info, cap + 8, 4) : 0;
				uint16 data = config(info, cap + (is64 ? 12 : 8), 2);
				printf("   MSI   enabled %u vectors %u of %u  address %#x%08x"
					" (dest APIC %u)  data %#06x (vector %u)\n",
					control & 1, 1 << ((control >> 4) & 7),
					1 << ((control >> 1) & 7), addressHigh, addressLow,
					(addressLow >> 12) & 0xff, data, data & 0xff);
			} else if (id == 0x11) {
				uint16 control = config(info, cap + 2, 2);
				printf("   MSI-X enabled %u masked %u table size %u\n",
					control >> 15, (control >> 14) & 1, (control & 0x7ff) + 1);
			}
		}
	}
	return 0;
}
