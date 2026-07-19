/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_MACRO_H
#define ARM64_MACRO_H

#define FUNC(fn)         \
	.global fn;          \
	.type fn, %function; \
fn:

#define END(fn)         \
	.size fn, . - fn

#define ASM_EMBED_BIN(bin, path)                                      \
	.section .rodata.embed.bin, "a", %progbits;                       \
	.p2align 12;                                                      \
	.globl arm64_##bin##_start;                                       \
arm64_##bin##_start:                                                  \
	.incbin path;                                                     \
	.globl arm64_##bin##_end;                                         \
arm64_##bin##_end:                                                    \
	.globl arm64_##bin##_size;                                        \
	.set arm64_##bin##_size, arm64_##bin##_end - arm64_##bin##_start; \
	.p2align 12

#endif /* ARM64_MACRO_H */
