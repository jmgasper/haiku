/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef TEST_MALI_SYNC_OS_H
#define TEST_MALI_SYNC_OS_H
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <inttypes.h>
#include <mutex>
#include <thread>

using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using status_t = int32_t;
using area_id = int32_t;
using thread_id = int32_t;
using team_id = int32_t;
using phys_addr_t = uint64_t;
using bigtime_t = int64_t;
static const int B_OK = 0, B_NOT_SUPPORTED = -1, B_BAD_VALUE = -2, B_BAD_DATA = -3,
	B_BAD_ADDRESS = -4, B_NO_MEMORY = -5, B_NOT_ALLOWED = -6, B_ENTRY_NOT_FOUND = -7,
	B_BUSY = -8, B_DEV_INVALID_IOCTL = -9, B_CANCELED = -10, B_IO_ERROR = -11,
	B_TIMED_OUT = -12, B_INTERRUPTED = -13, B_FILE_ERROR = -14, B_NO_MORE_FDS = -15,
	B_WOULD_BLOCK = -16, B_NORMAL_PRIORITY = 10;
static const uint32 B_SYSTEM_TEAM = 1, B_CONTIGUOUS = 3,
	B_KERNEL_READ_AREA = 4, B_KERNEL_WRITE_AREA = 8,
	B_RELATIVE_TIMEOUT = 1, B_ABSOLUTE_TIMEOUT = 2, B_CAN_INTERRUPT = 4;
#define B_PRId32 PRId32
#define B_PRIx64 PRIx64
#define ASSERT(x) assert(x)

static bigtime_t system_time()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline void memory_full_barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
static inline void KernelPrint(const char*, ...) {}
#define dprintf KernelPrint
struct mutex { std::mutex value; };
#define MUTEX_INITIALIZER(name) {}
struct MutexLocker {
	mutex& lock;
	explicit MutexLocker(mutex& value) : lock(value) { lock.value.lock(); }
	~MutexLocker() { lock.value.unlock(); }
};
static std::atomic<bool> sInterruptWait{false};
static std::atomic<unsigned> sConditionWaiters{0};
struct ConditionVariable {
	std::condition_variable changed;
	void Init(const void*, const char*) {}
	void NotifyAll() { changed.notify_all(); }
	status_t Wait(mutex* mutex, uint32 flags = 0, bigtime_t timeout = 0)
	{
		if ((flags & B_CAN_INTERRUPT) && sInterruptWait.exchange(false)) return B_INTERRUPTED;
		std::unique_lock<std::mutex> lock(mutex->value, std::adopt_lock);
		status_t status = B_OK;
		sConditionWaiters++;
		if (flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) {
			bigtime_t remaining = (flags & B_ABSOLUTE_TIMEOUT) ? timeout - system_time() : timeout;
			if (changed.wait_for(lock, std::chrono::microseconds(remaining)) == std::cv_status::timeout)
				status = B_TIMED_OUT;
		} else changed.wait(lock);
		sConditionWaiters--;
		lock.release(); return status;
	}
};
static std::atomic<unsigned> sCopies{0}, sFailCopy{0};
static status_t user_memcpy(void* output, const void* input, size_t bytes)
{
	if (++sCopies == sFailCopy || output == NULL || input == NULL) return B_BAD_ADDRESS;
	memcpy(output, input, bytes); return B_OK;
}
template<class Predicate>
static void SyncAwait(Predicate condition)
{
	bigtime_t deadline = system_time() + 3000000;
	while (!condition()) {
		assert(system_time() < deadline);
		std::this_thread::yield();
	}
}
#endif
