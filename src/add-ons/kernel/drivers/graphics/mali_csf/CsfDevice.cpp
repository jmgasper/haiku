/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <lock.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arch/arm64/cache_line_size.h>
#endif

#include "CsfRun.h"
#include "CsfCommands.h"
#include "CsfDevice.h"

using namespace MaliCSF;

#include "CsfHardware.h"

static const char* const kDmaAreaName = "Mali CSF firmware DMA";

bool
FirmwareMemoryRetained()
{
	// The area belongs to the system team and survives close, process exit,
	// controller removal and module unload. The name also makes a reloaded
	// driver refuse another cycle after uncertain cleanup. Only a verified
	// normal cycle deletes it; reboot reclaims a deliberately retained area.
	return find_area(kDmaAreaName) >= B_OK;
}

static status_t
MakeFirmwareRamNoncacheable(area_id area, void* address, phys_addr_t physical, size_t bytes)
{
#if defined(__aarch64__)
	// Same private ARM64 Normal-NC allocation discipline as the AHCI adapter.
	// Evict cached allocation zeroing before changing the CPU memory type.
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	status_t status = vm_set_area_memory_type(area, physical, B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		return status;
	}
	memory_full_barrier();
#else
	return B_NOT_SUPPORTED;
#endif
	return B_OK;
}

template<typename Memory>
static area_id
AllocateFirmwareMemory(Memory& memory)
{
	void* address = NULL;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(1) << 40; // exclusive
	size_t bytes = memory.RequiredBytes();
	area_id area = create_area_etc(B_SYSTEM_TEAM, kDmaAreaName, bytes, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < B_OK)
		return area;
	physical_entry entry;
	status_t status = get_memory_map(address, bytes, &entry, 1);
	if (status != B_OK || entry.size < bytes || (entry.address & 4095) != 0
		|| entry.address >= (UINT64_C(1) << 40)
		|| bytes > (UINT64_C(1) << 40) - entry.address) {
		delete_area(area);
		return B_BAD_VALUE;
	}
	status = MakeFirmwareRamNoncacheable(area, address, entry.address, bytes);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}
	if (!memory.Build(address, bytes, entry.address)) {
		delete_area(area);
		return B_BAD_DATA;
	}
	memory_full_barrier();
	return area;
}

class FirmwareHardware : public ResetHardware {
public:
	explicit FirmwareHardware(bool commands = false) : fCommands(commands) {}
	~FirmwareHardware() { StopFirmwareHandlers(); }
	status_t Init(const ResourceInfo& resources)
	{
		fGpuBase = resources.gpuBase;
		for (unsigned i = 0; i < 3; i++) {
			fInterrupts[i].hardware = this;
			fInterrupts[i].index = i;
			fInterrupts[i].number = resources.interrupts[i];
		}
		return ResetHardware::Init(resources);
	}
	bool MapGpu()
	{
		if (!ResetHardware::MapGpu())
			return false;
		fDoorbellArea.SetTo(map_physical_memory("Mali global doorbell",
			fGpuBase + 0x80000, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fDoorbell));
		if (fDoorbellArea.Get() < B_OK) {
			ResetHardware::UnmapGpu();
			return false;
		}
		return true;
	}
	void UnmapGpu()
	{
		StopFirmwareHandlers();
		fDoorbellArea.SetTo(-1);
		fDoorbell = NULL;
		ResetHardware::UnmapGpu();
	}
	void MemoryBarrier() { memory_full_barrier(); }
	uint32 TimerRate()
	{
#if defined(__aarch64__)
		uint64 rate;
		asm volatile("mrs %0, cntfrq_el0" : "=r"(rate));
		return rate <= UINT32_MAX ? uint32(rate) : 0;
#else
		return 0;
#endif
	}
	void RingFirmwareDoorbell() { WritePlatformRegister(fDoorbell, 0, 1); }
	bool InstallFirmwareHandlers()
	{
		for (unsigned i = 0; i < 3; i++) {
			InterruptSource& source = fInterrupts[i];
			if (install_io_interrupt_handler(source.number, Interrupt, &source, 0) != B_OK)
				return false;
			source.installed = true;
		}
		return true;
	}
	bool ArmFirmwareInterrupts()
	{
		InterruptsSpinLocker locker(fLock);
		// An unhandled old job/MMU event is a failed admission, never a boot.
		if (ReadGpu(0x1000) != 0 || (ReadGpu(0x2000) & ~(Mask(1) << 16)) != 0
			|| (ReadGpu(kGpuRaw) & 3) != 0)
			return false;
		for (unsigned i = 0; i < 3; i++) {
			fInterrupts[i].capture = {};
			fInterrupts[i].armed = true;
			WriteGpu(MaskOffset(i), Mask(i));
			if (ReadGpu(MaskOffset(i)) != Mask(i))
				return false;
		}
		return true;
	}
	bool ArmFirmwareJob()
	{
		return ArmJob(kGlobalInterrupt, false);
	}
	bool ArmCommandJob()
	{
		return fCommands && ArmJob(kGlobalInterrupt | 1, true);
	}
	uint32 CommandJobCount()
	{
		InterruptsSpinLocker locker(fLock);
		return fGroupJobs;
	}
	bool ArmJob(uint32 mask, bool continuous)
	{
		InterruptsSpinLocker locker(fLock);
		if (ReadGpu(0x1000) != 0 || ReadGpu(0x1008) != 0)
			return false;
		fInterrupts[0].capture = {};
		fInterrupts[0].armed = true;
		fJobMask = mask;
		fContinuousJobs = continuous;
		fGroupJobs = 0;
		WriteGpu(0x1008, mask);
		return ReadGpu(0x1008) == mask;
	}
	FirmwareCapture ReadFirmwareCapture(unsigned index)
	{
		InterruptsSpinLocker locker(fLock);
		FirmwareCapture capture = fInterrupts[index].capture;
		if (index > 0 && capture.count == 0) {
			uint32 raw = ReadGpu(index == 1 ? 0x2000 : kGpuRaw);
			if ((raw & (index == 1 ? kMmuFaultBits : 3)) != 0) {
				// Also preserve a fault that precedes IRQ arming. count=0 and
				// cpu=-1 explicitly distinguish polling from IRQ delivery.
				capture.raw = raw;
				capture.cpu = -1;
				capture.whenMicros = system_time();
				uint32 base = FaultBase(raw);
				capture.deviceStatus = ReadGpu(index == 1 ? base + 0x1c : 0x3c);
				capture.address = ReadGpu64(*this, index == 1 ? base + 0x20 : 0x40);
				if (index == 1)
					capture.extra = ReadGpu64(*this, base + 0x38);
			}
		}
		return capture;
	}
	bool FirmwareFaulted()
	{
		InterruptsSpinLocker locker(fLock);
		return fInterrupts[1].capture.count != 0 || fInterrupts[2].capture.count != 0
			|| (ReadGpu(0x2000) & kMmuFaultBits) != 0 || (ReadGpu(kGpuRaw) & 3) != 0;
	}
	void StopFirmwareHandlers()
	{
		bool installed = false;
		for (unsigned i = 0; i < 3; i++)
			installed |= fInterrupts[i].installed;
		if (!installed)
			return;
		{
			InterruptsSpinLocker locker(fLock);
			for (unsigned i = 0; i < 3; i++) {
				WriteGpu(MaskOffset(i), 0);
				fInterrupts[i].armed = false;
			}
		}
		for (unsigned i = 0; i < 3; i++) {
			InterruptSource& source = fInterrupts[i];
			if (source.installed) {
				remove_io_interrupt_handler(source.number, Interrupt, &source);
				source.installed = false;
			}
		}
	}
private:
	struct InterruptSource {
		FirmwareHardware* hardware;
		uint32 number;
		unsigned index;
		bool installed;
		bool armed;
		FirmwareCapture capture;
	};
	static uint32 MaskOffset(unsigned i) { return i == 0 ? 0x1008 : i == 1 ? 0x2008 : kGpuMask; }
	uint32 Mask(unsigned i) const { return i == 0 ? fJobMask : i == 1 ? (fCommands ? 3 : 1) : 3; }
	static uint32 FaultBase(uint32 raw)
	{
		for (unsigned as = 0; as < 16; as++) {
			if ((raw & (1u << as)) != 0)
				return 0x2400 + as * 0x40;
		}
		return 0x2400;
	}
	static int32 Interrupt(void* cookie)
	{
		InterruptSource& source = *(InterruptSource*)cookie;
		FirmwareHardware& io = *source.hardware;
		InterruptsSpinLocker locker(io.fLock);
		uint32 mask = MaskOffset(source.index);
		if (!source.armed)
			return B_UNHANDLED_INTERRUPT;
		uint32 status = io.ReadGpu(mask + 4);
		if (status == 0)
			return B_UNHANDLED_INTERRUPT;
		FirmwareCapture& capture = source.capture;
		capture.count++;
		capture.status |= status;
		capture.raw |= io.ReadGpu(mask - 8);
		capture.cpu = smp_get_current_cpu();
		capture.whenMicros = system_time();
		if (source.index == 1) {
			uint32 base = FaultBase(capture.raw);
			capture.deviceStatus = io.ReadGpu(base + 0x1c);
			capture.address = ReadGpu64(io, base + 0x20);
			capture.extra = ReadGpu64(io, base + 0x38);
		} else if (source.index == 2) {
			capture.deviceStatus = io.ReadGpu(0x3c);
			capture.address = ReadGpu64(io, 0x40);
		}
		if (source.index == 0 && io.fContinuousJobs) {
			if ((status & 1) != 0)
				io.fGroupJobs++;
		} else {
			io.WriteGpu(mask, 0);
			source.armed = false;
		}
		io.WriteGpu(mask - 4, status & io.Mask(source.index));
		return B_HANDLED_INTERRUPT;
	}
	spinlock fLock = B_SPINLOCK_INITIALIZER;
	InterruptSource fInterrupts[3] = {};
	uint64 fGpuBase = 0;
	AreaDeleter fDoorbellArea;
	volatile uint32* fDoorbell = NULL;
	bool fCommands = false;
	bool fContinuousJobs = false;
	uint32 fJobMask = kGlobalInterrupt;
	uint32 fGroupJobs = 0;
};

status_t
RunFirmwareRequest(const ResourceInfo& resources, void* buffer, size_t length,
	bool& needsRecovery)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	if (length < sizeof(FirmwareRunInfo) || length - sizeof(FirmwareRunInfo) > kMaxFirmwareBytes)
		return B_BAD_VALUE;
	uint32 header[2];
	status_t status = user_memcpy(header, buffer, sizeof(header));
	if (status != B_OK)
		return status;
	if (header[0] != kFirmwareRunVersion || header[1] < 20
		|| header[1] != length - sizeof(FirmwareRunInfo))
		return B_BAD_VALUE;
	void* file = malloc(header[1]);
	if (file == NULL)
		return B_NO_MEMORY;
	status = user_memcpy(file, (uint8*)buffer + sizeof(FirmwareRunInfo), header[1]);
	FirmwareImage image;
	FirmwareMemory memory;
	if (status == B_OK && (image.Init(file, header[1]) != FIRMWARE_OK
		|| image.Info().versionHash != 0x01050000 || !memory.Plan(image)))
		status = B_BAD_DATA;
	area_id area = status == B_OK ? AllocateFirmwareMemory(memory) : status;
	if (area < B_OK) {
		free(file);
		return area;
	}
	FirmwareRunInfo info = {};
	info.version = kFirmwareRunVersion;
	info.firmwareBytes = header[1];
	info.flags = kFirmwareAllocated;
	info.tablePages = memory.TablePages();
	info.allocationBytes = memory.RequiredBytes();
	info.rootPhysical = memory.RootPhysical();
	info.translationConfig = FirmwareMemory::TranslationConfig();
	info.memoryAttributes = FirmwareMemory::MemoryAttributes();
	FirmwareHardware hardware;
	status = hardware.Init(resources);
	if (status == B_OK)
		CycleFirmware(hardware, memory, info);
	// Retain one arena after any failed hardware cycle, also preserving the
	// recovery guard across module removal when failure preceded AS exposure.
	// Failures before the cycle starts need no hardware recovery. Never let
	// closing the diagnostic process release uncertain GPU-owned memory.
	bool releasable = (info.flags & kFirmwareNeedsRecovery) == 0
		&& ((info.flags & kFirmwareAddressSpace) == 0
			|| ((info.flags & (kFirmwareCleaned | kFirmwarePowerRestored))
				== (kFirmwareCleaned | kFirmwarePowerRestored)));
	if (releasable && delete_area(area) == B_OK)
		area = -1;
	else {
		info.flags |= kFirmwareMemoryRetained | kFirmwareNeedsRecovery;
	}
	needsRecovery = (info.flags & kFirmwareNeedsRecovery) != 0;
	free(file);
	dprintf("mali_csf: firmware result=%u cleanup=%u flags=%#x tables=%u bytes=%u"
		" root=%#" B_PRIx64 " boot=%u ping=%u version=%#x power=%u restore=%u\n",
		info.result, info.cleanupResult, info.flags, info.tablePages, info.allocationBytes,
		info.rootPhysical, info.boot.count, info.ping.count, info.interface.version,
		info.power.result, info.power.restoreResult);
	if (status != B_OK)
		return status;
	return user_memcpy(buffer, &info, sizeof(info));
}

status_t
RunCommandRequest(const ResourceInfo& resources, void* buffer, size_t length,
	bool& needsRecovery, bool shader)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	if (length < sizeof(CommandRunInfo) || length - sizeof(CommandRunInfo) > kMaxFirmwareBytes)
		return B_BAD_VALUE;
	uint32 header[2];
	status_t status = user_memcpy(header, buffer, sizeof(header));
	if (status != B_OK)
		return status;
	if (header[0] != kCommandRunVersion || header[1] < 20
		|| header[1] != length - sizeof(CommandRunInfo))
		return B_BAD_VALUE;
	void* file = malloc(header[1]);
	CommandRunInfo* info = (CommandRunInfo*)calloc(1, sizeof(CommandRunInfo));
	if (file == NULL || info == NULL) {
		free(file); free(info);
		return B_NO_MEMORY;
	}
	status = user_memcpy(file, (uint8*)buffer + sizeof(CommandRunInfo), header[1]);
	FirmwareImage image;
	CommandMemory memory;
	if (status == B_OK && (image.Init(file, header[1]) != FIRMWARE_OK
		|| image.Info().versionHash != 0x01050000 || !memory.Plan(image)))
		status = B_BAD_DATA;
	area_id area = status == B_OK ? AllocateFirmwareMemory(memory) : status;
	if (area < B_OK) {
		free(file); free(info);
		return area;
	}
	info->version = kCommandRunVersion;
	info->firmwareBytes = header[1];
	info->result = info->cleanupResult = kCommandNotAttempted;
	if (shader) {
		// No application GPU mapping exists yet. Replace the default command
		// regression with the fixed Linux-matched shader and descriptors.
		BuildStoreShader(memory.Code());
		info->flags = kCommandShader;
	}
	info->rootPhysical = memory.RootPhysical();
	info->userTablePages = CommandMemory::kUserTablePages;
	info->userBytes = CommandMemory::kUserBytes;
	info->arenaBytes = memory.RequiredBytes();
	FirmwareRunInfo& firmware = info->firmware;
	firmware.version = kFirmwareRunVersion;
	firmware.firmwareBytes = header[1];
	firmware.flags = kFirmwareAllocated;
	firmware.tablePages = memory.Firmware().TablePages();
	firmware.allocationBytes = memory.Firmware().RequiredBytes();
	firmware.rootPhysical = memory.Firmware().RootPhysical();
	firmware.translationConfig = FirmwareMemory::TranslationConfig();
	firmware.memoryAttributes = FirmwareMemory::MemoryAttributes();
	FirmwareHardware hardware(true);
	status = hardware.Init(resources);
	if (status == B_OK) {
		CommandOperation operation(memory, *info);
		CycleFirmware(hardware, memory.Firmware(), firmware, operation);
	}
	bool releasable = (firmware.flags & kFirmwareNeedsRecovery) == 0
		&& ((firmware.flags & kFirmwareAddressSpace) == 0
			|| ((firmware.flags & (kFirmwareCleaned | kFirmwarePowerRestored))
				== (kFirmwareCleaned | kFirmwarePowerRestored)))
		&& ((info->flags & kCommandMapped) == 0 || (info->flags & kCommandUnmapped) != 0);
	if (!releasable || delete_area(area) != B_OK)
		firmware.flags |= kFirmwareMemoryRetained | kFirmwareNeedsRecovery;
	needsRecovery = (firmware.flags & kFirmwareNeedsRecovery) != 0;
	free(file);
	dprintf("mali_csf: commands result=%u cleanup=%u flags=%#x rounds=%u"
		" fw=%u cleanup=%u flags=%#x root=%#" B_PRIx64 " events=%#x\n",
		info->result, info->cleanupResult, info->flags, info->roundsCompleted,
		firmware.result, firmware.cleanupResult, firmware.flags, info->rootPhysical,
		info->groupEvents);
	if (status == B_OK)
		status = user_memcpy(buffer, info, sizeof(*info));
	free(info);
	return status;
}
