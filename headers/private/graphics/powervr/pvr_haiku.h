/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _PVR_HAIKU_H
#define _PVR_HAIKU_H


/*	The PowerVR (Imagination Rogue) driver's interface to userland: Linux's
	pvr_drm.h and drm.h structures, unchanged, behind Haiku ioctl op codes
	PVR_HAIKU_OP(nr), nr being the Linux DRM ioctl number (driver ioctls
	0x40-0x4d, generic DRM ioctls keep theirs). The length argument of
	ioctl() is the structure's size. Haiku-only ops use nr 0xe0-0xef. */


#include <SupportDefs.h>


#define PVR_HAIKU_DEVICE_PATH		"/dev/graphics/powervr/0"
#define PVR_HAIKU_DEVICE_ENV		"HAIKU_PVR_DEVICE"

// 'P' 'V' 'R' nn
#define PVR_HAIKU_OP_BASE			0x50565200u
#define PVR_HAIKU_OP(nr)			(PVR_HAIKU_OP_BASE | ((nr) & 0xffu))
#define PVR_HAIKU_OP_NR(op)			((op) & 0xffu)
#define PVR_HAIKU_IS_OP(op)			(((op) & 0xffffff00u) == PVR_HAIKU_OP_BASE)

#define PVR_HAIKU_NR_STAGE			0xef	// bring-up state

#define PVR_HAIKU_ABI_VERSION		1


enum {
	PVR_HAIKU_STAGE_OFF = 0,		// disabled by the driver settings
	PVR_HAIKU_STAGE_POWERED,		// clocks, resets, power domains on
	PVR_HAIKU_STAGE_IDENTIFIED,		// the core's BVNC read
	PVR_HAIKU_STAGE_FIRMWARE		// the firmware runs
};

struct pvr_haiku_stage {
	uint32	version;				// PVR_HAIKU_ABI_VERSION
	uint32	stage;
	uint64	bvnc;					// packed 16:16:16:16
	uint32	core_id;
	uint32	core_clock;				// Hz
	uint64	reserved[4];
};


#endif	/* _PVR_HAIKU_H */
