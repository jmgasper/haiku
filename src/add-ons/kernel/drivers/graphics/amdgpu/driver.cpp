/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <Drivers.h>
#include <KernelExport.h>
#include <PCI.h>
#include <boot_item.h>
#include <frame_buffer_console.h>
#include <lock.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <amdgpu_haiku.h>
#include "Rom.h"
#include "Sdma.h"
#include "Smu.h"

// Polaris 10 register indices, from AMD's MIT-licensed register headers:
// Linux drivers/gpu/drm/amd/include/asic_reg/{bif/bif_5_0_d.h,
// gmc/gmc_8_1_d.h,gca/gfx_8_0_d.h,oss/oss_3_0_d.h,uvd/uvd_6_0_d.h,
// dce/dce_11_2_d.h}.
// Only direct, non-destructive reads; no indexed or interrupt-ack registers.
static const uint32 kSnapshotRegisters[] = {
	0x150a,	// CONFIG_MEMSIZE (MiB)
	0x0809,	// MC_VM_FB_LOCATION
	0x081a,	// MC_VM_FB_OFFSET
	0x080d,	// MC_VM_SYSTEM_APERTURE_LOW_ADDR
	0x080e,	// MC_VM_SYSTEM_APERTURE_HIGH_ADDR
	0x0504,	// VM_CONTEXT0_CNTL
	0x2004,	// GRBM_STATUS
	0x263e,	// GB_ADDR_CONFIG
	0x0394,	// SRBM_STATUS
	0x340d,	// SDMA0_STATUS_REG
	0x3412,	// SDMA0_F32_CNTL
	0x3480,	// SDMA0_GFX_RB_CNTL
	0x360d,	// SDMA1_STATUS_REG
	0x3612,	// SDMA1_F32_CNTL
	0x3daf,	// UVD_STATUS
	0x38c4,	// UVD_POWER_STATUS
	// Each head: CRTC_CONTROL, GRPH_PRIMARY_SURFACE_ADDRESS, _HIGH, PITCH.
	0x1b9c, 0x1a04, 0x1a07, 0x1a06,
	0x1d9c, 0x1c04, 0x1c07, 0x1c06,
	0x1f9c, 0x1e04, 0x1e07, 0x1e06,
	0x419c, 0x4004, 0x4007, 0x4006,
	0x439c, 0x4204, 0x4207, 0x4206,
	0x459c, 0x4404, 0x4407, 0x4406,
};

static const uint64 kRegisterMapSize = 0x40000;
static pci_module_info* sPCI;
static pci_info sDevice;
static amdgpu_info sInfo;
static area_id sRegisterArea = -1;
static volatile uint32* sRegisters;
static mutex sLock = MUTEX_INITIALIZER("amdgpu");
static uint32 sOpenCount;
static const char* sDeviceNames[] = { AMDGPU_DEVICE_NAME, NULL };

int32 api_version = B_CUR_DRIVER_API_VERSION;


static bool
is_supported(const pci_info& info)
{
	// Admit only the board being brought up. Sharing a Polaris family name
	// does not establish that another board has been tested.
	return info.vendor_id == 0x1002 && info.device_id == 0x67c7
		&& info.class_base == PCI_display && (info.header_type & 0x7f) == 0;
}


static status_t
find_device(pci_module_info* pci, pci_info& info)
{
	for (uint32 index = 0; pci->get_nth_pci_info(index, &info) == B_OK; index++) {
		if (is_supported(info))
			return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
map_registers()
{
	memset(&sInfo, 0, sizeof(sInfo));
	sInfo.version = AMDGPU_HAIKU_ABI_VERSION;
	sInfo.size = sizeof(sInfo);
	sInfo.vendor = sDevice.vendor_id;
	sInfo.device = sDevice.device_id;
	sInfo.subsystem_vendor = sDevice.u.h0.subsystem_vendor_id;
	sInfo.subsystem_device = sDevice.u.h0.subsystem_id;
	sInfo.revision = sDevice.revision;
	sInfo.bus = sDevice.bus;
	sInfo.slot = sDevice.device;
	sInfo.function = sDevice.function;
	sInfo.interrupt = sDevice.u.h0.interrupt_line;
	sInfo.pci_command = sPCI->read_pci_config(sDevice.bus, sDevice.device,
		sDevice.function, PCI_command, 2);
	if ((sInfo.pci_command & PCI_command_memory) == 0)
		return B_NOT_ALLOWED;

	for (uint32 bar = 0; bar < 6; bar++) {
		sInfo.bar_address[bar] = sDevice.u.h0.base_registers[bar];
		sInfo.bar_size[bar] = sDevice.u.h0.base_register_sizes[bar];
		uint32 flags = sDevice.u.h0.base_register_flags[bar];
		if ((flags & PCI_address_space) == 0
			&& (flags & PCI_address_type) == PCI_address_type_64) {
			if (bar == 5)
				return B_BAD_VALUE;
			sInfo.bar_address[bar]
				|= (uint64)sDevice.u.h0.base_registers[bar + 1] << 32;
			sInfo.bar_size[bar]
				|= (uint64)sDevice.u.h0.base_register_sizes[bar + 1] << 32;
			bar++;
		}
	}

	// GCN 1.2 and newer expose registers at BAR5, not the older BAR2.
	if ((sDevice.u.h0.base_register_flags[5] & PCI_address_space)
			!= 0
		|| sInfo.bar_address[5] == 0 || sInfo.bar_size[5] < kRegisterMapSize)
		return B_BAD_VALUE;

	sRegisterArea = map_physical_memory("amdgpu registers", sInfo.bar_address[5],
		kRegisterMapSize, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA,
		(void**)&sRegisters);
	if (sRegisterArea < 0)
		return sRegisterArea;

	sInfo.vram_size = (uint64)sRegisters[0x150a] << 20;
	sInfo.vram_gpu_base = (uint64)(sRegisters[0x0809] & 0xffff) << 24;
	const frame_buffer_boot_info* boot = (const frame_buffer_boot_info*)
		get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (boot != NULL && boot->width > 0 && boot->height > 0
		&& boot->bytes_per_row > 0) {
		sInfo.boot_framebuffer = boot->physical_frame_buffer;
		sInfo.boot_width = boot->width;
		sInfo.boot_height = boot->height;
		sInfo.boot_stride = boot->bytes_per_row;
		sInfo.boot_framebuffer_size
			= (uint64)boot->height * boot->bytes_per_row;
	}
	dprintf("amdgpu: WX 5100 %02x:%02x.%u rev %02x, %" B_PRIu64
		" MiB VRAM, read-only observation\n", sDevice.bus, sDevice.device,
		sDevice.function, sDevice.revision, sInfo.vram_size >> 20);
	return B_OK;
}


static status_t
device_open(const char* name, uint32 flags, void** cookie)
{
	if (strcmp(name, AMDGPU_DEVICE_NAME) != 0)
		return B_ENTRY_NOT_FOUND;
	mutex_lock(&sLock);
	status_t status = sOpenCount == 0 ? map_registers() : B_OK;
	if (status == B_OK) {
		sOpenCount++;
		*cookie = &sInfo;
	}
	mutex_unlock(&sLock);
	return status;
}


static status_t
device_close(void* cookie)
{
	return B_OK;
}


static status_t
device_free(void* cookie)
{
	mutex_lock(&sLock);
	if (--sOpenCount == 0) {
		delete_area(sRegisterArea);
		sRegisterArea = -1;
		sRegisters = NULL;
	}
	mutex_unlock(&sLock);
	return B_OK;
}


static status_t
device_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if (op == AMDGPU_SMC_BOOTSTRAP) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_smc_bootstrap))
			return B_BAD_VALUE;
		amdgpu_smc_bootstrap request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION
			|| request.size != sizeof(request) || request.reserved != 0
			|| request.firmware_size < 36 || request.firmware_size > 0x20100)
			return B_BAD_VALUE;
		void* firmware = malloc(request.firmware_size);
		if (firmware == NULL)
			return B_NO_MEMORY;
		status_t status = user_memcpy(firmware, (void*)(addr_t)request.firmware,
			request.firmware_size);
		amdgpu::FirmwareView view;
		if (status == B_OK
			&& !amdgpu::ParseSmcFirmware(firmware, request.firmware_size, view))
			status = B_BAD_DATA;
		amdgpu_smc_bootstrap result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		if (status == B_OK) {
			mutex_lock(&sLock);
			status = set_area_protection(sRegisterArea,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
			if (status == B_OK) {
				status = amdgpu_smc_bootstrap_firmware(sRegisters, view, result);
				set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
			}
			mutex_unlock(&sLock);
		}
		free(firmware);
		result.status = status;
		return user_memcpy(buffer, &result, sizeof(result));
	}
	if (op == AMDGPU_SDMA_TEST) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_sdma_test_result))
			return B_BAD_VALUE;
		amdgpu_sdma_test_result request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION
			|| request.size != sizeof(request) || request.reserved != 0
			|| request.firmware_size < 52 || request.firmware_size > 65536)
			return B_BAD_VALUE;
		void* firmware = malloc(request.firmware_size);
		void* rom = malloc(AMDGPU_ROM_SIZE);
		if (firmware == NULL || rom == NULL) {
			free(firmware);
			free(rom);
			return B_NO_MEMORY;
		}
		status_t status = user_memcpy(firmware, (void*)(addr_t)request.firmware,
			request.firmware_size);
		amdgpu::FirmwareView view;
		if (status == B_OK
			&& !amdgpu::ParseSdmaFirmware(firmware, request.firmware_size, view))
			status = B_BAD_DATA;
		amdgpu_sdma_test_result result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		if (status == B_OK) {
			mutex_lock(&sLock);
			status = set_area_protection(sRegisterArea,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
			if (status == B_OK) {
				status = amdgpu_read_rom(sDevice, sRegisters, rom, AMDGPU_ROM_SIZE);
				amdgpu::AtomVramReservation reservation;
				if (status == B_OK && !amdgpu::ParseAtomVramReservation(rom,
						AMDGPU_ROM_SIZE, reservation))
					status = B_BAD_DATA;
				if (status == B_OK)
					status = amdgpu_sdma_test(sRegisters, sInfo, view, reservation,
						result);
				set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
			}
			mutex_unlock(&sLock);
		}
		free(firmware);
		free(rom);
		result.status = status;
		return user_memcpy(buffer, &result, sizeof(result));
	}
	if (op == AMDGPU_READ_ROM) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_rom))
			return B_BAD_VALUE;
		amdgpu_rom request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION
			|| request.size != sizeof(request) || request.reserved != 0
			|| request.capacity != AMDGPU_ROM_SIZE)
			return B_BAD_VALUE;
		void* rom = malloc(AMDGPU_ROM_SIZE);
		if (rom == NULL)
			return B_NO_MEMORY;
		mutex_lock(&sLock);
		status_t status = set_area_protection(sRegisterArea,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (status == B_OK) {
			status = amdgpu_read_rom(sDevice, sRegisters, rom, AMDGPU_ROM_SIZE);
			set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
		}
		mutex_unlock(&sLock);
		if (status == B_OK)
			status = user_memcpy((void*)(addr_t)request.data, rom, AMDGPU_ROM_SIZE);
		free(rom);
		return status;
	}
	if (op != AMDGPU_GET_INFO)
		return B_DEV_INVALID_IOCTL;
	if (length != sizeof(amdgpu_info))
		return B_BAD_VALUE;

	uint32 header[2];
	if (user_memcpy(header, buffer, sizeof(header)) != B_OK)
		return B_BAD_ADDRESS;
	if (header[0] != AMDGPU_HAIKU_ABI_VERSION || header[1] != sizeof(amdgpu_info))
		return B_BAD_VALUE;

	mutex_lock(&sLock);
	amdgpu_info info = sInfo;
	info.register_count = B_COUNT_OF(kSnapshotRegisters);
	static_assert(B_COUNT_OF(kSnapshotRegisters) == AMDGPU_INFO_REGISTER_COUNT,
		"register snapshot size");
	for (uint32 i = 0; i < info.register_count; i++) {
		info.registers[i].index = kSnapshotRegisters[i];
		info.registers[i].value = sRegisters[kSnapshotRegisters[i]];
	}
	mutex_unlock(&sLock);
	return user_memcpy(buffer, &info, sizeof(info));
}


static status_t
device_read(void* cookie, off_t position, void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	device_open, device_close, device_free, device_control,
	device_read, device_write, NULL, NULL, NULL, NULL
};


status_t
init_hardware()
{
	pci_module_info* pci;
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&pci);
	if (status != B_OK)
		return status;
	pci_info info;
	status = find_device(pci, info);
	put_module(B_PCI_MODULE_NAME);
	dprintf("amdgpu: probe: %s\n", status == B_OK
		? "WX 5100 found" : "no supported device");
	return status;
}


status_t
init_driver()
{
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI);
	if (status != B_OK)
		return status;
	status = find_device(sPCI, sDevice);
	if (status != B_OK) {
		put_module(B_PCI_MODULE_NAME);
		sPCI = NULL;
	}
	return status;
}


void
uninit_driver()
{
	put_module(B_PCI_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return strcmp(name, AMDGPU_DEVICE_NAME) == 0 ? &sHooks : NULL;
}
