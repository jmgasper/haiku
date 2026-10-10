/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	DRM sync objects (Linux's drm_syncobj.c) for one open of the device:
	binary ones that hold the current fence, and timelines that hold a fence
	per point. Mesa's Vulkan driver builds its semaphores and fences on
	these (vk_drm_syncobj), so the semantics are Linux's:

	- A timeline point is signaled once its fence and those of all earlier
	  points are (dma_fence_chain); point 0 means the current fence, the
	  newest point's of a timeline.
	- "completed" is the highest point known signaled with everything
	  before it, which is what a query reports; fences up to there are
	  dropped as soon as they are seen signaled.
	- A wait on a point nobody has submitted a fence for yet fails with
	  EINVAL, unless WAIT_FOR_SUBMIT says to wait for one.
	- Timeouts are absolute CLOCK_MONOTONIC nanoseconds (system_time()),
	  0 polls; a timeout returns ETIME, a signal EINTR.

	All sync object state is under one lock; waits sleep on lx_fence_queue,
	which every fence signal and every change here wakes. */


#include "pvr_device.h"
#include "pvr_haiku_device.h"
#include "pvr_haiku_drm.h"

#include <linux/dma-fence.h>


/* the most handles one wait or array call takes */
#define MAX_SYNCOBJ_HANDLES		4096

/* Linux's limit for WAIT_FOR_SUBMIT in drm_syncobj_find_fence() */
#define FIND_FENCE_SUBMIT_TIMEOUT	10000000


struct syncobj_point {
	struct list_head	link;
	u64					point;
	struct dma_fence*	fence;
};

struct pvr_haiku_syncobj {
	struct kref			refcount;
	bool				attached;	/* has a fence (Linux: ->fence set) */
	u64					completed;
	struct list_head	points;		/* not yet seen signaled, ascending */
};

enum syncobj_state {
	SYNCOBJ_NOT_SUBMITTED,
	SYNCOBJ_PENDING,
	SYNCOBJ_SIGNALED
};


static mutex sSyncLock = MUTEX_INITIALIZER("powervr syncobjs");


/* #pragma mark - state (sSyncLock held) */


static void
drop_points(struct pvr_haiku_syncobj* syncobj)
{
	struct syncobj_point* entry;
	struct syncobj_point* next;
	list_for_each_entry_safe(entry, next, &syncobj->points, link) {
		list_del(&entry->link);
		dma_fence_put(entry->fence);
		kfree(entry);
	}
}


/*!	Drops the signaled fences at the start of the timeline, moving
	"completed" past them.
*/
static void
collect(struct pvr_haiku_syncobj* syncobj)
{
	while (!list_empty(&syncobj->points)) {
		struct syncobj_point* entry = list_first_entry(&syncobj->points,
			struct syncobj_point, link);
		if (!dma_fence_is_signaled(entry->fence))
			break;
		if (entry->point > syncobj->completed)
			syncobj->completed = entry->point;
		list_del(&entry->link);
		dma_fence_put(entry->fence);
		kfree(entry);
	}
}


static u64
last_point(struct pvr_haiku_syncobj* syncobj)
{
	if (list_empty(&syncobj->points))
		return syncobj->completed;
	struct syncobj_point* last = list_last_entry(&syncobj->points,
		struct syncobj_point, link);
	return max(last->point, syncobj->completed);
}


static enum syncobj_state
state_of(struct pvr_haiku_syncobj* syncobj, u64 point)
{
	collect(syncobj);
	if (!syncobj->attached)
		return SYNCOBJ_NOT_SUBMITTED;
	if (point == 0)
		return list_empty(&syncobj->points) ? SYNCOBJ_SIGNALED
			: SYNCOBJ_PENDING;
	if (last_point(syncobj) < point)
		return SYNCOBJ_NOT_SUBMITTED;
	return syncobj->completed >= point ? SYNCOBJ_SIGNALED : SYNCOBJ_PENDING;
}


/*!	Linux's drm_syncobj_replace_fence(): \a fence (or none) becomes the
	only one; the timeline starts over.
*/
static int
replace_fence(struct pvr_haiku_syncobj* syncobj, struct dma_fence* fence)
{
	struct syncobj_point* entry = NULL;
	if (fence != NULL) {
		entry = (struct syncobj_point*)kzalloc(sizeof(*entry), GFP_KERNEL);
		if (entry == NULL)
			return -ENOMEM;
	}

	drop_points(syncobj);
	syncobj->completed = 0;
	syncobj->attached = fence != NULL;
	if (entry != NULL) {
		entry->point = 0;
		entry->fence = dma_fence_get(fence);
		list_add_tail(&entry->link, &syncobj->points);
	}
	return 0;
}


/*!	Linux's drm_syncobj_add_point(): \a fence for \a point, after the
	points before it (Linux warns about points that do not grow, and so
	does this).
*/
static int
add_point(struct pvr_haiku_syncobj* syncobj, u64 point,
	struct dma_fence* fence)
{
	struct syncobj_point* entry
		= (struct syncobj_point*)kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL)
		return -ENOMEM;
	entry->point = point;
	entry->fence = dma_fence_get(fence);

	if (syncobj->attached && point <= last_point(syncobj)) {
		lx_log(LX_LOG_WARNING, "sync object point %llu after %llu",
			(unsigned long long)point,
			(unsigned long long)last_point(syncobj));
	}

	struct list_head* before = &syncobj->points;
	struct syncobj_point* other;
	list_for_each_entry(other, &syncobj->points, link) {
		if (other->point > point) {
			before = &other->link;
			break;
		}
	}
	list_add_tail(&entry->link, before);
	syncobj->attached = true;
	return 0;
}


/*!	A fence that signals with \a point: the current fence for point 0,
	otherwise those of all pending points up to the first one at or past
	\a point. NULL if there is none yet.
*/
static struct dma_fence*
fence_for(struct pvr_haiku_syncobj* syncobj, u64 point, int* _error)
{
	*_error = 0;
	enum syncobj_state state = state_of(syncobj, point);
	if (state == SYNCOBJ_NOT_SUBMITTED)
		return NULL;
	if (state == SYNCOBJ_SIGNALED)
		return dma_fence_get_stub();

	u32 count = 0;
	struct syncobj_point* entry;
	list_for_each_entry(entry, &syncobj->points, link) {
		count++;
		if (point != 0 && entry->point >= point)
			break;
	}
	if (count == 1) {
		entry = list_first_entry(&syncobj->points, struct syncobj_point,
			link);
		return dma_fence_get(entry->fence);
	}

	struct dma_fence** fences
		= (struct dma_fence**)kcalloc(count, sizeof(*fences), GFP_KERNEL);
	if (fences == NULL) {
		*_error = -ENOMEM;
		return NULL;
	}
	u32 index = 0;
	list_for_each_entry(entry, &syncobj->points, link) {
		if (index == count)
			break;
		fences[index++] = dma_fence_get(entry->fence);
	}
	struct dma_fence* all = lx_dma_fence_all(fences, count);
	kfree(fences);
	if (all == NULL)
		*_error = -ENOMEM;
	return all;
}


/* #pragma mark - objects and handles */


static struct pvr_haiku_syncobj*
syncobj_create(void)
{
	struct pvr_haiku_syncobj* syncobj
		= (struct pvr_haiku_syncobj*)kzalloc(sizeof(*syncobj), GFP_KERNEL);
	if (syncobj == NULL)
		return NULL;
	kref_init(&syncobj->refcount);
	INIT_LIST_HEAD(&syncobj->points);
	return syncobj;
}


static void
syncobj_release(struct kref* kref)
{
	struct pvr_haiku_syncobj* syncobj
		= container_of(kref, struct pvr_haiku_syncobj, refcount);
	mutex_lock(&sSyncLock);
	drop_points(syncobj);
	mutex_unlock(&sSyncLock);
	kfree(syncobj);
}


static void
syncobj_put(struct pvr_haiku_syncobj* syncobj)
{
	if (syncobj != NULL)
		kref_put(&syncobj->refcount, syncobj_release);
}


static struct pvr_haiku_syncobj*
syncobj_lookup(struct pvr_haiku_file* file, u32 handle)
{
	xa_lock(&file->syncobjs);
	struct pvr_haiku_syncobj* syncobj
		= (struct pvr_haiku_syncobj*)xa_load(&file->syncobjs, handle);
	if (syncobj != NULL)
		kref_get(&syncobj->refcount);
	xa_unlock(&file->syncobjs);
	return syncobj;
}


static void
put_syncobjs(struct pvr_haiku_syncobj** syncobjs, u32 count)
{
	for (u32 i = 0; i < count; i++)
		syncobj_put(syncobjs[i]);
	kfree(syncobjs);
}


/*!	The sync objects for \a count handles from userland, referenced; with
	\a points also their points (0s if \a userPoints is 0).
*/
static int
lookup_array(struct pvr_haiku_file* file, u64 userHandles, u64 userPoints,
	u32 count, struct pvr_haiku_syncobj*** _syncobjs, u64** _points)
{
	if (count == 0 || count > MAX_SYNCOBJ_HANDLES)
		return -EINVAL;

	u32* handles = (u32*)kcalloc(count, sizeof(u32), GFP_KERNEL);
	u64* points = NULL;
	struct pvr_haiku_syncobj** syncobjs = (struct pvr_haiku_syncobj**)
		kcalloc(count, sizeof(*syncobjs), GFP_KERNEL);
	if (_points != NULL)
		points = (u64*)kcalloc(count, sizeof(u64), GFP_KERNEL);
	if (handles == NULL || syncobjs == NULL
		|| (_points != NULL && points == NULL)) {
		kfree(handles);
		kfree(syncobjs);
		kfree(points);
		return -ENOMEM;
	}

	int error = 0;
	if (copy_from_user(handles, u64_to_user_ptr(userHandles),
			count * sizeof(u32)) != 0) {
		error = -EFAULT;
	} else if (points != NULL && userPoints != 0
		&& copy_from_user(points, u64_to_user_ptr(userPoints),
			count * sizeof(u64)) != 0) {
		error = -EFAULT;
	}
	for (u32 i = 0; error == 0 && i < count; i++) {
		syncobjs[i] = syncobj_lookup(file, handles[i]);
		if (syncobjs[i] == NULL)
			error = -ENOENT;
	}
	kfree(handles);
	if (error != 0) {
		put_syncobjs(syncobjs, count);
		kfree(points);
		return error;
	}

	*_syncobjs = syncobjs;
	if (_points != NULL)
		*_points = points;
	return 0;
}


void
pvr_haiku_syncobjs_init(struct pvr_haiku_file* file)
{
	xa_init_flags(&file->syncobjs, XA_FLAGS_ALLOC1);
}


void
pvr_haiku_syncobjs_fini(struct pvr_haiku_file* file)
{
	unsigned long handle;
	struct pvr_haiku_syncobj* syncobj;
	xa_for_each(&file->syncobjs, handle, syncobj) {
		xa_erase(&file->syncobjs, handle);
		syncobj_put(syncobj);
	}
	xa_destroy(&file->syncobjs);
}


/* #pragma mark - waits */


/*	Linux's drm_timeout_abs_to_jiffies(), as a system_time() deadline. */
static bigtime_t
deadline_for(s64 timeoutNanoseconds)
{
	if (timeoutNanoseconds <= 0)
		return 0;
	if (timeoutNanoseconds >= B_INFINITE_TIMEOUT - 1000)
		return B_INFINITE_TIMEOUT;
	return (bigtime_t)(timeoutNanoseconds / 1000);
}


static int
wait_array(struct pvr_haiku_syncobj** syncobjs, const u64* points,
	u32 count, u32 flags, s64 timeoutNanoseconds, u32* _firstSignaled)
{
	const bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	const bool forSubmit = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
		!= 0;
	const enum syncobj_state wanted
		= (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0
			? SYNCOBJ_PENDING : SYNCOBJ_SIGNALED;
	const bigtime_t deadline = deadline_for(timeoutNanoseconds);

	for (;;) {
		int32 generation = lx_wait_queue_generation(&lx_fence_queue);

		u32 done = 0;
		u32 first = count;
		bool notSubmitted = false;
		mutex_lock(&sSyncLock);
		for (u32 i = 0; i < count; i++) {
			enum syncobj_state state = state_of(syncobjs[i],
				points != NULL ? points[i] : 0);
			if (state == SYNCOBJ_NOT_SUBMITTED)
				notSubmitted = true;
			if (state >= wanted) {
				done++;
				if (first == count)
					first = i;
			}
		}
		mutex_unlock(&sSyncLock);

		if (notSubmitted && !forSubmit)
			return -EINVAL;
		if (all ? done == count : done > 0) {
			*_firstSignaled = first;
			return 0;
		}
		if (system_time() >= deadline)
			return -ETIME;
		if (lx_wait_queue_sleep_interruptible(&lx_fence_queue, generation,
				deadline) == B_INTERRUPTED) {
			return -EINTR;
		}
	}
}


#define VALID_WAIT_FLAGS \
	(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL \
		| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT \
		| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE \
		| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)


int
pvr_haiku_syncobj_wait(struct pvr_haiku_file* file,
	struct drm_syncobj_wait* args)
{
	// WAIT_AVAILABLE only makes sense on a timeline (as in Linux)
	if ((args->flags & ~VALID_WAIT_FLAGS) != 0
		|| (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0) {
		return -EINVAL;
	}

	struct pvr_haiku_syncobj** syncobjs;
	int error = lookup_array(file, args->handles, 0, args->count_handles,
		&syncobjs, NULL);
	if (error != 0)
		return error;

	u32 first = 0;
	error = wait_array(syncobjs, NULL, args->count_handles, args->flags,
		args->timeout_nsec, &first);
	if (error == 0)
		args->first_signaled = first;
	put_syncobjs(syncobjs, args->count_handles);
	return error;
}


int
pvr_haiku_syncobj_timeline_wait(struct pvr_haiku_file* file,
	struct drm_syncobj_timeline_wait* args)
{
	if ((args->flags & ~VALID_WAIT_FLAGS) != 0)
		return -EINVAL;

	struct pvr_haiku_syncobj** syncobjs;
	u64* points;
	int error = lookup_array(file, args->handles, args->points,
		args->count_handles, &syncobjs, &points);
	if (error != 0)
		return error;

	u32 first = 0;
	error = wait_array(syncobjs, points, args->count_handles, args->flags,
		args->timeout_nsec, &first);
	if (error == 0)
		args->first_signaled = first;
	put_syncobjs(syncobjs, args->count_handles);
	kfree(points);
	return error;
}


/* #pragma mark - the other ioctls */


int
pvr_haiku_syncobj_create(struct pvr_haiku_file* file,
	struct drm_syncobj_create* args)
{
	if ((args->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0)
		return -EINVAL;

	struct pvr_haiku_syncobj* syncobj = syncobj_create();
	if (syncobj == NULL)
		return -ENOMEM;
	if ((args->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0) {
		struct dma_fence* stub = dma_fence_get_stub();
		mutex_lock(&sSyncLock);
		int error = replace_fence(syncobj, stub);
		mutex_unlock(&sSyncLock);
		dma_fence_put(stub);
		if (error != 0) {
			syncobj_put(syncobj);
			return error;
		}
	}

	u32 handle;
	int error = xa_alloc(&file->syncobjs, &handle, syncobj, xa_limit_32b,
		GFP_KERNEL);
	if (error != 0) {
		syncobj_put(syncobj);
		return error;
	}
	args->handle = handle;
	return 0;
}


int
pvr_haiku_syncobj_destroy(struct pvr_haiku_file* file,
	struct drm_syncobj_destroy* args)
{
	if (args->pad != 0)
		return -EINVAL;
	struct pvr_haiku_syncobj* syncobj = (struct pvr_haiku_syncobj*)
		xa_erase(&file->syncobjs, args->handle);
	if (syncobj == NULL)
		return -EINVAL;
	syncobj_put(syncobj);
	return 0;
}


/*!	Sets each object of an array: its fence replaced by \a fence (NULL:
	none) or, with \a userPoints, \a fence added at each point (point 0
	replaces).
*/
static int
set_array(struct pvr_haiku_file* file, u64 userHandles, u64 userPoints,
	u32 count, struct dma_fence* fence)
{
	struct pvr_haiku_syncobj** syncobjs;
	u64* points = NULL;
	int error = lookup_array(file, userHandles, userPoints, count,
		&syncobjs, userPoints != 0 ? &points : NULL);
	if (error != 0)
		return error;

	mutex_lock(&sSyncLock);
	for (u32 i = 0; i < count && error == 0; i++) {
		if (points != NULL && points[i] != 0)
			error = add_point(syncobjs[i], points[i], fence);
		else
			error = replace_fence(syncobjs[i], fence);
	}
	mutex_unlock(&sSyncLock);
	wake_up_all(&lx_fence_queue);

	put_syncobjs(syncobjs, count);
	kfree(points);
	return error;
}


int
pvr_haiku_syncobj_reset(struct pvr_haiku_file* file,
	struct drm_syncobj_array* args)
{
	if (args->pad != 0)
		return -EINVAL;
	return set_array(file, args->handles, 0, args->count_handles, NULL);
}


int
pvr_haiku_syncobj_signal(struct pvr_haiku_file* file,
	struct drm_syncobj_array* args)
{
	if (args->pad != 0)
		return -EINVAL;
	struct dma_fence* stub = dma_fence_get_stub();
	int error = set_array(file, args->handles, 0, args->count_handles, stub);
	dma_fence_put(stub);
	return error;
}


int
pvr_haiku_syncobj_timeline_signal(struct pvr_haiku_file* file,
	struct drm_syncobj_timeline_array* args)
{
	if (args->flags != 0 || args->points == 0)
		return -EINVAL;
	struct dma_fence* stub = dma_fence_get_stub();
	int error = set_array(file, args->handles, args->points,
		args->count_handles, stub);
	dma_fence_put(stub);
	return error;
}


int
pvr_haiku_syncobj_query(struct pvr_haiku_file* file,
	struct drm_syncobj_timeline_array* args)
{
	if ((args->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0)
		return -EINVAL;

	struct pvr_haiku_syncobj** syncobjs;
	int error = lookup_array(file, args->handles, 0, args->count_handles,
		&syncobjs, NULL);
	if (error != 0)
		return error;
	u64* points = (u64*)kcalloc(args->count_handles, sizeof(u64),
		GFP_KERNEL);
	if (points == NULL) {
		put_syncobjs(syncobjs, args->count_handles);
		return -ENOMEM;
	}

	mutex_lock(&sSyncLock);
	for (u32 i = 0; i < args->count_handles; i++) {
		collect(syncobjs[i]);
		points[i] = (args->flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
			!= 0 ? last_point(syncobjs[i]) : syncobjs[i]->completed;
	}
	mutex_unlock(&sSyncLock);

	if (copy_to_user(u64_to_user_ptr(args->points), points,
			args->count_handles * sizeof(u64)) != 0) {
		error = -EFAULT;
	}
	kfree(points);
	put_syncobjs(syncobjs, args->count_handles);
	return error;
}


/*!	Linux's drm_syncobj_find_fence(), with the object already looked up:
	waits up to 10 s for a fence to be submitted when \a flags say so.
*/
static int
find_fence(struct pvr_haiku_syncobj* syncobj, u64 point, u32 flags,
	struct dma_fence** _fence)
{
	bigtime_t deadline = system_time() + FIND_FENCE_SUBMIT_TIMEOUT;
	for (;;) {
		int32 generation = lx_wait_queue_generation(&lx_fence_queue);
		int error;
		mutex_lock(&sSyncLock);
		struct dma_fence* fence = fence_for(syncobj, point, &error);
		mutex_unlock(&sSyncLock);
		if (fence != NULL) {
			*_fence = fence;
			return 0;
		}
		if (error != 0)
			return error;
		if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) == 0)
			return -EINVAL;
		if (system_time() >= deadline)
			return -ETIME;
		if (lx_wait_queue_sleep_interruptible(&lx_fence_queue, generation,
				deadline) == B_INTERRUPTED) {
			return -EINTR;
		}
	}
}


int
pvr_haiku_syncobj_transfer(struct pvr_haiku_file* file,
	struct drm_syncobj_transfer* args)
{
	if (args->pad != 0
		|| (args->flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0) {
		return -EINVAL;
	}

	struct pvr_haiku_syncobj* source = syncobj_lookup(file, args->src_handle);
	struct pvr_haiku_syncobj* target = syncobj_lookup(file, args->dst_handle);
	int error = source == NULL || target == NULL ? -ENOENT : 0;

	struct dma_fence* fence = NULL;
	if (error == 0)
		error = find_fence(source, args->src_point, args->flags, &fence);
	if (error == 0) {
		mutex_lock(&sSyncLock);
		if (args->dst_point != 0)
			error = add_point(target, args->dst_point, fence);
		else
			error = replace_fence(target, fence);
		mutex_unlock(&sSyncLock);
		wake_up_all(&lx_fence_queue);
	}

	dma_fence_put(fence);
	syncobj_put(source);
	syncobj_put(target);
	return error;
}


/* #pragma mark - for job submission */


int
pvr_haiku_syncobj_find_fence(struct pvr_haiku_file* file, u32 handle,
	u64 point, struct dma_fence** _fence)
{
	struct pvr_haiku_syncobj* syncobj = syncobj_lookup(file, handle);
	if (syncobj == NULL)
		return -ENOENT;
	int error = find_fence(syncobj, point, 0, _fence);
	syncobj_put(syncobj);
	return error;
}


int
pvr_haiku_syncobj_add_fence(struct pvr_haiku_file* file, u32 handle,
	u64 point, struct dma_fence* fence)
{
	struct pvr_haiku_syncobj* syncobj = syncobj_lookup(file, handle);
	if (syncobj == NULL)
		return -ENOENT;
	mutex_lock(&sSyncLock);
	int error = point != 0 ? add_point(syncobj, point, fence)
		: replace_fence(syncobj, fence);
	mutex_unlock(&sSyncLock);
	wake_up_all(&lx_fence_queue);
	syncobj_put(syncobj);
	return error;
}
