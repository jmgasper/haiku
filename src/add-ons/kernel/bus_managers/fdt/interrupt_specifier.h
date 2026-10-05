/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef FDT_INTERRUPT_SPECIFIER_H
#define FDT_INTERRUPT_SPECIFIER_H

#include <stddef.h>
#include <stdint.h>


static inline uint32_t
fdt_interrupt_cell(const uint8_t* bytes)
{
	return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16)
		| (uint32_t(bytes[2]) << 8) | bytes[3];
}


// Decode the simple interrupt encodings supported by the kernel FDT bus.
// GIC affinity partitions and extended interrupt ranges need routing support
// beyond a single global IRQ number, so do not silently discard them.
static inline bool
fdt_decode_interrupt(const void* property, size_t length, uint32_t cells,
	uint32_t index, uint32_t& interrupt)
{
	if (property == NULL || cells == 0 || cells > 4)
		return false;
	const size_t entrySize = cells * sizeof(uint32_t);
	if (length % entrySize != 0 || index >= length / entrySize)
		return false;
	const uint8_t* entry = (const uint8_t*)property + index * entrySize;
	uint32_t number = fdt_interrupt_cell(entry);
	if (cells >= 3) {
		const uint32_t type = number;
		number = fdt_interrupt_cell(entry + 4);
		if (cells == 4 && fdt_interrupt_cell(entry + 12) != 0)
			return false;
		if (type == 0 && number <= 987)
			number += 32;
		else if (type == 1 && number <= 15)
			number += 16;
		else
			return false;
	}
	interrupt = number;
	return true;
}

#endif // FDT_INTERRUPT_SPECIFIER_H
