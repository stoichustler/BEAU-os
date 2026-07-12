/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Note: used to reboot guests (vmid 1/2/3)
 */

#include <types.h>
#include <cpu.h>
#include <vcpu.h>
#include <vm.h>
#include <logmsg.h>
#include <schedule.h>
#include <asm/psci.h>
#include <asm/guest/vm_reset.h>

/* [20260710] PSCI virtualization principle:
 *
 * PSCI calls are guest power-management requests delivered through the same
 * HVC/SMC exit path as other synchronous guest exits. BEAU handles only the
 * local VM lifecycle edges needed by the static ARM64 model; host power state
 * and dynamic VM creation stay outside this file.
 *
 *   guest PSCI call
 *          |
 *          v
 *   vcpu_exit.c decodes function ID
 *          |
 *          +-- CPU_ON/OFF style calls -> vCPU lifecycle helpers
 *          |
 *          +-- SYSTEM_OFF             -> stop current vCPU
 *          |
 *          +-- SYSTEM_RESET           -> queue reset to idle owner
 *
 * Reset is deferred because the current vCPU exit still owns a live register
 * frame. The idle thread is the stable owner that can pause all vCPUs, reset
 * virtual devices, rebuild boot state, and then wake the BSP again.
 */

int64_t arm64_vpsci_system_off(struct acrn_vcpu *vcpu)
{
	if (vcpu == NULL) {
		return PSCI_RET_INVALID_PARAMS;
	}

	LOG_INF("vm%u:vcpu%u psci system off", vcpu->vm->vm_id, vcpu->vcpu_id);
	zombie_vcpu(vcpu);

	return PSCI_RET_SUCCESS;
}

int64_t arm64_vpsci_system_reset(struct acrn_vcpu *vcpu)
{
	int32_t ret;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return PSCI_RET_INVALID_PARAMS;
	}

	/*
	 * PSCI SYSTEM_RESET is a guest self-reset request, not an EL2 host reset.
	 *
	 *   guest SMC/HVC -> vCPU exit -> queue vm reset on this pCPU
	 *                              -> block current vCPU
	 *                              -> pCPU reaches idle and restarts VM
	 *
	 * The reset itself is deferred because the exit handler still owns the
	 * current vCPU register frame. Resetting it in-place would make the normal
	 * exit-return bookkeeping race the new boot context.
	 */
	ret = make_reset_vm_request(pcpuid_from_vcpu(vcpu), vcpu->vm->vm_id);
	if (ret != 0) {
		return PSCI_RET_DENIED;
	}

	LOG_INF("vm%u:vcpu%u psci system reset queued", vcpu->vm->vm_id,
		vcpu->vcpu_id);
	zombie_vcpu(vcpu);

	return PSCI_RET_SUCCESS;
}
