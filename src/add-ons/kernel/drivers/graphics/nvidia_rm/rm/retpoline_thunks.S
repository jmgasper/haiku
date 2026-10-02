/* Retpoline thunks referenced by the RM core of NVIDIA's proprietary driver. */
	.text
.macro THUNK reg
	.globl __x86_indirect_thunk_\reg
	.type __x86_indirect_thunk_\reg, @function
__x86_indirect_thunk_\reg:
	jmp *%\reg
.endm
	THUNK rax
	THUNK rbx
	THUNK rcx
	THUNK rdx
	THUNK rsi
	THUNK rdi
	THUNK rbp
	THUNK r8
	THUNK r9
	THUNK r10
	THUNK r11
	THUNK r12
	THUNK r13
	THUNK r14
	THUNK r15
