/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* [20260716] AArch64 Image header
 *
 * raw image offset 0                    EL2 bootstrap
 *        |                                   |
 *        +-- 64-byte Image header ---------->| _start
 *             - branch instruction           |
 *             - load/image size              v
 *             - 4 KiB page flag         BEAU C runtime
 *             - "ARM\x64" magic
 *
 * Key rules:
 *   - the linker owns ld_image_size and keeps this section before .text.entry;
 *   - U-Boot booti executes the first branch and passes the FDT in x0;
 *   - the header describes the file-backed image, not the larger BSS footprint;
 *   - a zero PE offset deliberately avoids claiming full UEFI PE/COFF support.
 */

#include <asm/boot/macro.h>

#define ARM64_IMAGE_HEADER_SIZE	64
#define ARM64_IMAGE_FLAGS_4K	(1 << 1)

	.section .image_header, "ax", %progbits
	.balign 8

HEAD(_image_header)
	b	_start
	.long	0
	.quad	0
	.quad	ld_image_size
	.quad	ARM64_IMAGE_FLAGS_4K
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"
	.long	0
END(_image_header)

	.if (. - _image_header) != ARM64_IMAGE_HEADER_SIZE
	.error "AArch64 Image header must be exactly 64 bytes"
	.endif
