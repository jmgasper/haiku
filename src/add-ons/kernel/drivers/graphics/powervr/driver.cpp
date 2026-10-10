/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Imagination PowerVR Rogue GPUs: the BXM-4-64 MC1 of the Allwinner A733
	(Radxa Cubie A7S). Bring-up stage 1: the GPU powered and identified.

	The plan (lab evidence/gpu/DESIGN.md): a native driver that reuses the
	hardware and firmware code of Linux's drm/imagination and speaks its
	pvr_drm.h structures to Mesa's PowerVR Vulkan driver. */


#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <pvr_haiku.h>

#include "A733Power.h"


#define POWERVR_DRIVER_MODULE_NAME	"drivers/graphics/powervr/driver_v1"
#define POWERVR_DEVICE_MODULE_NAME	"drivers/graphics/powervr/device_v1"

#define TRACE(x...)	dprintf("powervr: " x)

// Rogue control registers (pvr_rogue_cr_defs.h)
#define ROGUE_CR_CORE_ID			0x0018
#define ROGUE_CR_CORE_ID__PBVNC		0x0020


struct powervr_info {
	device_node*	node;
	uint64			registerBase;
	uint64			registerSize;
	uint32			interrupt;

	area_id			registerArea;
	volatile uint8*	registers;

	uint32			stage;
	uint64			bvnc;
	uint32			coreId;
	uint32			coreClock;
};


static device_manager_info* sDeviceManager;


static bool
disabled_by_settings()
{
	void* handle = load_driver_settings("powervr");
	if (handle == NULL)
		return false;
	bool disabled = get_driver_boolean_parameter(handle, "disable", false,
		true);
	unload_driver_settings(handle);
	return disabled;
}


static status_t
bring_up(powervr_info* info)
{
	info->stage = PVR_HAIKU_STAGE_OFF;
	if (disabled_by_settings()) {
		TRACE("disabled by the driver settings\n");
		return B_OK;
	}

	status_t status = powervr::a733_gpu_power_on(&info->coreClock);
	if (status != B_OK) {
		TRACE("the GPU does not power up: %s\n", strerror(status));
		return status;
	}
	info->stage = PVR_HAIKU_STAGE_POWERED;

	info->registerArea = map_physical_memory("powervr registers",
		info->registerBase, info->registerSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->registers);
	if (info->registerArea < 0)
		return info->registerArea;

	info->bvnc = *(volatile uint64*)(info->registers + ROGUE_CR_CORE_ID__PBVNC);
	info->coreId = *(volatile uint32*)(info->registers + ROGUE_CR_CORE_ID);
	TRACE("BVNC %u.%u.%u.%u (%#" B_PRIx64 "), core ID %#" B_PRIx32
		", %" B_PRIu32 " MHz\n", (unsigned)(info->bvnc >> 48),
		(unsigned)((info->bvnc >> 32) & 0xffff),
		(unsigned)((info->bvnc >> 16) & 0xffff),
		(unsigned)(info->bvnc & 0xffff), info->bvnc, info->coreId,
		info->coreClock / 1000000);
	if (info->bvnc != 0)
		info->stage = PVR_HAIKU_STAGE_IDENTIFIED;
	return B_OK;
}


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
		if (length != sizeof(pvr_haiku_stage))
			return B_BAD_VALUE;
		pvr_haiku_stage stage = {};
		stage.version = PVR_HAIKU_ABI_VERSION;
		stage.stage = info->stage;
		stage.bvnc = info->bvnc;
		stage.core_id = info->coreId;
		stage.core_clock = info->coreClock;
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

	powervr_info* info = (powervr_info*)calloc(1, sizeof(powervr_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->registerArea = -1;

	uint64 interrupt;
	if (!fdt->get_reg(device, 0, &info->registerBase, &info->registerSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		free(info);
		return B_BAD_DATA;
	}
	info->interrupt = interrupt;

	status = bring_up(info);
	if (status != B_OK) {
		if (info->registerArea >= 0)
			delete_area(info->registerArea);
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
	if (info->registerArea >= 0)
		delete_area(info->registerArea);
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
