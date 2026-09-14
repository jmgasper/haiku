/* Named sampling loops with normal and deliberately invalid frame pointers. */
.text
.align 2
.global rock5_profile_user_leaf
.type rock5_profile_user_leaf, %function
rock5_profile_user_leaf:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov x1, #1
1:
	eor x1, x1, x1, lsl #7
	add x1, x1, #3
	subs x0, x0, #1
	b.ne 1b
	mov x0, x1
	ldp x29, x30, [sp], #16
	ret
.size rock5_profile_user_leaf, .-rock5_profile_user_leaf

.align 2
.global rock5_profile_bad_fp
.type rock5_profile_bad_fp, %function
rock5_profile_bad_fp:
	stp x29, x30, [sp, #-16]!
	mov x29, x1
	mov x1, #1
1:
	eor x1, x1, x1, lsl #7
	add x1, x1, #3
	subs x0, x0, #1
	b.ne 1b
	mov x0, x1
	// Restore from SP: the supplied FP is never dereferenced by the workload.
	ldp x29, x30, [sp], #16
	ret
.size rock5_profile_bad_fp, .-rock5_profile_bad_fp

.section .note.GNU-stack,"",%progbits
