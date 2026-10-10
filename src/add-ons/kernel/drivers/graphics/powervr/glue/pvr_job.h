/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PVR_JOB_H
#define PVR_JOB_H


/*	Haiku's replacement for drm/imagination's pvr_job.h (drm_sched jobs):
	SUBMIT_JOBS on the driver's own queues (pvr_haiku_job.c). */


#include <uapi/drm/pvr_drm.h>

#include <linux/types.h>

#include "pvr_power.h"


struct pvr_device;
struct pvr_file;


int		pvr_submit_jobs(struct pvr_device* pvr_dev, struct pvr_file* pvr_file,
			struct drm_pvr_ioctl_submit_jobs_args* args);


#endif	/* PVR_JOB_H */
