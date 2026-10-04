/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>

#include <arch_cpu.h>
#include <arch/system_info.h>
#include <boot/kernel_args.h>
#include <cpu.h>
#include <libfdt.h>


extern void* gFDT;

static uint64 sCPUFrequency[SMP_MAX_CPUS];


static int
CPUNodeForMpidr(const void* fdt, uint64 mpidr)
{
	int cpus = fdt_path_offset(fdt, "/cpus");
	if (cpus < 0)
		return -1;

	int length;
	const fdt32_t* addressCells = (const fdt32_t*)fdt_getprop(fdt, cpus,
		"#address-cells", &length);
	uint32 cells = addressCells != NULL && length == 4
		? fdt32_to_cpu(*addressCells) : 1;
	if (cells != 1 && cells != 2)
		return -1;

	const uint64 affinityMask = UINT64_C(0x000000ff00ffffff);
	for (int node = fdt_first_subnode(fdt, cpus); node >= 0;
			node = fdt_next_subnode(fdt, node)) {
		const char* type = (const char*)fdt_getprop(fdt, node, "device_type",
			&length);
		if (type == NULL || length < 4 || strcmp(type, "cpu") != 0)
			continue;
		const fdt32_t* reg = (const fdt32_t*)fdt_getprop(fdt, node, "reg",
			&length);
		if (reg == NULL || length != (int)(cells * sizeof(fdt32_t)))
			continue;
		uint64 id = fdt32_to_cpu(reg[0]);
		if (cells == 2)
			id = (id << 32) | fdt32_to_cpu(reg[1]);
		if ((id & affinityMask) == (mpidr & affinityMask))
			return node;
	}
	return -1;
}


static uint64
CPUFrequency(const void* fdt, int node)
{
	if (node < 0)
		return 0;

	int length;
	const fdt32_t* opp = (const fdt32_t*)fdt_getprop(fdt, node,
		"operating-points-v2", &length);
	if (opp != NULL && length == 4) {
		int table = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*opp));
		if (table >= 0) {
			uint64 maximum = 0;
			for (int entry = fdt_first_subnode(fdt, table); entry >= 0;
					entry = fdt_next_subnode(fdt, entry)) {
				const char* status = (const char*)fdt_getprop(fdt, entry,
					"status", NULL);
				if (status != NULL && strcmp(status, "disabled") == 0)
					continue;
				const fdt64_t* hz = (const fdt64_t*)fdt_getprop(fdt, entry,
					"opp-hz", &length);
				if (hz != NULL && length == 8) {
					uint64 frequency = fdt64_to_cpu(*hz);
					if (frequency > maximum && frequency < UINT64_C(10000000000))
						maximum = frequency;
				}
			}
			if (maximum != 0)
				return maximum;
		}
	}

	const fdt32_t* clock = (const fdt32_t*)fdt_getprop(fdt, node,
		"clock-frequency", &length);
	return clock != NULL && length == 4 ? fdt32_to_cpu(*clock) : 0;
}


void
arch_fill_topology_node(cpu_topology_node_info* node, int32 cpu)
{
	switch (node->type) {
		case B_TOPOLOGY_ROOT:
			node->data.root.platform = B_CPU_ARM_64;
			break;
		case B_TOPOLOGY_PACKAGE:
			node->data.package.vendor = CPU_IMPL(gCPU[cpu].arch.midr)
				== CPU_IMPL_ARM ? B_CPU_VENDOR_ARM : B_CPU_VENDOR_UNKNOWN;
			node->data.package.cache_line_size = CACHE_LINE_SIZE;
			break;
		case B_TOPOLOGY_CORE:
			node->data.core.model = gCPU[cpu].arch.midr;
			node->data.core.default_frequency = sCPUFrequency[cpu];
			break;
		default:
			break;
	}
}


status_t
arch_system_info_init(struct kernel_args *args)
{
	if (gFDT != NULL && fdt_check_header(gFDT) == 0) {
		for (uint32 cpu = 0; cpu < args->num_cpus && cpu < SMP_MAX_CPUS;
				cpu++) {
			sCPUFrequency[cpu] = CPUFrequency(gFDT,
				CPUNodeForMpidr(gFDT, gCPU[cpu].arch.mpidr));
		}
	}
	return B_OK;
}


status_t
arch_get_frequency(uint64 *frequency, int32 cpu)
{
	if (frequency == NULL || cpu < 0 || cpu >= SMP_MAX_CPUS)
		return B_BAD_VALUE;
	*frequency = sCPUFrequency[cpu];
	return B_OK;
}
