/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <generated/offsets.h>

#include <asm/boot/macro.h>

	.text
	.align 4

/*
 * x0: struct arm64_suspend_callee_context *
 * x1: physical resume entry
 * x2: firmware context id (the physical context address)
 */
FUNC(arm64_suspend_enter)
	stp	x19, x20, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_X19]
	stp	x21, x22, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_X21]
	stp	x23, x24, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_X23]
	stp	x25, x26, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_X25]
	stp	x27, x28, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_X27]
	stp	x29, x30, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_X29]
	mov	x9, sp
	str	x9, [x0, #ARM64_SUSPEND_CALLEE_OFFSET_SP]
	mov	x19, x0
	mov	x0, x1
	mov	x1, x2
	bl	psci_system_suspend
	mov	x9, x0
	mov	x10, x19
	ldp	x19, x20, [x10, #ARM64_SUSPEND_CALLEE_OFFSET_X19]
	ldp	x21, x22, [x10, #ARM64_SUSPEND_CALLEE_OFFSET_X21]
	ldp	x23, x24, [x10, #ARM64_SUSPEND_CALLEE_OFFSET_X23]
	ldp	x25, x26, [x10, #ARM64_SUSPEND_CALLEE_OFFSET_X25]
	ldp	x27, x28, [x10, #ARM64_SUSPEND_CALLEE_OFFSET_X27]
	ldp	x29, x30, [x10, #ARM64_SUSPEND_CALLEE_OFFSET_X29]
	mov	x0, x9
	ret
END(arm64_suspend_enter)

/* Firmware resumes with x0 equal to the context id supplied above. */
FUNC(arm64_suspend_resume)
	mov	x9, x0
	add	x12, x9, #ARM64_HOST_PM_OFFSET_EL2
	/* Restore translation controls before returning to the C resume path. */
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_VBAR_EL2]
	msr	vbar_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_MAIR_EL2]
	msr	mair_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_TCR_EL2]
	msr	tcr_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_TTBR0_EL2]
	msr	ttbr0_el2, x11
	dsb	ish
	isb
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_SCTLR_EL2]
	msr	sctlr_el2, x11
	isb
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_HCR_EL2]
	msr	hcr_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_VTCR_EL2]
	msr	vtcr_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_VTTBR_EL2]
	msr	vttbr_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_CPTR_EL2]
	msr	cptr_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_CNTHCTL_EL2]
	msr	cnthctl_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_CNTVOFF_EL2]
	msr	cntvoff_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_MDCR_EL2]
	msr	mdcr_el2, x11
	ldr	x11, [x12, #ARM64_EL2_PM_OFFSET_TPIDR_EL2]
	msr	tpidr_el2, x11
	isb
	add	x12, x9, #ARM64_HOST_PM_OFFSET_CALLEE
	ldr	x10, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_SP]
	mov	sp, x10
	ldp	x19, x20, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_X19]
	ldp	x21, x22, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_X21]
	ldp	x23, x24, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_X23]
	ldp	x25, x26, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_X25]
	ldp	x27, x28, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_X27]
	ldp	x29, x30, [x12, #ARM64_SUSPEND_CALLEE_OFFSET_X29]
	mov	x0, #0
	ret
END(arm64_suspend_resume)
