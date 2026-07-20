/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

	.text
	.align 2

	.globl arm64_ddb_break
	.type arm64_ddb_break, %function
arm64_ddb_break:
	brk	#0x0ddb
	ret
	.size arm64_ddb_break, . - arm64_ddb_break

	.globl arm64_ddb_probe_read8
	.type arm64_ddb_probe_read8, %function
arm64_ddb_probe_read8:
	.globl arm64_ddb_probe_load
arm64_ddb_probe_load:
	ldrb	w2, [x0]
	strb	w2, [x1]
	mov	w0, #0
	ret

	.globl arm64_ddb_probe_recover
arm64_ddb_probe_recover:
	mov	w0, #-14
	ret
	.size arm64_ddb_probe_read8, . - arm64_ddb_probe_read8
