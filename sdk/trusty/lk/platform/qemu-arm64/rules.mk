# Copyright (C) 2026 The BEAU OS Authors
# SPDX-License-Identifier: BSD-3-Clause

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

ARCH := arm64
ARM_CPU := armv8-a
WITH_SMP := 1
ARM_MERGE_FIQ_IRQ := true

MEMBASE := 0x0e100000
MEMSIZE := 0x00f00000

GLOBAL_INCLUDES += \
	$(LOCAL_DIR)/include

MODULE_DEFINES += \
	GIC_VERSION=$(GIC_VERSION)

MODULE_SRCS += \
	$(LOCAL_DIR)/platform.c \
	$(LOCAL_DIR)/debug.c

MODULE_DEPS += \
	dev/interrupt/arm_gic \
	dev/timer/arm_generic

GLOBAL_DEFINES += \
	MEMBASE=$(MEMBASE) \
	MEMSIZE=$(MEMSIZE) \
	MMU_WITH_TRAMPOLINE=1

ifeq (true,$(call TOBOOL,$(LK_USE_GNU_TOOLCHAIN)))
ARM64_LINKER_TEMPLATE := $(LOCAL_DIR)/system-onesegment-gcc.ld
ARM64_LINKER_OUTPUT := qemu-onesegment-gcc.ld
endif

include make/module.mk
