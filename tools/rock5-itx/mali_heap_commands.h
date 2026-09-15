/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef ROCK5_MALI_HEAP_COMMANDS_H
#define ROCK5_MALI_HEAP_COMMANDS_H

#include <stdint.h>

struct HeapSample { uint64_t address, expected; };
static const uint64_t kHeapReadbackAddress = UINT64_C(0x150000000);

// The caller supplies 32 KiB of command storage and 8192 sample slots.
static inline unsigned
HeapCopyProgram(uint64_t* code, const HeapSample* samples, unsigned first, unsigned count)
{
	if (first > 8192 || count == 0 || count > 512 || count > 8192 - first)
		return 0;
	unsigned n = 0;
	code[n++] = UINT64_C(0x0300000000ff0000); // WAIT all
	code[n++] = UINT64_C(0x1700000000000002); // load/store scoreboard 0
	// LOAD/STORE_MULTIPLE use signed 16-bit offsets. Rebase each batch so
	// the destination stays correct when the readback crosses 32 KiB.
	code[n++] = UINT64_C(0x0104000000000000) | (kHeapReadbackAddress + first * 8);
	for (unsigned i = first; i < first + count; i++) {
		code[n++] = UINT64_C(0x0100000000000000) | samples[i].address;
		code[n++] = UINT64_C(0x1402000000030000); // LOAD r2:r3, [d0], mask3
		code[n++] = UINT64_C(0x0300000000010000);
		code[n++] = UINT64_C(0x1502040000030000) | ((i - first) * 8); // STORE r2:r3, [d4+offset]
		code[n++] = UINT64_C(0x0300000000010000);
	}
	return n * 8;
}

#endif
