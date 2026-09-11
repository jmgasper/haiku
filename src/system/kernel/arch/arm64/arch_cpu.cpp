/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <KernelExport.h>

#include <arch/cpu.h>
#include <arch/arm64/cache_line_size.h>
#include <boot/kernel_args.h>
#include <commpage.h>
#include <elf.h>


extern "C" void _exception_vectors(void);


static const uint32 kPsciVersion = 0x84000000;
static const uint32 kPsciSystemOff = 0x84000008;
static const uint32 kPsciSystemReset = 0x84000009;

static uint32 sPsciConduit = ARM64_PSCI_NONE;


static int32
psci_call(uint32 function)
{
	register uint64 x0 asm("x0") = function;
	register uint64 x1 asm("x1") = 0;
	register uint64 x2 asm("x2") = 0;
	register uint64 x3 asm("x3") = 0;

	// These PSCI functions use the SMC32/HVC32 convention. Conservatively
	// clobber the caller-saved registers for older SMCCC implementations.
	if (sPsciConduit == ARM64_PSCI_SMC) {
		asm volatile("smc #0"
			: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
			:
			: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			  "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	} else if (sPsciConduit == ARM64_PSCI_HVC) {
		asm volatile("hvc #0"
			: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
			:
			: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			  "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	} else
		return -1;

	return (int32)x0;
}


status_t
arch_cpu_preboot_init_percpu(kernel_args *args, int curr_cpu)
{
	WRITE_SPECIALREG(VBAR_EL1, _exception_vectors);
	return B_OK;
}


status_t
arch_cpu_init_percpu(kernel_args *args, int curr_cpu)
{
	uint64_t tcr = READ_SPECIALREG(TCR_EL1);
	uint64_t mmfr1 = READ_SPECIALREG(ID_AA64MMFR1_EL1);

	uint64_t hafdbs = ID_AA64MMFR1_HAFDBS(mmfr1);
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF) {
		tcr |= (1UL << 39);
	}
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF_DBS) {
		tcr |= (1UL << 40) | (1UL << 39);
	}

	WRITE_SPECIALREG(TCR_EL1, tcr);

	gCPU[curr_cpu].arch.mpidr = READ_SPECIALREG(MPIDR_EL1);

	return 0;
}


status_t
arch_cpu_init(kernel_args *args)
{
	sPsciConduit = args->arch_args.psci_conduit;
	if (sPsciConduit == ARM64_PSCI_SMC || sPsciConduit == ARM64_PSCI_HVC) {
		int32 version = psci_call(kPsciVersion);
		if (version >= 2) {
			dprintf("PSCI: version %" B_PRId32 ".%" B_PRId32 " via %s\n",
				version >> 16, version & 0xffff,
				sPsciConduit == ARM64_PSCI_SMC ? "SMC" : "HVC");
		} else {
			dprintf("PSCI: unsupported version response %" B_PRId32 "\n", version);
			sPsciConduit = ARM64_PSCI_NONE;
		}
	} else
		sPsciConduit = ARM64_PSCI_NONE;

	for (uint32 i = 0; i < args->num_cpus; i++) {
		cpu_ent* cpu = &gCPU[i];

		cpu->topology_id[CPU_TOPOLOGY_PACKAGE] = 0;
		cpu->topology_id[CPU_TOPOLOGY_CORE] = i;
		cpu->topology_id[CPU_TOPOLOGY_SMT] = 0;
	}
	return B_OK;
}


status_t
arch_cpu_init_post_vm(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_init_post_modules(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_shutdown(bool reboot)
{
	if (sPsciConduit == ARM64_PSCI_NONE)
		return B_NOT_SUPPORTED;

	dprintf("PSCI: requesting system %s\n", reboot ? "reset" : "off");
	cpu_status state = disable_interrupts();
	int32 result = psci_call(reboot ? kPsciSystemReset : kPsciSystemOff);
	restore_interrupts(state);

	// A successful SYSTEM_RESET or SYSTEM_OFF never returns.
	dprintf("PSCI: system %s returned %" B_PRId32 "\n",
		reboot ? "reset" : "off", result);
	return B_ERROR;
}


void
arch_cpu_sync_icache(void *address, size_t len)
{
	uint64_t ctr_el0 = 0;
	asm volatile ("mrs\t%0, ctr_el0":"=r" (ctr_el0));

	const uint64_t icache_line_size = arm64_instruction_cache_line_size(ctr_el0);
	const uint64_t dcache_line_size = arm64_data_cache_line_size(ctr_el0);
	uint64_t addr = (uint64_t)address;
	uint64_t end = addr + len;

	for (uint64_t address_dcache = ROUNDDOWN(addr, dcache_line_size);
	     address_dcache < end; address_dcache += dcache_line_size) {
		asm volatile ("dc cvau, %0" : : "r"(address_dcache) : "memory");
	}

	asm volatile("dsb ish" : : : "memory");

	for (uint64_t address_icache = ROUNDDOWN(addr, icache_line_size);
	     address_icache < end; address_icache += icache_line_size) {
		asm volatile ("ic ivau, %0" : : "r"(address_icache) : "memory");
	}
	asm volatile("dsb ish" : : : "memory");
	asm volatile("isb" : : : "memory");
}


void
arch_cpu_invalidate_tlb_range(intptr_t, addr_t start, addr_t end)
{
	arch_cpu_global_tlb_invalidate();
}


void
arch_cpu_invalidate_tlb_list(intptr_t, addr_t pages[], int num_pages)
{
	arch_cpu_global_tlb_invalidate();
}


void
arch_cpu_global_tlb_invalidate()
{
	asm(
		"dsb ishst\n"
		"tlbi vmalle1\n"
		"dsb ish\n"
		"isb\n"
	);
}


void
arch_cpu_user_tlb_invalidate(intptr_t)
{
	arch_cpu_global_tlb_invalidate();
}
