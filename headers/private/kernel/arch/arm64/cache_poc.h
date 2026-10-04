/*
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_ARM64_CACHE_POC_H
#define _KERNEL_ARCH_ARM64_CACHE_POC_H

#include <arch/arm64/cache_line_size.h>


// Callers must prevent migration between reading CTR and walking cache lines,
// and complete maintenance with DSB before transferring memory ownership.
static inline uint64_t
arm64_current_data_cache_line_size()
{
	uint64_t cacheType;
	asm volatile("mrs %0, ctr_el0" : "=r"(cacheType));
	return arm64_data_cache_line_size(cacheType);
}


static inline void
arm64_clean_data_cache_line_poc(uintptr_t address)
{
	asm volatile("dc cvac, %0" :: "r"(address) : "memory");
}


static inline void
arm64_invalidate_data_cache_line_poc(uintptr_t address)
{
	asm volatile("dc ivac, %0" :: "r"(address) : "memory");
}


static inline void
arm64_clean_invalidate_data_cache_line_poc(uintptr_t address)
{
	asm volatile("dc civac, %0" :: "r"(address) : "memory");
}

#endif // _KERNEL_ARCH_ARM64_CACHE_POC_H
