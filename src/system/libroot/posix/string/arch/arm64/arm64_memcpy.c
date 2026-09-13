/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#include <stdint.h>
#include <string.h>
#undef memcpy


// Normal memory on ARM64 permits unaligned accesses. The packed alias type
// expresses that without forming a misaligned uint64_t lvalue in C. This is
// ordinary memory copying, not an accessor for Device memory or registers.
typedef struct __attribute__((packed, may_alias)) {
	uint64_t value;
} unaligned_word;


void*
memcpy(void* dest, const void* source, size_t count)
{
	if (count == 0 || dest == source)
		return dest;

	uint8_t* d = (uint8_t*)dest;
	const uint8_t* s = (const uint8_t*)source;

	// Do not require matching alignment: Ethernet packets commonly start two
	// bytes into a cluster while their DMA bounce buffers are page aligned.
	// Keep each access inside the requested range, including at page edges.
	while (count >= 32) {
		uint64_t a = ((const unaligned_word*)(s + 0))->value;
		uint64_t b = ((const unaligned_word*)(s + 8))->value;
		uint64_t c = ((const unaligned_word*)(s + 16))->value;
		uint64_t e = ((const unaligned_word*)(s + 24))->value;
		((unaligned_word*)(d + 0))->value = a;
		((unaligned_word*)(d + 8))->value = b;
		((unaligned_word*)(d + 16))->value = c;
		((unaligned_word*)(d + 24))->value = e;
		s += 32;
		d += 32;
		count -= 32;
	}

	while (count >= 8) {
		((unaligned_word*)d)->value = ((const unaligned_word*)s)->value;
		s += 8;
		d += 8;
		count -= 8;
	}

	while (count > 0) {
		*d++ = *s++;
		count--;
	}
	return dest;
}
