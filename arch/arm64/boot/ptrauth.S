/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/boot/macro.h>

	.text

FUNC_ALIGN(arm64_ptrauth_activate_and_run_idle, 4)
	/* x0 points at the current pCPU's strong-random APIA key. */
	ldp     x1, x2, [x0]
	.arch_extension pauth
	msr     APIAKeyLo_EL1, x1
	msr     APIAKeyHi_EL1, x2
	.arch_extension nopauth
	mrs     x3, sctlr_el2
	mov     x4, #1
	lsl     x4, x4, #31
	orr     x3, x3, x4
	msr     sctlr_el2, x3
	isb
	b       run_idle_thread
END(arm64_ptrauth_activate_and_run_idle)
