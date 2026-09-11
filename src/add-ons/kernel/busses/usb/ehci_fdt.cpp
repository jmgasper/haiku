/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <KernelExport.h>
#include <stdio.h>

#include "ehci.h"


extern device_manager_info* gDeviceManager;
extern usb_for_controller_interface* gUSB;

#define EHCI_FDT_DRIVER "busses/usb/ehci/fdt/driver_v1"
#define EHCI_FDT_BUS "busses/usb/ehci/fdt/device_v1"

struct ehci_fdt_info {
	device_node* node;
	ehci_platform_info platform;
};


static bool
property_has_string(fdt_device_module_info* fdt, fdt_device* device,
	const char* property, const char* value)
{
	int length;
	const char* data = (const char*)fdt->get_prop(device, property, &length);
	while (data != NULL && length > 0) {
		const char* end = (const char*)memchr(data, 0, length);
		if (end == NULL)
			return false;
		if (strcmp(data, value) == 0)
			return true;
		length -= end - data + 1;
		data = end + 1;
	}
	return false;
}


static float
supports_fdt(device_node* parent)
{
#ifndef __aarch64__
	// The noncoherent DMA allocator currently implements ARM64 only.
	return 0;
#endif
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0;
	}
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK) {
		return 0;
	}
	if (!property_has_string(fdt, device, "compatible", "generic-ehci"))
		return 0;
	if (fdt->get_prop(device, "status", NULL) != NULL
		&& !property_has_string(fdt, device, "status", "okay")
		&& !property_has_string(fdt, device, "status", "ok")) {
		return 0;
	}
	return 0.8f;
}


static status_t
register_fdt(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "EHCI FDT" } },
		{}
	};
	return gDeviceManager->register_node(parent, EHCI_FDT_DRIVER, attrs, NULL, NULL);
}


static status_t
init_fdt(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
		(void**)&device);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}
	const char* name = fdt->get_name(device);
	ehci_platform_info platform = {};
	uint64 base, size, irq;
	device_node* interruptController = NULL;
	bool supported = fdt->get_reg(device, 0, &base, &size)
		&& fdt->get_interrupt(device, 0, &interruptController, &irq)
		&& base <= UINT64_MAX - size && size >= 0x100 && size <= SIZE_MAX
		&& irq >= 32 && irq < 1020;

	// The current ARM64 GIC setup uses level-sensitive SPIs. Require that
	// encoding until the FDT API can propagate trigger/polarity information.
	int irqLength = 0;
	const uint8* spec = (const uint8*)fdt->get_prop(device, "interrupts", &irqLength);
	if (spec == NULL || (irqLength != 12 && irqLength != 16)
		|| spec[0] != 0 || spec[1] != 0 || spec[2] != 0 || spec[3] != 0
		|| spec[8] != 0 || spec[9] != 0 || spec[10] != 0 || spec[11] != 4) {
		supported = false;
	}
	// FDT nodes are registered in source order. The controller node may not
	// exist yet, although get_interrupt() can already decode its DT specifier.
	if (interruptController != NULL) {
		fdt_device_module_info* controllerFDT;
		fdt_device* controller;
		if (gDeviceManager->get_driver(interruptController,
				(driver_module_info**)&controllerFDT, (void**)&controller) != B_OK
			|| !property_has_string(controllerFDT, controller, "compatible", "arm,gic-v3")) {
			supported = false;
		}
	}
	const char* unsupported[] = {
		"big-endian", "big-endian-regs", "big-endian-desc",
		"has-transaction-translator", "iommus", "iommu-map", NULL
	};
	for (size_t i = 0; unsupported[i] != NULL; i++) {
		if (fdt->get_prop(device, unsupported[i], NULL) != NULL)
			supported = false;
	}

	// get_reg() currently returns bus addresses without translating ranges.
	// Accept identity buses only, and do not invent a DMA/IOMMU translation.
	device_node* current = parent;
	while (current != NULL) {
		const char* bus;
		if (gDeviceManager->get_attr_string(current, B_DEVICE_BUS, &bus, false) != B_OK
			|| strcmp(bus, "fdt") != 0) {
			gDeviceManager->put_node(current);
			break;
		}
		fdt_device_module_info* ancestorFDT;
		fdt_device* ancestor;
		if (gDeviceManager->get_driver(current, (driver_module_info**)&ancestorFDT,
				(void**)&ancestor) != B_OK) {
			supported = false;
			gDeviceManager->put_node(current);
			break;
		}
		int length;
		const void* ranges = ancestorFDT->get_prop(ancestor, "dma-ranges", &length);
		if (ranges != NULL && length != 0)
			supported = false;
		if (ancestorFDT->get_prop(ancestor, "dma-coherent", NULL) != NULL)
			platform.dma_coherent = true;
		if (ancestorFDT->get_prop(ancestor, "status", NULL) != NULL
			&& !property_has_string(ancestorFDT, ancestor, "status", "okay")
			&& !property_has_string(ancestorFDT, ancestor, "status", "ok")) {
			supported = false;
		}
		if (current != parent && ancestorFDT->get_name(ancestor)[0] != 0) {
			ranges = ancestorFDT->get_prop(ancestor, "ranges", &length);
			if (ranges == NULL || length != 0)
				supported = false;
		}
		device_node* next = gDeviceManager->get_parent_node(current);
		gDeviceManager->put_node(current);
		current = next;
	}
	if (!supported) {
		dprintf("ehci: unsupported FDT resources for %s\n", name);
		return B_NOT_SUPPORTED;
	}
	platform.register_base = base;
	platform.register_size = size;
	platform.interrupt = irq;
	ehci_fdt_info* info = new(std::nothrow) ehci_fdt_info;
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->platform = platform;
	*cookie = info;
	dprintf("ehci: FDT %s, registers %#" B_PRIx64 ", IRQ %" B_PRIu64
		", DMA %s; retaining firmware PHY/clock configuration\n",
		name, base, irq, platform.dma_coherent ? "coherent" : "noncoherent");
	return B_OK;
}


static void
uninit_fdt(void* cookie)
{
	delete (ehci_fdt_info*)cookie;
}


static status_t
register_fdt_children(void* cookie)
{
	ehci_fdt_info* info = (ehci_fdt_info*)cookie;
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "EHCI Controller" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE, { .string = USB_FOR_CONTROLLER_MODULE_NAME } },
		{}
	};
	return gDeviceManager->register_node(info->node, EHCI_FDT_BUS, attrs, NULL, NULL);
}


static status_t
init_fdt_bus(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	ehci_fdt_info* info;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&info);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;
	Stack* stack;
	status = gUSB->get_stack((void**)&stack);
	if (status != B_OK)
		return status;
	EHCI* ehci = new(std::nothrow) EHCI(NULL, NULL, NULL, stack, node, &info->platform);
	if (ehci == NULL)
		return B_NO_MEMORY;
	status = ehci->InitCheck();
	if (status == B_OK)
		status = ehci->Start();
	if (status != B_OK) {
		delete ehci;
		return status;
	}
	*cookie = ehci;
	return B_OK;
}


static void
uninit_fdt_bus(void* cookie)
{
	delete (EHCI*)cookie;
}


driver_module_info gEHCIFDTDriver = {
	{ EHCI_FDT_DRIVER, 0, NULL },
	supports_fdt,
	register_fdt,
	init_fdt,
	uninit_fdt,
	register_fdt_children,
	NULL,
	NULL
};

usb_bus_interface gEHCIFDTBus = {
	{
		{ EHCI_FDT_BUS, 0, NULL },
		NULL, NULL,
		init_fdt_bus,
		uninit_fdt_bus,
		NULL, NULL, NULL
	}
};
