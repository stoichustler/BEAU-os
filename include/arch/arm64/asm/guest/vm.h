/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VM_H
#define ARM64_GUEST_VM_H

#include <types.h>
#ifndef CONFIG_STATIC_ARM64_PLATFORM
#include <vm_configurations.h>
#endif
#include <bsp/vuart.h>
#include <fdt_api.h>
#include <asm/guest/vgicv3.h>

#define INVALID_PIO_IDX		-1U
#define UART_PIO_IDX0		INVALID_PIO_IDX
#define EMUL_PIO_IDX_MAX	1U

struct arm64_vm_pm_state {
	uint64_t epoch;
	uint64_t resume_entry;
	uint64_t resume_context;
	bool valid;
};

struct vm_arch {
	int64_t time_delta;
	/*
	 * Common ptdev uses this as an interrupt-storm throttle. ARM64 keeps the
	 * same field so the shared passthrough IRQ path can compile unchanged;
	 * zero means inject immediately.
	 */
	uint64_t intr_inject_delay_delta;
	struct arm64_vm_pm_state pm;
	struct arm64_vgicv3 vgic;
};

struct acrn_vcpu;
struct acrn_vm;

uint64_t vcpu_get_vmpidr(struct acrn_vcpu *vcpu);
struct acrn_vcpu *vcpu_from_vmpidr(struct acrn_vm *vm, uint64_t vmpidr);

#endif /* ARM64_GUEST_VM_H */
