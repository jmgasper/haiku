#include "TimerQueue.h"

#include <new>

#include <util/AutoLock.h>

#include "ContainerOf.h"
#include "KmsDriver.h"


struct nvkms_timer_t final: public DPCCallback, public NvTimerQueue::TimerListItem {
	NvTimerQueue &fQueue;

	struct timer fTimer {};

	nvkms_timer_proc_t *fProc;
	void *fDataPtr;
	NvU32 fDataU32;

	bool fIsRefPtr: 1;
	bool fKernelTimer: 1;
	bool fPooled: 1;			// came from the set-aside timers
	// fCancel, fComplete are guarded by fQueue.fSpinlock.
	bool fCancel: 1;
	bool fComplete: 1;

	nvkms_timer_t(NvTimerQueue &queue, nvkms_timer_proc_t *proc, void *dataPtr,
			NvU32 dataU32, bool isRefPtr, bool kernelTimer, bool pooled):
		fQueue(queue),
		fProc(proc),
		fDataPtr(dataPtr),
		fDataU32(dataU32),
		fIsRefPtr(isRefPtr),
		fKernelTimer(kernelTimer),
		fPooled(pooled),
		fCancel(false),
		fComplete(false)
	{
	}

	static int32 TimerHook(struct timer *timer);
	void DoDPC(class DPCQueue *queue) final;
};


// Timer fires in interrupt context. The only job is to push the work onto the
// DPC queue, which is internally synchronized.
int32 nvkms_timer_t::TimerHook(struct timer *timer)
{
	nvkms_timer_t &nvkmsTimer = ContainerOf(*timer, &nvkms_timer_t::fTimer);
	nvkmsTimer.fQueue.fDpcQueue.Add(&nvkmsTimer);
	return B_HANDLED_INTERRUPT;
}


void nvkms_timer_t::DoDPC(class DPCQueue *queue)
{
	// Detach from the pending list before doing any user-visible work.
	{
		InterruptsSpinLocker locker(fQueue.fSpinlock);
		fQueue.fTimerList.Remove(static_cast<NvTimerQueue::TimerListItem*>(this));
	}

	void *dataPtr;
	bool cancel;

	{
		MutexLocker mutexLock(&NvHaikuKmsDriver::Instance().Locker());

		if (fIsRefPtr) {
			auto refPtr = static_cast<struct nvkms_ref_ptr*>(fDataPtr);
			dataPtr = nvkms_dec_ref(refPtr);
			// A NULL data pointer means the object this timer referred to has
			// been freed; treat as a cancellation.
			cancel = (dataPtr == NULL);
		} else {
			dataPtr = fDataPtr;
			cancel = false;
		}

		// Pick up any cancellation requested before we got here.
		{
			InterruptsSpinLocker locker(fQueue.fSpinlock);
			cancel = cancel || fCancel;
		}

		if (!cancel) {
			fProc(dataPtr, fDataU32);

			// Marking complete must happen before dropping the NVKMS lock so
			// that a subsequent Free() observes it atomically with respect to
			// our execution.
			InterruptsSpinLocker locker(fQueue.fSpinlock);
			fComplete = true;
			cancel = fCancel;
		}
	}

	// Ownership:
	//  - ref-ptr timers: this DPC always owns deletion (Linux: kfree in callback).
	//  - non-ref-ptr canceled timers: Free() abandoned us; we delete.
	//  - non-ref-ptr completed timers: caller's eventual Free() deletes us.
	if (fIsRefPtr || cancel) {
		fQueue.Destroy(this);
	}
}


// #pragma mark - NvTimerQueue

// With interrupts off - which is where resman calls NVKMS's vblank callback
// from - the allocator is out of bounds, so take one of the timers set aside
// in advance. Anywhere else, allocate as usual and leave the set-aside ones
// for the callback.
void *NvTimerQueue::AllocStorage(bool &pooled)
{
	if (are_interrupts_enabled()) {
		pooled = false;
		return malloc(sizeof(nvkms_timer_t));
	}

	InterruptsSpinLocker locker(fSpinlock);
	if (fPoolCount == 0) {
		pooled = false;
		return NULL;
	}
	pooled = true;
	return fPool[--fPoolCount];
}

void NvTimerQueue::FreeStorage(void *storage, bool pooled)
{
	if (storage == NULL) {
		return;
	}
	if (!pooled) {
		free(storage);
		return;
	}

	InterruptsSpinLocker locker(fSpinlock);
	if (fPoolCount < kPoolSize) {
		fPool[fPoolCount++] = storage;
		return;
	}
	// Cannot happen - nothing hands back more than it took - but not worth
	// leaking over.
	locker.Unlock();
	free(storage);
}

void NvTimerQueue::Destroy(nvkms_timer_t *timer)
{
	if (timer == NULL) {
		return;
	}
	const bool pooled = timer->fPooled;
	timer->~nvkms_timer_t();
	FreeStorage(timer, pooled);
}


NvTimerQueue::NvTimerQueue()
{
}

NvTimerQueue::~NvTimerQueue()
{
}

status_t NvTimerQueue::Init()
{
	for (uint32 i = 0; i < kPoolSize; i++) {
		void *storage = malloc(sizeof(nvkms_timer_t));
		if (storage == NULL) {
			break;
		}
		fPool[fPoolCount++] = storage;
	}

	return fDpcQueue.Init("NvTimerQueue", B_URGENT_DISPLAY_PRIORITY, 0);
}

void NvTimerQueue::Fini()
{
	// Phase 1: refuse new allocations and mark every live timer for cancel.
	// Phase 2: for each kernel-timer entry, try to cancel before it fires; if
	//          we win the race, free it ourselves. Otherwise the DPC drain
	//          below will handle it.
restart:
	{
		InterruptsSpinLocker locker(fSpinlock);
		fClosing = true;

		for (auto it = fTimerList.GetIterator(); it.HasNext();) {
			TimerListItem *item = it.Next();
			nvkms_timer_t *timer = static_cast<nvkms_timer_t*>(item);

			if (timer->fCancel) {
				// Already marked by a prior pass or by an external Free();
				// the DPC will clean it up.
				continue;
			}
			timer->fCancel = true;

			if (!timer->fKernelTimer) {
				// usec==0 entries are already in the DPC queue; we cannot
				// pre-empt them. DoDPC will observe fCancel.
				continue;
			}

			// cancel_timer may busy-wait for an in-flight hook on another CPU.
			// Drop the spinlock first to avoid deadlock with TimerHook callers.
			locker.Unlock();
			bool firedOrInFlight = cancel_timer(&timer->fTimer);
			locker.Lock();

			if (firedOrInFlight) {
				// Either the hook already enqueued a DPC, or it just finished
				// doing so. The DPC will execute (or has already) and free us.
				continue;
			}

			// We caught the timer before it fired: no DPC will run. Remove
			// from the list and free here.
			fTimerList.Remove(item);

			locker.Unlock();
			if (timer->fIsRefPtr) {
				auto refPtr = static_cast<struct nvkms_ref_ptr*>(timer->fDataPtr);
				nvkms_dec_ref(refPtr);
			}
			Destroy(timer);
			goto restart;
		}
	}

	// Phase 3: drain. Close(false) lets the worker run every remaining queued
	// DPC to completion, then terminates. Every surviving timer self-deletes
	// inside DoDPC because we set fCancel above.
	fDpcQueue.Close(false);

	InterruptsSpinLocker locker(fSpinlock);
	while (fPoolCount > 0) {
		void *storage = fPool[--fPoolCount];
		locker.Unlock();
		free(storage);
		locker.Lock();
	}
}


nvkms_timer_handle_t *NvTimerQueue::Alloc(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec)
{
	bool pooled;
	void *storage = AllocStorage(pooled);
	if (storage == NULL) {
		return NULL;
	}
	auto *timer = new(storage) nvkms_timer_t(*this, proc, dataPtr, dataU32,
		false, usec != 0, pooled);

	// List insertion and scheduling must happen atomically: a parallel Fini()
	// must either see the timer on the list (and handle it) or not have
	// allowed the Alloc to begin (fClosing).
	InterruptsSpinLocker locker(fSpinlock);
	if (fClosing) {
		locker.Unlock();
		Destroy(timer);
		return NULL;
	}

	fTimerList.Insert(static_cast<TimerListItem*>(timer));

	if (usec == 0) {
		fDpcQueue.Add(timer);
	} else {
		add_timer(&timer->fTimer, &nvkms_timer_t::TimerHook, usec, B_ONE_SHOT_RELATIVE_TIMER);
	}

	return timer;
}

NvBool NvTimerQueue::AllocWithRefPtr(nvkms_timer_proc_t *proc, struct nvkms_ref_ptr *ref_ptr, NvU32 dataU32, NvU64 usec)
{
	bool pooled;
	void *storage = AllocStorage(pooled);
	if (storage == NULL) {
		return NV_FALSE;
	}
	auto *timer = new(storage) nvkms_timer_t(*this, proc, ref_ptr, dataU32,
		true, usec != 0, pooled);

	// Hold a reference for the lifetime of the timer; DoDPC releases it.
	nvkms_inc_ref(ref_ptr);

	InterruptsSpinLocker locker(fSpinlock);
	if (fClosing) {
		locker.Unlock();
		nvkms_dec_ref(ref_ptr);
		Destroy(timer);
		return NV_FALSE;
	}

	fTimerList.Insert(static_cast<TimerListItem*>(timer));

	if (usec == 0) {
		fDpcQueue.Add(timer);
	} else {
		add_timer(&timer->fTimer, &nvkms_timer_t::TimerHook, usec, B_ONE_SHOT_RELATIVE_TIMER);
	}

	return NV_TRUE;
}

void NvTimerQueue::Free(nvkms_timer_handle_t *handle)
{
	if (handle == NULL) {
		return;
	}

	bool destroy;
	{
		InterruptsSpinLocker locker(handle->fQueue.fSpinlock);
		if (handle->fComplete) {
			destroy = true;
		} else {
			// Cooperative cancellation: DoDPC will observe fCancel and free us.
			handle->fCancel = true;
			destroy = false;
		}
	}

	if (destroy) {
		handle->fQueue.Destroy(handle);
	}
}
