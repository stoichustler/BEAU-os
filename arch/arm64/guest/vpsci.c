/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Note: used to reboot guests (vmid 1/2/3)
 */

#include <types.h>
#include <rtl.h>
#include <cpu.h>
#include <vcpu.h>
#include <vm.h>
#include <event.h>
#include <guest_memory.h>
#include <hv_pm.h>
#include <logmsg.h>
#include <schedule.h>
#include <asm/sysreg.h>
#include <asm/psci.h>
#include <asm/guest/vm_reset.h>

#define PSCI_POWER_STATE_ID_MASK	0x0000ffffUL
#define PSCI_POWER_STATE_TYPE_MASK	0x00010000UL
#define PSCI_POWER_STATE_AFFL_MASK	0x03000000UL
#define PSCI_POWER_STATE_VALID_MASK	(PSCI_POWER_STATE_ID_MASK | \
	PSCI_POWER_STATE_TYPE_MASK | PSCI_POWER_STATE_AFFL_MASK)
#define PSCI_ENTRY_ALIGN_MASK		0x3UL
#define PSCI_SPSR_EL1H			0x5UL

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
 *          +-- CPU_SUSPEND standby    -> wait, then continue after the call
 *          |
 *          +-- CPU_SUSPEND powerdown  -> wait, then resume at entry/context
 *          |
 *          +-- SYSTEM_OFF             -> stop current vCPU
 *          |
 *          +-- SYSTEM_RESET           -> queue reset to idle owner
 *
 * Reset is deferred because the current vCPU exit still owns a live register
 * frame. The idle thread is the stable owner that can pause all vCPUs, reset
 * virtual devices, rebuild boot state, and then wake the BSP again.
 */

static bool vpsci_resume_entry_is_valid(struct acrn_vcpu *vcpu,
	uint64_t entry_point)
{
	return ((entry_point & PSCI_ENTRY_ALIGN_MASK) == 0UL) &&
		(entry_point <= (UINT64_MAX - PSCI_ENTRY_ALIGN_MASK)) &&
		(gpa2hva(vcpu->vm, entry_point) != NULL) &&
		(gpa2hva(vcpu->vm, entry_point + PSCI_ENTRY_ALIGN_MASK) != NULL);
}

static void vpsci_prepare_powerdown_resume(struct acrn_vcpu *vcpu,
	uint64_t entry_point, uint64_t context_id)
{
	struct cpu_regs *regs = &vcpu->arch.regs;
	uint64_t host_tpidr = regs->host_tpidr;
	uint64_t exc_sp = regs->exc_sp;

	/* Preserve only EL2-private return fields; the powered-down CPU loses GPRs. */
	(void)memset(regs, 0U, sizeof(*regs));
	regs->host_tpidr = host_tpidr;
	regs->exc_sp = exc_sp;
	regs->x0 = context_id;
	regs->elr = entry_point;
	regs->spsr = PSCI_SPSR_EL1H | DAIF_ALL;
}

int64_t arm64_vpsci_cpu_suspend(struct acrn_vcpu *vcpu, uint64_t power_state,
	uint64_t entry_point, uint64_t context_id, bool advance_elr)
{
	struct arm64_vcpu_pm_state *pm;
	bool powerdown;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return PSCI_RET_INVALID_PARAMS;
	}

	pm = &vcpu->arch.pm;
	if (pm->blocked) {
		return PSCI_RET_DENIED;
	}
	if ((power_state & ~PSCI_POWER_STATE_VALID_MASK) != 0UL) {
		return PSCI_RET_INVALID_PARAMS;
	}
	if ((power_state & PSCI_POWER_STATE_AFFL_MASK) != 0UL) {
		return PSCI_RET_INVALID_PARAMS;
	}

	powerdown = (power_state & PSCI_POWER_STATE_TYPE_MASK) != 0UL;
	if (powerdown && !vpsci_resume_entry_is_valid(vcpu, entry_point)) {
		return PSCI_RET_INVALID_ADDRESS;
	}

	pm->power_state = (uint32_t)power_state;
	pm->mode = powerdown ? ARM64_VCPU_SUSPEND_POWERDOWN :
		ARM64_VCPU_SUSPEND_STANDBY;
	pm->resume_entry = powerdown ? entry_point : 0UL;
	pm->resume_context = powerdown ? context_id : 0UL;

	if (!powerdown) {
		vcpu->arch.regs.x0 = (uint64_t)PSCI_RET_SUCCESS;
		if (advance_elr) {
			vcpu->arch.regs.elr += 4UL;
		}
	}

	/*
	 * The virtual-IRQ event may retain an old notification after normal guest
	 * delivery. Clear that edge first, then query the authoritative vGIC/request
	 * state. A new IRQ racing either check leaves event->set asserted, so the
	 * subsequent wait cannot lose the wakeup.
	 */
	reset_event(&vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
	if (!arm64_vcpu_has_pending_event(vcpu)) {
		pm->blocked = true;
		wait_event(&vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
		pm->blocked = false;
	}

	if (powerdown) {
		vpsci_prepare_powerdown_resume(vcpu, entry_point, context_id);
	}

	return PSCI_RET_SUCCESS;
}

int64_t arm64_vpsci_system_suspend(struct acrn_vcpu *vcpu,
	uint64_t entry_point, uint64_t context_id)
{
	(void)vcpu;
	(void)entry_point;
	(void)context_id;

	/* Host transparent freeze is deliberately not exposed as guest PSCI STR. */
	return PSCI_RET_NOT_SUPPORTED;
}

int64_t arm64_vpsci_system_off(struct acrn_vcpu *vcpu)
{
	int64_t ret;

	if (vcpu == NULL) {
		return PSCI_RET_INVALID_PARAMS;
	}
	if (hv_pm_begin_vm_topology_change(vcpu->vm->vm_id) != 0) {
		return PSCI_RET_DENIED;
	}

	LOG_INF("vm%u:vcpu%u psci system off", vcpu->vm->vm_id, vcpu->vcpu_id);
	ret = poweroff_vcpu(vcpu) ? PSCI_RET_SUCCESS : PSCI_RET_DENIED;
	hv_pm_end_vm_topology_change(vcpu->vm->vm_id);

	return ret;
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
	pause_vcpu(vcpu);

	return PSCI_RET_SUCCESS;
}
