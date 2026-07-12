/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VSVE_H
#define ARM64_GUEST_VSVE_H

#include <types.h>
#include <asm/sve.h>

#ifndef ASSEMBLER

struct acrn_vcpu;

struct arm64_vcpu_sve_state {
	struct arm64_sve_state regs;
	uint32_t vl_bits;
	uint32_t vl_bytes;
	bool valid;
};

void arm64_vcpu_vsve_init(struct acrn_vcpu *vcpu);
void arm64_vcpu_vsve_load(struct acrn_vcpu *vcpu);
void arm64_vcpu_vsve_unload(struct acrn_vcpu *vcpu);
int32_t arm64_vsve_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg);
int32_t arm64_vsve_handle_trap(struct acrn_vcpu *vcpu);

#endif /* ASSEMBLER */

#endif /* ARM64_GUEST_VSVE_H */
