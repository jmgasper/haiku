/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_LX_HAIKU_H
#define POWERVR_LX_HAIKU_H


/*	What linux_compat.h (C) and linux_compat_haiku.cpp (C++) share: plain
	structures and the functions the C++ side implements on Haiku's kernel
	API. Errors are negative Linux codes; the LX_E* values below are the
	Linux ones, and linux_compat.h's E* codes are defined from them. */


#include <OS.h>
#include <StorageDefs.h>
#include <SupportDefs.h>


#define LX_ENOENT		2
#define LX_EIO			5
#define LX_E2BIG		7
#define LX_ENOMEM		12
#define LX_EINVAL		22
#define LX_ERANGE		34


#ifdef __cplusplus
extern "C" {
#endif


struct device;


/* Wait queues: a generation count and a ConditionVariable in the storage. */
typedef struct wait_queue_head {
	int32				generation;
	unsigned long long	storage[12];
} wait_queue_head_t;

void	init_waitqueue_head(wait_queue_head_t* queue);
void	wake_up_all(wait_queue_head_t* queue);
int32	lx_wait_queue_generation(wait_queue_head_t* queue);
void	lx_wait_queue_sleep(wait_queue_head_t* queue, int32 generation,
			bigtime_t deadline);


/* The highest physical address + 1 mask the GPU can reach (dma_set_mask). */
extern unsigned long long lx_dma_mask;


/* One physically contiguous piece of a buffer. */
struct lx_dma_run {
	unsigned long long	address;
	unsigned long long	size;
};

/* A locked kernel area, Normal-NC, with its physical pieces. */
struct lx_dma_buffer {
	area_id				area;
	void*				address;
	size_t				size;
	uint32				run_count;
	struct lx_dma_run*	runs;
};

int		lx_dma_buffer_alloc(struct lx_dma_buffer* buffer, size_t size,
			const char* name);
void	lx_dma_buffer_free(struct lx_dma_buffer* buffer);
int		lx_dma_buffer_address(const struct lx_dma_buffer* buffer,
			size_t offset, unsigned long long* _address);


/* Single pages for page tables; struct page is private to the C++ side. */
struct page;

enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3
};

typedef struct {
	unsigned long	value;
} pgprot_t;

#define LX_PGPROT_WRITECOMBINE	0x1

struct page*		alloc_page(unsigned int flags);
void				__free_page(struct page* page);
unsigned long long	dma_map_page(struct device* device, struct page* page,
						size_t offset, size_t size,
						enum dma_data_direction direction);
void				dma_unmap_page(struct device* device,
						unsigned long long address, size_t size,
						enum dma_data_direction direction);
void*				vmap(struct page** pages, unsigned int count,
						unsigned long flags, pgprot_t protection);
void				vunmap(const void* address);


/* Firmware files. */
struct firmware {
	size_t			size;
	const uint8*	data;
	char			path[B_PATH_NAME_LENGTH];
};

int		request_firmware(const struct firmware** _firmware, const char* name,
			struct device* device);
void	release_firmware(const struct firmware* firmware);


#ifdef __cplusplus
}
#endif


#endif	/* POWERVR_LX_HAIKU_H */
