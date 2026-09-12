/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <bus/PCI.h>
#include <driver_settings.h>
#include <KernelExport.h>
#include <AutoDeleterDrivers.h>
#include <AutoDeleterOS.h>
#include <new>
#include <string.h>

#include "firmware_profile.h"

using namespace RK3588Firmware;

#define DRIVER_NAME "busses/pci/rk3588_firmware/driver_v1"

static device_manager_info* sDeviceManager;

struct Controller {
	AreaDeleter rootArea;
	AreaDeleter endpointArea;
	volatile uint8* config[2]{};
	pci_resource_range memory{};
};


static bool
HasString(fdt_device_module_info* fdt, fdt_device* device, const char* property,
	const char* value)
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


static bool
ProfileEnabled()
{
	void* settings = load_driver_settings("rk3588_pcie");
	if (settings == NULL)
		return false;
	const char* profile = get_driver_parameter(settings, "firmware_profile", "", "");
	bool enabled = strcmp(profile, "rock5-itx-edk2-v1.1-dt-samsung950") == 0;
	unload_driver_settings(settings);
	return enabled;
}


static bool
FirmwareIommuDescription(fdt_device_module_info* fdt, fdt_device* device)
{
	int length;
	const uint32* map = (const uint32*)fdt->get_prop(device, "iommu-map", &length);
	if (map == NULL)
		return true;
	if (length != 16 || B_BENDIAN_TO_HOST_INT32(map[0]) != 0
		|| B_BENDIAN_TO_HOST_INT32(map[2]) != 0
		|| B_BENDIAN_TO_HOST_INT32(map[3]) != 0x1000) {
		return false;
	}
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(fdt->get_bus(device),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK) {
		return false;
	}
	device_node* iommu = busModule->node_by_phandle(bus, B_BENDIAN_TO_HOST_INT32(map[1]));
	fdt_device_module_info* iommuModule;
	fdt_device* iommuDevice;
	if (iommu == NULL || sDeviceManager->get_driver(iommu,
			(driver_module_info**)&iommuModule, (void**)&iommuDevice) != B_OK) {
		return false;
	}
	uint64 base, size;
	// This describes Linux's future configuration, not the firmware DMA map.
	// EDK2 v1.1 uses NonCoherentIoMmuDxe/NonCoherentDmaLib with offset 0;
	// Haiku has no SMMUv3 driver that changes that handoff. The explicit profile
	// retains it, just as it retains the firmware's non-DT PCI config/ranges.
	return HasString(iommuModule, iommuDevice, "compatible", "arm,smmu-v3")
		&& iommuModule->get_reg(iommuDevice, 0, &base, &size)
		&& base == 0xfc900000 && size == 0x200000;
}


static bool
MatchesNode(device_node* node)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(node, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return false;
	}
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&fdt,
			(void**)&device) != B_OK
		|| !HasString(fdt, device, "compatible", "rockchip,rk3588-pcie")) {
		return false;
	}
	uint64 base, size;
	if (!fdt->get_reg(device, 0, &base, &size)
		|| base != kRootConfig || size != 0x400000) {
		return false;
	}

	if (!FirmwareIommuDescription(fdt, device))
		return false;
	// Require an untranslated parent bus on the exact board. This explicit
	// firmware profile is not a general driver for the Linux DT resources.
	bool board = false;
	bool supported = true;
	device_node* current = node;
	// The caller owns node; get_parent_node() owns subsequent references.
	while (current != NULL) {
		if (sDeviceManager->get_attr_string(current, B_DEVICE_BUS, &bus, false) != B_OK
			|| strcmp(bus, "fdt") != 0) {
			if (current != node)
				sDeviceManager->put_node(current);
			break;
		}
		if (sDeviceManager->get_driver(current, (driver_module_info**)&fdt,
				(void**)&device) != B_OK) {
			if (current != node)
				sDeviceManager->put_node(current);
			return false;
		}
		if (fdt->get_prop(device, "status", NULL) != NULL
			&& !HasString(fdt, device, "status", "okay")
			&& !HasString(fdt, device, "status", "ok")) {
			supported = false;
		}
		const char* unsupported[] = {
			"iommus", "big-endian", "dma-coherent", NULL
		};
		for (unsigned i = 0; unsupported[i] != NULL; i++) {
			if (fdt->get_prop(device, unsupported[i], NULL) != NULL)
				supported = false;
		}
		if (current != node && fdt->get_prop(device, "iommu-map", NULL) != NULL)
			supported = false;
		int length;
		if (fdt->get_prop(device, "dma-ranges", &length) != NULL && length != 0)
			supported = false;
		if (fdt->get_name(device)[0] == 0) {
			board = HasString(fdt, device, "compatible", "radxa,rock-5-itx");
		} else if (current != node) {
			if (fdt->get_prop(device, "ranges", &length) == NULL || length != 0)
				supported = false;
		}
		device_node* next = sDeviceManager->get_parent_node(current);
		if (current != node)
			sDeviceManager->put_node(current);
		current = next;
	}
	return supported && board;
}


static float
SupportsDevice(device_node* parent)
{
	return MatchesNode(parent) && ProfileEnabled() ? 1.0f : 0.0f;
}


static status_t
RegisterDevice(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "RK3588 EDK2 v1.1 NVMe PCIe host"} },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = "bus_managers/pci/root/driver_v1"} },
		{}
	};
	return sDeviceManager->register_node(parent, DRIVER_NAME, attrs, NULL, NULL);
}


static void
ReadSnapshot(volatile uint8* config, uint32* snapshot)
{
	// Device-nGnRnE, individual 32-bit config transactions (never memcpy MMIO).
	for (unsigned i = 0; i < 64; i++)
		snapshot[i] = ((volatile uint32*)config)[i];
	memory_full_barrier();
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	DeviceNodePutter<&sDeviceManager> parent(sDeviceManager->get_parent_node(node));
	if (!MatchesNode(parent.Get()) || !ProfileEnabled())
		return B_NOT_SUPPORTED;
	ObjectDeleter<Controller> controller(new(std::nothrow) Controller);
	if (!controller.IsSet())
		return B_NO_MEMORY;
	controller->rootArea.SetTo(map_physical_memory("RK3588 root config", kRootConfig,
		kConfigSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->config[0]));
	if (controller->rootArea.Get() < B_OK)
		return controller->rootArea.Get();
	uint32 snapshot[64];
	uint64 memoryBase, memorySize;
	// Check the ID before reading the rest of the root configuration.
	if (*(volatile uint32*)controller->config[0] != 0x35881d87)
		return B_NOT_SUPPORTED;
	ReadSnapshot(controller->config[0], snapshot);
	if (!RootMatches(snapshot, memoryBase, memorySize)) {
		dprintf("rk3588_pcie: firmware root configuration does not match profile\n");
		return B_NOT_SUPPORTED;
	}
	controller->endpointArea.SetTo(map_physical_memory("RK3588 SSD config", kEndpointConfig,
		kConfigSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->config[1]));
	if (controller->endpointArea.Get() < B_OK)
		return controller->endpointArea.Get();
	if (*(volatile uint32*)controller->config[1] != 0xa802144d)
		return B_NOT_SUPPORTED;
	ReadSnapshot(controller->config[1], snapshot);
	if (!EndpointMatches(snapshot, memoryBase, memorySize)) {
		dprintf("rk3588_pcie: firmware SSD configuration does not match profile\n");
		return B_NOT_SUPPORTED;
	}
	controller->memory.type = B_IO_MEMORY;
	controller->memory.address_type = PCI_address_type_32;
	controller->memory.host_address = memoryBase;
	controller->memory.pci_address = memoryBase;
	controller->memory.size = memorySize;
	dprintf("rk3588_pcie: EDK2 v1.1 segment 0, buses 0..1, Samsung 950 Pro; "
		"MMIO %#" B_PRIx64 "+%#" B_PRIx64 "; retaining firmware PHY/clocks/iATU, "
		"identity noncoherent DMA; NVMe polling only\n", memoryBase, memorySize);
	*cookie = controller.Detach();
	return B_OK;
}


static status_t
ReadConfig(void* cookie, uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32* value)
{
	*value = 0xffffffff;
	if (!ValidAccess(bus, device, function, offset, size))
		return B_BAD_VALUE;
	volatile uint8* address = ((Controller*)cookie)->config[bus] + offset;
	switch (size) {
		case 1: *value = *address; break;
		case 2: *value = *(volatile uint16*)address; break;
		case 4: *value = *(volatile uint32*)address; break;
	}
	memory_full_barrier();
	return B_OK;
}


static status_t
WriteConfig(void* cookie, uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32 value)
{
	if (!ValidAccess(bus, device, function, offset, size))
		return B_BAD_VALUE;
	volatile uint8* address = ((Controller*)cookie)->config[bus] + offset;
	memory_full_barrier();
	switch (size) {
		case 1: *address = value; break;
		case 2: *(volatile uint16*)address = value; break;
		case 4: *(volatile uint32*)address = value; break;
	}
	memory_full_barrier();
	return B_OK;
}


static pci_controller_module_info sController = {
	.info = {
		.info = { .name = DRIVER_NAME },
		.supports_device = SupportsDevice,
		.register_device = RegisterDevice,
		.init_driver = InitDriver,
		.uninit_driver = [](void* cookie) { delete (Controller*)cookie; },
	},
	.read_pci_config = ReadConfig,
	.write_pci_config = WriteConfig,
	.get_max_bus_devices = [](void*, int32* count) { *count = 1; return B_OK; },
	.read_pci_irq = [](void*, uint8, uint8, uint8, uint8, uint8*) {
		return B_NOT_SUPPORTED;
	},
	.write_pci_irq = [](void*, uint8, uint8, uint8, uint8, uint8) {
		return B_NOT_SUPPORTED;
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		// I/O ports and prefetchable windows are not part of this NVMe profile.
		if (index != 0)
			return B_BAD_INDEX;
		*range = ((Controller*)cookie)->memory;
		return B_OK;
	},
	.finalize = [](void*) { return B_OK; }
};

_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};

_EXPORT module_info* modules[] = {
	(module_info*)&sController,
	NULL
};
