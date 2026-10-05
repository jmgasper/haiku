/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_HEAP_GROWTH_H
#define MALI_CSF_HEAP_GROWTH_H

#include <stdint.h>

namespace MaliCSF {

// Kernel-only transaction. Prepare retains the heap and reserves allocations
// without changing exposed PTEs. Commit requires the worker's acknowledged AS
// lock; the memory then belongs to the retained heap even if GPU cleanup fails.
// The worker must flush/unlock before handing encodedChunk to the firmware.
struct HeapGrowth {
	void* state;
	uint64_t address, bytes, encodedChunk;
};

enum HeapGrowthResult { kHeapGrowthOK, kHeapGrowthNoMemory, kHeapGrowthInvalid };

} // namespace MaliCSF
#endif
