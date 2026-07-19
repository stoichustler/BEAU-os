/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/boot/macro.h>

ASM_EMBED_BIN(lk_image, "sdk/imgs/lk.bin")
ASM_EMBED_BIN(platform_dtb, ARM64_PLATFORM_DTB_PATH)
ASM_EMBED_BIN(beau_linux_vm2_dtb, "sdk/imgs/linux/vm2/beau-linux.dtb")
ASM_EMBED_BIN(beau_linux_vm3_dtb, "sdk/imgs/linux/vm3/beau-linux.dtb")
ASM_EMBED_BIN(zephyr_image, "sdk/imgs/zephyr.bin")
ASM_EMBED_BIN(rtthread_image, "sdk/imgs/rtthread.bin")
ASM_EMBED_BIN(beau_rtthread_dtb, "sdk/imgs/rtthread/beau-rtthread.dtb")
