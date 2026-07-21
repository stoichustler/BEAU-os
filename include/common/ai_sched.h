/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef AI_SCHED_H
#define AI_SCHED_H

#include <types.h>

struct acrn_vcpu;
struct acrn_vm;

int32_t hcall_ai_sched(struct acrn_vcpu *vcpu, struct acrn_vm *target_vm,
	uint64_t param1, uint64_t param2);
void ai_sched_invalidate_vm(uint16_t vmid);

#endif /* AI_SCHED_H */
