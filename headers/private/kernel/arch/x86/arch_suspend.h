/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_X86_SUSPEND_H
#define _KERNEL_ARCH_X86_SUSPEND_H


#include <SupportDefs.h>


#define X86_SUSPEND_SYSCALLS			"x86_suspend"
#define X86_SUSPEND_SYSCALLS_VERSION	1

// generic syscall functions
enum {
	X86_SUSPEND_RESTART_CPU		= 1,
		// int32 cpu: park the CPU and restart it through the wakeup
		// trampoline, without sleeping
	X86_SUSPEND_ENTER_S3		= 2,
		// uint32 flags or struct x86_suspend_s3_args
	X86_SUSPEND_GET_TRACE		= 3,
		// char buffer: the trace of the last suspend and resume
};

// Debugging aid: without working devices after resume, the only observable
// progress is the power state. When power_off_checkpoint is non-zero, the
// resume path powers the machine off once it reaches that checkpoint.
struct x86_suspend_s3_args {
	uint32	flags;
	uint32	power_off_checkpoint;
};

// flags for X86_SUSPEND_ENTER_S3
enum {
	X86_SUSPEND_POWER_OFF_AFTER_RESUME	= 0x01,
		// power the machine off a few seconds after resuming (for testing
		// the core resume path without working devices)
	X86_SUSPEND_SKIP_DEVICES			= 0x02,
		// don't suspend and resume devices
	X86_SUSPEND_SKIP_POWER_HOOKS		= 0x08,
		// don't call the drivers registered outside the device tree
	X86_SUSPEND_SKIP_DEVICE_TREE		= 0x10,
		// don't suspend and resume the device tree
	X86_SUSPEND_VERBOSE					= 0x04,
		// log every step and pause afterwards, so that the log is written
		// before a step that hangs
};


#ifdef __cplusplus
extern "C" {
#endif

status_t x86_suspend_init(void);
status_t x86_suspend_restart_cpu(int32 cpu);
status_t x86_suspend_enter_s3(const struct x86_suspend_s3_args* args);

#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_ARCH_X86_SUSPEND_H
