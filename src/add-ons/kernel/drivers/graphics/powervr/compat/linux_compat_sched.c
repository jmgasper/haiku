/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	linux_compat_sched.h: work queues, reservation objects, drm_exec and the
	GPU scheduler, as far as drm/imagination's job code uses them. The
	scheduler follows drm_sched's rules where pvr_queue.c depends on them:

	- dependencies stay in the job's xarray once waited for (pvr_queue.c
	  turns the native ones into firmware waits when the job runs);
	- a dependency on a job of the same scheduler only waits until that
	  job has been handed to the hardware ("scheduled"), one on the same
	  entity not at all;
	- prepare_job() is asked for more fences until it has none;
	- a job enters the pending list before run_job(), and leaves it, to be
	  freed, once its finished fence signaled, oldest first.

	The hardware fence's callback runs where the fence is signaled, which
	in pvr_queue.c is under the pending list's lock; the free work takes
	that lock too, so a job is never freed under a callback still using
	it (as on Linux). */


#include "linux_compat.h"


#define TRACE(x...)	dprintf("powervr: " x)


/* #pragma mark - work queues */


struct workqueue_struct {
	char				name[B_OS_NAME_LENGTH];
	spinlock_t			lock;
	struct list_head	pending;
	struct work_struct*	running;	/* compared only, never followed */
	wait_queue_head_t	wake;		/* new work, or an item done */
	thread_id			thread;
	bool				quit;
};


static status_t
workqueue_thread(void* data)
{
	struct workqueue_struct* queue = (struct workqueue_struct*)data;

	for (;;) {
		int32 generation = lx_wait_queue_generation(&queue->wake);

		spin_lock(&queue->lock);
		if (list_empty(&queue->pending)) {
			bool quit = queue->quit;
			spin_unlock(&queue->lock);
			if (quit)
				return B_OK;
			lx_wait_queue_sleep(&queue->wake, generation,
				B_INFINITE_TIMEOUT);
			continue;
		}

		struct work_struct* work = list_first_entry(&queue->pending,
			struct work_struct, entry);
		list_del_init(&work->entry);
		work->queue = NULL;
		queue->running = work;
		spin_unlock(&queue->lock);

		// the item may free itself
		work->func(work);

		spin_lock(&queue->lock);
		queue->running = NULL;
		spin_unlock(&queue->lock);
		wake_up_all(&queue->wake);
	}
}


struct workqueue_struct*
alloc_workqueue(const char* name, unsigned int flags, int maxActive, ...)
{
	(void)flags;
	(void)maxActive;

	struct workqueue_struct* queue = (struct workqueue_struct*)kzalloc(
		sizeof(*queue), GFP_KERNEL);
	if (queue == NULL)
		return NULL;

	strlcpy(queue->name, name, sizeof(queue->name));
	spin_lock_init(&queue->lock);
	INIT_LIST_HEAD(&queue->pending);
	init_waitqueue_head(&queue->wake);
	queue->thread = spawn_kernel_thread(workqueue_thread, queue->name,
		B_URGENT_DISPLAY_PRIORITY, queue);
	if (queue->thread < 0) {
		kfree(queue);
		return NULL;
	}
	resume_thread(queue->thread);
	return queue;
}


void
destroy_workqueue(struct workqueue_struct* queue)
{
	if (queue == NULL)
		return;

	// what is queued still runs, as on Linux
	spin_lock(&queue->lock);
	queue->quit = true;
	spin_unlock(&queue->lock);
	wake_up_all(&queue->wake);

	status_t result;
	wait_for_thread(queue->thread, &result);
	mutex_destroy(&queue->lock.lock);
	kfree(queue);
}


bool
queue_work(struct workqueue_struct* queue, struct work_struct* work)
{
	spin_lock(&queue->lock);
	if (work->queue != NULL) {
		spin_unlock(&queue->lock);
		return false;
	}
	work->queue = queue;
	list_add_tail(&work->entry, &queue->pending);
	spin_unlock(&queue->lock);

	wake_up_all(&queue->wake);
	return true;
}


/*!	Only for the items of \a queue: Linux's version finds the queue itself.
*/
static bool
cancel_work_on(struct workqueue_struct* queue, struct work_struct* work)
{
	bool wasPending = false;

	for (;;) {
		int32 generation = lx_wait_queue_generation(&queue->wake);
		spin_lock(&queue->lock);
		if (work->queue == queue) {
			list_del_init(&work->entry);
			work->queue = NULL;
			wasPending = true;
		}
		bool running = queue->running == work;
		spin_unlock(&queue->lock);

		// an item cancelling itself does not wait for itself
		if (!running || find_thread(NULL) == queue->thread)
			return wasPending;
		lx_wait_queue_sleep(&queue->wake, generation, B_INFINITE_TIMEOUT);
	}
}


bool
cancel_work_sync(struct work_struct* work)
{
	struct workqueue_struct* queue = work->queue;
	if (queue == NULL)
		return false;
	return cancel_work_on(queue, work);
}


void
flush_workqueue(struct workqueue_struct* queue)
{
	if (find_thread(NULL) == queue->thread)
		return;

	for (;;) {
		int32 generation = lx_wait_queue_generation(&queue->wake);
		spin_lock(&queue->lock);
		bool idle = list_empty(&queue->pending) && queue->running == NULL;
		spin_unlock(&queue->lock);
		if (idle)
			return;
		lx_wait_queue_sleep(&queue->wake, generation, B_INFINITE_TIMEOUT);
	}
}


/* #pragma mark - reservation objects */


struct lx_resv_fence {
	struct list_head		link;
	struct dma_fence*		fence;
	enum dma_resv_usage		usage;
};


void
dma_resv_init(struct dma_resv* resv)
{
	(mutex_init)(&resv->lock, "powervr dma_resv");
	INIT_LIST_HEAD(&resv->fences);
}


void
dma_resv_fini(struct dma_resv* resv)
{
	struct lx_resv_fence* entry;
	struct lx_resv_fence* next;
	list_for_each_entry_safe(entry, next, &resv->fences, link) {
		list_del(&entry->link);
		dma_fence_put(entry->fence);
		kfree(entry);
	}
	mutex_destroy(&resv->lock);
}


int
dma_resv_lock(struct dma_resv* resv, void* context)
{
	(void)context;
	mutex_lock(&resv->lock);
	return 0;
}


void
dma_resv_unlock(struct dma_resv* resv)
{
	mutex_unlock(&resv->lock);
}


int
dma_resv_reserve_fences(struct dma_resv* resv, unsigned int count)
{
	(void)resv;
	(void)count;
	return 0;
}


/*!	Linux's rule: a fence replaces a signaled one, or one of its own
	context with the same or a weaker use; otherwise it is added. Called
	with the submission lock (drm_exec) held.
*/
void
dma_resv_add_fence(struct dma_resv* resv, struct dma_fence* fence,
	enum dma_resv_usage usage)
{
	struct lx_resv_fence* entry;
	list_for_each_entry(entry, &resv->fences, link) {
		struct dma_fence* old = entry->fence;
		if ((old->context == fence->context && entry->usage >= usage
				&& fence->seqno >= old->seqno)
			|| dma_fence_is_signaled(old)) {
			entry->fence = dma_fence_get(fence);
			entry->usage = usage;
			dma_fence_put(old);
			return;
		}
	}

	entry = (struct lx_resv_fence*)kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL) {
		// Linux reserves first; waiting here keeps the order instead
		lx_dma_fence_wait(fence, false, B_INFINITE_TIMEOUT);
		return;
	}
	entry->fence = dma_fence_get(fence);
	entry->usage = usage;
	list_add_tail(&entry->link, &resv->fences);
}


/* #pragma mark - drm_exec */


static mutex sExecLock = MUTEX_INITIALIZER("powervr submission");


void
drm_exec_init(struct drm_exec* exec, u32 flags, unsigned int count)
{
	(void)count;
	exec->flags = flags;
	exec->once = false;
	mutex_lock(&sExecLock);
	exec->locked = true;
}


void
drm_exec_fini(struct drm_exec* exec)
{
	if (exec->locked) {
		exec->locked = false;
		mutex_unlock(&sExecLock);
	}
}


int
drm_exec_lock_obj(struct drm_exec* exec, struct drm_gem_object* object)
{
	(void)exec;
	(void)object;
	return 0;
}


int
drm_exec_prepare_obj(struct drm_exec* exec, struct drm_gem_object* object,
	unsigned int fenceCount)
{
	(void)exec;
	(void)object;
	(void)fenceCount;
	return 0;
}


/* #pragma mark - scheduler fences */


static const char*
sched_fence_driver_name(struct dma_fence* fence)
{
	(void)fence;
	return "drm_sched";
}


static const char*
sched_fence_timeline_name(struct dma_fence* fence)
{
	struct drm_sched_fence* s_fence = to_drm_sched_fence(fence);
	return s_fence != NULL && s_fence->sched != NULL
		? s_fence->sched->name : "drm_sched";
}


static void
sched_fence_release_scheduled(struct dma_fence* fence)
{
	struct drm_sched_fence* s_fence = container_of(fence,
		struct drm_sched_fence, scheduled);
	dma_fence_put(s_fence->parent);
	kfree(s_fence);
}


static void
sched_fence_release_finished(struct dma_fence* fence)
{
	struct drm_sched_fence* s_fence = container_of(fence,
		struct drm_sched_fence, finished);
	// the finished fence holds the scheduled one, which owns the memory
	dma_fence_put(&s_fence->scheduled);
}


static const struct dma_fence_ops sSchedFenceScheduledOps = {
	.get_driver_name = sched_fence_driver_name,
	.get_timeline_name = sched_fence_timeline_name,
	.release = sched_fence_release_scheduled,
};

static const struct dma_fence_ops sSchedFenceFinishedOps = {
	.get_driver_name = sched_fence_driver_name,
	.get_timeline_name = sched_fence_timeline_name,
	.release = sched_fence_release_finished,
};


struct drm_sched_fence*
to_drm_sched_fence(struct dma_fence* fence)
{
	if (fence->ops == &sSchedFenceScheduledOps)
		return container_of(fence, struct drm_sched_fence, scheduled);
	if (fence->ops == &sSchedFenceFinishedOps)
		return container_of(fence, struct drm_sched_fence, finished);
	return NULL;
}


/* #pragma mark - jobs */


static void sched_job_done(struct drm_sched_job* job, int result);


int
drm_sched_job_init(struct drm_sched_job* job, struct drm_sched_entity* entity,
	u32 credits, void* owner, u64 drmClientID)
{
	if (entity == NULL || entity->sched == NULL)
		return -ENOENT;
	if (credits == 0)
		return -EINVAL;

	memset(job, 0, sizeof(*job));
	job->s_fence = (struct drm_sched_fence*)kzalloc(sizeof(*job->s_fence),
		GFP_KERNEL);
	if (job->s_fence == NULL)
		return -ENOMEM;
	job->s_fence->owner = owner;

	job->entity = entity;
	job->credits = credits;
	job->drm_client_id = drmClientID;
	INIT_LIST_HEAD(&job->list);
	INIT_LIST_HEAD(&job->queue_link);
	INIT_LIST_HEAD(&job->cb.node);
	xa_init_flags(&job->dependencies, XA_FLAGS_ALLOC);
	return 0;
}


void
drm_sched_job_arm(struct drm_sched_job* job)
{
	struct drm_sched_entity* entity = job->entity;
	struct drm_sched_fence* s_fence = job->s_fence;
	u64 seqno = (u64)atomic_inc_return(&entity->fence_seq);

	job->sched = entity->sched;
	s_fence->sched = entity->sched;
	dma_fence_init(&s_fence->scheduled, &sSchedFenceScheduledOps, NULL,
		entity->fence_context, seqno);
	dma_fence_init(&s_fence->finished, &sSchedFenceFinishedOps, NULL,
		entity->fence_context + 1, seqno);
}


int
drm_sched_job_add_dependency(struct drm_sched_job* job,
	struct dma_fence* fence)
{
	if (fence == NULL)
		return 0;

	// one fence per context is enough: keep the later one
	unsigned long index;
	struct dma_fence* entry;
	xa_for_each(&job->dependencies, index, entry) {
		if (entry->context != fence->context)
			continue;
		if (fence->seqno > entry->seqno) {
			xa_store(&job->dependencies, index, fence, GFP_KERNEL);
			dma_fence_put(entry);
		} else
			dma_fence_put(fence);
		return 0;
	}

	u32 id;
	int error = xa_alloc(&job->dependencies, &id, fence, xa_limit_32b,
		GFP_KERNEL);
	if (error != 0)
		dma_fence_put(fence);
	return error;
}


int
drm_sched_job_add_resv_dependencies(struct drm_sched_job* job,
	struct dma_resv* resv, enum dma_resv_usage usage)
{
	struct lx_resv_fence* entry;
	list_for_each_entry(entry, &resv->fences, link) {
		if (entry->usage > usage)
			continue;
		int error = drm_sched_job_add_dependency(job,
			dma_fence_get(entry->fence));
		if (error != 0)
			return error;
	}
	return 0;
}


bool
drm_sched_job_has_dependency(struct drm_sched_job* job,
	struct dma_fence* fence)
{
	unsigned long index;
	struct dma_fence* entry;
	xa_for_each(&job->dependencies, index, entry) {
		if (entry == fence)
			return true;
	}
	return false;
}


void
drm_sched_job_cleanup(struct drm_sched_job* job)
{
	struct drm_sched_fence* s_fence = job->s_fence;
	if (s_fence != NULL) {
		if (s_fence->finished.ops != NULL)
			dma_fence_put(&s_fence->finished);
		else
			kfree(s_fence);		// never armed
		job->s_fence = NULL;
	}

	unsigned long index;
	struct dma_fence* fence;
	xa_for_each(&job->dependencies, index, fence)
		dma_fence_put(fence);
	xa_destroy(&job->dependencies);
}


/*!	Signals a job that never ran: scheduled, then finished with \a error;
	then the driver frees it.
*/
static void
sched_job_kill(struct drm_sched_job* job, int error)
{
	struct drm_sched_fence* s_fence = job->s_fence;
	if (!dma_fence_is_signaled(&s_fence->scheduled))
		dma_fence_signal(&s_fence->scheduled);
	if (s_fence->finished.error == 0)
		dma_fence_set_error(&s_fence->finished, error);
	dma_fence_signal(&s_fence->finished);
	job->sched->ops->free_job(job);
}


/* #pragma mark - running jobs */


static void
sched_queue_run(struct drm_gpu_scheduler* sched)
{
	if (!sched->pause_submit)
		queue_work(sched->submit_wq, &sched->work_run_job);
}


static void
sched_job_done(struct drm_sched_job* job, int result)
{
	struct drm_gpu_scheduler* sched = job->sched;
	struct drm_sched_fence* s_fence = job->s_fence;

	atomic_sub(job->credits, &sched->credit_count);
	if (result != 0 && s_fence->finished.error == 0)
		dma_fence_set_error(&s_fence->finished, result);
	dma_fence_get(&s_fence->finished);
	dma_fence_signal(&s_fence->finished);
	dma_fence_put(&s_fence->finished);

	queue_work(sched->submit_wq, &sched->work_free_job);
	sched_queue_run(sched);
}


static void
sched_job_done_callback(struct dma_fence* fence, struct dma_fence_cb* cb)
{
	struct drm_sched_job* job = container_of(cb, struct drm_sched_job, cb);
	sched_job_done(job, fence->error);
}


static void
entity_dependency_callback(struct dma_fence* fence, struct dma_fence_cb* cb)
{
	struct drm_sched_entity* entity = container_of(cb,
		struct drm_sched_entity, cb);
	struct drm_gpu_scheduler* sched = entity->sched;

	spin_lock(&entity->lock);
	if (entity->dependency == fence)
		entity->dependency = NULL;
	spin_unlock(&entity->lock);
	dma_fence_put(fence);

	if (sched != NULL)
		sched_queue_run(sched);
}


/*!	drm_sched_entity_add_dependency_cb(): true if the entity now waits for
	\a fence (a reference the entity keeps until the callback), false if
	there is nothing to wait for (and the reference is dropped).
*/
static bool
entity_wait_for(struct drm_sched_entity* entity, struct dma_fence* fence)
{
	struct drm_gpu_scheduler* sched = entity->sched;

	// our own fences are in order already
	if (fence->context == entity->fence_context
		|| fence->context == entity->fence_context + 1) {
		dma_fence_put(fence);
		return false;
	}

	// a job of this scheduler only has to have been handed on
	struct drm_sched_fence* s_fence = to_drm_sched_fence(fence);
	if (fence->error == 0 && s_fence != NULL && s_fence->sched == sched) {
		struct dma_fence* scheduled = dma_fence_get(&s_fence->scheduled);
		dma_fence_put(fence);
		fence = scheduled;
	}

	if (dma_fence_is_signaled(fence)) {
		dma_fence_put(fence);
		return false;
	}

	spin_lock(&entity->lock);
	entity->dependency = fence;
	spin_unlock(&entity->lock);
	if (dma_fence_add_callback(fence, &entity->cb,
			entity_dependency_callback) == 0) {
		return true;
	}

	spin_lock(&entity->lock);
	entity->dependency = NULL;
	spin_unlock(&entity->lock);
	dma_fence_put(fence);
	return false;
}


/*!	drm_sched_job_dependency(): the next dependency (a new reference), or
	the driver's next internal one once those are done.
*/
static struct dma_fence*
job_next_dependency(struct drm_sched_job* job, struct drm_sched_entity* entity)
{
	struct dma_fence* fence = (struct dma_fence*)xa_load(&job->dependencies,
		job->last_dependency);
	if (fence == NULL) {
		// xa_alloc() IDs may have holes after xa_store(); find the next
		unsigned long index = job->last_dependency;
		fence = (struct dma_fence*)lx_xa_find(&job->dependencies, &index);
		if (fence != NULL)
			job->last_dependency = index;
	}
	if (fence != NULL) {
		job->last_dependency++;
		return dma_fence_get(fence);
	}

	if (job->sched->ops->prepare_job != NULL)
		return job->sched->ops->prepare_job(job, entity);
	return NULL;
}


static void
sched_run_job(struct drm_gpu_scheduler* sched, struct drm_sched_entity* entity,
	struct drm_sched_job* job)
{
	struct drm_sched_fence* s_fence = job->s_fence;

	atomic_add(job->credits, &sched->credit_count);
	spin_lock(&sched->job_list_lock);
	list_add_tail(&job->list, &sched->pending_list);
	spin_unlock(&sched->job_list_lock);

	if (entity->guilty != NULL && atomic_read(entity->guilty) != 0)
		dma_fence_set_error(&s_fence->finished, -ECANCELED);

	// drm_sched_fence_scheduled(): the hardware fence is in place before
	// "scheduled" signals, since a job of another queue waiting for that
	// then waits for the hardware fence in the firmware
	struct dma_fence* fence = sched->ops->run_job(job);
	if (!IS_ERR_OR_NULL(fence))
		s_fence->parent = dma_fence_get(fence);
	dma_fence_signal(&s_fence->scheduled);

	if (IS_ERR_OR_NULL(fence)) {
		sched_job_done(job, IS_ERR(fence) ? PTR_ERR(fence) : 0);
		return;
	}
	if (dma_fence_add_callback(fence, &job->cb, sched_job_done_callback)
			== -ENOENT) {
		sched_job_done(job, fence->error);
	}
	dma_fence_put(fence);
}


static void
sched_run_work(struct work_struct* work)
{
	struct drm_gpu_scheduler* sched = container_of(work,
		struct drm_gpu_scheduler, work_run_job);

	for (;;) {
		struct drm_sched_entity* entity = sched->entity;
		if (entity == NULL || sched->pause_submit)
			return;

		spin_lock(&entity->lock);
		struct drm_sched_job* job = list_first_entry_or_null(
			&entity->job_queue, struct drm_sched_job, queue_link);
		bool waiting = entity->dependency != NULL;
		spin_unlock(&entity->lock);
		if (job == NULL || waiting)
			return;

		struct dma_fence* dependency;
		while ((dependency = job_next_dependency(job, entity)) != NULL) {
			if (entity_wait_for(entity, dependency))
				return;
		}

		if ((u32)atomic_read(&sched->credit_count) + job->credits
				> sched->credit_limit) {
			return;		// a finished job will kick us again
		}

		spin_lock(&entity->lock);
		list_del_init(&job->queue_link);
		spin_unlock(&entity->lock);

		sched_run_job(sched, entity, job);
	}
}


/*!	Frees the oldest finished job. The driver's free_job() may drop the
	last reference to what holds the scheduler, so nothing is touched
	after it.
*/
static void
sched_free_work(struct work_struct* work)
{
	struct drm_gpu_scheduler* sched = container_of(work,
		struct drm_gpu_scheduler, work_free_job);

	spin_lock(&sched->job_list_lock);
	struct drm_sched_job* job = list_first_entry_or_null(&sched->pending_list,
		struct drm_sched_job, list);
	bool more = false;
	if (job != NULL && dma_fence_is_signaled(&job->s_fence->finished)) {
		list_del_init(&job->list);
		struct drm_sched_job* next = list_first_entry_or_null(
			&sched->pending_list, struct drm_sched_job, list);
		more = next != NULL
			&& dma_fence_is_signaled(&next->s_fence->finished);
	} else
		job = NULL;
	spin_unlock(&sched->job_list_lock);

	if (job == NULL)
		return;
	if (more)
		queue_work(sched->submit_wq, &sched->work_free_job);
	sched_queue_run(sched);
	sched->ops->free_job(job);
}


/* #pragma mark - schedulers and entities */


int
drm_sched_init(struct drm_gpu_scheduler* sched,
	const struct drm_sched_init_args* args)
{
	if (args->submit_wq == NULL)
		return -EINVAL;

	memset(sched, 0, sizeof(*sched));
	sched->ops = args->ops;
	sched->dev = args->dev;
	sched->name = args->name;
	sched->credit_limit = args->credit_limit;
	atomic_set(&sched->credit_count, 0);
	sched->submit_wq = args->submit_wq;
	INIT_WORK(&sched->work_run_job, sched_run_work);
	INIT_WORK(&sched->work_free_job, sched_free_work);
	INIT_LIST_HEAD(&sched->pending_list);
	spin_lock_init(&sched->job_list_lock);
	sched->ready = true;
	return 0;
}


void
drm_sched_fini(struct drm_gpu_scheduler* sched)
{
	sched->pause_submit = true;
	cancel_work_on(sched->submit_wq, &sched->work_run_job);
	cancel_work_on(sched->submit_wq, &sched->work_free_job);

	spin_lock(&sched->job_list_lock);
	bool busy = !list_empty(&sched->pending_list);
	spin_unlock(&sched->job_list_lock);
	if (busy) {
		// as on Linux without cancel_job(): the jobs are leaked
		TRACE("scheduler %s destroyed with jobs pending\n", sched->name);
	}
	sched->ready = false;
	mutex_destroy(&sched->job_list_lock.lock);
}


/*!	Stops handing on jobs and detaches the pending ones from their hardware
	fences (for a reset); drm_sched_start() attaches them again.
*/
void
drm_sched_stop(struct drm_gpu_scheduler* sched, struct drm_sched_job* bad)
{
	(void)bad;
	sched->pause_submit = true;
	cancel_work_on(sched->submit_wq, &sched->work_run_job);

	struct drm_sched_job* job;
	list_for_each_entry(job, &sched->pending_list, list) {
		struct dma_fence* parent = job->s_fence->parent;
		if (parent != NULL && dma_fence_remove_callback(parent, &job->cb)) {
			dma_fence_put(parent);
			job->s_fence->parent = NULL;
			atomic_sub(job->credits, &sched->credit_count);
		}
	}
}


void
drm_sched_start(struct drm_gpu_scheduler* sched, int error)
{
	struct drm_sched_job* job;
	struct drm_sched_job* next;
	list_for_each_entry_safe(job, next, &sched->pending_list, list) {
		struct dma_fence* parent = job->s_fence->parent;
		if (dma_fence_is_signaled(&job->s_fence->finished))
			continue;
		atomic_add(job->credits, &sched->credit_count);
		if (parent == NULL) {
			sched_job_done(job, error != 0 ? error : -ECANCELED);
			continue;
		}
		if (dma_fence_add_callback(parent, &job->cb,
				sched_job_done_callback) != 0) {
			sched_job_done(job, parent->error != 0 ? parent->error : error);
		}
	}

	sched->pause_submit = false;
	sched_queue_run(sched);
}


int
drm_sched_entity_init(struct drm_sched_entity* entity,
	enum drm_sched_priority priority, struct drm_gpu_scheduler** schedList,
	unsigned int schedCount, atomic_t* guilty)
{
	(void)priority;
	if (schedList == NULL || schedCount != 1 || schedList[0] == NULL)
		return -EINVAL;

	memset(entity, 0, sizeof(*entity));
	entity->sched = schedList[0];
	spin_lock_init(&entity->lock);
	INIT_LIST_HEAD(&entity->job_queue);
	INIT_LIST_HEAD(&entity->cb.node);
	entity->fence_context = dma_fence_context_alloc(2);
	atomic_set(&entity->fence_seq, 0);
	entity->guilty = guilty;
	entity->sched->entity = entity;
	return 0;
}


void
drm_sched_entity_push_job(struct drm_sched_job* job)
{
	struct drm_sched_entity* entity = job->entity;

	spin_lock(&entity->lock);
	bool stopped = entity->stopped;
	if (!stopped)
		list_add_tail(&job->queue_link, &entity->job_queue);
	spin_unlock(&entity->lock);

	if (stopped) {
		TRACE("job pushed to a stopped entity\n");
		sched_job_kill(job, -ENOENT);
		return;
	}
	sched_queue_run(entity->sched);
}


/*!	drm_sched_entity_flush() and drm_sched_entity_fini(): gives the queued
	jobs up to a second to be handed on, then kills the rest.
*/
void
drm_sched_entity_destroy(struct drm_sched_entity* entity)
{
	bigtime_t deadline = system_time() + 1000000;
	for (;;) {
		spin_lock(&entity->lock);
		bool empty = list_empty(&entity->job_queue);
		spin_unlock(&entity->lock);
		if (empty || system_time() >= deadline)
			break;
		snooze(1000);
	}
	drm_sched_entity_fini(entity);
}


void
drm_sched_entity_fini(struct drm_sched_entity* entity)
{
	struct drm_gpu_scheduler* sched = entity->sched;
	if (sched == NULL)
		return;

	spin_lock(&entity->lock);
	entity->stopped = true;
	struct dma_fence* dependency = entity->dependency;
	spin_unlock(&entity->lock);

	if (dependency != NULL
		&& dma_fence_remove_callback(dependency, &entity->cb)) {
		spin_lock(&entity->lock);
		entity->dependency = NULL;
		spin_unlock(&entity->lock);
		dma_fence_put(dependency);
	}

	// no new runs from here on (the run work checks sched->entity)
	cancel_work_on(sched->submit_wq, &sched->work_run_job);
	if (sched->entity == entity)
		sched->entity = NULL;

	for (;;) {
		spin_lock(&entity->lock);
		struct drm_sched_job* job = list_first_entry_or_null(
			&entity->job_queue, struct drm_sched_job, queue_link);
		if (job != NULL)
			list_del_init(&job->queue_link);
		spin_unlock(&entity->lock);
		if (job == NULL)
			break;
		job->sched = sched;
		sched_job_kill(job, -ESRCH);
	}
	entity->sched = NULL;
}
