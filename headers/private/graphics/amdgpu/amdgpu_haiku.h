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
	AMDGPU_GET_INFO = B_DEVICE_OP_CODES_END + 1,
	AMDGPU_READ_ROM,
	AMDGPU_SDMA_TEST,
	AMDGPU_SMC_BOOTSTRAP
};

#define AMDGPU_ROM_SIZE (256 * 1024)
struct amdgpu_rom {
	uint32 version;
	uint32 size;
	uint64 data; // userspace destination, AMDGPU_ROM_SIZE bytes
	uint32 capacity;
	uint32 reserved;
};

// Root-only bring-up operation. The kernel builds all packets and addresses;
// userspace supplies only the firmware image, never a GPU program or address.
struct amdgpu_sdma_test_result {
	uint32 version;
	uint32 size;
	uint64 firmware;
	uint32 firmware_size;
	uint32 reserved;
	int32 status;
	uint32 stage; // 1: validated, 2: VRAM mapped, 3: firmware, 4: submitted, 5: done
	uint64 scratch_gpu;
	uint32 firmware_version;
	uint32 checked_bytes;
	uint32 mismatches;
	uint32 fence;
	uint32 rptr;
	uint32 wptr;
	uint32 engine_status;
	uint32 halted;
	uint64 elapsed_us;
};

// Protected-mode SMU startup for the measured Polaris 10 hard-key board.
// This starts signed controller firmware; it does not enable DPM or engines.
struct amdgpu_smc_bootstrap {
	uint32 version;
	uint32 size;
	uint64 firmware;
	uint32 firmware_size;
	uint32 reserved;
	int32 status;
	uint32 stage;
	uint32 firmware_version;
	uint32 clock;
	uint32 pc;
	uint32 security;
	uint32 smu_status;
	uint32 response;
	uint32 soft_registers;
	uint32 reserved_out;
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
