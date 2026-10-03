/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Broadcom V3D 4.2, the 3D core of the BCM2711 (Raspberry Pi 4).

	This is the render-only device that Mesa's v3d and v3dv drivers talk to;
	scan-out is somebody else's business. So far the driver powers the core
	up and identifies it.

	The device tree's node is "disabled" unless the firmware was told to hand
	the display pipeline to the OS (vc4-kms-v3d); the core is there all the
	same and the driver ignores the status. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include <kernel.h>
#include <rpi_firmware.h>

#include "v3d_regs.h"


#define INFO(x...)	dprintf("v3d: " x)
#define ERROR(x...)	dprintf("v3d: " x)

#define V3D_DRIVER_MODULE_NAME	"drivers/graphics/v3d/driver_v1"
#define V3D_DEVICE_MODULE_NAME	"drivers/graphics/v3d/device_v1"

// The power management block is not a child of the V3D node and the FDT
// bus offers no way to walk to it; these are its addresses on the BCM2711.
#define BCM2711_PM_BASE			0xfe100000
#define BCM2711_RPIVID_ASB_BASE	0xfec11000


struct v3d_info {
	device_node*	node;
	uint64			hubBase;
	uint64			hubSize;
	uint64			coreBase;
	uint64			coreSize;
	uint32			interrupt;

	area_id			hubArea;
	area_id			coreArea;
	area_id			pmArea;
	area_id			asbArea;
	volatile uint8*	hub;
	volatile uint8*	core;
	volatile uint8*	pm;
	volatile uint8*	asb;

	uint32			version;	// 42 for V3D 4.2
	uint32			clockRate;
};


static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;


static inline uint32
read32(volatile uint8* base, uint32 reg)
{
	return *(volatile uint32*)(base + reg);
}


static inline void
write32(volatile uint8* base, uint32 reg, uint32 value)
{
	*(volatile uint32*)(base + reg) = value;
}


static status_t
map(const char* name, uint64 base, uint64 size, area_id& area,
	volatile uint8*& address)
{
	uint64 offset = base & (B_PAGE_SIZE - 1);
	void* mapped;
	area = map_physical_memory(name, base - offset,
		ROUNDUP(size + offset, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapped);
	if (area < 0)
		return area;

	address = (volatile uint8*)mapped + offset;
	return B_OK;
}


static status_t
asb_enable(v3d_info* info, uint32 reg)
{
	write32(info->asb, reg,
		PM_PASSWORD | (read32(info->asb, reg) & ~ASB_REQ_STOP));

	bigtime_t timeout = system_time() + 10000;
	while ((read32(info->asb, reg) & ASB_ACK) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(5);
	}
	return B_OK;
}


/*!	The V3D power domain: take the core out of reset with its clock running
	and open the bus bridges to it (Linux: bcm2835_asb_power_on()).
*/
static status_t
power_on(v3d_info* info)
{
	// The firmware owns the clock.
	uint32 maximum = 0;
	sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_V3D, true, &maximum);

	status_t status = sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, true);
	if (status != B_OK)
		return status;
	spin(1);
	sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, false);

	write32(info->pm, PM_GRAFX,
		PM_PASSWORD | read32(info->pm, PM_GRAFX) | PM_V3DRSTN);

	status = sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, true);
	if (status != B_OK)
		return status;
	if (maximum != 0)
		sFirmware->set_clock_rate(RPI_FIRMWARE_CLOCK_V3D, maximum);
	sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_V3D, false, &info->clockRate);

	status = asb_enable(info, ASB_V3D_M_CTRL);
	if (status == B_OK)
		status = asb_enable(info, ASB_V3D_S_CTRL);
	if (status != B_OK)
		ERROR("the bus bridges to the core do not open\n");
	return status;
}


//	#pragma mark - device


static status_t
v3d_init_device(void* _info, void** _cookie)
{
	v3d_info* info = (v3d_info*)_info;

	status_t status = map("v3d hub", info->hubBase, info->hubSize,
		info->hubArea, info->hub);
	if (status == B_OK) {
		status = map("v3d core", info->coreBase, info->coreSize,
			info->coreArea, info->core);
	}
	if (status == B_OK) {
		status = map("v3d pm", BCM2711_PM_BASE, 0x200, info->pmArea,
			info->pm);
	}
	if (status == B_OK) {
		status = map("v3d asb", BCM2711_RPIVID_ASB_BASE, 0x24, info->asbArea,
			info->asb);
	}
	if (status != B_OK)
		return status;

	status = power_on(info);
	if (status != B_OK) {
		ERROR("power on failed: %s\n", strerror(status));
		return status;
	}

	uint32 hubIdent0 = read32(info->hub, V3D_HUB_IDENT0);
	uint32 hubIdent1 = read32(info->hub, V3D_HUB_IDENT1);
	uint32 hubIdent2 = read32(info->hub, V3D_HUB_IDENT2);
	uint32 hubIdent3 = read32(info->hub, V3D_HUB_IDENT3);
	uint32 coreIdent0 = read32(info->core, V3D_CTL_IDENT0);

	// "VHUB" and "V3D" plus the technology version
	if (hubIdent0 != ('V' | 'H' << 8 | 'U' << 16 | 'B' << 24)
		|| (coreIdent0 & 0x00ffffff) != ('V' | '3' << 8 | 'D' << 16)) {
		ERROR("no V3D behind the registers: hub %#" B_PRIx32 ", core %#"
			B_PRIx32 "\n", hubIdent0, coreIdent0);
		return B_DEVICE_NOT_FOUND;
	}

	info->version = V3D_HUB_IDENT1_TVER(hubIdent1) * 10
		+ V3D_HUB_IDENT1_REV(hubIdent1);

	INFO("V3D %" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 ", %"
		B_PRIu32 " core(s)%s%s, clock %" B_PRIu32 " Hz, interrupt %" B_PRIu32
		"\n", info->version / 10, info->version % 10, (hubIdent3 >> 8) & 0xff,
		hubIdent3 & 0xff, V3D_HUB_IDENT1_NCORES(hubIdent1),
		(hubIdent1 & V3D_HUB_IDENT1_WITH_TFU) != 0 ? ", TFU" : "",
		(hubIdent2 & V3D_HUB_IDENT2_WITH_MMU) != 0 ? ", MMU" : "",
		info->clockRate, info->interrupt);

	*_cookie = info;
	return B_OK;
}


static void
v3d_uninit_device(void* cookie)
{
	v3d_info* info = (v3d_info*)cookie;
	delete_area(info->hubArea);
	delete_area(info->coreArea);
	delete_area(info->pmArea);
	delete_area(info->asbArea);
}


static status_t
v3d_open(void* _info, const char* path, int openMode, void** _cookie)
{
	*_cookie = _info;
	return B_OK;
}


static status_t
v3d_close(void* cookie)
{
	return B_OK;
}


static status_t
v3d_free(void* cookie)
{
	return B_OK;
}


static status_t
v3d_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	// Not a display driver: app_server asks every device under graphics/
	// for an accelerant and has to be turned away.
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
v3d_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "brcm,2711-v3d") != 0) {
		return 0.0f;
	}

	return 1.0f;
}


static status_t
v3d_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Broadcom V3D"}},
		{}
	};

	return sDeviceManager->register_node(parent, V3D_DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
v3d_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	v3d_info* info = (v3d_info*)calloc(1, sizeof(v3d_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;

	// reg: "hub", "core0"
	uint64 interrupt;
	if (!fdt->get_reg(device, 0, &info->hubBase, &info->hubSize)
		|| !fdt->get_reg(device, 1, &info->coreBase, &info->coreSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		free(info);
		return B_BAD_DATA;
	}
	info->interrupt = interrupt;

	*_cookie = info;
	return B_OK;
}


static void
v3d_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
v3d_register_child_devices(void* cookie)
{
	v3d_info* info = (v3d_info*)cookie;
	return sDeviceManager->publish_device(info->node, "graphics/v3d/0",
		V3D_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware},
	{}
};

static device_module_info sV3dDevice = {
	{
		V3D_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	v3d_init_device,
	v3d_uninit_device,
	NULL,	// removed
	v3d_open,
	v3d_close,
	v3d_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	v3d_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sV3dDriver = {
	{
		V3D_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	v3d_supports_device,
	v3d_register_device,
	v3d_init_driver,
	v3d_uninit_driver,
	v3d_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sV3dDriver,
	(module_info*)&sV3dDevice,
	NULL
};
