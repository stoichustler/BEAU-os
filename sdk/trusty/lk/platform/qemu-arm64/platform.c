/*
 * Copyright (C) 2026 The BEAU OS Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <debug.h>
#include <dev/interrupt/arm_gic.h>
#include <dev/timer/arm_generic.h>
#include <kernel/vm.h>
#include <lk/init.h>

#define QEMU_GICD_BASE 0x08000000U
#define QEMU_GICD_SIZE 0x00010000U
#define QEMU_GICR_BASE 0x080a0000U
#define QEMU_GICR_STRIDE 0x00020000U
#define QEMU_SECURE_TIMER_IRQ 29U

struct mmu_initial_mapping mmu_initial_mappings[] = {
        {.phys = MEMBASE + KERNEL_LOAD_OFFSET,
         .virt = KERNEL_BASE + KERNEL_LOAD_OFFSET,
         .size = MEMSIZE,
         .flags = MMU_INITIAL_MAPPING_FLAG_DYNAMIC,
         .name = "ram"},
        {0, 0, 0, 0, 0},
};

static pmm_arena_t ram_arena = {
        .name = "ram",
        .base = MEMBASE + KERNEL_LOAD_OFFSET,
        .size = MEMSIZE,
        .flags = PMM_ARENA_FLAG_KMAP,
};

void platform_init_mmu_mappings(void) {
    pmm_add_arena(&ram_arena);
}

static void qemu_arm64_platform_after_vm(uint level) {
    struct arm_gic_init_info gic_info = {
            .gicc_paddr = 0,
            .gicc_size = 0,
            .gicd_paddr = QEMU_GICD_BASE,
            .gicd_size = QEMU_GICD_SIZE,
            .gicr_paddr = QEMU_GICR_BASE,
            .gicr_size = QEMU_GICR_STRIDE * SMP_MAX_CPUS,
    };

    arm_gic_init_map(&gic_info);
    arm_generic_timer_init(QEMU_SECURE_TIMER_IRQ, 0);
}

LK_INIT_HOOK(qemu_arm64_platform, qemu_arm64_platform_after_vm,
             LK_INIT_LEVEL_VM + 1);
