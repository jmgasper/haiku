/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDGPU_HAIKU_H
#define AMDGPU_HAIKU_H

#include <Drivers.h>

// Native bring-up interface. This is not the Linux DRM ABI. Keep it separate
// from the eventual Mesa command-submission interface.
#define AMDGPU_HAIKU_ABI_VERSION 1
#define AMDGPU_DEVICE_NAME "dri/amdgpu/0"

enum {
	AMDGPU_GET_INFO = B_DEVICE_OP_CODES_END + 1
};

struct amdgpu_register_value {
	uint32 index;	// AMD register indices are in dwords, not bytes
	uint32 value;
};

#define AMDGPU_INFO_REGISTER_COUNT 40

struct amdgpu_info {
	uint32 version;	// caller must supply AMDGPU_HAIKU_ABI_VERSION
	uint32 size;		// caller must supply sizeof(amdgpu_info)
	uint16 vendor;
	uint16 device;
	uint16 subsystem_vendor;
	uint16 subsystem_device;
	uint8 revision;
	uint8 bus;
	uint8 slot;
	uint8 function;
	uint32 interrupt;
	uint32 pci_command;
	uint64 bar_address[6];
	uint64 bar_size[6];
	uint64 vram_size;
	uint64 vram_gpu_base;
	uint64 boot_framebuffer;
	uint64 boot_framebuffer_size;
	uint32 boot_width;
	uint32 boot_height;
	uint32 boot_stride;
	uint32 register_count;
	amdgpu_register_value registers[AMDGPU_INFO_REGISTER_COUNT];
};

#endif
