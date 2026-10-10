/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_GFX_H
#define AMDGPU_GFX_H
#include "Gart.h"
struct GfxEngine {
	volatile uint32* regs;
	volatile uint32* memory;
	volatile uint32* ring;
	area_id area, vmArea;
	uint64 gpu;
	uint32 wptr, sequence;
	bool attempted, ready, faulted, vmEnabled, mecStarted;
	status_t InitializeVM(const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation, const Gart& gart);
	status_t Test(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation,
		const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
		amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec);
	void Snapshot(amdgpu_gfx_test& result);
	void DumpExecutionState(const char* point);
	void Halt();
	void Uninitialize();
};
#endif
