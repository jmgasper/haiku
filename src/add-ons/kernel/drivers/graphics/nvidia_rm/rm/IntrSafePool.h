#pragma once

#include <KernelExport.h>
#include <lock.h>


// Pre-allocated, fixed-size-class memory pool serving NVIDIA's os_alloc_mem
// in interrupt context (where Haiku's malloc() cannot be called because the
// slab allocator's depot lock is an rw_lock).
//
// IRQ-time frees do NOT go through this pool: os_free_mem uses deferred_free()
// which is safe with interrupts disabled. The pool is therefore drain-only;
// Maintain() refills it from a safe context.
class IntrSafePool {
private:
	static constexpr uint32 kGroupCount = 4;
	static constexpr uint32 kGroupMaxItemCount = 16;
	static constexpr size_t kMinSize = 16;

	struct Group {
		uint32 count = 0;
		void *items[kGroupMaxItemCount] {};
	};

	Group fGroups[kGroupCount];
	spinlock fSpinlock = B_SPINLOCK_INITIALIZER;

public:
	void ReclaimAll();
	void *Alloc(size_t size);
	void Maintain();
};
