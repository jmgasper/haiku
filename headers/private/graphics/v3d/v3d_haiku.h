/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _V3D_HAIKU_H
#define _V3D_HAIKU_H


/*	The interface of the V3D render device (/dev/graphics/v3d/0).

	The requests carry the structures of Linux' DRM V3D interface
	(v3d_drm.h, MIT licensed, part of Mesa's sources), so Mesa's v3d and v3dv
	drivers need no translation. What differs:

	- Buffers are mapped by the driver: V3D_HAIKU_MMAP_BO clones the buffer
	  into the calling team and returns the address in drm_v3d_mmap_bo's
	  "offset". Unmap with delete_area(area_for(address)).
	- There are no DRM sync objects or sync files. Jobs run strictly in the
	  order they were submitted and every job has a sequence number; a
	  "sync object" here just remembers the number of the last job it was
	  given as out_sync. in_sync handles are accepted and ignored: the order
	  already guarantees what they ask for.
	- No PRIME/dma-buf. */


#include <Drivers.h>
#include <SupportDefs.h>


#define V3D_HAIKU_DEVICE_PATH	"/dev/graphics/v3d/0"

enum {
	V3D_HAIKU_FIRST_OP = B_DEVICE_OP_CODES_END + 0x7600,

	// struct drm_v3d_*
	V3D_HAIKU_GET_PARAM = V3D_HAIKU_FIRST_OP,
	V3D_HAIKU_CREATE_BO,
	V3D_HAIKU_MMAP_BO,
	V3D_HAIKU_GET_BO_OFFSET,
	V3D_HAIKU_WAIT_BO,
	V3D_HAIKU_SUBMIT_CL,
	V3D_HAIKU_SUBMIT_TFU,
	V3D_HAIKU_SUBMIT_CSD,

	// struct v3d_haiku_handle
	V3D_HAIKU_CLOSE_BO,
	V3D_HAIKU_SYNC_CREATE,
	V3D_HAIKU_SYNC_DESTROY,

	// struct v3d_haiku_sync
	V3D_HAIKU_SYNC_WAIT,
		// waits for the job a sync object stands for
	V3D_HAIKU_SYNC_GET,
		// returns that job's sequence number
	V3D_HAIKU_SEQNO_WAIT,
		// waits for a job by its sequence number
};

// As the handle of V3D_HAIKU_SEQNO_WAIT: "seqno" is a fence token, the low
// 31 bits of the sequence number of a job submitted earlier.
#define V3D_HAIKU_TOKEN	0xffffffffu

struct v3d_haiku_handle {
	uint32	handle;
	uint32	pad;
};

struct v3d_haiku_sync {
	uint32	handle;
	uint32	pad;
	uint64	seqno;
	int64	timeout_ns;
		// relative; negative waits for good, 0 only asks
};


#endif	/* _V3D_HAIKU_H */
