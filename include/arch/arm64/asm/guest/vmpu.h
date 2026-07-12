/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VMPU_H
#define ARM64_GUEST_VMPU_H

#include <types.h>
#include <asm/vm_config.h>

#ifndef ASSEMBLER

struct acrn_vm;
struct acrn_vcpu;

enum arm64_vm_mpu_sve_reason {
	ARM64_VM_MPU_SVE_REASON_OK = 0,
	ARM64_VM_MPU_SVE_REASON_DISABLED,
	ARM64_VM_MPU_SVE_REASON_HOST_MISSING,
	ARM64_VM_MPU_SVE_REASON_VL_TOO_LARGE,
	ARM64_VM_MPU_SVE_REASON_RTOS_DENIED,
};

struct arm64_vm_mpu_sve_status {
	bool configured;
	bool host_supported;
	bool active;
	uint32_t vl_bits;
	uint32_t host_vl_bits;
	enum arm64_vm_mpu_sve_reason reason;
};

uint64_t arm64_vm_mpu_host_features(void);
bool arm64_vm_mpu_feature_enabled(const struct acrn_vm *vm, uint64_t feature);
bool arm64_vcpu_mpu_sve_enabled(const struct acrn_vcpu *vcpu);
uint32_t arm64_vm_mpu_sve_vl_bits(const struct acrn_vm *vm);
void arm64_vm_mpu_get_sve_status(const struct acrn_vm *vm,
	struct arm64_vm_mpu_sve_status *status);
const char *arm64_vm_mpu_sve_reason_str(enum arm64_vm_mpu_sve_reason reason);
void arm64_vcpu_mpu_init(struct acrn_vcpu *vcpu);
void arm64_vcpu_mpu_load(struct acrn_vcpu *vcpu);
void arm64_vcpu_mpu_unload(struct acrn_vcpu *vcpu);
int32_t arm64_vm_mpu_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg);
int32_t arm64_vm_mpu_handle_sve_trap(struct acrn_vcpu *vcpu);

#endif /* ASSEMBLER */

#endif /* ARM64_GUEST_VMPU_H */
