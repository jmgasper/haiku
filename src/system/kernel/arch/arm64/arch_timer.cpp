/*
 * Copyright 2019-2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <boot/stage2.h>
#include <interrupts.h>

#include <timer.h>
#include <arch/timer.h>

#include "arch_timer.h"
#include "soc.h"


static uint64 sTimerTicksUS;
static bigtime_t sTimerMaxInterval;
static bool sUsePhysicalTimer;
static uint32 sTimerIRQ;

#define TIMER_DISABLED (0)
#define TIMER_ENABLE (1)
#define TIMER_IMASK (2)
#define TIMER_ISTATUS (4)

// Standard PPIs used by the supported GIC platforms. With VHE, CNTP_EL0
// accesses the EL2 physical timer; CNTV_EL0 would access the EL2 virtual
// timer and no longer signal the EL1 virtual timer's interrupt.
#define VIRTUAL_TIMER_IRQ 27
#define HYP_PHYSICAL_TIMER_IRQ 26


static void
set_timer_control(uint64 control)
{
	if (sUsePhysicalTimer)
		WRITE_SPECIALREG(CNTP_CTL_EL0, control);
	else
		WRITE_SPECIALREG(CNTV_CTL_EL0, control);
	asm volatile("isb" ::: "memory");
}


void
arch_timer_set_hardware_timer(bigtime_t timeout)
{
	if (timeout > sTimerMaxInterval)
		timeout = sTimerMaxInterval;

	if (sUsePhysicalTimer)
		WRITE_SPECIALREG(CNTP_TVAL_EL0, timeout * sTimerTicksUS);
	else
		WRITE_SPECIALREG(CNTV_TVAL_EL0, timeout * sTimerTicksUS);
	set_timer_control(TIMER_ENABLE);
}


void
arch_timer_clear_hardware_timer()
{
	set_timer_control(TIMER_DISABLED);
}


int32
arch_timer_interrupt(void *data)
{
	set_timer_control(TIMER_DISABLED);
	return timer_interrupt();
}


int
arch_init_timer(kernel_args *args)
{
	sTimerTicksUS = READ_SPECIALREG(CNTFRQ_EL0) / 1000000;
	sTimerMaxInterval = INT32_MAX / sTimerTicksUS;
	sUsePhysicalTimer = (READ_SPECIALREG(CurrentEL) >> 2) == 2;
	sTimerIRQ = sUsePhysicalTimer ? HYP_PHYSICAL_TIMER_IRQ : VIRTUAL_TIMER_IRQ;
	dprintf("ARM64 timer: %s, IRQ %u\n",
		sUsePhysicalTimer ? "EL2 physical" : "EL1 virtual", sTimerIRQ);

	set_timer_control(TIMER_DISABLED);
	install_io_interrupt_handler(sTimerIRQ, &arch_timer_interrupt, NULL, 0);

	return B_OK;
}


status_t
arm64_timer_per_cpu_init()
{
	InterruptController* ic = InterruptController::Get();

	if (ic == NULL)
		return B_ERROR;

	set_timer_control(TIMER_DISABLED);
	ic->EnableInterrupt(sTimerIRQ);

	return B_OK;
}
