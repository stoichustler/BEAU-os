/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vcpu.h>
#include <vm.h>
#include <vm_config.h>
#include <asm/mpu.h>
#include <asm/sve.h>
#include <asm/sysreg.h>
#include <asm/guest/vmpu.h>
#include <asm/guest/vsve.h>

/*
 * vMPU keeps VM-visible CPU extensions behind one policy boundary:
 *
 *   platform DTS -> arch_vm_config.guest_feature_mask
 *        -> vMPU policy
 *            -> guest extension module
 *                -> ID filtering, trap gating, context save/restore
 *
 * Stage-2 still owns memory isolation. vMPU owns feature authorization so RTOS
 * VMs can run with a small deterministic CPU surface while Linux VMs can opt in
 * to extensions that need a larger context image.
 */
uint64_t arm64_vm_mpu_host_features(void)
{
	return arm64_mpu_host_features();
}

const char *arm64_vm_mpu_sve_reason_str(enum arm64_vm_mpu_sve_reason reason)
{
	const char *ret;

	switch (reason) {
	case ARM64_VM_MPU_SVE_REASON_OK:
		ret = "ok";
		break;
	case ARM64_VM_MPU_SVE_REASON_DISABLED:
		ret = "disabled";
		break;
	case ARM64_VM_MPU_SVE_REASON_HOST_MISSING:
		ret = "host-missing";
		break;
	case ARM64_VM_MPU_SVE_REASON_VL_TOO_LARGE:
		ret = "vl-too-large";
		break;
	case ARM64_VM_MPU_SVE_REASON_RTOS_DENIED:
		ret = "rtos-denied";
		break;
	default:
		ret = "unknown";
		break;
	}

	return ret;
}

void arm64_vm_mpu_get_sve_status(const struct acrn_vm *vm,
	struct arm64_vm_mpu_sve_status *status)
{
	const struct acrn_vm_config *vm_config;
	uint32_t vl_bits;

	if (status == NULL) {
		return;
	}

	status->configured = false;
	status->host_supported = arm64_mpu_host_feature_enabled(ARM64_VM_FEATURE_SVE);
	status->active = false;
	status->vl_bits = 0U;
	status->host_vl_bits = arm64_sve_host_vl_bits();
	status->reason = ARM64_VM_MPU_SVE_REASON_DISABLED;

	if (vm == NULL) {
		return;
	}

	vm_config = get_vm_config(vm->vm_id);
	vl_bits = vm_config->arch.guest_sve_vl_bits;
	if (vl_bits == 0U) {
		vl_bits = ARM64_SVE_VL_BITS_DEFAULT;
	}

	status->configured =
		(vm_config->arch.guest_feature_mask & ARM64_VM_FEATURE_SVE) != 0UL;
	status->vl_bits = vl_bits;

	if (!status->configured) {
		return;
	}
	if (vm_config->os_config.os_family != VM_OS_LINUX) {
		status->reason = ARM64_VM_MPU_SVE_REASON_RTOS_DENIED;
		return;
	}
	if (!status->host_supported) {
		status->reason = ARM64_VM_MPU_SVE_REASON_HOST_MISSING;
		return;
	}
	if ((status->host_vl_bits == 0U) || (vl_bits > status->host_vl_bits)) {
		status->reason = ARM64_VM_MPU_SVE_REASON_VL_TOO_LARGE;
		return;
	}

	status->active = true;
	status->reason = ARM64_VM_MPU_SVE_REASON_OK;
}

bool arm64_vm_mpu_feature_enabled(const struct acrn_vm *vm, uint64_t feature)
{
	const struct acrn_vm_config *vm_config;
	struct arm64_vm_mpu_sve_status sve_status;

	if (vm == NULL) {
		return false;
	}
	if (feature == ARM64_VM_FEATURE_SVE) {
		arm64_vm_mpu_get_sve_status(vm, &sve_status);
		return sve_status.active;
	}

	vm_config = get_vm_config(vm->vm_id);
	return ((vm_config->arch.guest_feature_mask & feature) != 0UL) &&
		arm64_mpu_host_feature_enabled(feature);
}

bool arm64_vcpu_mpu_sve_enabled(const struct acrn_vcpu *vcpu)
{
	return (vcpu != NULL) && arm64_vm_mpu_feature_enabled(vcpu->vm,
		ARM64_VM_FEATURE_SVE);
}

uint32_t arm64_vm_mpu_sve_vl_bits(const struct acrn_vm *vm)
{
	const struct acrn_vm_config *vm_config;
	uint32_t vl_bits;

	if (vm == NULL) {
		return 0U;
	}

	vm_config = get_vm_config(vm->vm_id);
	vl_bits = vm_config->arch.guest_sve_vl_bits;
	if (vl_bits == 0U) {
		vl_bits = ARM64_SVE_VL_BITS_DEFAULT;
	}

	return vl_bits;
}

void arm64_vcpu_mpu_init(struct acrn_vcpu *vcpu)
{
	if (vcpu == NULL) {
		return;
	}

	vcpu->arch.gctx.hcr_el2 |= HCR_TID3;
	arm64_vcpu_vsve_init(vcpu);
}

void arm64_vcpu_mpu_load(struct acrn_vcpu *vcpu)
{
	arm64_vcpu_vsve_load(vcpu);
}

void arm64_vcpu_mpu_unload(struct acrn_vcpu *vcpu)
{
	arm64_vcpu_vsve_unload(vcpu);
}

int32_t arm64_vm_mpu_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg)
{
	return arm64_vsve_handle_sysreg(vcpu, sysreg, read, reg);
}

int32_t arm64_vm_mpu_handle_sve_trap(struct acrn_vcpu *vcpu)
{
	return arm64_vsve_handle_trap(vcpu);
}
