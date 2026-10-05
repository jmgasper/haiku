/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MMU_H
#define MMU_H


#include <SupportDefs.h>

#include <boot/platform.h>
#include <util/FixedWidthPointer.h>


#ifdef __cplusplus
extern "C" {
#endif

extern addr_t get_next_virtual_address(size_t size);
extern phys_addr_t mmu_allocate_page();
extern phys_addr_t mmu_allocate_physical(size_t size, phys_addr_t maxAddress);
bool mmu_next_region(void** cookie, addr_t* vaddr, phys_addr_t* paddr,
	size_t* size);

extern status_t platform_assign_kernel_address_for_region(void* address,
	addr_t assign);
extern status_t platform_allocate_region_below(void** _address, size_t size,
	phys_addr_t maxAddress);

#ifdef __cplusplus
}
#endif

void mmu_init_early();
	// turns the MMU and the caches on, on an identity map
void mmu_init();
	// once the device tree's memory ranges are known
uint64 mmu_init_for_kernel();
	// builds the kernel's page tables; returns TTBR1
void mmu_enable_on_this_cpu();
void mmu_flush_data_cache(void* address, size_t size);


inline addr_t
fix_address(addr_t address)
{
	addr_t result;
	if (platform_bootloader_address_to_kernel_address((void*)address, &result)
			!= B_OK) {
		return address;
	}
	return result;
}


template<typename Type>
inline void
fix_address(FixedWidthPointer<Type>& p)
{
	if (p != NULL)
		p.SetTo(fix_address(p.Get()));
}


#endif	/* MMU_H */
