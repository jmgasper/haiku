/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_GFX_H
#define AMDGPU_GFX_H
#include "Gart.h"
struct GfxEngine {
	volatile uint32* regs;
	volatile uint32* memory;
	area_id area;
	uint64 gpu;
	uint32 wptr, sequence;
	bool attempted, ready, faulted;
	status_t Test(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation,
		const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
		amdgpu_gfx_test& result);
	void Snapshot(amdgpu_gfx_test& result);
	void Uninitialize();
};
#endif
