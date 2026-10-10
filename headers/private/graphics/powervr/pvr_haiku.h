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

#define PVR_HAIKU_ABI_VERSION		2


// stage
enum {
	PVR_HAIKU_STAGE_OFF = 0,		// disabled by the driver settings
	PVR_HAIKU_STAGE_POWERED,		// clocks, resets, power domains on
	PVR_HAIKU_STAGE_IDENTIFIED,		// the core's BVNC read
	PVR_HAIKU_STAGE_FIRMWARE		// the firmware runs and answers
};

// command
enum {
	PVR_HAIKU_STAGE_QUERY = 0,		// report only
	PVR_HAIKU_STAGE_HEALTH_CHECK,	// send a HEALTH_CHECK first (root)
	PVR_HAIKU_STAGE_DUMP			// log registers, firmware state and
									// trace to the syslog first (root)
};

/*	PVR_HAIKU_OP(PVR_HAIKU_NR_STAGE): in: version and command; out: the
	rest. The firmware fields are zero until the firmware stage ran; it is
	off unless the driver settings file ("powervr") says "firmware true". */
struct pvr_haiku_stage {
	uint32	version;				// in: PVR_HAIKU_ABI_VERSION
	uint32	command;				// in: PVR_HAIKU_STAGE_QUERY, ...
	uint32	stage;					// PVR_HAIKU_STAGE_*
	uint32	core_id;
	uint64	bvnc;					// packed 16:16:16:16
	uint32	core_clock;				// Hz

	int32	firmware_status;		// B_OK, B_NO_INIT if the stage did not
									// run, or why it failed
	uint32	firmware_running;		// SYSINIT firmware_started seen
	uint32	fw_version_major;
	uint32	fw_version_minor;
	uint32	fw_version_build;
	uint64	fw_boot_time;			// microseconds, MIPS out of reset to
									// firmware_started
	uint32	health_checks;			// HEALTH_CHECKs answered
	uint32	health_check_failures;
	uint32	last_kccb_return;		// return slot of the last KCCB command
	uint32	kccb_cmds_executed;		// as the firmware counts them (OSDATA)
	uint32	irq_count;				// MIPS wrapper interrupts taken
	uint32	irq_spurious;			// GPU interrupts without one
	uint32	mips_exception_status;	// ROGUE_CR_MIPS_EXCEPTION_STATUS
	uint32	fw_faults;				// SYSDATA fw_faults
	uint64	reserved[4];
};


#endif	/* _PVR_HAIKU_H */
