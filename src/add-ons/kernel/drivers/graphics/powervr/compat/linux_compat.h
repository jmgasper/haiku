/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_LINUX_COMPAT_H
#define POWERVR_LINUX_COMPAT_H


/*	The part of the Linux kernel API that the reused drm/imagination files
	(upstream/) use, on Haiku's kernel. C only: the upstream files are GNU C
	and are compiled as such; the driver's C++ code never sees this header.

	Every <linux/...>, <drm/...> and <asm/...> header the upstream files
	include is a one-line stub next to this file that includes it, so the
	upstream files compile unchanged. What is here is what those files use,
	plus the hooks the driver's native code needs; nothing is speculative.

	Errors follow Linux: functions return 0 or a negative E* code with the
	Linux values defined below (Haiku's own E* codes are negative already,
	so -EINVAL would be positive and IS_ERR() would not see it).
	lx_status() turns one into a Haiku status_t. */


#ifdef __cplusplus
#	error "linux_compat.h is for the C sources only"
#endif
#ifndef __aarch64__
#	error "the barriers and MMIO accessors here are written for arm64"
#endif


/* Haiku first: everything below redefines names Haiku uses as well. */
#include <KernelExport.h>
#include <OS.h>
#include <SupportDefs.h>
#include <lock.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "lx_haiku.h"


/* #pragma mark - errors */


#undef EPERM
#undef ENOENT
#undef EIO
#undef E2BIG
#undef EAGAIN
#undef ENOMEM
#undef EFAULT
#undef EBUSY
#undef EEXIST
#undef ENODEV
#undef EINVAL
#undef ENOSPC
#undef ERANGE
#undef EOVERFLOW
#undef ETIMEDOUT

#define EPERM		1
#define ENOENT		LX_ENOENT
#define EIO			LX_EIO
#define E2BIG		LX_E2BIG
#define EAGAIN		11
#define ENOMEM		LX_ENOMEM
#define EFAULT		14
#define EBUSY		16
#define EEXIST		17
#define ENODEV		19
#define EINVAL		LX_EINVAL
#define ENOSPC		28
#define ERANGE		LX_ERANGE
#define EOVERFLOW	75
#define ETIMEDOUT	110

#define MAX_ERRNO	4095

status_t lx_status(int error);

static inline void*
ERR_PTR(long error)
{
	return (void*)error;
}

static inline long
PTR_ERR(const void* pointer)
{
	return (long)pointer;
}

static inline bool
IS_ERR(const void* pointer)
{
	return (unsigned long)pointer >= (unsigned long)-MAX_ERRNO;
}

static inline bool
IS_ERR_OR_NULL(const void* pointer)
{
	return pointer == NULL || IS_ERR(pointer);
}


/* #pragma mark - types */


typedef uint8_t				u8;
typedef uint16_t			u16;
typedef uint32_t			u32;
typedef unsigned long long	u64;
typedef int8_t				s8;
typedef int16_t				s16;
typedef int32_t				s32;
typedef long long			s64;

typedef u8	__u8;
typedef u16	__u16;
typedef u32	__u32;
typedef u64	__u64;
typedef s8	__s8;
typedef s16	__s16;
typedef s32	__s32;
typedef s64	__s64;

typedef u64 __attribute__((aligned(8)))	__aligned_u64;
#define aligned_u64		__aligned_u64

typedef u64				dma_addr_t;
typedef u64				resource_size_t;
typedef unsigned int	gfp_t;
typedef long long		loff_t;
typedef s64				ktime_t;

#define U8_MAX		((u8)~0U)
#define U16_MAX		((u16)~0U)
#define U32_MAX		((u32)~0U)
#define U64_MAX		((u64)~0ULL)
#define S32_MAX		((s32)(U32_MAX >> 1))

#define BITS_PER_LONG	64


/* #pragma mark - compiler */


#undef __aligned
#undef __packed
#undef __always_inline
#undef likely
#undef unlikely

#define __aligned(x)		__attribute__((aligned(x)))
#define __packed			__attribute__((packed))
#define __always_inline		inline __attribute__((always_inline))
#define __maybe_unused		__attribute__((unused))
#define __must_check		__attribute__((warn_unused_result))
#define __iomem
#define __user
#define __force
#define fallthrough			__attribute__((fallthrough))
#define likely(x)			__builtin_expect(!!(x), 1)
#define unlikely(x)			__builtin_expect(!!(x), 0)
#define barrier()			__asm__ __volatile__("" ::: "memory")

#define READ_ONCE(x)		(*(const volatile __typeof__(x)*)&(x))
#define WRITE_ONCE(x, value) \
	do { *(volatile __typeof__(x)*)&(x) = (value); } while (0)

#define container_of(pointer, type, member) \
	((type*)((char*)(pointer) - offsetof(type, member)))
#define container_of_const(pointer, type, member) \
	container_of(pointer, type, member)

#define sizeof_field(type, member)	sizeof((((type*)0)->member))
#define flex_array_size(pointer, member, count) \
	((count) * sizeof(*(pointer)->member))
#define ARRAY_SIZE(array)			(sizeof(array) / sizeof((array)[0]))

#undef static_assert
#define __lx_static_assert(expression, message, ...) \
	_Static_assert(expression, message)
#define static_assert(expression, ...) \
	__lx_static_assert(expression, ##__VA_ARGS__, #expression)
#define BUILD_BUG_ON(condition) \
	_Static_assert(!(condition), "BUILD_BUG_ON(" #condition ")")

#define check_add_overflow(a, b, result) \
	__builtin_add_overflow(a, b, result)

/* IS_ENABLED(): Linux's preprocessor trick, usable in #if as well. */
#define __LX_ARG_PLACEHOLDER_1			0,
#define __lx_take_second_arg(ignored, value, ...)	value
#define __lx_is_defined(x)				___lx_is_defined(x)
#define ___lx_is_defined(value) \
	____lx_is_defined(__LX_ARG_PLACEHOLDER_##value)
#define ____lx_is_defined(arg1_or_junk) \
	__lx_take_second_arg(arg1_or_junk 1, 0)
#define IS_ENABLED(option)				__lx_is_defined(option)

/* debugfs exists in the form of lx_debugfs_*() below; the firmware trace
   is read through it. */
#define CONFIG_DEBUG_FS	1


/* #pragma mark - bits and arithmetic */


#define _BITUL(x)		(1UL << (x))
#define _BITULL(x)		(1ULL << (x))
#define BIT(nr)			(1UL << (nr))
#define BIT_ULL(nr)		(1ULL << (nr))
#define GENMASK(high, low) \
	(((~0UL) - (1UL << (low)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (high))))
#define GENMASK_ULL(high, low) \
	(((~0ULL) - (1ULL << (low)) + 1) & (~0ULL >> (63 - (high))))
#define ULL(x)			(x##ULL)
#define U64_C(x)		(x##ULL)

#define FIELD_GET(mask, value) \
	((__typeof__(mask))(((value) & (mask)) >> __builtin_ctzll(mask)))
#define FIELD_PREP(mask, value) \
	(((__typeof__(mask))(value) << __builtin_ctzll(mask)) & (mask))

#define __ffs(x)		((unsigned long)__builtin_ctzl(x))

#define SZ_1K		0x00000400
#define SZ_4K		0x00001000
#define SZ_8K		0x00002000
#define SZ_16K		0x00004000
#define SZ_32K		0x00008000
#define SZ_64K		0x00010000
#define SZ_128K		0x00020000
#define SZ_256K		0x00040000
#define SZ_512K		0x00080000
#define SZ_1M		0x00100000
#define SZ_2M		0x00200000
#define SZ_4M		0x00400000
#define SZ_8M		0x00800000
#define SZ_16M		0x01000000
#define SZ_32M		0x02000000
#define SZ_64M		0x04000000
#define SZ_128M		0x08000000
#define SZ_256M		0x10000000
#define SZ_512M		0x20000000
#define SZ_1G		0x40000000
#define SZ_2G		0x80000000ULL
#define SZ_4G		0x100000000ULL
#define SZ_1T		0x10000000000ULL

#undef min
#undef max
#define min(a, b) \
	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) \
	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(type, a, b)	min((type)(a), (type)(b))
#define max_t(type, a, b)	max((type)(a), (type)(b))

#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define round_up(x, y)		((((x) - 1) | ((__typeof__(x))((y) - 1))) + 1)
#define round_down(x, y)	((x) & ~((__typeof__(x))((y) - 1)))
#define ALIGN(x, a) \
	(((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)	(((x) & ((__typeof__(x))(a) - 1)) == 0)

#define PAGE_SHIFT		12
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x)	ALIGN(x, PAGE_SIZE)


/* #pragma mark - barriers and MMIO */


/*	Linux's arm64 mb()/rmb()/wmb() are DSBs. The MMIO write accessors issue
	a full "dsb st" before the store (Linux: "dmb oshst"), so that firmware
	memory written through Normal-NC mappings has reached memory before the
	GPU is told about it; reads order later accesses behind them. */
#define mb()		__asm__ __volatile__("dsb sy" ::: "memory")
#define rmb()		__asm__ __volatile__("dsb ld" ::: "memory")
#define wmb()		__asm__ __volatile__("dsb st" ::: "memory")
#define dma_rmb()	__asm__ __volatile__("dmb oshld" ::: "memory")
#define dma_wmb()	__asm__ __volatile__("dmb oshst" ::: "memory")
#define smp_mb()	__asm__ __volatile__("dmb ish" ::: "memory")

/*	Register tracing: while lx_mmio_trace_base is set, every access through
	these accessors is passed to lx_mmio_trace(), which the driver supplies
	(it filters by range and thread and logs). */
extern const volatile void* volatile lx_mmio_trace_base;
void lx_mmio_trace(const volatile void* address, u64 value, unsigned int bits,
	bool write);

static __always_inline u32
ioread32(const volatile void __iomem* address)
{
	u32 value = *(const volatile u32*)address;
	__asm__ __volatile__("dmb oshld" ::: "memory");
	if (unlikely(lx_mmio_trace_base != NULL))
		lx_mmio_trace(address, value, 32, false);
	return value;
}

static __always_inline u64
ioread64(const volatile void __iomem* address)
{
	u64 value = *(const volatile u64*)address;
	__asm__ __volatile__("dmb oshld" ::: "memory");
	if (unlikely(lx_mmio_trace_base != NULL))
		lx_mmio_trace(address, value, 64, false);
	return value;
}

static __always_inline void
iowrite32(u32 value, volatile void __iomem* address)
{
	__asm__ __volatile__("dsb st" ::: "memory");
	*(volatile u32*)address = value;
	if (unlikely(lx_mmio_trace_base != NULL))
		lx_mmio_trace(address, value, 32, true);
}

static __always_inline void
iowrite64(u64 value, volatile void __iomem* address)
{
	__asm__ __volatile__("dsb st" ::: "memory");
	*(volatile u64*)address = value;
	if (unlikely(lx_mmio_trace_base != NULL))
		lx_mmio_trace(address, value, 64, true);
}

#define readl(address)			ioread32(address)
#define readq(address)			ioread64(address)
#define writel(value, address)	iowrite32(value, address)
#define writeq(value, address)	iowrite64(value, address)


/* #pragma mark - time */


#define HZ	1000

static inline unsigned long
lx_jiffies(void)
{
	return (unsigned long)(system_time() / (1000000 / HZ));
}

#define jiffies					lx_jiffies()
#define time_after(a, b)		((long)((b) - (a)) < 0)
#define time_before(a, b)		time_after(b, a)
#define msecs_to_jiffies(ms)	((unsigned long)(ms) * HZ / 1000)
#define jiffies_to_usecs(j)		((u64)(j) * (1000000 / HZ))

static inline ktime_t
ktime_get(void)
{
	return (ktime_t)system_time() * 1000;
}

static inline ktime_t
ktime_add_us(ktime_t time, u64 microseconds)
{
	return time + (s64)microseconds * 1000;
}

static inline ktime_t
ktime_sub(ktime_t a, ktime_t b)
{
	return a - b;
}

static inline s64
ktime_to_ns(ktime_t time)
{
	return time;
}

#define udelay(microseconds)	spin(microseconds)
#define ndelay(nanoseconds)		spin(((nanoseconds) + 999) / 1000)
#define msleep(milliseconds)	snooze((bigtime_t)(milliseconds) * 1000)
#define usleep_range(minimum, maximum)	snooze(minimum)

#define cpu_relax()		__asm__ __volatile__("yield" ::: "memory")

/*	readx_poll_timeout(): no time limit with a timeout of 0, polling without
	sleeping with a sleep time of 0, like Linux's. */
#define readx_poll_timeout(op, address, value, condition, sleep_us, \
		timeout_us) \
	({ \
		bigtime_t __deadline = system_time() + (bigtime_t)(timeout_us); \
		int __result = 0; \
		for (;;) { \
			(value) = op(address); \
			if (condition) \
				break; \
			if ((timeout_us) != 0 && system_time() > __deadline) { \
				(value) = op(address); \
				__result = (condition) ? 0 : -ETIMEDOUT; \
				break; \
			} \
			if ((sleep_us) != 0) \
				snooze(sleep_us); \
			else \
				cpu_relax(); \
		} \
		__result; \
	})
#define readl_poll_timeout(address, value, condition, sleep_us, timeout_us) \
	readx_poll_timeout(ioread32, address, value, condition, sleep_us, \
		timeout_us)
#define readq_poll_timeout(address, value, condition, sleep_us, timeout_us) \
	readx_poll_timeout(ioread64, address, value, condition, sleep_us, \
		timeout_us)


/* #pragma mark - printing */


enum {
	LX_LOG_ERROR,
	LX_LOG_WARNING,
	LX_LOG_INFO,
	LX_LOG_DEBUG
};

struct drm_device;

void lx_log(int level, const char* format, ...)
	__attribute__((format(printf, 2, 3)));
void lx_warn_on(const char* file, int line, const char* condition);

/* The device argument is evaluated (callers keep variables only for it). */
static inline void
lx_unused_device(const void* device)
{
	(void)device;
}

#define drm_err(drm, format, ...) \
	do { lx_unused_device(drm); \
		lx_log(LX_LOG_ERROR, format, ##__VA_ARGS__); } while (0)
#define drm_warn(drm, format, ...) \
	do { lx_unused_device(drm); \
		lx_log(LX_LOG_WARNING, format, ##__VA_ARGS__); } while (0)
#define drm_info(drm, format, ...) \
	do { lx_unused_device(drm); \
		lx_log(LX_LOG_INFO, format, ##__VA_ARGS__); } while (0)
#define drm_dbg(drm, format, ...) \
	do { lx_unused_device(drm); } while (0)
#define drm_warn_once(drm, format, ...) \
	do { \
		static bool __warned; \
		lx_unused_device(drm); \
		if (!__warned) { \
			__warned = true; \
			lx_log(LX_LOG_WARNING, format, ##__VA_ARGS__); \
		} \
	} while (0)
#define dev_err(dev, format, ...)	drm_err(dev, format, ##__VA_ARGS__)
#define dev_warn(dev, format, ...)	drm_warn(dev, format, ##__VA_ARGS__)
#define dev_info(dev, format, ...)	drm_info(dev, format, ##__VA_ARGS__)

#define WARN_ON(condition) \
	({ \
		bool __condition = !!(condition); \
		if (unlikely(__condition)) \
			lx_warn_on(__FILE__, __LINE__, #condition); \
		unlikely(__condition); \
	})
#define WARN_ON_ONCE(condition) \
	({ \
		static bool __warned; \
		bool __condition = !!(condition); \
		if (unlikely(__condition) && !__warned) { \
			__warned = true; \
			lx_warn_on(__FILE__, __LINE__, #condition); \
		} \
		unlikely(__condition); \
	})
#define WARN(condition, format, ...) \
	({ \
		bool __condition = !!(condition); \
		if (unlikely(__condition)) \
			lx_log(LX_LOG_WARNING, format, ##__VA_ARGS__); \
		unlikely(__condition); \
	})


/* #pragma mark - memory */


#define GFP_KERNEL		0x01u
#define GFP_ATOMIC		0x02u
#define __GFP_ZERO		0x100u

static inline void*
kmalloc(size_t size, gfp_t flags)
{
	return (flags & __GFP_ZERO) != 0 ? calloc(1, size) : malloc(size);
}

static inline void*
kzalloc(size_t size, gfp_t flags)
{
	(void)flags;
	return calloc(1, size);
}

static inline void*
kcalloc(size_t count, size_t size, gfp_t flags)
{
	(void)flags;
	return calloc(count, size);
}

static inline void
kfree(const void* pointer)
{
	free((void*)pointer);
}

#define kzalloc_obj(object, ...) \
	((__typeof__(object)*)kzalloc(sizeof(object), GFP_KERNEL))

static inline bool
mem_is_zero(const void* memory, size_t size)
{
	const u8* bytes = (const u8*)memory;
	for (size_t i = 0; i < size; i++) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}


/* #pragma mark - atomics and reference counts */


typedef struct {
	int	counter;
} atomic_t;

#define ATOMIC_INIT(value)	{ (value) }

static inline int
lx_atomic_read(const atomic_t* atomic)
{
	return __atomic_load_n(&atomic->counter, __ATOMIC_RELAXED);
}

static inline void
lx_atomic_set(atomic_t* atomic, int value)
{
	__atomic_store_n(&atomic->counter, value, __ATOMIC_RELAXED);
}

static inline int
lx_atomic_add_return(int value, atomic_t* atomic)
{
	return __atomic_add_fetch(&atomic->counter, value, __ATOMIC_SEQ_CST);
}

static inline int
lx_atomic_fetch_or(int value, atomic_t* atomic)
{
	return __atomic_fetch_or(&atomic->counter, value, __ATOMIC_SEQ_CST);
}

static inline int
lx_atomic_xchg(atomic_t* atomic, int value)
{
	return __atomic_exchange_n(&atomic->counter, value, __ATOMIC_SEQ_CST);
}

/* Haiku's atomic_set() and friends take other arguments: these shadow them
   from here on. */
#undef atomic_read
#undef atomic_set
#undef atomic_add
#undef atomic_or
#define atomic_read(atomic)			lx_atomic_read(atomic)
#define atomic_set(atomic, value)	lx_atomic_set(atomic, value)
#define atomic_add(value, atomic) \
	((void)lx_atomic_add_return(value, atomic))
#define atomic_inc(atomic)			((void)lx_atomic_add_return(1, atomic))
#define atomic_dec(atomic)			((void)lx_atomic_add_return(-1, atomic))
#define atomic_inc_return(atomic)	lx_atomic_add_return(1, atomic)
#define atomic_dec_return(atomic)	lx_atomic_add_return(-1, atomic)
#define atomic_dec_and_test(atomic) \
	(lx_atomic_add_return(-1, atomic) == 0)
#define atomic_fetch_or(value, atomic) \
	lx_atomic_fetch_or(value, atomic)
#define atomic_or(value, atomic) \
	((void)lx_atomic_fetch_or(value, atomic))
#define atomic_xchg(atomic, value)	lx_atomic_xchg(atomic, value)

struct kref {
	atomic_t	refcount;
};

static inline void
kref_init(struct kref* kref)
{
	atomic_set(&kref->refcount, 1);
}

static inline unsigned int
kref_read(const struct kref* kref)
{
	return (unsigned int)atomic_read(&kref->refcount);
}

static inline void
kref_get(struct kref* kref)
{
	atomic_inc(&kref->refcount);
}

static inline int
kref_get_unless_zero(struct kref* kref)
{
	int value = atomic_read(&kref->refcount);
	while (value != 0) {
		if (__atomic_compare_exchange_n(&kref->refcount.counter, &value,
				value + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
			return 1;
		}
	}
	return 0;
}

static inline int
kref_put(struct kref* kref, void (*release)(struct kref* kref))
{
	if (atomic_dec_and_test(&kref->refcount)) {
		release(kref);
		return 1;
	}
	return 0;
}


/* #pragma mark - lists */


struct list_head {
	struct list_head*	next;
	struct list_head*	prev;
};

#define LIST_HEAD_INIT(name)	{ &(name), &(name) }

static inline void
INIT_LIST_HEAD(struct list_head* list)
{
	list->next = list;
	list->prev = list;
}

static inline void
__lx_list_add(struct list_head* entry, struct list_head* prev,
	struct list_head* next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

static inline void
list_add(struct list_head* entry, struct list_head* head)
{
	__lx_list_add(entry, head, head->next);
}

static inline void
list_add_tail(struct list_head* entry, struct list_head* head)
{
	__lx_list_add(entry, head->prev, head);
}

/* Leaves the entry self-linked, so a second list_del() is harmless. */
static inline void
list_del(struct list_head* entry)
{
	entry->next->prev = entry->prev;
	entry->prev->next = entry->next;
	INIT_LIST_HEAD(entry);
}

#define list_del_init(entry)	list_del(entry)

static inline bool
list_empty(const struct list_head* head)
{
	return head->next == head;
}

#define list_entry(pointer, type, member)	container_of(pointer, type, member)
#define list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)
#define list_next_entry(entry, member) \
	list_entry((entry)->member.next, __typeof__(*(entry)), member)
#define list_for_each(position, head) \
	for (position = (head)->next; position != (head); \
		position = position->next)
#define list_for_each_entry(entry, head, member) \
	for (entry = list_first_entry(head, __typeof__(*entry), member); \
		&entry->member != (head); \
		entry = list_next_entry(entry, member))
#define list_for_each_entry_safe(entry, next, head, member) \
	for (entry = list_first_entry(head, __typeof__(*entry), member), \
			next = list_next_entry(entry, member); \
		&entry->member != (head); \
		entry = next, next = list_next_entry(next, member))


/* #pragma mark - locks */


/*	Haiku's spinlocks need interrupts off, as Linux's spin_lock_irqsave()
	has them; the plain variants do the same. */
typedef struct {
	spinlock	lock;
	cpu_status	state;
} spinlock_t;

#define spin_lock_init(spinlock_) \
	do { B_INITIALIZE_SPINLOCK(&(spinlock_)->lock); } while (0)

static inline void
spin_lock(spinlock_t* lock)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&lock->lock);
	lock->state = state;
}

static inline void
spin_unlock(spinlock_t* lock)
{
	cpu_status state = lock->state;
	release_spinlock(&lock->lock);
	restore_interrupts(state);
}

#define spin_lock_irqsave(lock, flags) \
	do { (flags) = 0; spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do { (void)(flags); spin_unlock(lock); } while (0)

/*	struct mutex is Haiku's (lock.h): mutex_lock(), mutex_unlock() and
	mutex_destroy() are Haiku's functions; Linux's mutex_init() has no
	name argument. */
#define mutex_init(lock)		mutex_init((lock), "powervr")
#define drmm_mutex_init(drm, lock) \
	({ lx_unused_device(drm); mutex_init(lock); 0; })
#define lockdep_assert_held(lock)	do { (void)(lock); } while (0)

struct rw_semaphore {
	rw_lock	lock;
};

#define init_rwsem(semaphore) \
	rw_lock_init(&(semaphore)->lock, "powervr reset")
#define down_read(semaphore)	rw_lock_read_lock(&(semaphore)->lock)
#define up_read(semaphore)		rw_lock_read_unlock(&(semaphore)->lock)
#define down_write(semaphore)	rw_lock_write_lock(&(semaphore)->lock)
#define up_write(semaphore)		rw_lock_write_unlock(&(semaphore)->lock)


/* #pragma mark - wait queues */


/*	wait_queue_head_t (lx_haiku.h) is a generation count and a Haiku
	ConditionVariable; wake_up_all() bumps the count and notifies, so a
	waiter that saw the old count before the condition cannot miss it. */

/*	Returns 0 if the condition is still false after the timeout (jiffies),
	otherwise the jiffies left, at least 1. */
#define wait_event_timeout(queue, condition, timeout) \
	({ \
		bigtime_t __deadline = system_time() \
			+ (bigtime_t)jiffies_to_usecs(timeout); \
		long __result; \
		for (;;) { \
			int32 __generation = lx_wait_queue_generation(&(queue)); \
			if (condition) { \
				__result = (long)((__deadline - system_time()) \
					/ (1000000 / HZ)); \
				if (__result < 1) \
					__result = 1; \
				break; \
			} \
			if (system_time() >= __deadline) { \
				__result = (condition) ? 1 : 0; \
				break; \
			} \
			lx_wait_queue_sleep(&(queue), __generation, __deadline); \
		} \
		__result; \
	})


/* #pragma mark - devices */


struct module;
struct dentry;
struct platform_device;
struct reset_control;
struct pwrseq_desc;
struct dev_pm_domain_list;
struct device_link;
struct workqueue_struct;

#define THIS_MODULE		((struct module*)NULL)

struct device {
	const char*	name;
	u64			dma_mask;
};

/*	The DRM device: only the fields the upstream files use, and the list of
	drmm_kzalloc() allocations, freed by lx_drm_dev_release(). */
struct drm_device {
	struct device*		dev;
	struct list_head	managed;
};

struct drm_file;

/* What the reused files use of a GEM object; pvr_gem.h embeds it. */
struct drm_gem_object {
	struct drm_device*	dev;
	size_t				size;
	struct kref			refcount;
};

void lx_drm_dev_init(struct drm_device* drm, struct device* device);
void lx_drm_dev_release(struct drm_device* drm);
void* drmm_kzalloc(struct drm_device* drm, size_t size, gfp_t flags);

static inline bool
drm_dev_enter(struct drm_device* drm, int* index)
{
	(void)drm;
	*index = 0;
	return true;
}

static inline void
drm_dev_exit(int index)
{
	(void)index;
}

static inline void
drm_dev_unplug(struct drm_device* drm)
{
	(void)drm;
}

struct resource {
	resource_size_t	start;
	resource_size_t	end;
};

/* The core clock: a fixed rate, set by the platform code. */
struct clk {
	unsigned long	rate;
};

static inline unsigned long
clk_get_rate(struct clk* clock)
{
	return clock != NULL ? clock->rate : 0;
}

/* Runtime power management: the GPU stays powered. */
static inline int
pm_runtime_resume_and_get(struct device* device)
{
	(void)device;
	return 0;
}

static inline int
pm_runtime_put(struct device* device)
{
	(void)device;
	return 0;
}

static inline void
pm_runtime_mark_last_busy(struct device* device)
{
	(void)device;
}

/* Only the structures pvr_device.h embeds; nothing uses them in M2. */
struct xarray {
	spinlock_t		xa_lock;
	void*			head;
	unsigned int	flags;
};

struct work_struct {
	void	(*func)(struct work_struct* work);
};

struct delayed_work {
	struct work_struct	work;
};


/* #pragma mark - DMA, pages and areas */


/*	No IOMMU and no coherency: a DMA address is the physical address, and
	everything the GPU sees is mapped Normal-NC (write-combined) after a
	"dc civac" of the pages. Every allocation stays below the device's DMA
	mask (dma_set_mask(), the GPU's physical bus width). */

#define DMA_BIT_MASK(n)		(((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR	(~(dma_addr_t)0)

static inline int
dma_set_mask(struct device* device, u64 mask)
{
	device->dma_mask = mask;
	lx_dma_mask = mask;
	return 0;
}

static inline unsigned int
dma_set_max_seg_size(struct device* device, unsigned int size)
{
	(void)device;
	(void)size;
	return 0;
}

static inline int
dma_mapping_error(struct device* device, dma_addr_t address)
{
	(void)device;
	return address == DMA_MAPPING_ERROR ? -ENOMEM : 0;
}

/*	Buffers are struct lx_dma_buffer (lx_dma_buffer_alloc()), single pages
	struct page (alloc_page(), dma_map_page(), vmap()): see lx_haiku.h. */

#define PAGE_KERNEL					((pgprot_t){ 0 })
#define pgprot_writecombine(prot) \
	((pgprot_t){ (prot).value | LX_PGPROT_WRITECOMBINE })
#define VM_MAP						0x4


/* #pragma mark - firmware files */


/*	request_firmware() (lx_haiku.h) reads "name" from data/firmware under
	/boot/system, then under /boot/system/non-packaged. */


/* #pragma mark - ELF */


#define EI_NIDENT	16
#define PT_LOAD		1

struct elf32_hdr {
	unsigned char	e_ident[EI_NIDENT];
	u16				e_type;
	u16				e_machine;
	u32				e_version;
	u32				e_entry;
	u32				e_phoff;
	u32				e_shoff;
	u32				e_flags;
	u16				e_ehsize;
	u16				e_phentsize;
	u16				e_phnum;
	u16				e_shentsize;
	u16				e_shnum;
	u16				e_shstrndx;
};

struct elf32_phdr {
	u32	p_type;
	u32	p_offset;
	u32	p_vaddr;
	u32	p_paddr;
	u32	p_filesz;
	u32	p_memsz;
	u32	p_flags;
	u32	p_align;
};


/* #pragma mark - drm_mm */


/*	A first-fit range allocator in the shape of drm_mm, for the firmware
	heap: nodes kept sorted by address. The caller locks. */

enum drm_mm_insert_mode {
	DRM_MM_INSERT_BEST = 0,
	DRM_MM_INSERT_LOW,
	DRM_MM_INSERT_HIGH,
	DRM_MM_INSERT_EVICT
};

struct drm_mm {
	u64					start;
	u64					size;
	struct list_head	nodes;
};

struct drm_mm_node {
	u64					start;
	u64					size;
	struct list_head	link;
	bool				allocated;
};

void drm_mm_init(struct drm_mm* mm, u64 start, u64 size);
void drm_mm_takedown(struct drm_mm* mm);
int drm_mm_insert_node_in_range(struct drm_mm* mm, struct drm_mm_node* node,
	u64 size, u64 alignment, unsigned long color, u64 rangeStart,
	u64 rangeEnd, enum drm_mm_insert_mode mode);
int drm_mm_reserve_node(struct drm_mm* mm, struct drm_mm_node* node);
void drm_mm_remove_node(struct drm_mm_node* node);

static inline bool
drm_mm_node_allocated(const struct drm_mm_node* node)
{
	return node->allocated;
}


/* #pragma mark - dma_fence */


/*	Only what pvr_ccb.c's KCCB slot reservation needs to compile and keep
	its reference counting; nobody waits on these fences before M3, which
	replaces this with the driver's own fences. */

struct dma_fence;

struct dma_fence_ops {
	const char*	(*get_driver_name)(struct dma_fence* fence);
	const char*	(*get_timeline_name)(struct dma_fence* fence);
	void		(*release)(struct dma_fence* fence);
};

struct dma_fence {
	const struct dma_fence_ops*	ops;
	spinlock_t*					lock;
	u64							context;
	u64							seqno;
	struct kref					refcount;
	unsigned long				flags;
};

#define DMA_FENCE_FLAG_SIGNALED_BIT	0

u64 dma_fence_context_alloc(unsigned int count);
void dma_fence_init(struct dma_fence* fence, const struct dma_fence_ops* ops,
	spinlock_t* lock, u64 context, u64 seqno);
void dma_fence_put(struct dma_fence* fence);
int dma_fence_signal(struct dma_fence* fence);
void dma_fence_free(struct dma_fence* fence);

static inline struct dma_fence*
dma_fence_get(struct dma_fence* fence)
{
	if (fence != NULL)
		kref_get(&fence->refcount);
	return fence;
}


/* #pragma mark - debugfs, seq_file and module parameters */


/*	debugfs files are kept in a small table instead of a file system;
	lx_debugfs_dump() reads one through its seq_file operations and passes
	each line on, lx_debugfs_attr_set() writes a DEFINE_DEBUGFS_ATTRIBUTE()
	value. This is how the driver reads the firmware trace. */

struct inode {
	void*	i_private;
};

struct file {
	void*	private_data;
};

struct file_operations {
	struct module*	owner;
	int				(*open)(struct inode* inode, struct file* file);
	ssize_t			(*read)(struct file* file, char __user* buffer,
						size_t size, loff_t* position);
	loff_t			(*llseek)(struct file* file, loff_t offset, int whence);
	int				(*release)(struct inode* inode, struct file* file);

	/* DEFINE_DEBUGFS_ATTRIBUTE() */
	int				(*attr_get)(void* data, u64* value);
	int				(*attr_set)(void* data, u64 value);
	const char*		attr_format;
};

struct seq_file;

struct seq_operations {
	void*	(*start)(struct seq_file* file, loff_t* position);
	void	(*stop)(struct seq_file* file, void* cookie);
	void*	(*next)(struct seq_file* file, void* cookie, loff_t* position);
	int		(*show)(struct seq_file* file, void* cookie);
};

struct seq_file {
	char*						buffer;
	size_t						size;
	size_t						count;
	const struct seq_operations* op;
	void*						private;
};

int seq_open(struct file* file, const struct seq_operations* operations);
int seq_release(struct inode* inode, struct file* file);
ssize_t seq_read(struct file* file, char __user* buffer, size_t size,
	loff_t* position);
loff_t seq_lseek(struct file* file, loff_t offset, int whence);
void seq_printf(struct seq_file* file, const char* format, ...)
	__attribute__((format(printf, 2, 3)));
void seq_puts(struct seq_file* file, const char* text);

struct dentry* debugfs_create_file(const char* name, umode_t mode,
	struct dentry* parent, void* data, const struct file_operations* fops);

#define DEFINE_DEBUGFS_ATTRIBUTE(fops_, get_, set_, format_) \
	static const struct file_operations fops_ = { \
		.attr_get = (get_), \
		.attr_set = (set_), \
		.attr_format = (format_) \
	}

int lx_debugfs_dump(const char* name,
	void (*line)(void* cookie, const char* text), void* cookie,
	unsigned int maxLines);
int lx_debugfs_attr_set(const char* name, u64 value);
void lx_debugfs_remove_all(void);

/*	module_param_cb() makes a global "lx_param_<name>" the driver can set
	from its settings file. */
struct kernel_param;

struct kernel_param_ops {
	int	(*set)(const char* value, const struct kernel_param* parameter);
	int	(*get)(char* buffer, const struct kernel_param* parameter);
};

struct kernel_param {
	const char*						name;
	const struct kernel_param_ops*	ops;
	void*							arg;
};

int param_get_hexint(char* buffer, const struct kernel_param* parameter);

#define module_param_cb(name_, ops_, arg_, permissions_) \
	const struct kernel_param lx_param_##name_ = { #name_, (ops_), (arg_) }
#define param_check_hexint(name_, pointer_) \
	static inline unsigned int* lx_param_check_##name_(void) \
		{ return (pointer_); }
#define __MODULE_PARM_TYPE(name_, type_)	_Static_assert(1, type_)
#define MODULE_PARM_DESC(name_, text_)		_Static_assert(1, text_)

int kstrtouint(const char* text, unsigned int base, unsigned int* _value);


#endif	/* POWERVR_LINUX_COMPAT_H */
