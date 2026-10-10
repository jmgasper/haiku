/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Imagination PowerVR Rogue GPUs: the BXM-4-64 MC1 of the Allwinner A733
	(Radxa Cubie A7S). Bring-up stage 1: the GPU powered and identified;
	stage 2: the firmware booted on the GPU's MIPS core and answering
	(PvrDevice, behind the driver settings).

	The plan (lab evidence/gpu/DESIGN.md): a native driver that reuses the
	hardware and firmware code of Linux's drm/imagination (upstream/, built
	on compat/ and glue/) and speaks its pvr_drm.h structures to Mesa's
	PowerVR Vulkan driver. */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <kernel.h>
#include <pvr_haiku.h>

#include "PvrDevice.h"


#define POWERVR_DRIVER_MODULE_NAME	"drivers/graphics/powervr/driver_v1"
#define POWERVR_DEVICE_MODULE_NAME	"drivers/graphics/powervr/device_v1"


using powervr::PvrDevice;


struct powervr_info {
	device_node*	node;
	PvrDevice*		device;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - device


static status_t
powervr_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static status_t
powervr_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	*_cookie = deviceCookie;
	return B_OK;
}


static status_t
powervr_close(void* cookie)
{
	return B_OK;
}


static status_t
powervr_free(void* cookie)
{
	return B_OK;
}


static status_t
powervr_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	powervr_info* info = (powervr_info*)cookie;

	if (op == PVR_HAIKU_OP(PVR_HAIKU_NR_STAGE)) {
		pvr_haiku_stage stage;
		if (length != sizeof(stage))
			return B_BAD_VALUE;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&stage, buffer, sizeof(stage)) != B_OK) {
			return B_BAD_ADDRESS;
		}
		if (stage.version != PVR_HAIKU_ABI_VERSION)
			return B_BAD_VALUE;
		status_t status = info->device->Stage(stage);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &stage, sizeof(stage));
	}

	// Not a display driver: app_server asks every device under graphics/
	// for an accelerant and has to be turned away.
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
powervr_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "allwinner,sun60i-a733-gpu") != 0) {
		return 0.0f;
	}
	return 1.0f;
}


static status_t
powervr_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Imagination PowerVR GPU" } },
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED } },
		{}
	};
	return sDeviceManager->register_node(parent, POWERVR_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
powervr_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	uint64 registerBase, registerSize, interrupt;
	if (!fdt->get_reg(device, 0, &registerBase, &registerSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		return B_BAD_DATA;
	}

	powervr_info* info = (powervr_info*)calloc(1, sizeof(powervr_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->device = new(std::nothrow) PvrDevice(registerBase, registerSize,
		(int32)interrupt);
	if (info->device == NULL) {
		free(info);
		return B_NO_MEMORY;
	}

	status = info->device->Init();
	if (status != B_OK) {
		delete info->device;
		free(info);
		return status;
	}
	*_cookie = info;
	return B_OK;
}


static void
powervr_uninit_driver(void* cookie)
{
	powervr_info* info = (powervr_info*)cookie;
	delete info->device;
	free(info);
}


static status_t
powervr_register_child_devices(void* cookie)
{
	powervr_info* info = (powervr_info*)cookie;
	return sDeviceManager->publish_device(info->node, "graphics/powervr/0",
		POWERVR_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static device_module_info sPowervrDevice = {
	{
		POWERVR_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	powervr_init_device,
	NULL,	// uninit: the hardware state lives with the driver node
	NULL,	// removed
	powervr_open,
	powervr_close,
	powervr_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	powervr_control,
	NULL,	// select
	NULL,	// deselect
};


static driver_module_info sPowervrDriver = {
	{
		POWERVR_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	powervr_supports_device,
	powervr_register_device,
	powervr_init_driver,
	powervr_uninit_driver,
	powervr_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sPowervrDriver,
	(module_info*)&sPowervrDevice,
	NULL
};
