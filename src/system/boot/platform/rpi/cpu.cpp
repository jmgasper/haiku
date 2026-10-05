/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "cpu.h"

#include <OS.h>

#include <arch/cpu.h>
#include <arch_cpu.h>
#include <boot/kernel_args.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <boot/vfs.h>
#include <kernel/arch/arm64/arm_registers.h>

#include "mailbox.h"


// what the armstub programs; used if CNTFRQ_EL0 was left unset
static const uint64 kDefaultTimerFrequency = 54000000;

static uint64 sTimerFrequency;
static uint64 sTimerBase;


bigtime_t
system_time()
{
	if (sTimerFrequency == 0) {
		sTimerFrequency = READ_SPECIALREG(CNTFRQ_EL0);
		if (sTimerFrequency == 0)
			sTimerFrequency = kDefaultTimerFrequency;
		sTimerBase = READ_SPECIALREG(CNTPCT_EL0);
	}

	uint64 ticks = READ_SPECIALREG(CNTPCT_EL0) - sTimerBase;
	return ticks / sTimerFrequency * 1000000
		+ ticks % sTimerFrequency * 1000000 / sTimerFrequency;
}


void
spin(bigtime_t microseconds)
{
	bigtime_t end = system_time() + microseconds;
	while (system_time() < end)
		;
}


void
cpu_init()
{
	gKernelArgs.num_cpus = 1;

	// The firmware starts the ARM cores at their lowest clock and leaves
	// raising it to the operating system.
	uint32 rate = 0;
	uint32 maximum = 0;
	if (mailbox_get_clock_rate(MAILBOX_CLOCK_ARM, true, maximum) == B_OK
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_ARM, false, rate) == B_OK) {
		if (rate < maximum
			&& mailbox_set_clock_rate(MAILBOX_CLOCK_ARM, maximum) == B_OK) {
			dprintf("cpu: ARM clock raised from %" B_PRIu32 " to %" B_PRIu32
				" MHz\n", rate / 1000000, maximum / 1000000);
		} else {
			dprintf("cpu: ARM clock at %" B_PRIu32 " MHz\n", rate / 1000000);
		}
	}

	boot_arch_cpu_init();
}


void
platform_load_ucode(BootVolume& volume)
{
}
