/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2021-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Cores 1-3 wait in the firmware's armstub, each polling its slot of the
	spin table (the device tree's cpu-release-addr). Writing an address there
	sends the core to rpi_secondary_entry in entry.S. */


#include "smp.h"

#include <string.h>

#include <KernelExport.h>

#include <boot/menu.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <kernel.h>
#include <safemode.h>

#include "dtb.h"
#include "mmu.h"


extern uint8 _start;
extern uint8 _image_end;

extern "C" void rpi_secondary_entry();
extern "C" void arch_enter_kernel(struct kernel_args* kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop, uint32 cpu);
extern "C" void rpi_secondary_main(uint64 affinity);

// indexed by MPIDR affinity level 0; read by rpi_secondary_entry
uint64 gSecondaryStacks[256];

static uint32 sCpuIndex[256];
static addr_t sKernelEntry;
static kernel_args* sKernelArgs;


void
smp_init()
{
	gKernelArgs.num_cpus = 1;

	// Only CPUs behind the boot CPU that can be started count.
	for (uint32 i = 1; i < gFDTCpuCount; i++) {
		if (gFDTCpus[i].releaseAddress == 0 || gFDTCpus[i].mpidr > 0xff)
			break;
		gKernelArgs.num_cpus++;
	}

	if (gKernelArgs.num_cpus > SMP_MAX_CPUS)
		gKernelArgs.num_cpus = SMP_MAX_CPUS;
}


void
smp_init_other_cpus()
{
	if (get_safemode_boolean(B_SAFEMODE_DISABLE_SMP, false))
		gKernelArgs.num_cpus = 1;

	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		void* stack = NULL;
		const size_t size = KERNEL_STACK_SIZE
			+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
		if (platform_allocate_region(&stack, size, 0) != B_OK)
			panic("Unable to allocate AP stack");

		memset(stack, 0, size);
		gKernelArgs.cpu_kstack[i].start = fix_address((addr_t)stack);
		gKernelArgs.cpu_kstack[i].size = size;

		uint64 affinity = gFDTCpus[i].mpidr & 0xff;
		gSecondaryStacks[affinity] = (addr_t)stack + size;
		sCpuIndex[affinity] = i;
	}
}


extern "C" void
rpi_secondary_main(uint64 affinity)
{
	// Still on the loader's stack at its physical address; the identity map
	// keeps it valid once the MMU is on.
	mmu_enable_on_this_cpu();

	uint32 cpu = sCpuIndex[affinity];
	arch_enter_kernel(sKernelArgs, sKernelEntry,
		gKernelArgs.cpu_kstack[cpu].start + gKernelArgs.cpu_kstack[cpu].size,
		cpu);
}


void
smp_boot_other_cpus(addr_t kernelEntry)
{
	sKernelEntry = kernelEntry;
	sKernelArgs = &gKernelArgs;

	// The other cores run uncached until they turn their MMU on: what they
	// read has to be in memory by then, and nothing they write may sit in
	// this core's cache, or it would come back as it was before.
	mmu_flush_data_cache(&_start, &_image_end - &_start);
	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		const size_t size = KERNEL_STACK_SIZE
			+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
		uint64 top = gSecondaryStacks[gFDTCpus[i].mpidr & 0xff];
		mmu_flush_data_cache((void*)(top - size), size);
	}

	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		uint64* release = (uint64*)gFDTCpus[i].releaseAddress;
		*release = (uint64)&rpi_secondary_entry;
		mmu_flush_data_cache(release, sizeof(*release));
	}
	asm("sev");
}


void
smp_add_safemode_menus(Menu* menu)
{
	if (gKernelArgs.num_cpus < 2)
		return;

	MenuItem* item = new(std::nothrow) MenuItem("Disable SMP");
	menu->AddItem(item);
	item->SetData(B_SAFEMODE_DISABLE_SMP);
	item->SetType(MENU_ITEM_MARKABLE);
	item->SetHelpText("Disables all but one CPU core.");
}
