/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_DMA_MEMORY_H
#define MALI_CSF_DMA_MEMORY_H

// Included after kernel allocation/cache declarations.
static const char* const kDmaAreaName = "Mali CSF firmware DMA";

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
AllocateFirmwareMemory(Memory& memory, const char* name = kDmaAreaName)
{
	void* address = NULL;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(1) << 40; // exclusive
	size_t bytes = memory.RequiredBytes();
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, bytes, B_CONTIGUOUS,
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

#endif
