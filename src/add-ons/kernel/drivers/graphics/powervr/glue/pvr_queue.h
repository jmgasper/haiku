/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PVR_QUEUE_H
#define PVR_QUEUE_H


/*	Haiku's replacement for drm/imagination's pvr_queue.h, which is built
	on drm_sched: the same functions for pvr_context.c and the device code,
	on the driver's own job queues (pvr_haiku_queue.c). */


#include "pvr_cccb.h"
#include "pvr_device.h"

#include <uapi/drm/pvr_drm.h>


struct pvr_context;
struct pvr_job;
struct pvr_queue;


struct pvr_queue*	pvr_queue_create(struct pvr_context* ctx,
						enum drm_pvr_job_type type,
						struct drm_pvr_ioctl_create_context_args* args,
						void* fw_ctx_map);
void				pvr_queue_kill(struct pvr_queue* queue);
void				pvr_queue_destroy(struct pvr_queue* queue,
						bool cleanup_queue_entity);
void				pvr_queue_process(struct pvr_queue* queue);
void				pvr_queue_device_pre_reset(struct pvr_device* pvr_dev);
void				pvr_queue_device_post_reset(struct pvr_device* pvr_dev);
int					pvr_queue_device_init(struct pvr_device* pvr_dev);
void				pvr_queue_device_fini(struct pvr_device* pvr_dev);


#endif	/* PVR_QUEUE_H */
