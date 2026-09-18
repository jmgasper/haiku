/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_HARDWARE_H
#define RK3588_DISPLAY_HARDWARE_H

#include "DisplayEdid.h"
#include "DisplayScanout.h"

// Kernel platform adapter; included after the kernel declarations.
// Host fixtures use the same definitions with modeled kernel services.
static uint32
ReadDisplayRegister(const volatile uint32* registers, uint32 offset)
{
	uint32 value = registers[offset / sizeof(uint32)];
	memory_read_barrier();
	return value;
}


static void
WriteDisplayRegister(volatile uint32* registers, uint32 offset, uint32 value)
{
	registers[offset / sizeof(uint32)] = value;
#if defined(__aarch64__)
	__asm__ __volatile__("dsb sy" ::: "memory");
#else
	memory_write_barrier();
#endif
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


// EDID transfer adapter. The always-on control blocks are mapped read-only to
// re-check the VO1 power domain, the HDMI APB clock and the hot-plug level;
// only then is the HDMI TX1 register window mapped writable for the I2C master.
class EdidHardware {
public:
	status_t Prepare(const RK3588Display::ResourceInfo& resources, uint32_t& hotPlug,
		uint32_t& result)
	{
		if (!RK3588Display::ResourcesMatch(resources))
			return B_NOT_SUPPORTED;
		void* address = NULL;
		fPmuArea.SetTo(map_physical_memory("RK3588 EDID PMU", resources.pmuBase, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, &address));
		if (fPmuArea.Get() < B_OK)
			return fPmuArea.Get();
		fPmu = (volatile uint32*)address;
		fClockArea.SetTo(map_physical_memory("RK3588 EDID CRU", resources.clockBase, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, &address));
		if (fClockArea.Get() < B_OK)
			return fClockArea.Get();
		fClock = (volatile uint32*)address;
		fGrfArea.SetTo(map_physical_memory("RK3588 EDID SYS GRF", resources.sysGrfBase, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, &address));
		if (fGrfArea.Get() < B_OK)
			return fGrfArea.Get();
		fGrf = (volatile uint32*)address;
		uint32_t repair = ReadDisplayRegister(fPmu, RK3588Display::kPmuOffsets[RK3588Display::kPmuRepairStatus]);
		uint32_t gate = ReadDisplayRegister(fClock, RK3588Display::kClockGateOffsets[RK3588Display::kClockGateHdmi]);
		hotPlug = ReadDisplayRegister(fGrf, RK3588Display::kSysGrfOffsets[2]);
		if ((repair & RK3588Display::kPmuVo1On) == 0
			|| (gate & RK3588Display::kClockGateHdmiMask) != 0) {
			result = RK3588Display::kEdidNotReady;
			return B_OK;
		}
		if ((hotPlug & RK3588Display::kHpdLevel1) == 0) {
			result = RK3588Display::kEdidNoHotPlug;
			return B_OK;
		}
		fHdmiArea.SetTo(map_physical_memory("RK3588 EDID HDMI TX1", resources.hdmiBase,
			RK3588Display::kHdmiEdidMapSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address));
		if (fHdmiArea.Get() < B_OK)
			return fHdmiArea.Get();
		fHdmi = (volatile uint32*)address;
		result = RK3588Display::kEdidOK;
		return B_OK;
	}
	bool Ready() const { return fHdmi != NULL; }
	uint32_t ReadHdmi(uint32_t offset) { return ReadDisplayRegister(fHdmi, offset); }
	void WriteHdmi(uint32_t offset, uint32_t value) { WriteDisplayRegister(fHdmi, offset, value); }
	int64_t Now() { return system_time(); }
	void Pause(unsigned micros) { spin(micros); }
private:
	AreaDeleter fPmuArea, fClockArea, fGrfArea, fHdmiArea;
	volatile uint32* fPmu = NULL;
	volatile uint32* fClock = NULL;
	volatile uint32* fGrf = NULL;
	volatile uint32* fHdmi = NULL;
};


// Scanout swap adapter: control blocks read-only for gating, then the VOP2
// window mapped read-only for a query or writable for a swap. Only the
// located window's buffer address and the port's configuration-done word are
// ever written through it.
class ScanoutHardware {
public:
	status_t Prepare(const RK3588Display::ResourceInfo& resources, bool writable,
		uint32_t& result)
	{
		if (!RK3588Display::ResourcesMatch(resources))
			return B_NOT_SUPPORTED;
		void* address = NULL;
		fPmuArea.SetTo(map_physical_memory("RK3588 scanout PMU", resources.pmuBase, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, &address));
		if (fPmuArea.Get() < B_OK)
			return fPmuArea.Get();
		fPmu = (volatile uint32*)address;
		fClockArea.SetTo(map_physical_memory("RK3588 scanout CRU", resources.clockBase, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, &address));
		if (fClockArea.Get() < B_OK)
			return fClockArea.Get();
		fClock = (volatile uint32*)address;
		uint32_t repair = ReadDisplayRegister(fPmu, RK3588Display::kPmuOffsets[RK3588Display::kPmuRepairStatus]);
		uint32_t gate = ReadDisplayRegister(fClock, RK3588Display::kClockGateOffsets[RK3588Display::kClockGateVop]);
		if ((repair & RK3588Display::kPmuVopOn) == 0
			|| (gate & RK3588Display::kClockGateVopMask) != 0) {
			result = RK3588Display::kScanoutNotReady;
			return B_OK;
		}
		fVopArea.SetTo(map_physical_memory("RK3588 scanout VOP2", resources.vopBase,
			RK3588Display::kVopMapSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | (writable ? B_KERNEL_WRITE_AREA : 0), &address));
		if (fVopArea.Get() < B_OK)
			return fVopArea.Get();
		fVop = (volatile uint32*)address;
		result = RK3588Display::kScanoutOK;
		return B_OK;
	}
	bool Ready() const { return fVop != NULL; }
	uint32_t ReadVop(uint32_t offset) { return ReadDisplayRegister(fVop, offset); }
	void WriteVop(uint32_t offset, uint32_t value) { WriteDisplayRegister(fVop, offset, value); }
	int64_t Now() { return system_time(); }
private:
	AreaDeleter fPmuArea, fClockArea, fVopArea;
	volatile uint32* fPmu = NULL;
	volatile uint32* fClock = NULL;
	volatile uint32* fVop = NULL;
};

#endif // RK3588_DISPLAY_HARDWARE_H
