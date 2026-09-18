/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_HARDWARE_H
#define RK3588_DISPLAY_HARDWARE_H

#include "DisplayObservation.h"

// Kernel platform adapter; included after the kernel declarations.
// Host fixtures use the same definitions with modeled kernel services.
static uint32
ReadDisplayRegister(const volatile uint32* registers, uint32 offset)
{
	uint32 value = registers[offset / sizeof(uint32)];
	memory_read_barrier();
	return value;
}


// Every mapping is read-only and uncached. Control blocks (PMU, CRU, GRFs)
// are always powered; VOP2 and HDMI TX1 are mapped only after the observation
// logic has confirmed their power domain and bus clock state.
class ObservationHardware {
public:
	status_t MapControl(const RK3588Display::ResourceInfo& resources)
	{
		status_t status = Map(fPmuArea, "RK3588 display PMU", resources.pmuBase,
			B_PAGE_SIZE, &fPmu);
		if (status == B_OK) {
			status = Map(fClockArea, "RK3588 display CRU", resources.clockBase,
				B_PAGE_SIZE, &fClock);
		}
		if (status == B_OK) {
			status = Map(fSysGrfArea, "RK3588 display SYS GRF", resources.sysGrfBase,
				B_PAGE_SIZE, &fSysGrf);
		}
		if (status == B_OK) {
			status = Map(fVopGrfArea, "RK3588 display VOP GRF", resources.vopGrfBase,
				B_PAGE_SIZE, &fVopGrf);
		}
		if (status == B_OK) {
			status = Map(fVo1GrfArea, "RK3588 display VO1 GRF", resources.vo1GrfBase,
				B_PAGE_SIZE, &fVo1Grf);
		}
		if (status == B_OK) {
			status = Map(fHdptxGrfArea, "RK3588 display HDPTX1 GRF",
				resources.hdptxGrfBase, B_PAGE_SIZE, &fHdptxGrf);
		}
		if (status != B_OK)
			Unmap();
		return status;
	}
	status_t MapVop(const RK3588Display::ResourceInfo& resources, size_t size)
	{
		return Map(fVopArea, "RK3588 display VOP2", resources.vopBase, size, &fVop);
	}
	status_t MapHdmi(const RK3588Display::ResourceInfo& resources, size_t size)
	{
		return Map(fHdmiArea, "RK3588 display HDMI TX1", resources.hdmiBase, size,
			&fHdmi);
	}
	void UnmapVop() { fVopArea.SetTo(-1); fVop = NULL; }
	void UnmapHdmi() { fHdmiArea.SetTo(-1); fHdmi = NULL; }
	void Unmap()
	{
		UnmapVop();
		UnmapHdmi();
		fHdptxGrfArea.SetTo(-1); fHdptxGrf = NULL;
		fVo1GrfArea.SetTo(-1); fVo1Grf = NULL;
		fVopGrfArea.SetTo(-1); fVopGrf = NULL;
		fSysGrfArea.SetTo(-1); fSysGrf = NULL;
		fClockArea.SetTo(-1); fClock = NULL;
		fPmuArea.SetTo(-1); fPmu = NULL;
	}
	~ObservationHardware() { Unmap(); }
	uint32_t ReadPmu(uint32_t offset) { return ReadDisplayRegister(fPmu, offset); }
	uint32_t ReadClock(uint32_t offset) { return ReadDisplayRegister(fClock, offset); }
	uint32_t ReadSysGrf(uint32_t offset) { return ReadDisplayRegister(fSysGrf, offset); }
	uint32_t ReadVopGrf(uint32_t offset) { return ReadDisplayRegister(fVopGrf, offset); }
	uint32_t ReadVo1Grf(uint32_t offset) { return ReadDisplayRegister(fVo1Grf, offset); }
	uint32_t ReadHdptxGrf(uint32_t offset) { return ReadDisplayRegister(fHdptxGrf, offset); }
	uint32_t ReadVop(uint32_t offset) { return ReadDisplayRegister(fVop, offset); }
	uint32_t ReadHdmi(uint32_t offset) { return ReadDisplayRegister(fHdmi, offset); }
	int64_t Now() { return system_time(); }
private:
	status_t Map(AreaDeleter& area, const char* name, uint64_t base, size_t size,
		volatile uint32** registers)
	{
		void* address = NULL;
		area.SetTo(map_physical_memory(name, base, size,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, &address));
		if (area.Get() < B_OK)
			return area.Get();
		*registers = (volatile uint32*)address;
		return B_OK;
	}

	AreaDeleter fPmuArea, fClockArea, fSysGrfArea, fVopGrfArea, fVo1GrfArea,
		fHdptxGrfArea, fVopArea, fHdmiArea;
	volatile uint32* fPmu = NULL;
	volatile uint32* fClock = NULL;
	volatile uint32* fSysGrf = NULL;
	volatile uint32* fVopGrf = NULL;
	volatile uint32* fVo1Grf = NULL;
	volatile uint32* fHdptxGrf = NULL;
	volatile uint32* fVop = NULL;
	volatile uint32* fHdmi = NULL;
};

#endif // RK3588_DISPLAY_HARDWARE_H
