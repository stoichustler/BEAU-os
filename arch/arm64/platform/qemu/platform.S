/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

	.section .rodata.lk_image, "a", %progbits
	.align 12
	.globl arm64_lk_image_start
arm64_lk_image_start:
	.incbin "sdk/image/lk.bin"
	.globl arm64_lk_image_end
arm64_lk_image_end:
	.globl arm64_lk_image_size
	.set arm64_lk_image_size, arm64_lk_image_end - arm64_lk_image_start
	.align 12

	.section .rodata.platform_dtb, "a", %progbits
	.align 12
	.globl arm64_platform_dtb_start
arm64_platform_dtb_start:
	.incbin ARM64_PLATFORM_DTB_PATH
	.globl arm64_platform_dtb_end
arm64_platform_dtb_end:
	.globl arm64_platform_dtb_size
	.set arm64_platform_dtb_size, arm64_platform_dtb_end - arm64_platform_dtb_start
	.align 12

	.section .rodata.beau_linux_vm2_dtb, "a", %progbits
	.align 12
	.globl arm64_beau_linux_vm2_dtb_start
arm64_beau_linux_vm2_dtb_start:
	.incbin "sdk/image/linux/vm2/beau-linux.dtb"
	.globl arm64_beau_linux_vm2_dtb_end
arm64_beau_linux_vm2_dtb_end:
	.globl arm64_beau_linux_vm2_dtb_size
	.set arm64_beau_linux_vm2_dtb_size, arm64_beau_linux_vm2_dtb_end - arm64_beau_linux_vm2_dtb_start
	.align 12

	.section .rodata.beau_linux_vm3_dtb, "a", %progbits
	.align 12
	.globl arm64_beau_linux_vm3_dtb_start
arm64_beau_linux_vm3_dtb_start:
	.incbin "sdk/image/linux/vm3/beau-linux.dtb"
	.globl arm64_beau_linux_vm3_dtb_end
arm64_beau_linux_vm3_dtb_end:
	.globl arm64_beau_linux_vm3_dtb_size
	.set arm64_beau_linux_vm3_dtb_size, arm64_beau_linux_vm3_dtb_end - arm64_beau_linux_vm3_dtb_start
	.align 12

	.section .rodata.zephyr_image, "a", %progbits
	.align 12
	.globl arm64_zephyr_image_start
arm64_zephyr_image_start:
	.incbin "sdk/image/zephyr.bin"
	.globl arm64_zephyr_image_end
arm64_zephyr_image_end:
	.globl arm64_zephyr_image_size
	.set arm64_zephyr_image_size, arm64_zephyr_image_end - arm64_zephyr_image_start
	.align 12

	.section .rodata.rtthread_image, "a", %progbits
	.align 12
	.globl arm64_rtthread_image_start
arm64_rtthread_image_start:
	.incbin "sdk/image/rtthread.bin"
	.globl arm64_rtthread_image_end
arm64_rtthread_image_end:
	.globl arm64_rtthread_image_size
	.set arm64_rtthread_image_size, arm64_rtthread_image_end - arm64_rtthread_image_start
	.align 12

	.section .rodata.beau_rtthread_dtb, "a", %progbits
	.align 12
	.globl arm64_beau_rtthread_dtb_start
arm64_beau_rtthread_dtb_start:
	.incbin "sdk/image/rtthread/beau-rtthread.dtb"
	.globl arm64_beau_rtthread_dtb_end
arm64_beau_rtthread_dtb_end:
	.globl arm64_beau_rtthread_dtb_size
	.set arm64_beau_rtthread_dtb_size, arm64_beau_rtthread_dtb_end - arm64_beau_rtthread_dtb_start
	.align 12
