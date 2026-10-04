/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2019-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	What the loader takes from the device tree that the firmware hands over:
	the memory layout, the boot archive, the kernel command line, and the
	devices the kernel needs before it can read the tree itself. */


#include "dtb.h"

#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <arch/arm/arch_uart_pl011.h>
#include <boot/kernel_args.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>

extern "C" {
#include <libfdt.h>
}

#include "mailbox.h"
#include "serial.h"


#define GIC_INTERRUPT_TYPE_SPI		0
#define GIC_INTERRUPT_TYPE_PPI		1
#define GIC_INTERRUPT_BASE_SPI		32
#define GIC_INTERRUPT_BASE_PPI		16

// what the firmware clocks the PL011 with unless config.txt says otherwise
#define PL011_DEFAULT_CLOCK			48000000


void* gFDT = NULL;
addr_range gBootArchive = {0, 0};
fdt_cpu gFDTCpus[MAX_FDT_CPUS];
uint32 gFDTCpuCount = 0;


static uint64
read_cells(const uint32*& cells, uint32 count)
{
	uint64 value = 0;
	for (uint32 i = 0; i < count; i++)
		value = (value << 32) | fdt32_to_cpu(*cells++);
	return value;
}


static uint32
get_cell_count(int node, const char* property, uint32 defaultCount)
{
	if (node < 0)
		return defaultCount;
	const uint32* cells = (const uint32*)fdt_getprop(gFDT, node, property,
		NULL);
	return cells != NULL ? fdt32_to_cpu(*cells) : defaultCount;
}


/*!	Translates \a address of a child of \a parent into a CPU address, through
	the "ranges" of all buses on the way up.
*/
static uint64
translate_address(int parent, uint64 address)
{
	while (parent >= 0) {
		int grandParent = fdt_parent_offset(gFDT, parent);
		if (grandParent < 0)
			break;

		int length;
		const uint32* ranges = (const uint32*)fdt_getprop(gFDT, parent,
			"ranges", &length);
		if (ranges != NULL && length > 0) {
			uint32 childCells = get_cell_count(parent, "#address-cells", 2);
			uint32 sizeCells = get_cell_count(parent, "#size-cells", 1);
			uint32 parentCells = get_cell_count(grandParent, "#address-cells",
				2);
			const uint32* end = ranges + length / 4;
			while (ranges + childCells + parentCells + sizeCells <= end) {
				uint64 childBase = read_cells(ranges, childCells);
				uint64 parentBase = read_cells(ranges, parentCells);
				uint64 size = read_cells(ranges, sizeCells);
				if (address >= childBase && address - childBase < size) {
					address = address - childBase + parentBase;
					break;
				}
			}
		}

		parent = grandParent;
	}

	return address;
}


bool
fdt_get_reg(int node, size_t index, addr_range& range)
{
	int parent = fdt_parent_offset(gFDT, node);
	uint32 addressCells = get_cell_count(parent, "#address-cells", 2);
	uint32 sizeCells = get_cell_count(parent, "#size-cells", 1);

	int length;
	const uint32* reg = (const uint32*)fdt_getprop(gFDT, node, "reg", &length);
	if (reg == NULL)
		return false;
	if ((index + 1) * (addressCells + sizeCells) * 4 > (size_t)length)
		return false;

	reg += index * (addressCells + sizeCells);
	range.start = translate_address(parent, read_cells(reg, addressCells));
	range.size = read_cells(reg, sizeCells);
	return true;
}


bool
fdt_find_compatible_reg(const char* compatible, addr_range& range)
{
	int node = fdt_node_offset_by_compatible(gFDT, -1, compatible);
	if (node < 0)
		return false;
	return fdt_get_reg(node, 0, range);
}


static bool
is_enabled(int node)
{
	const char* status = (const char*)fdt_getprop(gFDT, node, "status", NULL);
	return status == NULL || strcmp(status, "okay") == 0
		|| strcmp(status, "ok") == 0;
}


static uint32
get_gic_interrupt(int node)
{
	int length;
	const uint32* interrupts = (const uint32*)fdt_getprop(gFDT, node,
		"interrupts", &length);
	if (interrupts == NULL || length < 12)
		return 0;

	uint32 type = fdt32_to_cpu(interrupts[0]);
	uint32 number = fdt32_to_cpu(interrupts[1]);
	if (type == GIC_INTERRUPT_TYPE_SPI)
		return number + GIC_INTERRUPT_BASE_SPI;
	return number + GIC_INTERRUPT_BASE_PPI;
}


static void
reserve_range(uint64 start, uint64 size)
{
	if (size == 0)
		return;

	uint64 end = ROUNDUP(start + size, B_PAGE_SIZE);
	start = ROUNDDOWN(start, B_PAGE_SIZE);

	// only what lies in RAM is of interest
	for (uint32 i = 0; i < gKernelArgs.num_physical_memory_ranges; i++) {
		const addr_range& range = gKernelArgs.physical_memory_range[i];
		uint64 from = start > range.start ? start : range.start;
		uint64 to = end < range.start + range.size
			? end : range.start + range.size;
		if (from < to)
			insert_physical_allocated_range(from, to - from);
	}
}


static void
init_memory()
{
	int node = -1;
	while ((node = fdt_node_offset_by_prop_value(gFDT, node, "device_type",
			"memory", 7)) >= 0) {
		addr_range range;
		for (size_t i = 0; fdt_get_reg(node, i, range); i++) {
			if (range.size == 0)
				continue;
			dprintf("memory: %#" B_PRIx64 " - %#" B_PRIx64 "\n", range.start,
				range.start + range.size);
			insert_physical_memory_range(range.start, range.size);
		}
	}

	if (gKernelArgs.num_physical_memory_ranges == 0)
		panic("The device tree describes no memory");

	// the tree itself
	reserve_range((addr_t)gFDT, fdt_totalsize(gFDT));

	int count = fdt_num_mem_rsv(gFDT);
	for (int i = 0; i < count; i++) {
		uint64 start, size;
		if (fdt_get_mem_rsv(gFDT, i, &start, &size) == 0) {
			dprintf("reserved: %#" B_PRIx64 " - %#" B_PRIx64 "\n", start,
				start + size);
			reserve_range(start, size);
		}
	}

	int reserved = fdt_path_offset(gFDT, "/reserved-memory");
	if (reserved >= 0) {
		int child;
		fdt_for_each_subnode(child, gFDT, reserved) {
			addr_range range;
			for (size_t i = 0; fdt_get_reg(child, i, range); i++) {
				dprintf("reserved: %#" B_PRIx64 " - %#" B_PRIx64 " (%s)\n",
					range.start, range.start + range.size,
					fdt_get_name(gFDT, child, NULL));
				reserve_range(range.start, range.size);
			}
		}
	}
}


static void
init_chosen()
{
	int chosen = fdt_path_offset(gFDT, "/chosen");
	if (chosen < 0)
		return;

	int startLength, endLength;
	const uint32* start = (const uint32*)fdt_getprop(gFDT, chosen,
		"linux,initrd-start", &startLength);
	const uint32* end = (const uint32*)fdt_getprop(gFDT, chosen,
		"linux,initrd-end", &endLength);
	if (start != NULL && end != NULL) {
		gBootArchive.start = read_cells(start, startLength / 4);
		gBootArchive.size = read_cells(end, endLength / 4) - gBootArchive.start;
		dprintf("boot archive: %#" B_PRIx64 ", %" B_PRIu64 " bytes\n",
			gBootArchive.start, gBootArchive.size);
		reserve_range(gBootArchive.start, gBootArchive.size);
	}
}


const char*
fdt_boot_arguments()
{
	int chosen = fdt_path_offset(gFDT, "/chosen");
	if (chosen < 0)
		return "";
	const char* arguments = (const char*)fdt_getprop(gFDT, chosen, "bootargs",
		NULL);
	return arguments != NULL ? arguments : "";
}


static bool
init_uart_node(int node)
{
	if (node < 0 || !is_enabled(node))
		return false;

	uart_info& uart = gKernelArgs.arch_args.uart;
	const char* kind;
	if (fdt_node_check_compatible(gFDT, node, "arm,pl011") == 0)
		kind = UART_KIND_PL011;
	else
		return false;

	if (!fdt_get_reg(node, 0, uart.regs))
		return false;

	strlcpy(uart.kind, kind, sizeof(uart.kind));
	uart.irq = get_gic_interrupt(node);
	uart.clock = PL011_DEFAULT_CLOCK;

	const uint32* frequency = (const uint32*)fdt_getprop(gFDT, node,
		"clock-frequency", NULL);
	uint32 rate;
	if (frequency != NULL)
		uart.clock = fdt32_to_cpu(*frequency);
	else if (mailbox_get_clock_rate(MAILBOX_CLOCK_UART, false, rate) == B_OK)
		uart.clock = rate;

	return true;
}


static void
init_uart()
{
	// The console the firmware names, then the usual alias.
	int chosen = fdt_path_offset(gFDT, "/chosen");
	if (chosen >= 0) {
		int length;
		const char* path = (const char*)fdt_getprop(gFDT, chosen,
			"stdout-path", &length);
		if (path != NULL) {
			const char* separator = strchr(path, ':');
			int nameLength = separator != NULL
				? separator - path : (int)strlen(path);
			if (init_uart_node(fdt_path_offset_namelen(gFDT, path,
					nameLength))) {
				return;
			}
		}
	}

	// Only a PL011 is taken, and only as the console: without the disable-bt
	// overlay the PL011 talks to the Bluetooth chip and the console is the
	// mini UART, which the kernel has no driver for.
	init_uart_node(fdt_path_offset(gFDT, "serial0"));
}


static void
init_interrupt_controller()
{
	static const char* const kCompatible[]
		= {"arm,gic-400", "arm,cortex-a15-gic"};

	intc_info& controller = gKernelArgs.arch_args.interrupt_controller;
	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		int node = fdt_node_offset_by_compatible(gFDT, -1, kCompatible[i]);
		if (node < 0)
			continue;

		strlcpy(controller.kind, INTC_KIND_GICV2, sizeof(controller.kind));
		fdt_get_reg(node, 0, controller.regs1);
		fdt_get_reg(node, 1, controller.regs2);
		return;
	}
}


static void
init_cpus()
{
	int cpus = fdt_path_offset(gFDT, "/cpus");
	if (cpus < 0)
		return;

	uint32 addressCells = get_cell_count(cpus, "#address-cells", 1);

	int node;
	fdt_for_each_subnode(node, gFDT, cpus) {
		const char* type = (const char*)fdt_getprop(gFDT, node, "device_type",
			NULL);
		if (type == NULL || strcmp(type, "cpu") != 0)
			continue;
		if (gFDTCpuCount == MAX_FDT_CPUS)
			break;

		fdt_cpu& cpu = gFDTCpus[gFDTCpuCount++];
		const uint32* reg = (const uint32*)fdt_getprop(gFDT, node, "reg",
			NULL);
		cpu.mpidr = reg != NULL ? read_cells(reg, addressCells) : 0;
		cpu.releaseAddress = 0;

		const char* method = (const char*)fdt_getprop(gFDT, node,
			"enable-method", NULL);
		int length;
		const uint32* release = (const uint32*)fdt_getprop(gFDT, node,
			"cpu-release-addr", &length);
		if (method != NULL && strcmp(method, "spin-table") == 0
			&& release != NULL) {
			cpu.releaseAddress = read_cells(release, length / 4);
		}
	}
}


void
fdt_init(void* fdt)
{
	if (fdt == NULL || fdt_check_header(fdt) != 0)
		panic("No device tree from the firmware");
	gFDT = fdt;

	mailbox_init();
	init_uart();
	serial_init();
		// from here on dprintf() reaches the UART

	dprintf("device tree at %p, %u bytes: %s\n", fdt,
		(unsigned)fdt_totalsize(fdt),
		(const char*)fdt_getprop(fdt, 0, "model", NULL));

	init_memory();
	init_chosen();
	init_interrupt_controller();
	init_cpus();

	gKernelArgs.arch_args.psci_conduit = ARM64_PSCI_NONE;
}


void
fdt_set_kernel_args()
{
	// The kernel gets a copy with room for what is added below; libfdt wants
	// the tree 8-byte aligned.
	uint32 size = fdt_totalsize(gFDT) + 1024;
	void* copy = kernel_args_malloc(size, 8);
	if (copy == NULL || fdt_open_into(gFDT, copy, size) != 0)
		panic("No memory for the device tree");
	gKernelArgs.arch_args.fdt = copy;

	// The firmware owns the clocks. Until the kernel can ask it, tell the
	// SD controller's driver what its clock runs at.
	int node = fdt_node_offset_by_compatible(copy, -1, "brcm,bcm2711-emmc2");
	uint32 rate;
	if (node >= 0
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_EMMC2, false, rate) == B_OK) {
		fdt_setprop_u32(copy, node, "clock-frequency", rate);
		dprintf("EMMC2 clock: %" B_PRIu32 " Hz\n", rate);
	}

	// The firmware's tree has no operating points for the cores either; the
	// kernel reports a core's "clock-frequency" as its maximum speed.
	int cpus = fdt_path_offset(copy, "/cpus");
	if (cpus >= 0
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_ARM, true, rate) == B_OK) {
		fdt_for_each_subnode(node, copy, cpus) {
			const char* type = (const char*)fdt_getprop(copy, node,
				"device_type", NULL);
			if (type != NULL && strcmp(type, "cpu") == 0)
				fdt_setprop_u32(copy, node, "clock-frequency", rate);
		}
		dprintf("ARM clock: up to %" B_PRIu32 " Hz\n", rate);
	}

	const uart_info& uart = gKernelArgs.arch_args.uart;
	dprintf("UART: %s at %#" B_PRIx64 ", irq %" B_PRIu32 "\n",
		uart.kind[0] != 0 ? uart.kind : "none", uart.regs.start, uart.irq);

	const intc_info& controller = gKernelArgs.arch_args.interrupt_controller;
	dprintf("interrupt controller: %s at %#" B_PRIx64 ", %#" B_PRIx64 "\n",
		controller.kind[0] != 0 ? controller.kind : "none",
		controller.regs1.start, controller.regs2.start);
}
