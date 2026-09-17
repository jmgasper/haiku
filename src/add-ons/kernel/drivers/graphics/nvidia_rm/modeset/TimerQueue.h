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

public:
	NvTimerQueue();
	~NvTimerQueue();

	status_t Init();
	void Fini();

	nvkms_timer_handle_t *Alloc(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec);
	NvBool AllocWithRefPtr(nvkms_timer_proc_t *proc, struct nvkms_ref_ptr *ref_ptr, NvU32 dataU32, NvU64 usec);
	void Free(nvkms_timer_handle_t *handle);
};
