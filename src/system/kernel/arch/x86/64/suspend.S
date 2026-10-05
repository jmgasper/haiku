/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Distributed under the terms of the MIT License.
 */


#include <asm_defs.h>

#include <arch/x86/descriptors.h>


// Offsets into struct x86_suspend_context (see arch_suspend.cpp).
#define CONTEXT_RBX				0
#define CONTEXT_RBP				8
#define CONTEXT_R12				16
#define CONTEXT_R13				24
#define CONTEXT_R14				32
#define CONTEXT_R15				40
#define CONTEXT_RSP				48
#define CONTEXT_RIP				56
#define CONTEXT_RFLAGS			64
#define CONTEXT_CR0				72
#define CONTEXT_CR3				80
#define CONTEXT_CR4				88
#define CONTEXT_EFER			96
#define CONTEXT_GS_BASE			104
#define CONTEXT_KERNEL_GS_BASE	112
#define CONTEXT_FS_BASE			120
#define CONTEXT_GDTR			128

// Progress marker in the argument page (see struct x86_wakeup_args).
#define PROGRESS_OFFSET			0xff0


.text


/*!	Real mode entry point used both as the ACPI firmware waking vector and as
	the SIPI target for restarting application processors. The code is copied
	to a page below 1 MB and must be position independent. The embedded
	argument pointer refers to a page aligned struct x86_wakeup_args (see
	arch_suspend.cpp), which is identity mapped by the page tables it names.
*/
.code16
FUNCTION(x86_wakeup_trampoline):
	cli
	cld

	// movl imm32, %esi
	.byte	0x66
	.byte	0xbe
.globl x86_wakeup_trampoline_args
x86_wakeup_trampoline_args:
	.long	0xdeadbeef

	// Address the argument page through all data segments and use it as
	// stack. Real mode offsets are limited to 64 KB.
	movl	%esi, %eax
	shrl	$4, %eax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	xorw	%sp, %sp
	movb	$1, PROGRESS_OFFSET

	// Switch to protected mode.
	popl	%ebx
	popl	%edx
	subl	%esi, %edx
	lgdt	(%edx)
	movb	$5, PROGRESS_OFFSET

	movl	%cr0, %eax
	orl		$0x01, %eax
	movl	%eax, %cr0
	movb	$6, PROGRESS_OFFSET

	pushl	$8
	leal	(.Lprotected - x86_wakeup_trampoline)(%ebx), %eax
	pushl	%eax
	.byte	0x66
.code32
	lret

.Lprotected:
	mov		$0x10, %ax
	mov		%ax, %ds
	mov		%ax, %es
	mov		%ax, %fs
	mov		%ax, %gs
	mov		%ax, %ss
	movb	$2, PROGRESS_OFFSET(%esi)
	movl	%esi, %esp
	addl	$8, %esp

	// Enable PAE and PGE.
	movl	%cr4, %eax
	orl		$(1 << 5) | (1 << 7), %eax
	movl	%eax, %cr4

	// Load the temporary page tables.
	popl	%eax
	movl	%eax, %cr3
	popl	%eax

	// Enable long mode, NX (the kernel page tables use it) and SYSCALL.
	movl	$0xc0000080, %ecx
	rdmsr
	orl		$(1 << 8) | (1 << 11) | (1 << 0), %eax
	wrmsr

	// Enable paging, putting us in compatibility mode.
	movl	%cr0, %ecx
	orl		$(1 << 31), %ecx
	movl	%ecx, %cr0

	pushl	$0x18
	leal	(.Llong - x86_wakeup_trampoline)(%ebx), %eax
	pushl	%eax
	lret

.align 8
.code64
.Llong:
	mov		$0x10, %ax
	mov		%ax, %ds
	mov		%ax, %es
	mov		%ax, %ss
	movb	$3, PROGRESS_OFFSET(%rsi)

	// entry, context, stack
	popq	%rax
	popq	%rdi
	popq	%rsp
	jmp		*%rax
.globl x86_wakeup_trampoline_end
x86_wakeup_trampoline_end:
FUNCTION_END(x86_wakeup_trampoline)


/*!	int x86_suspend_save_context(struct x86_suspend_context* context)
	Returns 0 after saving the context and 1 when the context is resumed via
	x86_resume_entry(). Must be called with interrupts disabled.
*/
FUNCTION(x86_suspend_save_context):
	movq	%rbx, CONTEXT_RBX(%rdi)
	movq	%rbp, CONTEXT_RBP(%rdi)
	movq	%r12, CONTEXT_R12(%rdi)
	movq	%r13, CONTEXT_R13(%rdi)
	movq	%r14, CONTEXT_R14(%rdi)
	movq	%r15, CONTEXT_R15(%rdi)
	leaq	8(%rsp), %rax
	movq	%rax, CONTEXT_RSP(%rdi)
	movq	(%rsp), %rax
	movq	%rax, CONTEXT_RIP(%rdi)
	pushfq
	popq	%rax
	movq	%rax, CONTEXT_RFLAGS(%rdi)

	movq	%cr0, %rax
	movq	%rax, CONTEXT_CR0(%rdi)
	movq	%cr3, %rax
	movq	%rax, CONTEXT_CR3(%rdi)
	movq	%cr4, %rax
	movq	%rax, CONTEXT_CR4(%rdi)

	movl	$0xc0000080, %ecx
	rdmsr
	shlq	$32, %rdx
	orq		%rdx, %rax
	movq	%rax, CONTEXT_EFER(%rdi)
	movl	$0xc0000101, %ecx
	rdmsr
	shlq	$32, %rdx
	orq		%rdx, %rax
	movq	%rax, CONTEXT_GS_BASE(%rdi)
	movl	$0xc0000102, %ecx
	rdmsr
	shlq	$32, %rdx
	orq		%rdx, %rax
	movq	%rax, CONTEXT_KERNEL_GS_BASE(%rdi)
	movl	$0xc0000100, %ecx
	rdmsr
	shlq	$32, %rdx
	orq		%rdx, %rax
	movq	%rax, CONTEXT_FS_BASE(%rdi)

	sgdt	CONTEXT_GDTR(%rdi)

	xorl	%eax, %eax
	ret
FUNCTION_END(x86_suspend_save_context)


/*!	Entered from the wakeup trampoline in long mode with temporary page tables
	and GDT, %rdi pointing to the struct x86_suspend_context to resume.
*/
FUNCTION(x86_resume_entry):
	// The saved stack is mapped by the kernel half of the temporary tables.
	movq	CONTEXT_RSP(%rdi), %rsp

	movq	CONTEXT_EFER(%rdi), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	movl	$0xc0000080, %ecx
	wrmsr

	movq	CONTEXT_CR4(%rdi), %rax
	movq	%rax, %cr4
	movq	CONTEXT_CR3(%rdi), %rax
	movq	%rax, %cr3
	movq	CONTEXT_CR0(%rdi), %rax
	movq	%rax, %cr0

	lgdt	CONTEXT_GDTR(%rdi)
	pushq	$KERNEL_CODE_SELECTOR
	leaq	.Lreload_cs(%rip), %rax
	pushq	%rax
	lretq

.Lreload_cs:
	movl	$KERNEL_DATA_SELECTOR, %eax
	movl	%eax, %ss
	xorl	%eax, %eax
	movl	%eax, %ds
	movl	%eax, %es
	movl	%eax, %fs
	movl	%eax, %gs

	// Loading the segment selectors cleared the bases.
	movq	CONTEXT_GS_BASE(%rdi), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	movl	$0xc0000101, %ecx
	wrmsr
	movq	CONTEXT_KERNEL_GS_BASE(%rdi), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	movl	$0xc0000102, %ecx
	wrmsr
	movq	CONTEXT_FS_BASE(%rdi), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	movl	$0xc0000100, %ecx
	wrmsr

	pushq	%rdi
	subq	$8, %rsp
	call	x86_suspend_restore_cpu
	addq	$8, %rsp
	popq	%rdi

	movq	CONTEXT_RBX(%rdi), %rbx
	movq	CONTEXT_RBP(%rdi), %rbp
	movq	CONTEXT_R12(%rdi), %r12
	movq	CONTEXT_R13(%rdi), %r13
	movq	CONTEXT_R14(%rdi), %r14
	movq	CONTEXT_R15(%rdi), %r15
	pushq	CONTEXT_RFLAGS(%rdi)
	popfq
	movl	$1, %eax
	jmpq	*CONTEXT_RIP(%rdi)
FUNCTION_END(x86_resume_entry)
