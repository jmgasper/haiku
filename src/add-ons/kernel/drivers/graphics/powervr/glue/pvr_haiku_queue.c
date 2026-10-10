/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	pvr_queue.h and pvr_job.h on Haiku, first part: no job queues yet. A
	context cannot be created (its queues fail with EOPNOTSUPP) and jobs
	cannot be submitted, which is enough for Mesa to find the device and
	read its properties. The queues themselves are the next step. */


#include "pvr_context.h"
#include "pvr_device.h"
#include "pvr_job.h"
#include "pvr_queue.h"


struct pvr_queue*
pvr_queue_create(struct pvr_context* ctx, enum drm_pvr_job_type type,
	struct drm_pvr_ioctl_create_context_args* args, void* fw_ctx_map)
{
	(void)type;
	(void)args;
	(void)fw_ctx_map;
	drm_info(from_pvr_device(ctx->pvr_dev), "no job queues yet\n");
	return ERR_PTR(-EOPNOTSUPP);
}


void
pvr_queue_kill(struct pvr_queue* queue)
{
	(void)queue;
}


void
pvr_queue_destroy(struct pvr_queue* queue, bool cleanup_queue_entity)
{
	(void)queue;
	(void)cleanup_queue_entity;
}


void
pvr_queue_process(struct pvr_queue* queue)
{
	(void)queue;
}


void
pvr_queue_device_pre_reset(struct pvr_device* pvr_dev)
{
	(void)pvr_dev;
}


void
pvr_queue_device_post_reset(struct pvr_device* pvr_dev)
{
	(void)pvr_dev;
}


int
pvr_queue_device_init(struct pvr_device* pvr_dev)
{
	(void)pvr_dev;
	return 0;
}


void
pvr_queue_device_fini(struct pvr_device* pvr_dev)
{
	(void)pvr_dev;
}


int
pvr_submit_jobs(struct pvr_device* pvr_dev, struct pvr_file* pvr_file,
	struct drm_pvr_ioctl_submit_jobs_args* args)
{
	(void)pvr_dev;
	(void)pvr_file;
	(void)args;
	return -EOPNOTSUPP;
}
