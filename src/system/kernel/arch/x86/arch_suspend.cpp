/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	ACPI S3 (suspend to RAM) support for x86_64.

	Suspending saves the state of every CPU in a struct x86_suspend_context.
	The application processors are parked in a halt loop from an ICI, the boot
	CPU then enters S3 through ACPICA. On wakeup the firmware jumps to the real
	mode trampoline in low memory, which switches to long mode on temporary
	page tables and continues in x86_resume_entry(). That restores the boot
	CPU, which afterwards restarts the application processors with INIT/SIPI
	through the same trampoline.
*/


#include <arch/x86/arch_suspend.h>

#include <string.h>

#include <ACPI.h>
#include <cpu.h>
#include <debug.h>
#include <kdevice_manager.h>
#include <generic_syscall.h>
#include <interrupts.h>
#include <kernel.h>
#include <real_time_clock.h>
#include <smp.h>
#include <thread.h>
#include <syscalls.h>
#include <timer.h>
#include <arch/timer.h>
#include <arch/x86/apic.h>
#include <arch/x86/arch_cpu.h>
#include <arch/x86/arch_smp.h>
#include <arch/x86/descriptors.h>
#include <arch/x86/ioapic.h>
#include <util/AutoLock.h>
#include <AutoDeleter.h>


//#define TRACE_SUSPEND
#ifdef TRACE_SUSPEND
#	define TRACE(x...) dprintf("suspend: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...) dprintf("suspend: " x)


struct x86_suspend_context {
	// saved and restored by suspend.S
	uint64	rbx;
	uint64	rbp;
	uint64	r12;
	uint64	r13;
	uint64	r14;
	uint64	r15;
	uint64	rsp;
	uint64	rip;
	uint64	rflags;
	uint64	cr0;
	uint64	cr3;
	uint64	cr4;
	uint64	efer;
	uint64	gs_base;
	uint64	kernel_gs_base;
	uint64	fs_base;
	uint8	gdtr[10];

	// handled in C
	uint64	star;
	uint64	lstar;
	uint64	cstar;
	uint64	fmask;
	uint64	sysenter_cs;
	uint64	sysenter_esp;
	uint64	sysenter_eip;
	uint64	pat;
	uint64	tsc_aux;
	uint64	de_cfg;
	uint64	tsc;
	int16	acpi_processor_id;
};


struct x86_wakeup_args {
	uint32	trampoline;
	uint32	gdtr32;
	uint32	pml4;
	uint32	padding;
	uint64	entry;
	uint64	context;
	uint64	stack;

	// temporary GDT
	uint16	gdt_limit;
	uint32	gdt_base;
	uint16	padding2;
	uint64	gdt[4];
} _PACKED;


extern "C" void x86_wakeup_trampoline(void);
extern "C" void x86_wakeup_trampoline_args(void);
extern "C" void x86_wakeup_trampoline_end(void);
extern "C" int x86_suspend_save_context(x86_suspend_context* context);
extern "C" void x86_resume_entry(void);
extern "C" void x86_suspend_restore_cpu(x86_suspend_context* context);

extern bool gHasXsave;
extern uint64 gXsaveMask;


// Pages below 1 MB used for waking up. The first 64 KB are avoided, since
// firmware tends to use them during resume.
static const phys_addr_t kWakeupArgsPage = 0x80000;
static const phys_addr_t kWakeupCodePage = 0x81000;
static const phys_addr_t kWakeupPML4 = 0x82000;
static const phys_addr_t kWakeupPDPT = 0x83000;
static const phys_addr_t kWakeupPageDirectory = 0x84000;
	// the trampoline stores its progress at this offset of the argument page
static const size_t kProgressOffset = 0xff0;

static const uint64 kPagePresent = 1 << 0;
static const uint64 kPageWritable = 1 << 1;
static const uint64 kPageLarge = 1 << 7;
static const uint64 kPageAddressMask = 0x000ffffffffff000ULL;

static x86_suspend_context sContexts[SMP_MAX_CPUS];
static int32 sParked[SMP_MAX_CPUS];
static int32 sRestarted[SMP_MAX_CPUS];
static bool sAdjustTSC;
static int64 sTSCDelta;
static bool sHasTSCAux;
static bool sHasDECfg;
static bool sHasPAT;

static uint32 sPowerOffCheckpoint;

static mutex sSuspendLock = MUTEX_INITIALIZER("x86 suspend");


static acpi_module_info* sACPI;


static void
checkpoint(uint32 number)
{
	if (sPowerOffCheckpoint != number || sACPI == NULL)
		return;

	// Powering off requires evaluating _PTS, writing the PM1 control
	// register alone is ignored by the firmware.
	sACPI->prepare_sleep_state(ACPI_POWER_STATE_OFF, NULL, 0);
	sACPI->enter_sleep_state(ACPI_POWER_STATE_OFF);
	while (true)
		asm volatile("hlt");
}


static inline void*
physical_page(phys_addr_t address)
{
	return (void*)(KERNEL_PMAP_BASE + address);
}


static void
save_msrs(x86_suspend_context* context, int32 cpu)
{
	context->star = x86_read_msr(IA32_MSR_STAR);
	context->lstar = x86_read_msr(IA32_MSR_LSTAR);
	context->cstar = x86_read_msr(IA32_MSR_CSTAR);
	context->fmask = x86_read_msr(IA32_MSR_FMASK);
	context->sysenter_cs = x86_read_msr(IA32_MSR_SYSENTER_CS);
	context->sysenter_esp = x86_read_msr(IA32_MSR_SYSENTER_ESP);
	context->sysenter_eip = x86_read_msr(IA32_MSR_SYSENTER_EIP);
	if (sHasPAT)
		context->pat = x86_read_msr(IA32_MSR_PAT);
	if (sHasTSCAux)
		context->tsc_aux = x86_read_msr(IA32_MSR_TSC_AUX);
	if (sHasDECfg)
		context->de_cfg = x86_read_msr(MSR_F10H_DE_CFG);
	context->acpi_processor_id = gCPU[cpu].arch.acpi_processor_id;
}


extern "C" void
x86_suspend_restore_cpu(x86_suspend_context* context)
{
	int32 cpu = context - sContexts;
	*(uint8*)physical_page(kWakeupArgsPage + kProgressOffset) = 4;

	if (sAdjustTSC) {
		// The TSC was reset while sleeping. Continue where the boot CPU
		// stopped, so that system_time() stays monotonic.
		uint64 tsc = x86_read_msr(IA32_MSR_TSC);
		if (cpu == 0)
			sTSCDelta = context->tsc - tsc;
		x86_write_msr(IA32_MSR_TSC, tsc + sTSCDelta);
	}

	x86_write_msr(IA32_MSR_STAR, context->star);
	x86_write_msr(IA32_MSR_LSTAR, context->lstar);
	x86_write_msr(IA32_MSR_CSTAR, context->cstar);
	x86_write_msr(IA32_MSR_FMASK, context->fmask);
	x86_write_msr(IA32_MSR_SYSENTER_CS, context->sysenter_cs);
	x86_write_msr(IA32_MSR_SYSENTER_ESP, context->sysenter_esp);
	x86_write_msr(IA32_MSR_SYSENTER_EIP, context->sysenter_eip);
	if (sHasPAT)
		x86_write_msr(IA32_MSR_PAT, context->pat);
	if (sHasTSCAux)
		x86_write_msr(IA32_MSR_TSC_AUX, context->tsc_aux);
	if (sHasDECfg)
		x86_write_msr(MSR_F10H_DE_CFG, context->de_cfg);

	x86_descriptors_resume_percpu(cpu);

	if (gHasXsave)
		xsetbv(0, gXsaveMask);
	asm volatile("fninit");

	apic_per_cpu_init(NULL, cpu);
	gCPU[cpu].arch.acpi_processor_id = context->acpi_processor_id;

	if (cpu == 0 && sAdjustTSC)
		checkpoint(1);
}


static void
prepare_wakeup_trampoline(uint64 cr3, x86_suspend_context* context)
{
	x86_wakeup_args* args = (x86_wakeup_args*)physical_page(kWakeupArgsPage);
	memset(args, 0, B_PAGE_SIZE);

	// Copy the trampoline code and point it to its arguments.
	size_t codeSize = (addr_t)x86_wakeup_trampoline_end
		- (addr_t)x86_wakeup_trampoline;
	void* code = physical_page(kWakeupCodePage);
	memcpy(code, (const void*)x86_wakeup_trampoline, codeSize);
	uint32* argsPointer = (uint32*)((addr_t)code
		+ (addr_t)x86_wakeup_trampoline_args - (addr_t)x86_wakeup_trampoline);
	*argsPointer = kWakeupArgsPage;

	// The temporary page tables identity map the first 2 MB and share the
	// kernel half with the kernel page tables.
	uint64* pageDirectory = (uint64*)physical_page(kWakeupPageDirectory);
	memset(pageDirectory, 0, B_PAGE_SIZE);
	pageDirectory[0] = kPagePresent | kPageWritable | kPageLarge;

	uint64* pdpt = (uint64*)physical_page(kWakeupPDPT);
	memset(pdpt, 0, B_PAGE_SIZE);
	pdpt[0] = kWakeupPageDirectory | kPagePresent | kPageWritable;

	uint64* pml4 = (uint64*)physical_page(kWakeupPML4);
	uint64* kernelPML4 = (uint64*)physical_page(cr3 & kPageAddressMask);
	memset(pml4, 0, B_PAGE_SIZE);
	pml4[0] = kWakeupPDPT | kPagePresent | kPageWritable;
	memcpy(pml4 + 256, kernelPML4 + 256, 256 * sizeof(uint64));

	args->trampoline = kWakeupCodePage;
	args->gdtr32 = kWakeupArgsPage + offsetof(x86_wakeup_args, gdt_limit);
	args->pml4 = kWakeupPML4;
	args->entry = (addr_t)x86_resume_entry;
	args->context = (addr_t)context;
	args->stack = kWakeupArgsPage + B_PAGE_SIZE - 16;

	args->gdt_limit = sizeof(args->gdt) - 1;
	args->gdt_base = kWakeupArgsPage + offsetof(x86_wakeup_args, gdt);
	args->gdt[0] = 0;
	args->gdt[1] = 0x00cf9a000000ffffULL;	// 32 bit code
	args->gdt[2] = 0x00cf92000000ffffULL;	// data
	args->gdt[3] = 0x00af9a000000ffffULL;	// 64 bit code
}


static void
park_cpu(void* /*cookie*/, int cpu)
{
	x86_suspend_context* context = &sContexts[cpu];
	save_msrs(context, cpu);

	if (x86_suspend_save_context(context) == 0) {
		atomic_set(&sParked[cpu], 1);
		while (true)
			asm volatile("hlt");
	}

	// We were restarted. The local APIC timer is stopped, get it going again
	// so that pending timer events are handled.
	atomic_set(&sRestarted[cpu], 1);
	arch_timer_set_hardware_timer(1000);
}


static status_t
wait_for_flag(int32* flag, bigtime_t timeout, bool interruptsEnabled)
{
	bigtime_t end = system_time() + timeout;
	while (atomic_get(flag) == 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		if (interruptsEnabled)
			snooze(1000);
		else
			spin(100);
	}
	return B_OK;
}


static status_t
start_cpu(int32 cpu)
{
	x86_suspend_context* context = &sContexts[cpu];
	prepare_wakeup_trampoline(context->cr3, context);

	uint32 apicID = x86_get_cpu_apic_id(cpu);
	atomic_set(&sRestarted[cpu], 0);

	apic_set_interrupt_command(apicID, APIC_TRIGGER_MODE_LEVEL
		| APIC_INTR_COMMAND_1_ASSERT | APIC_DELIVERY_MODE_INIT);
	while (!apic_interrupt_delivered())
		cpu_pause();
	spin(200);

	apic_set_interrupt_command(apicID,
		APIC_TRIGGER_MODE_LEVEL | APIC_DELIVERY_MODE_INIT);
	while (!apic_interrupt_delivered())
		cpu_pause();
	spin(10000);

	for (int i = 0; i < 2; i++) {
		apic_set_interrupt_command(apicID,
			APIC_DELIVERY_MODE_STARTUP | (kWakeupCodePage >> 12));
		spin(200);
		while (!apic_interrupt_delivered())
			cpu_pause();
		if (atomic_get(&sRestarted[cpu]) != 0)
			break;
	}

	status_t status = wait_for_flag(&sRestarted[cpu], 1000000, false);
	if (status != B_OK) {
		INFO("CPU %" B_PRId32 " did not restart, progress %u\n", cpu,
			*(uint8*)physical_page(kWakeupArgsPage + kProgressOffset));
	}
	return status;
}


static void
check_features()
{
	cpu_ent* cpu = get_cpu_struct();
	sHasPAT = x86_check_feature(IA32_FEATURE_PAT, FEATURE_COMMON);
	sHasTSCAux = x86_check_feature(IA32_FEATURE_AMD_EXT_RDTSCP,
			FEATURE_EXT_AMD)
		|| x86_check_feature(IA32_FEATURE_RDPID, FEATURE_7_ECX);
	uint32 family = cpu->arch.family + cpu->arch.extended_family;
	sHasDECfg = cpu->arch.vendor == VENDOR_AMD && family >= 0x10
		&& family != 0x11;
}


status_t
x86_suspend_restart_cpu(int32 cpu)
{
	if (cpu <= 0 || cpu >= smp_get_num_cpus())
		return B_BAD_VALUE;

	MutexLocker locker(sSuspendLock);

	bool wasEnabled = !gCPU[cpu].disabled;
	if (wasEnabled) {
		status_t status = cpu_set_enabled(cpu, false);
		if (status != B_OK)
			return status;
	}

	sAdjustTSC = false;
	atomic_set(&sParked[cpu], 0);
	call_single_cpu(cpu, &park_cpu, NULL);

	status_t status = wait_for_flag(&sParked[cpu], 1000000, true);
	if (status != B_OK) {
		INFO("CPU %" B_PRId32 " did not park\n", cpu);
		return status;
	}

	thread_pin_to_current_cpu(thread_get_current_thread());
	cpu_status state = disable_interrupts();
	status = start_cpu(cpu);
	restore_interrupts(state);
	thread_unpin_from_current_cpu(thread_get_current_thread());

	if (status == B_OK && wasEnabled)
		cpu_set_enabled(cpu, true);

	INFO("restart of CPU %" B_PRId32 ": %s\n", cpu, strerror(status));
	return status;
}


status_t
x86_suspend_enter_s3(const x86_suspend_s3_args* args)
{
	uint32 flags = args->flags;

	MutexLocker locker(sSuspendLock);

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return B_NOT_SUPPORTED;
	BPrivate::CObjectDeleter<const char, status_t, put_module>
		acpiPutter(B_ACPI_MODULE_NAME);

	int32 cpuCount = smp_get_num_cpus();
	INFO("entering S3, flags %#" B_PRIx32 "\n", flags);

	sPowerOffCheckpoint = args->power_off_checkpoint;
	sACPI = acpi;

	// Move everything to the boot CPU.
	bool wasEnabled[SMP_MAX_CPUS] = {};
	for (int32 cpu = 0; cpu < cpuCount; cpu++)
		wasEnabled[cpu] = !gCPU[cpu].disabled;
	if (!wasEnabled[0])
		cpu_set_enabled(0, true);
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		if (wasEnabled[cpu])
			cpu_set_enabled(cpu, false);
	}
	while (smp_get_current_cpu() != 0)
		thread_yield();
	thread_pin_to_current_cpu(thread_get_current_thread());

	_kern_sync();

	status_t status = B_OK;
	if ((flags & X86_SUSPEND_SKIP_DEVICES) == 0)
		status = device_manager_suspend(ACPI_POWER_STATE_SLEEP_S3);

	if (status == B_OK) {
		status = acpi->prepare_sleep_state(ACPI_POWER_STATE_SLEEP_S3,
			(void (*)(void))(addr_t)kWakeupCodePage, 0);
		if (status != B_OK)
			INFO("preparing the sleep state failed: %s\n", strerror(status));
	}

	// park the application processors
	int32 parkedCount = 1;
	if (status == B_OK) {
		for (int32 cpu = 1; cpu < cpuCount; cpu++) {
			atomic_set(&sParked[cpu], 0);
			call_single_cpu(cpu, &park_cpu, NULL);
			if (wait_for_flag(&sParked[cpu], 1000000, true) != B_OK) {
				INFO("CPU %" B_PRId32 " did not park\n", cpu);
				status = B_TIMED_OUT;
				break;
			}
			parkedCount++;
		}
	}

	cpu_status state = disable_interrupts();

	bool resumed = false;
	if (status == B_OK) {
		x86_suspend_context* context = &sContexts[0];
		save_msrs(context, 0);
		ioapic_suspend();

		sAdjustTSC = true;
		context->tsc = x86_read_msr(IA32_MSR_TSC);
		prepare_wakeup_trampoline(x86_read_cr3(), context);

		if (x86_suspend_save_context(context) == 0) {
			acpi->enter_sleep_state(ACPI_POWER_STATE_SLEEP_S3);
			// only returns on failure
			sAdjustTSC = false;
			status = B_ERROR;
		} else
			resumed = true;
	}

	if (resumed) {
		checkpoint(2);

		// Mask the legacy PICs again, the firmware may have unmasked them.
		out8(0xff, 0x21);
		out8(0xff, 0xa1);
		ioapic_resume();
		arch_timer_set_hardware_timer(1000);
	}

	if (resumed)
		checkpoint(3);

	for (int32 cpu = 1; cpu < parkedCount; cpu++)
		start_cpu(cpu);
	sAdjustTSC = false;

	if (resumed) {
		checkpoint(4);
		acpi->leave_sleep_state(ACPI_POWER_STATE_SLEEP_S3, true);
		checkpoint(5);
	}

	restore_interrupts(state);

	if (resumed) {
		checkpoint(6);
		acpi->leave_sleep_state(ACPI_POWER_STATE_SLEEP_S3, false);
		checkpoint(7);
		rtc_resync_from_hardware();
		INFO("resumed from S3\n");
		checkpoint(8);
		snooze(1000000);
		checkpoint(9);
	}

	thread_unpin_from_current_cpu(thread_get_current_thread());
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		if (wasEnabled[cpu])
			cpu_set_enabled(cpu, true);
	}

	if (resumed)
		checkpoint(10);

	if ((flags & X86_SUSPEND_SKIP_DEVICES) == 0)
		device_manager_resume();

	if (resumed)
		checkpoint(11);

	if (resumed && (flags & X86_SUSPEND_POWER_OFF_AFTER_RESUME) != 0) {
		snooze(5000000);
		INFO("powering off after resume\n");
		_kern_sync();
		snooze(2000000);
		acpi->prepare_sleep_state(ACPI_POWER_STATE_OFF, NULL, 0);
		acpi->enter_sleep_state(ACPI_POWER_STATE_OFF);
	}

	return resumed ? B_OK : status;
}


static status_t
suspend_syscall(const char* subsystem, uint32 function, void* buffer,
	size_t bufferSize)
{
	if (geteuid() != 0)
		return B_PERMISSION_DENIED;

	switch (function) {
		case X86_SUSPEND_RESTART_CPU:
		{
			int32 cpu;
			if (bufferSize != sizeof(cpu) || !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&cpu, buffer, sizeof(cpu)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return x86_suspend_restart_cpu(cpu);
		}

		case X86_SUSPEND_ENTER_S3:
		{
			x86_suspend_s3_args args = {};
			if ((bufferSize != sizeof(args.flags) && bufferSize != sizeof(args))
				|| !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&args, buffer, bufferSize) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return x86_suspend_enter_s3(&args);
		}
	}

	return B_BAD_VALUE;
}


status_t
x86_suspend_init(void)
{
	STATIC_ASSERT(offsetof(x86_suspend_context, gdtr) == 128);

	check_features();

	return register_generic_syscall(X86_SUSPEND_SYSCALLS, &suspend_syscall,
		X86_SUSPEND_SYSCALLS_VERSION, 0);
}
