/*
 * Copyright (C) 2018-2022 Intel Corporation.
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file hcall.h
 *
 * @brief architecture-neutral hcall handler contract
 */

#ifndef HCALL_H
#define HCALL_H

#include <types.h>

struct acrn_vcpu;
struct acrn_vm;

/* [20260713] multi-architecture hcall boundary
 *
 * guest trap owner
 *     |
 *     v
 * architecture decoder: HVC/VMCALL/SVC
 *     |
 *     v
 * architecture dispatch table
 *     |
 *     +--> common handler: VM watchdog, shared VM lifecycle service
 *     +--> BSP hook: platform-specific service
 *     +--> architecture handler: PSCI, vIPC, virtio proxy, legacy ABI
 *
 * Key rule:
 *   - this header describes the handler shape, not one architecture's ABI;
 *   - architecture-specific interrupt, paging, and device-assignment models
 *     stay out of the common hcall contract;
 *   - every architecture owns hcall ID decoding and target permission checks;
 *   - common handlers must validate caller/target state before publishing VM,
 *     IRQ, memory, or device ownership changes.
 */
typedef int32_t (*hcall_handler_t)(struct acrn_vcpu *vcpu,
	struct acrn_vm *target_vm, uint64_t param1, uint64_t param2);

struct hcall_dispatch_entry {
	hcall_handler_t handler;
	uint64_t permission_flags;
};

/*
 * Common hcalls used by architecture dispatchers.
 */
int32_t hcall_vm_wdt_kick(struct acrn_vcpu *vcpu, struct acrn_vm *target_vm,
	uint64_t param1, uint64_t param2);

/*
 * Trusty / secure-world hcalls. These remain architecture-neutral at the
 * dispatch contract layer; the VM architecture state owns the actual context
 * switch implementation.
 */
int32_t hcall_world_switch(struct acrn_vcpu *vcpu, struct acrn_vm *target_vm,
	uint64_t param1, uint64_t param2);
int32_t hcall_initialize_trusty(struct acrn_vcpu *vcpu, struct acrn_vm *target_vm,
	uint64_t param1, uint64_t param2);
int32_t hcall_save_restore_sworld_ctx(struct acrn_vcpu *vcpu,
	struct acrn_vm *target_vm, uint64_t param1, uint64_t param2);

int32_t hcall_get_hw_info(struct acrn_vcpu *vcpu, struct acrn_vm *target_vm,
	uint64_t param1, uint64_t param2);

#endif /* HCALL_H */
