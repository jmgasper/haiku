/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_HARDWARE_H
#define MALI_CSF_HARDWARE_H

#include "CsfReset.h"

// Kernel platform adapter; included after the kernel/locking declarations.
// Host fixtures use the same definitions with modeled kernel services.
static uint32
ReadPlatformRegister(const volatile uint32* registers, uint32 offset)
{
	uint32 value = registers[offset / sizeof(uint32)];
	memory_read_barrier();
	return value;
}


static void
WritePlatformRegister(volatile uint32* registers, uint32 offset, uint32 value)
{
	registers[offset / sizeof(uint32)] = value;
#if defined(__aarch64__)
	__asm__ __volatile__("dsb sy" ::: "memory");
#else
	memory_write_barrier();
#endif
}


class IdentityHardware {
public:
	status_t Init(const ResourceInfo& resources)
	{
		if (!ResourcesMatch(resources))
			return B_NOT_SUPPORTED;
		fGpuBase = resources.gpuBase;
		fClockArea.SetTo(map_physical_memory("Mali identity CRU",
			resources.clockBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fClock));
		if (fClockArea.Get() < B_OK)
			return fClockArea.Get();
		fPowerArea.SetTo(map_physical_memory("Mali identity PMU",
			resources.powerBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fPower));
		return fPowerArea.Get() < B_OK ? fPowerArea.Get() : B_OK;
	}
	uint32_t ReadClock(uint32_t offset) { return ReadPlatformRegister(fClock, offset); }
	uint32_t ReadPower(uint32_t offset) { return ReadPlatformRegister(fPower, offset); }
	void WriteClock(uint32_t offset, uint32_t value) { WritePlatformRegister(fClock, offset, value); }
	void WritePower(uint32_t offset, uint32_t value) { WritePlatformRegister(fPower, offset, value); }
	int64_t Now() { return system_time(); }
	void Pause() { spin(10); }
	bool MapGpu()
	{
		dprintf("mali_csf: identity power/idle ready; mapping GPU for static reads\n");
		return MapGpuWindow("Mali static identity", B_PAGE_SIZE, B_KERNEL_READ_AREA);
	}
	void UnmapGpu() { fGpuArea.SetTo(-1); fGpu = NULL; }
	uint32_t ReadGpu(uint32_t offset) { return ReadPlatformRegister(fGpu, offset); }
protected:
	bool MapGpuWindow(const char* name, size_t size, uint32 protection)
	{
		fGpuArea.SetTo(map_physical_memory(name, fGpuBase, size,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, protection, (void**)&fGpu));
		return fGpuArea.Get() >= B_OK;
	}
	void WriteGpuRegister(uint32 offset, uint32 value) { WritePlatformRegister(fGpu, offset, value); }
private:
	AreaDeleter fClockArea;
	AreaDeleter fPowerArea;
	AreaDeleter fGpuArea;
	volatile uint32* fClock = NULL;
	volatile uint32* fPower = NULL;
	volatile uint32* fGpu = NULL;
	uint64_t fGpuBase = 0;
};


class ResetHardware : public IdentityHardware {
public:
	~ResetHardware() { StopResetHandler(); }
	status_t Init(const ResourceInfo& resources)
	{
		fInterrupt = resources.interrupts[2];
		return IdentityHardware::Init(resources);
	}
	bool MapGpu()
	{
		dprintf("mali_csf: reset power/idle ready; mapping GPU control pages\n");
		return MapGpuWindow("Mali reset", 3 * B_PAGE_SIZE,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	}
	void WriteGpu(uint32 offset, uint32 value) { WriteGpuRegister(offset, value); }
	int32_t Cpu() { return smp_get_current_cpu(); }
	void PauseReset() { snooze(100); }
	bool InstallResetHandler()
	{
		fInstalled = install_io_interrupt_handler(fInterrupt, Interrupt, this, 0) == B_OK;
		return fInstalled;
	}
	ResetResult BeginReset()
	{
		InterruptsSpinLocker locker(fLock);
		return ArmReset(*this, fState);
	}
	ResetCapture ReadResetCapture()
	{
		InterruptsSpinLocker locker(fLock);
		return fState.capture;
	}
	void StopResetHandler()
	{
		if (!fInstalled)
			return;
		{
			InterruptsSpinLocker locker(fLock);
			WriteGpu(kGpuMask, 0);
			fState.armed = false;
		}
		// Only this object installs/removes this exact tuple. With flags=0,
		// removal joins any handler running under the kernel's vector lock.
		remove_io_interrupt_handler(fInterrupt, Interrupt, this);
		fInstalled = false;
	}
private:
	static int32 Interrupt(void* cookie)
	{
		ResetHardware& hardware = *(ResetHardware*)cookie;
		InterruptsSpinLocker locker(hardware.fLock);
		return CaptureResetInterrupt(hardware, hardware.fState)
			? B_HANDLED_INTERRUPT : B_UNHANDLED_INTERRUPT;
	}
	spinlock fLock = B_SPINLOCK_INITIALIZER;
	ResetInterruptState fState = {};
	uint32 fInterrupt = 0;
	bool fInstalled = false;
};



#endif
