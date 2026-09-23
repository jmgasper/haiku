/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "driver.h"

#include "audio_registers.h"

#include <AutoDeleterOS.h>
#include <bus/FDT.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>


using namespace RK3588Audio;

device_manager_info* gDeviceManager;


class FdtNode {
public:
	bool SetTo(device_node* node)
	{
		driver_module_info* module;
		void* cookie;
		if (node == NULL
			|| gDeviceManager->get_driver(node, &module, &cookie) != B_OK
			|| strcmp(module->info.name, "bus_managers/fdt/driver_v1") != 0) {
			return false;
		}
		fModule = (fdt_device_module_info*)module;
		fDevice = (fdt_device*)cookie;
		return true;
	}

	bool HasString(const char* property, const char* wanted) const
	{
		int length = 0;
		const char* data = (const char*)fModule->get_prop(fDevice, property, &length);
		while (data != NULL && length > 0) {
			const char* end = (const char*)memchr(data, 0, length);
			if (end == NULL || end == data)
				return false;
			if (strcmp(data, wanted) == 0)
				return true;
			length -= end - data + 1;
			data = end + 1;
		}
		return false;
	}

	bool NameIs(const char* name) const
	{
		return strcmp(fModule->get_name(fDevice), name) == 0;
	}

	bool Enabled() const
	{
		int length = 0;
		const char* status = (const char*)fModule->get_prop(fDevice, "status", &length);
		return status == NULL || (length == 5 && memcmp(status, "okay", 5) == 0)
			|| (length == 3 && memcmp(status, "ok", 3) == 0);
	}

	bool Reg(uint64& base, uint64& size) const
	{
		return fModule->get_reg(fDevice, 0, &base, &size);
	}

	fdt_device_module_info* fModule = NULL;
	fdt_device* fDevice = NULL;
};


static bool
BoardMatches(device_node* node)
{
	device_node* current = gDeviceManager->get_parent_node(node);
	while (current != NULL) {
		FdtNode fdt;
		if (!fdt.SetTo(current)) {
			gDeviceManager->put_node(current);
			return false;
		}
		if (fdt.NameIs("")) {
			bool result = fdt.HasString("compatible", "radxa,rock-5-itx");
			gDeviceManager->put_node(current);
			return result;
		}
		device_node* parent = gDeviceManager->get_parent_node(current);
		gDeviceManager->put_node(current);
		current = parent;
	}
	return false;
}


static bool
ReadResources(device_node* parent, uint32& interrupt)
{
	FdtNode i2s;
	uint64 base = 0;
	uint64 size = 0;
	if (!i2s.SetTo(parent) || !i2s.Enabled()
		|| !i2s.HasString("compatible", "rockchip,rk3588-i2s-tdm")
		|| !BoardMatches(parent) || !i2s.Reg(base, size)
		|| base != kI2sBase || size != B_PAGE_SIZE) {
		return false;
	}
	device_node* controller = NULL;
	uint64 irq = 0;
	if (!i2s.fModule->get_interrupt(i2s.fDevice, 0, &controller, &irq)
		|| controller == NULL || irq != 212) {
		return false;
	}
	interrupt = (uint32)irq;
	return true;
}


static status_t
MapRegion(const char* name, uint64 base, size_t size, area_id& area,
	volatile uint32*& registers)
{
	void* address = NULL;
	area = map_physical_memory(name, base, size,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < B_OK)
		return area;
	registers = (volatile uint32*)address;
	return B_OK;
}


static void
UnmapController(AudioController* controller)
{
	rk3588_audio_stop(controller);
	rk3588_audio_release_buffers(controller);
	area_id* areas[] = {&controller->i2sArea, &controller->i2cArea,
		&controller->mclkArea, &controller->iocArea, &controller->pmuArea,
		&controller->cruArea};
	for (size_t index = 0; index < sizeof(areas) / sizeof(areas[0]); index++) {
		if (*areas[index] >= B_OK) {
			delete_area(*areas[index]);
			*areas[index] = -1;
		}
	}
}


static float
SupportsDevice(device_node* parent)
{
	const char* bus;
	uint32 interrupt;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0;
	}
	return ReadResources(parent, interrupt) ? 0.8f : 0;
}


static status_t
RegisterDevice(device_node* parent)
{
	device_attr attributes[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "RK3588 I2S0 / ES8316 audio"}},
		{NULL}
	};
	return gDeviceManager->register_node(parent, RK3588_AUDIO_DRIVER_NAME,
		attributes, NULL, NULL);
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	AudioController* controller = (AudioController*)calloc(1,
		sizeof(AudioController));
	if (controller == NULL)
		return B_NO_MEMORY;
	controller->cruArea = controller->pmuArea = controller->iocArea = -1;
	controller->mclkArea = controller->i2cArea = controller->i2sArea = -1;
	controller->stream.area = -1;
	controller->stream.readySem = -1;
	controller->stream.lock = B_SPINLOCK_INITIALIZER;
	mutex_init(&controller->hardwareLock, "RK3588 audio hardware");
	device_node* parent = gDeviceManager->get_parent_node(node);
	bool valid = ReadResources(parent, controller->interrupt);
	if (parent != NULL)
		gDeviceManager->put_node(parent);
	status_t status = valid ? B_OK : B_NOT_SUPPORTED;
	if (status == B_OK)
		status = MapRegion("RK3588 audio CRU", kCruBase, B_PAGE_SIZE,
			controller->cruArea, controller->cru);
	if (status == B_OK)
		status = MapRegion("RK3588 audio PMU", kPmuBase, B_PAGE_SIZE,
			controller->pmuArea, controller->pmu);
	if (status == B_OK)
		status = MapRegion("RK3588 audio IOC", kIocBase, 0x10000,
			controller->iocArea, controller->ioc);
	if (status == B_OK)
		status = MapRegion("RK3588 audio MCLK gate", kMclkBase, B_PAGE_SIZE,
			controller->mclkArea, controller->mclk);
	if (status == B_OK)
		status = MapRegion("RK3588 audio I2C7", kI2cBase, B_PAGE_SIZE,
			controller->i2cArea, controller->i2c);
	if (status == B_OK)
		status = MapRegion("RK3588 audio I2S0", kI2sBase, B_PAGE_SIZE,
			controller->i2sArea, controller->i2s);
	if (status != B_OK) {
		UnmapController(controller);
		mutex_destroy(&controller->hardwareLock);
		free(controller);
		return status;
	}
	controller->node = node;
	*cookie = controller;
	dprintf("rk3588_audio: validated ROCK 5 ITX I2S0 at %#" B_PRIx64
		" interrupt %" B_PRIu32 "\n", kI2sBase, controller->interrupt);
	return B_OK;
}


static void
UninitDriver(void* cookie)
{
	AudioController* controller = (AudioController*)cookie;
	UnmapController(controller);
	mutex_destroy(&controller->hardwareLock);
	free(controller);
}


static status_t InitDevice(void* driver, void** device)
{
	*device = driver;
	return B_OK;
}


static void UninitDevice(void*) {}


static status_t
PublishDevices(void* cookie)
{
	return gDeviceManager->publish_device(((AudioController*)cookie)->node,
		RK3588_AUDIO_DEVICE_PATH, RK3588_AUDIO_DEVICE_NAME);
}


static status_t
Open(void* cookie, const char*, int, void** handle)
{
	AudioController* controller = (AudioController*)cookie;
	if (atomic_add(&controller->openCount, 1) != 0) {
		atomic_add(&controller->openCount, -1);
		return B_BUSY;
	}
	*handle = controller;
	return B_OK;
}


static status_t
Close(void* cookie)
{
	rk3588_audio_stop((AudioController*)cookie);
	return B_OK;
}


static status_t
Free(void* cookie)
{
	AudioController* controller = (AudioController*)cookie;
	rk3588_audio_release_buffers(controller);
	atomic_add(&controller->openCount, -1);
	return B_OK;
}


static status_t
Read(void*, off_t, void*, size_t* bytes)
{
	*bytes = 0;
	return B_IO_ERROR;
}


static status_t
Write(void*, off_t, const void*, size_t* bytes)
{
	*bytes = 0;
	return B_IO_ERROR;
}


static status_t
Control(void* cookie, uint32 operation, void* buffer, size_t length)
{
	return rk3588_audio_control((AudioController*)cookie, operation, buffer,
		length);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager},
	{}
};

static device_module_info sDevice = {
	{RK3588_AUDIO_DEVICE_NAME, 0, NULL},
	InitDevice, UninitDevice, NULL,
	Open, Close, Free, Read, Write, NULL, Control, NULL, NULL
};

static driver_module_info sDriver = {
	{RK3588_AUDIO_DRIVER_NAME, 0, NULL},
	SupportsDevice, RegisterDevice, InitDriver, UninitDriver, PublishDevices,
	NULL, NULL, NULL, NULL
};

module_info* modules[] = {
	(module_info*)&sDriver,
	(module_info*)&sDevice,
	NULL
};
