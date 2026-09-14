/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <KernelExport.h>
#include <fcntl.h>
#include <stdlib.h>

#include "CsfResources.h"


using namespace MaliCSF;

#define DRIVER_NAME "drivers/graphics/mali_csf/driver_v1"
#define DEVICE_NAME "drivers/graphics/mali_csf/device_v1"

static device_manager_info* sDeviceManager;

struct Controller {
	device_node* node;
	ResourceInfo resources;
};


class FdtNode {
public:
	bool SetTo(device_node* node)
	{
		driver_module_info* module;
		void* cookie;
		if (node == NULL
			|| sDeviceManager->get_driver(node, &module, &cookie) != B_OK
			|| strcmp(module->info.name, "bus_managers/fdt/driver_v1") != 0) {
			return false;
		}
		fModule = (fdt_device_module_info*)module;
		fDevice = (fdt_device*)cookie;
		return true;
	}

	bool HasString(const char* property, const char* value) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return StringIndex(data, length, value) >= 0;
	}

	bool Cells(const char* property, uint32_t* cells, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return ReadCells(data, length, cells, count);
	}

	bool Names(const char* property, const char* const* names, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		size_t expected = 0;
		for (size_t i = 0; i < count; i++) {
			expected += strlen(names[i]) + 1;
			if (StringIndex(data, length, names[i]) != (int)i)
				return false;
		}
		return length >= 0 && (size_t)length == expected;
	}

	bool Reg(uint64_t& base, uint64_t& size) const
	{
		uint64 address, length;
		if (!fModule->get_reg(fDevice, 0, &address, &length))
			return false;
		base = address;
		size = length;
		return true;
	}

	bool Enabled() const
	{
		const void* status = fModule->get_prop(fDevice, "status", NULL);
		return status == NULL || HasString("status", "okay")
			|| HasString("status", "ok");
	}

	fdt_device_module_info* fModule = NULL;
	fdt_device* fDevice = NULL;
};


static bool
BoardMatches(device_node* gpu)
{
	device_node* node = sDeviceManager->get_parent_node(gpu);
	while (node != NULL) {
		FdtNode fdt;
		if (!fdt.SetTo(node)) {
			sDeviceManager->put_node(node);
			return false;
		}
		bool found = strcmp(fdt.fModule->get_name(fdt.fDevice), "") == 0;
		if (found) {
			bool matches = fdt.HasString("compatible", "radxa,rock-5-itx");
			sDeviceManager->put_node(node);
			return matches;
		}
		device_node* parent = sDeviceManager->get_parent_node(node);
		sDeviceManager->put_node(node);
		node = parent;
	}
	return false;
}


static bool
ReadResources(device_node* parent, ResourceInfo& output)
{
	ResourceInfo info = {};
	FdtNode gpu;
	if (!gpu.SetTo(parent) || !gpu.Enabled()
		|| !gpu.HasString("compatible", "rockchip,rk3588-mali")
		|| !gpu.HasString("compatible", "arm,mali-valhall-csf")
		|| !BoardMatches(parent) || !gpu.Reg(info.gpuBase, info.gpuSize)) {
		return false;
	}
	const char* const irqNames[] = {"job", "mmu", "gpu"};
	const char* const clockNames[] = {"core", "coregroup", "stacks"};
	uint32_t specifiers[12], clocks[6], power[2], supply;
	if (!gpu.Names("interrupt-names", irqNames, 3)
		|| !gpu.Names("clock-names", clockNames, 3)
		|| !gpu.Cells("interrupts", specifiers, 12)
		|| !gpu.Cells("clocks", clocks, 6)
		|| !gpu.Cells("power-domains", power, 2)
		|| !gpu.Cells("mali-supply", &supply, 1)
		|| gpu.fModule->get_prop(gpu.fDevice, "interrupts-extended", NULL) != NULL) {
		return false;
	}
	device_node* gicNode = NULL;
	for (unsigned i = 0; i < 3; i++) {
		device_node* controller;
		uint64 irq;
		if (specifiers[i * 4] != 0 || specifiers[i * 4 + 1] != 92 + i
			|| specifiers[i * 4 + 2] != 4 || specifiers[i * 4 + 3] != 0
			|| !gpu.fModule->get_interrupt(gpu.fDevice, i, &controller, &irq)
			|| controller == NULL || irq != 124 + i
			|| (gicNode != NULL && gicNode != controller)) {
			return false;
		}
		gicNode = controller;
		info.interrupts[i] = irq;
	}
	FdtNode gic;
	uint64_t gicSize;
	uint32_t cells;
	if (!gic.SetTo(gicNode) || !gic.HasString("compatible", "arm,gic-v3")
		|| !gic.Cells("#interrupt-cells", &cells, 1) || cells != 4
		|| !gic.Reg(info.interruptBase, gicSize) || gicSize != 0x10000) {
		return false;
	}
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(gpu.fModule->get_bus(gpu.fDevice),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK) {
		return false;
	}
	if (clocks[0] == 0 || clocks[0] != clocks[2] || clocks[0] != clocks[4])
		return false;
	FdtNode clock;
	if (!clock.SetTo(busModule->node_by_phandle(bus, clocks[0]))
		|| !clock.Enabled() || !clock.HasString("compatible", "rockchip,rk3588-cru")
		|| !clock.Cells("#clock-cells", &cells, 1) || cells != 1
		|| !clock.Reg(info.clockBase, info.clockSize)) {
		return false;
	}
	for (unsigned i = 0; i < 3; i++)
		info.clockIds[i] = clocks[i * 2 + 1];
	device_node* powerNode = busModule->node_by_phandle(bus, power[0]);
	FdtNode powerController;
	if (!powerController.SetTo(powerNode) || !powerController.Enabled()
		|| !powerController.HasString("compatible", "rockchip,rk3588-power-controller")
		|| !powerController.Cells("#power-domain-cells", &cells, 1) || cells != 1) {
		return false;
	}
	device_node* pmuNode = sDeviceManager->get_parent_node(powerNode);
	FdtNode pmu;
	bool validPower = pmu.SetTo(pmuNode) && pmu.Enabled()
		&& pmu.HasString("compatible", "rockchip,rk3588-pmu")
		&& pmu.Reg(info.powerBase, info.powerSize);
	if (pmuNode != NULL)
		sDeviceManager->put_node(pmuNode);
	if (!validPower)
		return false;
	info.powerDomain = power[1];
	FdtNode regulator;
	if (!regulator.SetTo(busModule->node_by_phandle(bus, supply))
		|| !regulator.Enabled() || !regulator.HasString("regulator-name", "vdd_gpu_s0")
		|| !regulator.Cells("regulator-min-microvolt", &info.supplyMinMicrovolt, 1)
		|| !regulator.Cells("regulator-max-microvolt", &info.supplyMaxMicrovolt, 1)) {
		return false;
	}
	info.supplyPhandle = supply;
	strcpy(info.supplyName, "vdd_gpu_s0");
	strcpy(info.boardCompatible, "radxa,rock-5-itx");
	info.version = kResourceVersion;
	info.flags = kDescriptionValidated;
	if (!ResourcesMatch(info))
		return false;
	output = info;
	return true;
}


static float
SupportsDevice(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0;
	}
	ResourceInfo resources;
	return ReadResources(parent, resources) ? 0.8f : 0;
}


static status_t
RegisterDevice(device_node* parent)
{
	device_attr attributes[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Mali CSF resource interface"}},
		{NULL}
	};
	return sDeviceManager->register_node(parent, DRIVER_NAME, attributes, NULL, NULL);
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	Controller* controller = (Controller*)calloc(1, sizeof(Controller));
	if (controller == NULL)
		return B_NO_MEMORY;
	device_node* parent = sDeviceManager->get_parent_node(node);
	bool valid = ReadResources(parent, controller->resources);
	if (parent != NULL)
		sDeviceManager->put_node(parent);
	if (!valid) {
		free(controller);
		return B_NOT_SUPPORTED;
	}
	controller->node = node;
	dprintf("mali_csf: validated firmware resources at %#" B_PRIx64
		"; GPU access disabled\n", controller->resources.gpuBase);
	*cookie = controller;
	return B_OK;
}


static void UninitDriver(void* cookie) { free(cookie); }
static status_t InitDevice(void* driver, void** device) { *device = driver; return B_OK; }
static void UninitDevice(void*) {}
static status_t Close(void*) { return B_OK; }
static status_t Free(void*) { return B_OK; }


static status_t
PublishDevices(void* cookie)
{
	return sDeviceManager->publish_device(((Controller*)cookie)->node,
		"graphics/mali_csf/0", DEVICE_NAME);
}


static status_t
Open(void* cookie, const char*, int mode, void** handle)
{
	if ((mode & O_ACCMODE) != O_RDONLY)
		return B_NOT_ALLOWED;
	*handle = cookie;
	return B_OK;
}


static status_t
Read(void*, off_t, void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}


static status_t
Write(void*, off_t, const void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}


static status_t
Control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if (op != kGetResources)
		return B_DEV_INVALID_IOCTL;
	if (length != sizeof(ResourceInfo))
		return B_BAD_VALUE;
	return user_memcpy(buffer, &((Controller*)cookie)->resources, sizeof(ResourceInfo));
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

static device_module_info sDevice = {
	{DEVICE_NAME, 0, NULL},
	InitDevice, UninitDevice, NULL,
	Open, Close, Free, Read, Write, NULL, Control, NULL, NULL
};

static driver_module_info sDriver = {
	{DRIVER_NAME, 0, NULL},
	SupportsDevice, RegisterDevice, InitDriver, UninitDriver, PublishDevices,
	NULL, NULL, NULL, NULL
};

module_info* modules[] = {
	(module_info*)&sDriver,
	(module_info*)&sDevice,
	NULL
};
