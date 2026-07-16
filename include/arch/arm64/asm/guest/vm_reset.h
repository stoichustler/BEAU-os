/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VM_RESET_H
#define ARM64_GUEST_VM_RESET_H

#include <types.h>

struct acrn_vcpu;
struct acrn_vm;

int64_t arm64_vpsci_cpu_suspend(struct acrn_vcpu *vcpu, uint64_t power_state,
	uint64_t entry_point, uint64_t context_id, bool advance_elr);
int64_t arm64_vpsci_system_suspend(struct acrn_vcpu *vcpu,
	uint64_t entry_point, uint64_t context_id);
int64_t arm64_vpsci_system_off(struct acrn_vcpu *vcpu);
int64_t arm64_vpsci_system_reset(struct acrn_vcpu *vcpu);

#endif /* ARM64_GUEST_VM_RESET_H */
