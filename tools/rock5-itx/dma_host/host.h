// Kernel substitutes for compiling the real bus_dma.cpp on the workstation.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>

typedef uint64_t phys_addr_t;
typedef uint64_t phys_size_t;
typedef uint64_t bus_addr_t;
typedef size_t bus_size_t;
typedef uintptr_t addr_t;
typedef uintptr_t vm_offset_t;
typedef uint64_t vm_paddr_t;
typedef int32_t int32;
typedef uint32_t uint32;
#define B_PAGE_SIZE 4096UL
#define PAGESIZE B_PAGE_SIZE
#define PAGE_MASK (B_PAGE_SIZE - 1)
#define BUS_SPACE_MAXADDR UINT64_MAX
#define BUS_SPACE_UNRESTRICTED (~0)
#define B_PRIxPHYSADDR PRIx64
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define M_ZERO 0x100
#define M_NOWAIT 1
#define M_WAITOK 2
#define M_DEVBUF 0
#define M_PKTHDR 2
#define KASSERT(condition, message) do { if (!(condition)) panic message; } while (0)
#define kernel_malloc(size, type, flags) _kernel_malloc(size, flags)
#define kernel_free(ptr, type) _kernel_free(ptr)
#define kernel_contigmalloc(size, type, flags, low, high, align, boundary) \
	_kernel_contigmalloc(__FILE__, __LINE__, size, flags, low, high, align, boundary)
#define kernel_contigfree(ptr, size, type) _kernel_contigfree(ptr, size)
#define vtophys(ptr) pmap_kextract((vm_offset_t)(ptr))
#define dprintf dma_debug

struct mtx {};
struct mbuf {
	mbuf* m_next;
	char* m_data;
	int m_len;
	int m_flags;
	struct { int len; } m_pkthdr;
};

#ifdef __cplusplus
extern "C" {
#endif
void panic(const char*, ...);
void dma_debug(const char*, ...);
void* _kernel_malloc(size_t, int);
void _kernel_free(void*);
void* _kernel_contigmalloc(const char*, int, size_t, int, vm_paddr_t,
	vm_paddr_t, unsigned long, unsigned long);
void _kernel_contigfree(void*, size_t);
bool _kernel_contig_dma_address(const void*, size_t, vm_paddr_t*);
vm_paddr_t pmap_kextract(vm_offset_t);
void memory_full_barrier(void);
static inline int32 atomic_add(int32* p, int32 value)
	{ int32 old = *p; *p += value; return old; }
static inline void mtx_lock(struct mtx*) {}
static inline void mtx_unlock(struct mtx*) {}
static inline bool vm_addr_align_ok(vm_paddr_t p, unsigned long alignment)
	{ return (p & (alignment - 1)) == 0; }
#ifdef __cplusplus
}
#endif
