/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The PCIe root complex of the BCM2711 (Raspberry Pi 4). On the Pi 4 B one
	device sits behind it, soldered to the board: the VL805 USB 3 controller.

	Nothing has set the bridge up when the kernel starts, so the driver does
	all of it (after Linux' pcie-brcmstb.c and U-Boot's pcie_brcmstb.c):
	reset, the inbound window through which devices reach RAM, link
	training, the outbound window for the devices' registers.

	The windows differ from the firmware's device tree on purpose. RAM is
	mapped at PCI address 0, so that a DMA address is the physical address
	and drivers need no offset. The devices' registers are put at PCI
	0xfc000000-0xffffffff, where the CPU has its peripherals and no RAM; they
	appear to the CPU at the address the tree gives (0x6'0000'0000).

	Boards without a VL805 EEPROM (the newer ones) lose the controller's
	firmware in the reset; the VideoCore firmware loads it again when asked
	through the mailbox. */


#include <string.h>

#include <new>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <bus/PCI.h>
#include <device_manager.h>

#include <AutoDeleterDrivers.h>
#include <AutoDeleterOS.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>


#define INFO(x...)	dprintf("bcm2711_pcie: " x)
#define ERROR(x...)	dprintf("bcm2711_pcie: " x)

#define BCM2711_PCIE_DRIVER_MODULE_NAME "busses/pci/bcm2711/driver_v1"


// registers
#define RC_CFG_VENDOR_SPECIFIC_REG1		0x0188
#define  ENDIAN_MODE_BAR2_MASK			0xc
#define RC_CFG_PRIV1_ID_VAL3			0x043c
#define  CLASS_CODE_MASK				0xffffff
#define MISC_CTRL						0x4008
#define  MISC_CTRL_SCB_ACCESS_EN		0x1000
#define  MISC_CTRL_CFG_READ_UR_MODE		0x2000
#define  MISC_CTRL_MAX_BURST_SIZE_MASK	0x300000
#define  MISC_CTRL_SCB0_SIZE_MASK		0xf8000000
#define  MISC_CTRL_SCB0_SIZE_SHIFT		27
#define MEM_WIN0_LO						0x400c
#define MEM_WIN0_HI						0x4010
#define RC_BAR1_CONFIG_LO				0x402c
#define RC_BAR2_CONFIG_LO				0x4034
#define RC_BAR2_CONFIG_HI				0x4038
#define RC_BAR3_CONFIG_LO				0x403c
#define  RC_BAR_SIZE_MASK				0x1f
#define PCIE_STATUS						0x4068
#define  STATUS_PHY_LINK_UP				0x10
#define  STATUS_DL_ACTIVE				0x20
#define  STATUS_PORT					0x80
#define MEM_WIN0_BASE_LIMIT				0x4070
#define MEM_WIN0_BASE_HI				0x4080
#define MEM_WIN0_LIMIT_HI				0x4084
#define HARD_DEBUG						0x4204
#define  HARD_DEBUG_CLKREQ_ENABLE		0x2
#define  HARD_DEBUG_SERDES_IDDQ			0x08000000
#define INTR2_CPU_CLEAR					0x4308
#define INTR2_CPU_MASK_SET				0x4310
#define EXT_CFG_DATA					0x8000
#define EXT_CFG_INDEX					0x9000
#define RGR1_SW_INIT_1					0x9210
#define  SW_INIT_PERST					0x1
#define  SW_INIT_BRIDGE					0x2

// the VideoCore mailbox, for the VL805's firmware
#define MAILBOX_BASE					0xfe00b000
#define MAILBOX_READ					0x880
#define MAILBOX_STATUS					0x898
#define MAILBOX_WRITE					0x8a0
#define  MAILBOX_FULL					0x80000000
#define  MAILBOX_EMPTY					0x40000000
#define MAILBOX_CHANNEL_PROPERTY		8
#define TAG_NOTIFY_XHCI_RESET			0x00030058
#define VC_BUS_OFFSET					0xc0000000

// where the devices' registers go in PCI address space
static const uint64 kPciMemoryBase = 0xfc000000;
static const uint64 kPciMemorySize = 0x04000000;
// RAM as devices see it: at 0, large enough for every board
static const uint64 kInboundSize = 8ull << 30;


device_manager_info* gDeviceManager;
pci_module_info* gPCI;


class BCM2711PCIController {
public:
								~BCM2711PCIController();

	static	float				SupportsDevice(device_node* parent);
	static	status_t			RegisterDevice(device_node* parent);
	static	status_t			InitDriver(device_node* node,
									BCM2711PCIController*& _driver);

			status_t			ReadConfig(uint8 bus, uint8 device,
									uint8 function, uint16 offset, uint8 size,
									uint32& value);
			status_t			WriteConfig(uint8 bus, uint8 device,
									uint8 function, uint16 offset, uint8 size,
									uint32 value);
			status_t			GetRange(uint32 index,
									pci_resource_range* range);
			status_t			Finalize();

private:
			status_t			_Init();
			status_t			_StartLink();
			volatile uint8*		_ConfigAddress(uint8 bus, uint8 device,
									uint8 function, uint16 offset);
			status_t			_NotifyXhciReset(uint8 bus, uint8 device,
									uint8 function);

			uint32				_Read(uint32 reg)
									{ return *(volatile uint32*)(fRegs + reg); }
			void				_Write(uint32 reg, uint32 value)
									{ *(volatile uint32*)(fRegs + reg) = value; }

private:
			mutex				fLock = MUTEX_INITIALIZER("bcm2711 pcie");
			device_node*		fNode = NULL;
			area_id				fRegsArea = -1;
			volatile uint8*		fRegs = NULL;
			uint64				fRegsSize = 0;
			uint64				fHostMemoryBase = 0;
			bool				fLinkUp = false;
};


BCM2711PCIController::~BCM2711PCIController()
{
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
}


float
BCM2711PCIController::SupportsDevice(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (gDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "brcm,bcm2711-pcie") != 0) {
		return 0.0f;
	}

	return 1.0f;
}


status_t
BCM2711PCIController::RegisterDevice(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "BCM2711 PCIe Host Controller"}},
		{B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = "bus_managers/pci/root/driver_v1"}},
		{}
	};

	return gDeviceManager->register_node(parent,
		BCM2711_PCIE_DRIVER_MODULE_NAME, attrs, NULL, NULL);
}


status_t
BCM2711PCIController::InitDriver(device_node* node,
	BCM2711PCIController*& _driver)
{
	BCM2711PCIController* driver = new(std::nothrow) BCM2711PCIController();
	if (driver == NULL)
		return B_NO_MEMORY;

	driver->fNode = node;
	status_t status = driver->_Init();
	if (status != B_OK) {
		delete driver;
		return status;
	}

	_driver = driver;
	return B_OK;
}


status_t
BCM2711PCIController::_Init()
{
	DeviceNodePutter<&gDeviceManager> parent(
		gDeviceManager->get_parent_node(fNode));
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent.Get(),
		(driver_module_info**)&fdt, (void**)&device);
	if (status != B_OK)
		return status;

	uint64 regs;
	if (!fdt->get_reg(device, 0, &regs, &fRegsSize))
		return B_BAD_DATA;

	// "ranges": <flags, PCI address (2 cells), CPU address (2), size (2)>;
	// only the CPU address is taken from it
	int length;
	const uint32* ranges = (const uint32*)fdt->get_prop(device, "ranges",
		&length);
	if (ranges == NULL || length < 7 * 4)
		return B_BAD_DATA;
	fHostMemoryBase = (uint64)B_BENDIAN_TO_HOST_INT32(ranges[3]) << 32
		| B_BENDIAN_TO_HOST_INT32(ranges[4]);
	uint64 hostSize = (uint64)B_BENDIAN_TO_HOST_INT32(ranges[5]) << 32
		| B_BENDIAN_TO_HOST_INT32(ranges[6]);
	if (hostSize < kPciMemorySize)
		return B_BAD_DATA;

	fRegsArea = map_physical_memory("bcm2711 pcie", regs, fRegsSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fRegs);
	if (fRegsArea < 0)
		return fRegsArea;

	// reset the bridge, and hold the devices in reset
	_Write(RGR1_SW_INIT_1, _Read(RGR1_SW_INIT_1) | SW_INIT_BRIDGE | SW_INIT_PERST);
	snooze(200);
	_Write(RGR1_SW_INIT_1, _Read(RGR1_SW_INIT_1) & ~SW_INIT_BRIDGE);
	_Write(HARD_DEBUG, _Read(HARD_DEBUG) & ~HARD_DEBUG_SERDES_IDDQ);
	snooze(200);

	// bus mastering towards the SoC, unsupported requests read as all ones,
	// 128-byte bursts
	uint32 control = _Read(MISC_CTRL);
	control |= MISC_CTRL_SCB_ACCESS_EN | MISC_CTRL_CFG_READ_UR_MODE;
	control &= ~MISC_CTRL_MAX_BURST_SIZE_MASK;
	_Write(MISC_CTRL, control);

	// the inbound window: RAM at PCI address 0 (size code: log2 - 15)
	uint32 sizeCode = 0;
	for (uint64 size = kInboundSize; size > 1; size >>= 1)
		sizeCode++;
	sizeCode -= 15;
	_Write(RC_BAR2_CONFIG_LO, sizeCode);
	_Write(RC_BAR2_CONFIG_HI, 0);
	control = _Read(MISC_CTRL) & ~MISC_CTRL_SCB0_SIZE_MASK;
	_Write(MISC_CTRL, control | sizeCode << MISC_CTRL_SCB0_SIZE_SHIFT);

	// the other inbound windows are not used
	_Write(RC_BAR1_CONFIG_LO, _Read(RC_BAR1_CONFIG_LO) & ~RC_BAR_SIZE_MASK);
	_Write(RC_BAR3_CONFIG_LO, _Read(RC_BAR3_CONFIG_LO) & ~RC_BAR_SIZE_MASK);

	// no interrupts from the bridge itself
	_Write(INTR2_CPU_CLEAR, 0xffffffff);
	_Write(INTR2_CPU_MASK_SET, 0xffffffff);

	status = _StartLink();
	if (status != B_OK) {
		// Not fatal: the bridge is there, there is just nothing behind it.
		ERROR("no PCIe link\n");
	}

	// the outbound window
	_Write(MEM_WIN0_LO, (uint32)kPciMemoryBase);
	_Write(MEM_WIN0_HI, kPciMemoryBase >> 32);
	uint64 baseMB = fHostMemoryBase >> 20;
	uint64 limitMB = (fHostMemoryBase + kPciMemorySize - 1) >> 20;
	_Write(MEM_WIN0_BASE_LIMIT,
		(uint32)(limitMB & 0xfff) << 20 | (uint32)(baseMB & 0xfff) << 4);
	_Write(MEM_WIN0_BASE_HI, baseMB >> 12);
	_Write(MEM_WIN0_LIMIT_HI, limitMB >> 12);

	// little endian, and a PCI-to-PCI bridge by class
	_Write(RC_CFG_VENDOR_SPECIFIC_REG1,
		_Read(RC_CFG_VENDOR_SPECIFIC_REG1) & ~ENDIAN_MODE_BAR2_MASK);
	_Write(RC_CFG_PRIV1_ID_VAL3,
		(_Read(RC_CFG_PRIV1_ID_VAL3) & ~CLASS_CODE_MASK) | 0x060400);

	INFO("registers %#" B_PRIx64 ", link %s, device memory %#" B_PRIx64
		" (PCI %#" B_PRIx64 "), %" B_PRIu64 " MB\n", regs,
		fLinkUp ? "up" : "down", fHostMemoryBase, kPciMemoryBase,
		kPciMemorySize >> 20);
	return B_OK;
}


status_t
BCM2711PCIController::_StartLink()
{
	// release the devices from reset and give them time to train the link
	_Write(RGR1_SW_INIT_1, _Read(RGR1_SW_INIT_1) & ~SW_INIT_PERST);
	snooze(100000);

	for (int i = 0; i < 100; i++) {
		uint32 status = _Read(PCIE_STATUS);
		if ((status & (STATUS_PHY_LINK_UP | STATUS_DL_ACTIVE))
				== (STATUS_PHY_LINK_UP | STATUS_DL_ACTIVE)) {
			if ((status & STATUS_PORT) == 0) {
				ERROR("the controller is not in root complex mode\n");
				return B_NOT_SUPPORTED;
			}
			fLinkUp = true;
			return B_OK;
		}
		snooze(5000);
	}

	return B_TIMED_OUT;
}


volatile uint8*
BCM2711PCIController::_ConfigAddress(uint8 bus, uint8 device, uint8 function,
	uint16 offset)
{
	if (offset >= 0x1000)
		return NULL;

	// the root port itself
	if (bus == 0)
		return device == 0 && function == 0 ? fRegs + offset : NULL;

	// One device per link; without a link an access would abort.
	if (!fLinkUp || device != 0)
		return NULL;

	_Write(EXT_CFG_INDEX, (uint32)bus << 20 | (uint32)device << 15
		| (uint32)function << 12);
	return fRegs + EXT_CFG_DATA + offset;
}


status_t
BCM2711PCIController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	MutexLocker locker(fLock);

	volatile uint8* address = _ConfigAddress(bus, device, function, offset);
	if (address == NULL) {
		value = 0xffffffff >> (8 * (4 - size));
		return B_OK;
	}

	switch (size) {
		case 1:
			value = *address;
			break;
		case 2:
			value = *(volatile uint16*)address;
			break;
		case 4:
			value = *(volatile uint32*)address;
			break;
		default:
			return B_BAD_VALUE;
	}
	return B_OK;
}


status_t
BCM2711PCIController::WriteConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32 value)
{
	MutexLocker locker(fLock);

	volatile uint8* address = _ConfigAddress(bus, device, function, offset);
	if (address == NULL)
		return B_OK;

	switch (size) {
		case 1:
			*address = value;
			break;
		case 2:
			*(volatile uint16*)address = value;
			break;
		case 4:
			*(volatile uint32*)address = value;
			break;
		default:
			return B_BAD_VALUE;
	}
	return B_OK;
}


status_t
BCM2711PCIController::GetRange(uint32 index, pci_resource_range* range)
{
	if (index != 0)
		return B_BAD_INDEX;

	*range = {};
	range->type = B_IO_MEMORY;
	range->address_type = PCI_address_type_32;
	range->host_address = fHostMemoryBase;
	range->pci_address = kPciMemoryBase;
	range->size = kPciMemorySize;
	return B_OK;
}


/*!	Asks the VideoCore firmware to load the VL805's firmware. The message
	has to be in memory the VideoCore reaches (the first gigabyte).
*/
status_t
BCM2711PCIController::_NotifyXhciReset(uint8 bus, uint8 device, uint8 function)
{
	volatile uint8* mailbox;
	area_id mailboxArea = map_physical_memory("bcm2711 mailbox", MAILBOX_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&mailbox);
	if (mailboxArea < 0)
		return mailboxArea;
	AreaDeleter mailboxDeleter(mailboxArea);

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	uint32* message;
	area_id messageArea = create_area_etc(B_SYSTEM_TEAM, "bcm2711 mailbox message",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, (void**)&message);
	if (messageArea < 0)
		return messageArea;
	AreaDeleter messageDeleter(messageArea);

	physical_entry entry;
	status_t status = get_memory_map(message, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;

	// keep the page out of the cache: the firmware answers in place
	for (addr_t line = (addr_t)message; line < (addr_t)message + B_PAGE_SIZE;
			line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	vm_set_area_memory_type(messageArea, entry.address,
		B_WRITE_COMBINING_MEMORY);

	message[0] = 7 * 4;
	message[1] = 0;
	message[2] = TAG_NOTIFY_XHCI_RESET;
	message[3] = 4;
	message[4] = 0;
	message[5] = (uint32)bus << 20 | (uint32)device << 15 | (uint32)function << 12;
	message[6] = 0;
	memory_full_barrier();

	uint32 address = ((uint32)entry.address | VC_BUS_OFFSET)
		| MAILBOX_CHANNEL_PROPERTY;

	bigtime_t timeout = system_time() + 1000000;
	while ((*(volatile uint32*)(mailbox + MAILBOX_STATUS) & MAILBOX_FULL) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(100);
	}
	*(volatile uint32*)(mailbox + MAILBOX_WRITE) = address;

	while (true) {
		while ((*(volatile uint32*)(mailbox + MAILBOX_STATUS) & MAILBOX_EMPTY)
				!= 0) {
			if (system_time() > timeout)
				return B_TIMED_OUT;
			snooze(100);
		}
		if (*(volatile uint32*)(mailbox + MAILBOX_READ) == address)
			break;
	}

	memory_full_barrier();
	return message[1] == 0x80000000 ? B_OK : B_ERROR;
}


status_t
BCM2711PCIController::Finalize()
{
	DeviceNodePutter<&gDeviceManager> parent(
		gDeviceManager->get_parent_node(fNode));
	fdt_device_module_info* fdt;
	fdt_device* fdtDevice;
	status_t status = gDeviceManager->get_driver(parent.Get(),
		(driver_module_info**)&fdt, (void**)&fdtDevice);
	if (status != B_OK)
		return status;

	struct fdt_interrupt_map* interruptMap = fdt->get_interrupt_map(fdtDevice);

	for (int bus = 0; bus < 2; bus++) {
		uint32 vendor = gPCI->read_pci_config(bus, 0, 0, PCI_vendor_id, 2);
		if (vendor == 0xffff || vendor == 0xffffffff)
			continue;

		uint32 pin = gPCI->read_pci_config(bus, 0, 0, PCI_interrupt_pin, 1);
		if (pin >= 1 && pin <= 4) {
			// INTA-INTD of the slot are wired to four interrupts of the SoC
			uint32 irq = fdt->lookup_interrupt_map(interruptMap, 0, pin);
			if (irq != 0xffffffff) {
				INFO("%d:0.0 INT%c -> interrupt %" B_PRIu32 "\n", bus,
					'A' + (int)pin - 1, irq);
				gPCI->update_interrupt_line(bus, 0, 0, irq);
			}
		}

		uint32 device = gPCI->read_pci_config(bus, 0, 0, PCI_device_id, 2);
		if (bus == 1 && vendor == 0x1106 && device == 0x3483) {
			// VL805: have its firmware loaded, now that it has its address
			uint32 command = gPCI->read_pci_config(bus, 0, 0, PCI_command, 2);
			gPCI->write_pci_config(bus, 0, 0, PCI_command, 2,
				command | PCI_command_memory | PCI_command_master);
			status = _NotifyXhciReset(bus, 0, 0);
			INFO("VL805 firmware load: %s\n", strerror(status));
			snooze(200000);
		}
	}

	return B_OK;
}


static pci_controller_module_info sBCM2711PCIDriver = {
	.info = {
		.info = {
			.name = BCM2711_PCIE_DRIVER_MODULE_NAME,
		},
		.supports_device = BCM2711PCIController::SupportsDevice,
		.register_device = BCM2711PCIController::RegisterDevice,
		.init_driver = [](device_node* node, void** cookie) {
			return BCM2711PCIController::InitDriver(node,
				*(BCM2711PCIController**)cookie);
		},
		.uninit_driver = [](void* cookie) {
			delete (BCM2711PCIController*)cookie;
		},
	},
	.read_pci_config = [](void* cookie, uint8 bus, uint8 device,
			uint8 function, uint16 offset, uint8 size, uint32* value) {
		return ((BCM2711PCIController*)cookie)->ReadConfig(bus, device,
			function, offset, size, *value);
	},
	.write_pci_config = [](void* cookie, uint8 bus, uint8 device,
			uint8 function, uint16 offset, uint8 size, uint32 value) {
		return ((BCM2711PCIController*)cookie)->WriteConfig(bus, device,
			function, offset, size, value);
	},
	.get_max_bus_devices = [](void* cookie, int32* count) {
		*count = 1;
		return B_OK;
	},
	.read_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
			uint8 pin, uint8* irq) {
		return (status_t)B_UNSUPPORTED;
	},
	.write_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
			uint8 pin, uint8 irq) {
		return (status_t)B_UNSUPPORTED;
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		return ((BCM2711PCIController*)cookie)->GetRange(index, range);
	},
	.finalize = [](void* cookie) {
		return ((BCM2711PCIController*)cookie)->Finalize();
	}
};

module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager},
	{B_PCI_MODULE_NAME, (module_info**)&gPCI},
	{}
};

module_info* modules[] = {
	(module_info*)&sBCM2711PCIDriver,
	NULL
};
