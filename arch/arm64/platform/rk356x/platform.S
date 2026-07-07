/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

	.section .rodata, "a"
	.align 12
	.globl arm64_lk_image_start
arm64_lk_image_start:
	.incbin "sdk/image/lk.bin"
	.globl arm64_lk_image_end
arm64_lk_image_end:
	.globl arm64_lk_image_size
	.set arm64_lk_image_size, arm64_lk_image_end - arm64_lk_image_start

	.align 12
	.globl arm64_platform_dtb_start
arm64_platform_dtb_start:
	.incbin ARM64_PLATFORM_DTB_PATH
	.globl arm64_platform_dtb_end
arm64_platform_dtb_end:
	.globl arm64_platform_dtb_size
	.set arm64_platform_dtb_size, arm64_platform_dtb_end - arm64_platform_dtb_start

	.align 12
	.globl arm64_zephyr_image_start
arm64_zephyr_image_start:
	.incbin "sdk/image/zephyr.bin"
	.globl arm64_zephyr_image_end
arm64_zephyr_image_end:
	.globl arm64_zephyr_image_size
	.set arm64_zephyr_image_size, arm64_zephyr_image_end - arm64_zephyr_image_start
