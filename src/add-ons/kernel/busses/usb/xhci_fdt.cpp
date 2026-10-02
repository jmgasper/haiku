/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	xHCI controllers described by a flattened device tree, such as the
	DesignWare USB3 cores (DWC3) of the Rockchip RK3588. The firmware (EDK2)
	powers the PHYs, enables the clocks and puts the cores in host mode; this
	attachment checks that state, completes the host mode setup where needed
	and runs the controller with uncached DMA memory.
*/


#include <stdio.h>

#include <KernelExport.h>

#include "usb_fdt.h"
#include "xhci.h"


extern device_manager_info* gDeviceManager;
extern usb_for_controller_interface* gUSB;

#define XHCI_FDT_DRIVER "busses/usb/xhci/fdt/driver_v1"
#define XHCI_FDT_BUS "busses/usb/xhci/fdt/device_v1"

// DesignWare USB3 global registers, relative to the xHCI register base
#define DWC3_GCTL					0xc110
#define DWC3_GCTL_PRTCAPDIR_MASK	(3 << 12)
#define DWC3_GCTL_PRTCAPDIR_HOST	(1 << 12)
#define DWC3_GSNPSID				0xc120
#define DWC3_GLOBALS_END			0xc200

// Rockchip RK3588 clock gates (CRU_CLKGATE_CON42; a set bit stops the clock)
#define RK3588_CRU_BASE				0xfd7c0000
#define RK3588_CRU_CLKGATE_CON42	0x8a8

struct xhci_fdt_info {
	device_node* node;
	xhci_platform_info platform;
};


static const char* const kCompatible[] = {
	"snps,dwc3", "generic-xhci", "xhci-platform", NULL
};


/*!	Reading an unclocked RK3588 block stalls the bus with an SError, so the
	DWC3 registers are touched only when the firmware left the controller's
	bus clocks running.
*/
static bool
rk3588_dwc3_clocks_running(phys_addr_t base, const char* name)
{
	static const struct {
		phys_addr_t base;
		uint32 gates;
	} kControllers[] = {
		// aclk_usb_root, aclk_usb, aclk_usb3otg0
		{ 0xfc000000, (1 << 0) | (1 << 2) | (1 << 4) },
		// aclk_usb_root, aclk_usb, aclk_usb3otg1
		{ 0xfc400000, (1 << 0) | (1 << 2) | (1 << 7) },
	};

	uint32 gates = 0;
	for (size_t i = 0; i < B_COUNT_OF(kControllers); i++) {
		if (kControllers[i].base == base)
			gates = kControllers[i].gates;
	}
	if (gates == 0) {
		dprintf("xhci: %s: unknown RK3588 DWC3 instance %#" B_PRIxPHYSADDR "\n",
			name, base);
		return false;
	}

	void* address;
	area_id area = map_physical_memory("RK3588 CRU (xhci)", RK3588_CRU_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
	if (area < 0)
		return false;
	uint32 value = *(volatile uint32*)((uint8*)address + RK3588_CRU_CLKGATE_CON42);
	delete_area(area);
	if ((value & gates) != 0) {
		dprintf("xhci: %s: bus clock gated by the firmware (CLKGATE_CON42 %#"
			B_PRIx32 ")\n", name, value);
		return false;
	}
	return true;
}


/*!	Checks a DesignWare USB3 core and leaves it in host mode.
	\a _brokenPortDisable is set for cores whose Port Disable does not work.
*/
static status_t
dwc3_prepare_host(phys_addr_t base, size_t size, const char* name,
	bool* _brokenPortDisable)
{
	if (size < DWC3_GLOBALS_END)
		return B_BAD_VALUE;

	void* address;
	area_id area = map_physical_memory("DWC3 globals (xhci)", base,
		DWC3_GLOBALS_END, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return area;
	volatile uint32* registers = (volatile uint32*)address;

	status_t status = B_OK;
	uint32 id = registers[DWC3_GSNPSID / 4];
	uint32 product = id >> 16;
	if (product != 0x5533 && product != 0x3331 && product != 0x3332) {
		dprintf("xhci: %s: not a DesignWare USB3 core (GSNPSID %#" B_PRIx32
			")\n", name, id);
		status = B_NOT_SUPPORTED;
	} else {
		uint32 control = registers[DWC3_GCTL / 4];
		if ((control & DWC3_GCTL_PRTCAPDIR_MASK) != DWC3_GCTL_PRTCAPDIR_HOST) {
			// The firmware drives every RK3588 port as a host. An OTG port
			// left in device or OTG mode is switched over the same way Linux
			// dwc3_set_prtcap() does it.
			registers[DWC3_GCTL / 4] = (control & ~DWC3_GCTL_PRTCAPDIR_MASK)
				| DWC3_GCTL_PRTCAPDIR_HOST;
			dprintf("xhci: %s: switched the DWC3 core to host mode (GCTL %#"
				B_PRIx32 ")\n", name, control);
			snooze(1000);
		}
		// Linux "quirk-broken-port-ped": DWC_usb3 3.00a and earlier.
		*_brokenPortDisable = product == 0x5533 && (id & 0xffff) <= 0x300a;
		dprintf("xhci: %s: DWC3 core %#" B_PRIx32 ", GCTL %#" B_PRIx32 "\n",
			name, id, registers[DWC3_GCTL / 4]);
	}

	delete_area(area);
	return status;
}


static float
supports_fdt(device_node* parent)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (!usb_fdt_get_device(gDeviceManager, parent, kCompatible, &fdt, &device))
		return 0;
	// An OTG core is used as a host; a peripheral-only one is not ours.
	if (usb_fdt_has_string(fdt, device, "dr_mode", "peripheral"))
		return 0;
	return 0.8f;
}


static status_t
register_fdt(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "XHCI FDT" } },
		{}
	};
	return gDeviceManager->register_node(parent, XHCI_FDT_DRIVER, attrs, NULL,
		NULL);
}


static status_t
init_fdt(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	usb_fdt_resources resources;
	if (status == B_OK)
		status = usb_fdt_get_resources(gDeviceManager, parent, "xhci", &resources);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}

	const char* name = fdt->get_name(device);
	xhci_platform_info platform = {};
	platform.register_base = resources.register_base;
	platform.register_size = resources.register_size;
	platform.interrupt = resources.interrupt;
	platform.dma_coherent = resources.dma_coherent;

	if (usb_fdt_has_string(fdt, device, "compatible", "snps,dwc3")) {
		if (usb_fdt_has_string(fdt, device, "compatible", "rockchip,rk3588-dwc3")
			&& !rk3588_dwc3_clocks_running(platform.register_base, name)) {
			status = B_NOT_SUPPORTED;
		}
		if (status == B_OK) {
			status = dwc3_prepare_host(platform.register_base,
				platform.register_size, name, &platform.broken_port_disable);
		}
	}
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	xhci_fdt_info* info = new(std::nothrow) xhci_fdt_info;
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->platform = platform;
	*cookie = info;
	dprintf("xhci: FDT %s, registers %#" B_PRIxPHYSADDR ", IRQ %" B_PRIu32
		", DMA %s%s; retaining firmware PHY/clock configuration\n", name,
		platform.register_base, platform.interrupt,
		platform.dma_coherent ? "coherent" : "noncoherent",
		platform.broken_port_disable ? ", no port disable" : "");
	return B_OK;
}


static void
uninit_fdt(void* cookie)
{
	delete (xhci_fdt_info*)cookie;
}


static status_t
register_fdt_children(void* cookie)
{
	xhci_fdt_info* info = (xhci_fdt_info*)cookie;
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "XHCI Controller" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = USB_FOR_CONTROLLER_MODULE_NAME } },
		{}
	};
	return gDeviceManager->register_node(info->node, XHCI_FDT_BUS, attrs, NULL,
		NULL);
}


static status_t
init_fdt_bus(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	xhci_fdt_info* info;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&info);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	Stack* stack;
	status = gUSB->get_stack((void**)&stack);
	if (status != B_OK)
		return status;

	XHCI* xhci = new(std::nothrow) XHCI(NULL, NULL, NULL, stack, node,
		&info->platform);
	if (xhci == NULL)
		return B_NO_MEMORY;
	status = xhci->InitCheck();
	if (status == B_OK)
		status = xhci->Start();
	if (status != B_OK) {
		delete xhci;
		return status;
	}
	*cookie = xhci;
	return B_OK;
}


static void
uninit_fdt_bus(void* cookie)
{
	delete (XHCI*)cookie;
}


driver_module_info gXHCIFDTDriver = {
	{ XHCI_FDT_DRIVER, 0, NULL },
	supports_fdt,
	register_fdt,
	init_fdt,
	uninit_fdt,
	register_fdt_children,
	NULL,
	NULL
};

usb_bus_interface gXHCIFDTBus = {
	{
		{ XHCI_FDT_BUS, 0, NULL },
		NULL, NULL,
		init_fdt_bus,
		uninit_fdt_bus,
		NULL, NULL, NULL
	}
};
