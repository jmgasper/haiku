/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "arch_int_gicv3_its.h"

#include <arch/arm64/arch_cpu.h>
#include <arch/arm64/cache_line_size.h>
#include <arch/arm64/rk3588_its.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

using namespace Gicv3Its;

static const uint64 kValid = UINT64_C(1) << 63;
static const uint64 kNormalNonCacheable = UINT64_C(1) << 59;
static const uint64 kBaserReadOnly = UINT64_C(0x071f000000000000);
static const size_t kTablePage = 65536;

static uint64
Counter()
{
	// Timer services are not initialized when the GIC constructor runs.
	return READ_SPECIALREG(cntvct_el0);
}


status_t
GICv3Its::Memory::Allocate(const char* name, size_t bytes, size_t alignment)
{
	virtual_address_restrictions virtualRestrictions{};
	physical_address_restrictions physicalRestrictions{};
	physicalRestrictions.high_address = UINT64_C(1) << 35;
	physicalRestrictions.alignment = alignment;
	area = create_area_etc(B_SYSTEM_TEAM, name, bytes, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;
	physical_entry entry;
	status_t status = get_memory_map(address, bytes, &entry, 1);
	if (status != B_OK || entry.size < bytes
		|| !ValidTableRange(entry.address, bytes, alignment)) {
		delete_area(area);
		area = -1;
		address = nullptr;
		return B_BAD_ADDRESS;
	}
	physical = entry.address;
	size = bytes;
	// The allocator zeroed these private pages through a cached mapping.
	// Remove those lines before retyping, as in the qualified NVMe DMA path.
	size_t line = arm64_data_cache_line_size(READ_SPECIALREG(ctr_el0));
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	status = vm_set_area_memory_type(area, physical, B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		delete_area(area);
		area = -1;
		address = nullptr;
		return status;
	}
	memory_full_barrier();
	dprintf("GICv3 ITS: %s physical=%#" B_PRIxPHYSADDR " bytes=%zu Normal-NC\n",
		name, physical, bytes);
	return B_OK;
}


uint32
GICv3Its::_Read32(size_t offset) const
{
	uint32 value = *(volatile uint32*)(fBase + offset);
	memory_full_barrier();
	return value;
}


uint64
GICv3Its::_Read64(size_t offset) const
{
	uint64 value = *(volatile uint64*)(fBase + offset);
	memory_full_barrier();
	return value;
}


void
GICv3Its::_Write32(size_t offset, uint32 value)
{
	*(volatile uint32*)(fBase + offset) = value;
	memory_full_barrier();
}


void
GICv3Its::_Write64(size_t offset, uint64 value)
{
	*(volatile uint64*)(fBase + offset) = value;
	memory_full_barrier();
}


bool
GICv3Its::_WriteChecked(size_t offset, uint64 value)
{
	_Write64(offset, value);
	uint64 readback = _Read64(offset);
	if (readback == value)
		return true;
	dprintf("GICv3 ITS: register %#zx wanted=%#" B_PRIx64 " read=%#" B_PRIx64 "\n",
		offset, value, readback);
	return false;
}


status_t
GICv3Its::_PrepareTables()
{
	// Flat device table: 65536 IDs, measured 8-byte entries. Collection table:
	// measured 2-byte entries; one 64 KiB page is more than the 16-ID capacity.
	// All tables use Non-shareable Normal-NC accesses (RK3588001).
	status_t status;
	if ((status = fDevices.Allocate("ITS devices", 8 * 65536, kTablePage)) != B_OK
		|| (status = fCollections.Allocate("ITS collections", kTablePage, kTablePage)) != B_OK
		|| (status = fCommands.Allocate("ITS commands", kQueueBytes, kTablePage)) != B_OK
		|| (status = fProperties.Allocate("LPI properties", kTablePage, kTablePage)) != B_OK
		|| (status = fPending.Allocate("CPU0 LPI pending", kTablePage, kTablePage)) != B_OK
		|| (status = fInterrupts.Allocate("NVMe ITS entries", B_PAGE_SIZE, B_PAGE_SIZE)) != B_OK) {
		return status;
	}
	// Configuration entry byte zero corresponds to INTID 8192, not INTID 0.
	memset(fProperties.address, 0xa2, fProperties.size); // priority, RES1, disabled
	memory_full_barrier();
	return B_OK;
}


status_t
GICv3Its::Init(volatile uint8* redistributors, uint32 count, bool trace)
{
	fRedistributors = redistributors;
	fTrace = trace;
	if (count != 8 || smp_get_current_cpu() != 0)
		return B_NOT_SUPPORTED;
	// Never replace a live Redistributor's pending/property tables. CommonLPIAff
	// also requires agreement with any other Redistributor with LPIs enabled.
	for (uint32 i = 0; i < count; i++) {
		uint32 control = *(volatile uint32*)(redistributors + i * 0x20000);
		uint64 typer = *(volatile uint64*)(redistributors + i * 0x20000 + 8);
		memory_full_barrier();
		dprintf("GICv3 ITS: CPU%u RD control=%#x typer=%#" B_PRIx64 "\n", i, control, typer);
		if ((control & 9) != 0 || (typer & 1) == 0)
			return B_BUSY;
		if (i == 0 && typer != 0x21)
			return B_NOT_SUPPORTED;
	}
	area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-its1",
		(void**)&fBase, B_ANY_KERNEL_ADDRESS, 0x10000,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, Rk3588Its::kController, false);
	if (area < 0)
		return area;
	uint32 control = _Read32(0), iidr = _Read32(4);
	uint64 typer = _Read64(8);
	dprintf("GICv3 ITS: ITS1 control=%#x IIDR=%#x TYPER=%#" B_PRIx64 "\n",
		control, iidr, typer);
	// Require the measured GIC-600 capabilities and a disabled, quiescent ITS
	// before touching table descriptors. This profile does not take over Linux.
	if (iidr != Rk3588Its::kIidr || typer != Rk3588Its::kTyper)
		return B_NOT_SUPPORTED;
	if ((control & 0x80000003) != 0x80000000)
		return B_BUSY;
	fDeviceBaser = _Read64(0x100) & kBaserReadOnly;
	fCollectionBaser = _Read64(0x108) & kBaserReadOnly;
	if (fDeviceBaser != UINT64_C(0x0107000000000000)
		|| fCollectionBaser != UINT64_C(0x0401000000000000))
		return B_NOT_SUPPORTED;
	for (unsigned i = 2; i < 8; i++) {
		if ((_Read64(0x100 + i * 8) & kBaserReadOnly) != 0)
			return B_NOT_SUPPORTED;
	}
	status_t status = _PrepareTables();
	if (status != B_OK)
		return status;
	// Hardware IDs are used as kernel vector IDs. Only this 32-vector range
	// is owned by the initial device profile.
	status = reserve_io_interrupt_vectors(kEventCount, kFirstLpi, INTERRUPT_TYPE_IRQ);
	if (status != B_OK)
		return status;
	if (!_WriteChecked(0x100, fDeviceBaser | kValid | kNormalNonCacheable
			| fDevices.physical | 0x207)
		|| !_WriteChecked(0x108, fCollectionBaser | kValid | kNormalNonCacheable
			| fCollections.physical | 0x200)
		|| !_WriteChecked(0x80, kValid | kNormalNonCacheable | fCommands.physical | 15)) {
		_Quarantine("table descriptor readback failed");
		return B_ERROR;
	}
	_Write64(0x88, 0);
	if (_Read64(0x90) != 0) {
		_Quarantine("command reader did not reset");
		return B_ERROR;
	}
	// GICD supports 16-bit IDs; this first profile provisions IDs up to 16383.
	uint64 property = fProperties.physical | 0x8d;
	uint64 pending = fPending.physical | 0x80;
	*(volatile uint64*)(redistributors + 0x70) = property;
	*(volatile uint64*)(redistributors + 0x78) = pending;
	memory_full_barrier();
	if (*(volatile uint64*)(redistributors + 0x70) != property
		|| *(volatile uint64*)(redistributors + 0x78) != pending) {
		_Quarantine("Redistributor table readback failed");
		return B_ERROR;
	}
	*(volatile uint32*)redistributors |= 1;
	memory_full_barrier();
	if ((*(volatile uint32*)redistributors & 1) == 0) {
		_Quarantine("LPI enable readback failed");
		return B_ERROR;
	}
	// PTA=0: use GICR_TYPER.Processor_Number, which is zero for this CPU.
	fTarget = 0;
	_Write32(0, 1);
	if ((_Read32(0) & 1) == 0
		|| !_Submit(MapCollection(0, fTarget, true)) || !_Submit(InvalidateAll(0))) {
		_Quarantine("collection initialization failed");
		return B_ERROR;
	}
	fReady = true;
	dprintf("GICv3 ITS: enabled ITS1, CPU 0, LPI 8192..8223, Samsung DeviceID 0x100\n");
	return B_OK;
}


void
GICv3Its::_Quarantine(const char* reason)
{
	fFaulted = true;
	// Hardware may still hold these physical addresses. Keep all allocations
	// and IDs until reboot, and refuse subsequent commands or leases.
	dprintf("GICv3 ITS: quarantined until reboot: %s\n", reason);
}


bool
GICv3Its::_WaitReadOffset(uint32 offset)
{
	uint64 start = Counter();
	uint64 ticks = READ_SPECIALREG(cntfrq_el0) / 10; // at most 100 ms
	for (unsigned attempt = 0; attempt < 1000000; attempt++) {
		uint64 reader = _Read64(0x90);
		if (reader == offset)
			return true;
		if ((reader & 1) != 0 || Counter() - start >= ticks) {
			dprintf("GICv3 ITS: command timeout/stall reader=%#" B_PRIx64 " wanted=%#x\n",
				reader, offset);
			break;
		}
		asm volatile("yield" ::: "memory");
	}
	_Quarantine("command queue did not drain");
	return false;
}


bool
GICv3Its::_Submit(const Command& command)
{
	// The caller serializes the entire submission/completion under fLock, or
	// runs during single-CPU initialization. Two entries can never fill 64 KiB.
	if (fFaulted || !_WaitReadOffset(fWriteOffset))
		return false;
	const Command commands[] = {command, Sync(fTarget)};
	for (const Command& current : commands) {
		volatile uint64* entry = (volatile uint64*)((uint8*)fCommands.address + fWriteOffset);
		for (unsigned word = 0; word < 4; word++)
			entry[word] = current.words[word];
		fWriteOffset = NextCommand(fWriteOffset);
	}
	memory_full_barrier();
	_Write64(0x88, fWriteOffset);
	return _WaitReadOffset(fWriteOffset);
}


status_t
GICv3Its::AllocateVectorsForDevice(const msi_requester* requester, uint32 count,
	uint32& startVector, uint64& address, uint32& data)
{
	if (requester == nullptr || !Rk3588Its::RequesterMatches(
			requester->controller_address, requester->device_id))
		return B_NOT_SUPPORTED;
	if (!ValidCount(count))
		return B_BAD_VALUE;
	InterruptsSpinLocker locker(fLock);
	if (!fReady || fFaulted)
		return B_NO_INIT;
	if (fCount != 0)
		return B_BUSY;
	// The sole device mapping is invalid between leases. Never zero a table
	// while a valid MAPD may still let hardware access it.
	memset(fInterrupts.address, 0, fInterrupts.size);
	memory_full_barrier();
	if (!_Submit(MapDevice(Rk3588Its::kDevice, fInterrupts.physical, 5, true)))
		return B_ERROR;
	for (uint32 event = 0; event < count; event++) {
		if (!_Submit(MapInterrupt(Rk3588Its::kDevice, event, kFirstLpi + event, 0)))
			return B_ERROR;
	}
	fCount = count;
	fEnabledMask = 0;
	startVector = kFirstLpi;
	address = Rk3588Its::kController + 0x10040;
	data = 0; // event IDs are per-device, so the MSI base is naturally aligned
	dprintf("GICv3 ITS: allocated DeviceID=0x100 count=%u vector=%u address=%#"
		B_PRIx64 " data=0\n", count, startVector, address);
	return B_OK;
}


void
GICv3Its::SetEnabled(uint32 vector, bool enabled)
{
	InterruptsSpinLocker locker(fLock);
	uint32 event = vector - kFirstLpi;
	if (!fReady || fFaulted || event >= fCount)
		return;
	uint32 mask = uint32(1) << event;
	volatile uint8* properties = (volatile uint8*)fProperties.address;
	properties[event] = enabled ? 0xa3 : 0xa2;
	memory_full_barrier();
	// A CPU cache flush alone cannot invalidate the GIC's property cache.
	if (!_Submit(EventCommand(0x0c, Rk3588Its::kDevice, event)))
		return;
	if (enabled)
		fEnabledMask |= mask;
	else
		fEnabledMask &= ~mask;
	if (fTrace)
		dprintf("GICv3 ITS: vector=%u %s\n", vector, enabled ? "enabled" : "disabled");
}


void
GICv3Its::FreeVectors(uint32 count, uint32 startVector)
{
	InterruptsSpinLocker locker(fLock);
	if (!fReady || fFaulted || !ValidCount(count) || startVector != kFirstLpi
		|| fCount != count || fEnabledMask != 0) {
		dprintf("GICv3 ITS: retaining invalid/live lease count=%u vector=%u\n",
			count, startVector);
		return;
	}
	// The caller must disable the PCI message source and synchronize removal
	// of all handlers first. CLEAR then DISCARD, each followed by SYNC, removes
	// pending state and the event translations before the device is unmapped.
	for (uint32 event = 0; event < count; event++) {
		if (!_Submit(EventCommand(0x04, Rk3588Its::kDevice, event))
			|| !_Submit(EventCommand(0x0f, Rk3588Its::kDevice, event)))
			return;
	}
	if (!_Submit(MapDevice(Rk3588Its::kDevice, fInterrupts.physical, 5, false)))
		return;
	fCount = 0;
	dprintf("GICv3 ITS: freed count=%u vector=%u\n", count, startVector);
}


void
GICv3Its::TraceInterrupt(uint32 vector)
{
	if (!fTrace || !Contains(vector))
		return;
	uint32 count = uint32(atomic_add(&fInterruptCounts[vector - kFirstLpi], 1)) + 1;
	if (count != 0 && (count <= 8 || (count & (count - 1)) == 0)) {
		dprintf("GICv3 ITS: received vector=%u count=%u cpu=%" B_PRId32 "\n",
			vector, count, smp_get_current_cpu());
	}
}
