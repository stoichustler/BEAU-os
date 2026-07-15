/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

	.text
	.align 4

/*
 * x0: struct arm64_suspend_callee_context *
 * x1: physical resume entry
 * x2: firmware context id (the physical context address)
 */
	.global arm64_suspend_enter
	.type arm64_suspend_enter, %function
arm64_suspend_enter:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x9, sp
	str	x9, [x0, #96]
	mov	x19, x0
	mov	x0, x1
	mov	x1, x2
	bl	psci_system_suspend
	mov	x9, x0
	mov	x10, x19
	ldp	x19, x20, [x10, #0]
	ldp	x21, x22, [x10, #16]
	ldp	x23, x24, [x10, #32]
	ldp	x25, x26, [x10, #48]
	ldp	x27, x28, [x10, #64]
	ldp	x29, x30, [x10, #80]
	mov	x0, x9
	ret
	.size arm64_suspend_enter, . - arm64_suspend_enter

/* Firmware resumes with x0 equal to the context id supplied above. */
	.global arm64_suspend_resume
	.type arm64_suspend_resume, %function
arm64_suspend_resume:
	mov	x9, x0
	/* Restore translation controls before returning to the C resume path. */
	ldr	x11, [x9, #104]
	msr	vbar_el2, x11
	ldr	x11, [x9, #136]
	msr	mair_el2, x11
	ldr	x11, [x9, #120]
	msr	tcr_el2, x11
	ldr	x11, [x9, #128]
	msr	ttbr0_el2, x11
	dsb	ish
	isb
	ldr	x11, [x9, #112]
	msr	sctlr_el2, x11
	isb
	ldr	x11, [x9, #144]
	msr	hcr_el2, x11
	ldr	x11, [x9, #152]
	msr	vtcr_el2, x11
	ldr	x11, [x9, #160]
	msr	vttbr_el2, x11
	ldr	x11, [x9, #168]
	msr	cptr_el2, x11
	ldr	x11, [x9, #176]
	msr	cnthctl_el2, x11
	ldr	x11, [x9, #184]
	msr	cntvoff_el2, x11
	ldr	x11, [x9, #192]
	msr	mdcr_el2, x11
	ldr	x11, [x9, #200]
	msr	tpidr_el2, x11
	isb
	ldr	x10, [x9, #96]
	mov	sp, x10
	ldp	x19, x20, [x9, #0]
	ldp	x21, x22, [x9, #16]
	ldp	x23, x24, [x9, #32]
	ldp	x25, x26, [x9, #48]
	ldp	x27, x28, [x9, #64]
	ldp	x29, x30, [x9, #80]
	mov	x0, #0
	ret
	.size arm64_suspend_resume, . - arm64_suspend_resume
