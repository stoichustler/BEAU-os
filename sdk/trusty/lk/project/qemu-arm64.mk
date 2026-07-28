# Copyright (C) 2026 The BEAU OS Authors
# SPDX-License-Identifier: BSD-3-Clause

# QEMU-only minimal Trusty BL32 profile. It deliberately omits user tasks,
# IPC, device-tree services, Binder, and guest-visible shared-memory paths.
KERNEL_32BIT := false
DEBUG := 1

LK_LIBC_IMPLEMENTATION := lk
WITH_TRUSTY_APP_CRASH := false
WITH_BACKTRACE := false
LK_USE_GNU_TOOLCHAIN := true
ARCH_arm64_TOOLCHAIN_PREFIX := aarch64-none-elf-
KERNEL_LTO_ENABLED := false
KERNEL_SCS_ENABLED := false
KERNEL_BASE_ASLR := false
PIE_KERNEL := false

TARGET := qemu-arm64
GIC_VERSION := 3
SMP_MAX_CPUS := 8
ARM64_BOOT_PROTOCOL := X0_MEMSIZE
TIMER_ARM_GENERIC_SELECTED := CNTPS

GLOBAL_DEFINES += \
	CACHE_LINE=64 \
	LK_USE_GNU_TOOLCHAIN=1 \
	MMU_IDENT_SIZE_SHIFT=29 \
	BOOT_ALLOC_RELOCATE_EARLY=1 \
	WITH_NO_PHYS_RELOCATION=1

MODULES += \
	trusty/kernel/lib/sm
