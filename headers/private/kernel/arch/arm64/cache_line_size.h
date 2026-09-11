/*
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_ARM64_CACHE_LINE_SIZE_H
#define _KERNEL_ARCH_ARM64_CACHE_LINE_SIZE_H

#include <stdint.h>


// CTR_EL0 encodes each line size as log2(number of four-byte words).
static inline uint64_t
arm64_instruction_cache_line_size(uint64_t cacheType)
{
	return UINT64_C(4) << (cacheType & 0xf);
}


static inline uint64_t
arm64_data_cache_line_size(uint64_t cacheType)
{
	return UINT64_C(4) << ((cacheType >> 16) & 0xf);
}

#endif // _KERNEL_ARCH_ARM64_CACHE_LINE_SIZE_H
