/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Entry for the Raspberry Pi boot platform. The VideoCore firmware
	(start4.elf) loads this image like a 64-bit Linux kernel: the built-in
	armstub jumps here on core 0 in EL2 with the MMU and caches off and the
	device tree in x0. Cores 1-3 wait on the armstub's spin table. */

#include <asm_defs.h>


	.section .text.head

	.globl _start
_start:
	mov		x19, x0					// the device tree
	msr		daifset, #0xf

	// Run at the link address, wherever the firmware put the image.
	adr		x1, _start
	ldr		x2, =_start
	cmp		x1, x2
	b.eq	.Lin_place
	ldr		x3, =_image_end
	sub		x3, x3, x2				// size, a multiple of 8
	b.lo	.Lcopy_backward
.Lcopy_forward:
	ldr		x4, [x1], #8
	str		x4, [x2], #8
	subs	x3, x3, #8
	b.ne	.Lcopy_forward
	b		.Lcopied
.Lcopy_backward:
	add		x1, x1, x3
	add		x2, x2, x3
1:	ldr		x4, [x1, #-8]!
	str		x4, [x2, #-8]!
	subs	x3, x3, #8
	b.ne	1b
.Lcopied:
	ic		iallu
	isb
	ldr		x3, =.Lin_place
	br		x3

.Lin_place:
	mrs		x1, CurrentEL
	lsr		x1, x1, #2
	cmp		x1, #2
	b.ne	1f
	bl		drop_to_el1
1:
	ldr		x1, =gStackEnd
	mov		sp, x1

	// FP and SIMD are used by compiler-generated code.
	mov		x1, #(3 << 20)
	msr		cpacr_el1, x1
	isb

	ldr		x1, =__bss_start
	ldr		x2, =__bss_end
2:	cmp		x1, x2
	b.hs	3f
	str		xzr, [x1], #8
	b		2b
3:
	mov		x29, #0
	mov		x30, #0
	mov		x0, x19
	bl		rpi_start
4:	wfe
	b		4b

	.ltorg


	.text

/*	Cores 1-3 arrive here from the spin table, in EL2 with the MMU off and
	no stack. gSecondaryStacks is indexed by MPIDR affinity level 0. */
FUNCTION(rpi_secondary_entry):
	msr		daifset, #0xf
	mrs		x1, CurrentEL
	lsr		x1, x1, #2
	cmp		x1, #2
	b.ne	1f
	bl		drop_to_el1
1:
	mov		x1, #(3 << 20)
	msr		cpacr_el1, x1
	isb

	mrs		x0, mpidr_el1
	and		x0, x0, #0xff
	ldr		x1, =gSecondaryStacks
	ldr		x1, [x1, x0, lsl #3]
	mov		sp, x1
	mov		x29, #0
	mov		x30, #0
	bl		rpi_secondary_main
2:	wfe
	b		2b
FUNCTION_END(rpi_secondary_entry)

	.ltorg


/*	status_t arch_enter_kernel(struct kernel_args* kernelArgs,
		addr_t kernelEntry, addr_t kernelStackTop, uint32 cpu); */
FUNCTION(arch_enter_kernel):
	mov		sp, x2
	mov		x4, x1
	mov		x1, x3
	mov		x29, #0
	br		x4
FUNCTION_END(arch_enter_kernel)


	.section .bss.stack, "aw", %nobits
	.balign 16
	.skip 256 * 1024
	.globl gStackEnd
gStackEnd:
