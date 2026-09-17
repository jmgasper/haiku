#include "IntrSafePool.h"

#include <malloc.h>

#include <util/AutoLock.h>


void IntrSafePool::ReclaimAll()
{
	InterruptsSpinLocker _(&fSpinlock);
	for (uint32 i = 0; i < kGroupCount; i++) {
		auto &group = fGroups[i];
		while (group.count > 0) {
			group.count--;
			void *ptr = group.items[group.count];
			group.items[group.count] = nullptr;
			release_spinlock(&fSpinlock);
			enable_interrupts();
			free(ptr);
			disable_interrupts();
			acquire_spinlock(&fSpinlock);
		}
	}
}


void *IntrSafePool::Alloc(size_t size)
{
	SpinLocker _(&fSpinlock);

	// Pick the smallest size class that fits; fall through to larger classes
	// if our preferred class is empty.
	uint32 n = 0;
	size_t groupSize = kMinSize;
	while (groupSize < size) {
		n++;
		groupSize *= 2;
	}
	for (uint32 i = n; i < kGroupCount; i++) {
		auto &group = fGroups[i];
		if (group.count > 0) {
			group.count--;
			void *ptr = group.items[group.count];
			group.items[group.count] = nullptr;
			return ptr;
		}
	}

	// Pool exhausted (or size > largest class); let the caller propagate
	// NV_ERR_NO_MEMORY rather than panicking the kernel.
	return nullptr;
}


void IntrSafePool::Maintain()
{
	InterruptsSpinLocker _(&fSpinlock);

	size_t size = kMinSize;
	for (uint32 i = 0; i < kGroupCount; i++) {
		auto &group = fGroups[i];
		while (group.count < kGroupMaxItemCount) {
			release_spinlock(&fSpinlock);
			enable_interrupts();
			void *ptr = calloc(1, size);
			disable_interrupts();
			acquire_spinlock(&fSpinlock);

			if (ptr == nullptr) {
				// Out of memory; leave the pool partially refilled and bail.
				return;
			}
			if (group.count >= kGroupMaxItemCount) {
				// Another CPU refilled this group while we were calling calloc.
				// Return the excess (outside the spinlock).
				release_spinlock(&fSpinlock);
				enable_interrupts();
				free(ptr);
				disable_interrupts();
				acquire_spinlock(&fSpinlock);
				break;
			}
			group.items[group.count] = ptr;
			group.count++;
		}
		size *= 2;
	}
}
