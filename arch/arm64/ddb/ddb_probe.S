/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/boot/macro.h>
#include <errno.h>

	.text
	.align 2

FUNC(arm64_ddb_break)
	brk	#0x0ddb
	ret
END(arm64_ddb_break)


FUNC(arm64_ddb_panic_break)
	brk	#0x0ddc
	ret
END(arm64_ddb_panic_break)


FUNC(arm64_ddb_probe_read8)
	.globl arm64_ddb_probe_load
arm64_ddb_probe_load:
	ldrb	w2, [x0]
	strb	w2, [x1]
	mov	w0, wzr
	ret
END(arm64_ddb_probe_read8)


FUNC(arm64_ddb_probe_recover)
	mov	w0, #-EFAULT
	ret
END(arm64_ddb_probe_recover)
