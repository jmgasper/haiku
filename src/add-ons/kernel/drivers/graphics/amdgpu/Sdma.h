/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_SDMA_H
#define AMDGPU_SDMA_H
#include <amdgpu_haiku.h>
#include "Firmware.h"

status_t amdgpu_sdma_test(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware,
	const amdgpu::AtomVramReservation& reservation, amdgpu_sdma_test_result& result);
#endif
