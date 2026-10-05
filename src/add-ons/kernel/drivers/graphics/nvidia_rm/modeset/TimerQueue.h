#pragma once

extern "C" {
#include <nvidia-modeset-os-interface.h>
}

#include <DPC.h>

#include <util/DoublyLinkedList.h>


struct nvkms_timer_t;


class NvTimerQueue {
private:
	friend struct nvkms_timer_t;

	class TimerListItem: public DoublyLinkedListLinkImpl<TimerListItem> {};

	DPCQueue fDpcQueue;

	DoublyLinkedList<TimerListItem> fTimerList;

	spinlock fSpinlock = B_SPINLOCK_INITIALIZER;
	bool fClosing = false;

	// NVKMS allocates a timer from inside its vblank callback, which resman
	// calls with interrupts off, and Haiku's allocator cannot be called there:
	// the slab allocator's depot lock is an rw_lock. So a handful of timers
	// are set aside in advance for that case. They come back as soon as their
	// work has run, so a few are enough for a display that blanks sixty times
	// a second; running out costs one missed notification, which is better
	// than the machine.
	static constexpr uint32 kPoolSize = 32;
	void *fPool[kPoolSize] {};
	uint32 fPoolCount = 0;

	void *AllocStorage(bool &pooled);
	void FreeStorage(void *storage, bool pooled);

public:
	NvTimerQueue();
	~NvTimerQueue();

	status_t Init();
	void Fini();

	void Destroy(nvkms_timer_t *timer);

	nvkms_timer_handle_t *Alloc(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec);
	NvBool AllocWithRefPtr(nvkms_timer_proc_t *proc, struct nvkms_ref_ptr *ref_ptr, NvU32 dataU32, NvU64 usec);
	void Free(nvkms_timer_handle_t *handle);
};
