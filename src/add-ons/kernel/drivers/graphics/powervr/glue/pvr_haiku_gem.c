/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	pvr_gem.h on Haiku: buffer objects are locked, Normal-NC kernel areas
	(lx_dma_buffer_alloc()), pinned and zeroed at creation as with GEM
	shmem in drm/imagination (pvr_gem.c: pvr_gem_object_create()). The
	kernel mapping lasts as long as the object, so vmap/vunmap only check. */


#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_haiku_device.h"


struct pvr_gem_object*
pvr_gem_object_create(struct pvr_device* pvr_dev, size_t size, u64 flags)
{
	if (size == 0 || (flags & PVR_BO_UNDEFINED_MASK) != 0)
		return ERR_PTR(-EINVAL);

	struct pvr_gem_object* object
		= (struct pvr_gem_object*)kzalloc(sizeof(*object), GFP_KERNEL);
	if (object == NULL)
		return ERR_PTR(-ENOMEM);

	int error = lx_dma_buffer_alloc(&object->buffer, size, "powervr buffer");
	if (error != 0) {
		kfree(object);
		return ERR_PTR(error);
	}

	object->base.dev = from_pvr_device(pvr_dev);
	object->base.size = object->buffer.size;
	kref_init(&object->base.refcount);
	// The GPU is not cache coherent: never CPU cached (pvr_gem.c does the
	// same when the device is not DMA coherent).
	object->flags = flags & ~PVR_BO_CPU_CACHED;
	return object;
}


static void
pvr_gem_object_release(struct kref* kref)
{
	struct pvr_gem_object* object = container_of(kref, struct pvr_gem_object,
		base.refcount);
	struct pvr_device* pvr_dev = to_pvr_device(object->base.dev);

	if (to_haiku_device(pvr_dev)->keep_memory) {
		// The GPU may still read or write it: leave it to the next boot.
		dprintf("powervr: keeping a %zu KiB firmware buffer (area %" B_PRId32
			"), the GPU may still use it\n", object->buffer.size / 1024,
			object->buffer.area);
		kfree(object);
		return;
	}

	lx_dma_buffer_free(&object->buffer);
	kfree(object);
}


void
pvr_gem_object_get(struct pvr_gem_object* pvr_obj)
{
	kref_get(&pvr_obj->base.refcount);
}


void
pvr_gem_object_put(struct pvr_gem_object* pvr_obj)
{
	if (pvr_obj != NULL)
		kref_put(&pvr_obj->base.refcount, pvr_gem_object_release);
}


void*
pvr_gem_object_vmap(struct pvr_gem_object* pvr_obj)
{
	if (pvr_obj->buffer.address == NULL)
		return ERR_PTR(-ENOMEM);
	return pvr_obj->buffer.address;
}


void
pvr_gem_object_vunmap(struct pvr_gem_object* pvr_obj)
{
	// the kernel mapping stays for the object's life
	(void)pvr_obj;
}


int
pvr_gem_get_dma_addr(struct pvr_gem_object* pvr_obj, u32 offset,
	dma_addr_t* dma_addr_out)
{
	return lx_dma_buffer_address(&pvr_obj->buffer, offset, dma_addr_out);
}
