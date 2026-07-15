/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <vm_wdt.h>
#include <vcpu.h>
#include <vm.h>
#include <guest_memory.h>
#include <acrn_hv_defs.h>
#include <bsp/pm.h>
#include <asm/irq.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vtimer.h>

#define BSP_PM_MAX_WAKE_IRQS	8U
#define BSP_PM_HOOK_PRIO_WDT	100U
#define BSP_PM_HOOK_PRIO_VTIMER	800U
#define BSP_PM_HOOK_PRIO_VGIC	900U

static uint32_t bsp_pm_wakeup_irqs[BSP_PM_MAX_WAKE_IRQS];
static uint16_t bsp_pm_wakeup_irq_count;
static uint32_t bsp_pm_event_virq;
static bool bsp_pm_retention_hooks_registered;

typedef int32_t (*bsp_pm_vm_hook_fn)(struct acrn_vm *vm, uint64_t epoch);

static int32_t bsp_pm_run_required_vm_hook(uint64_t epoch,
	bsp_pm_vm_hook_fn hook)
{
	struct beau_pm_snapshot snapshot;
	uint16_t vmid;

	if ((epoch == 0UL) || (hook == NULL)) {
		return -EINVAL;
	}
	hv_pm_get_snapshot(&snapshot);
	if (snapshot.epoch != epoch) {
		return -EINVAL;
	}

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct acrn_vm *vm;
		int32_t status;

		if ((snapshot.required_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}
		vm = get_vm_from_vmid(vmid);
		if (vm == NULL) {
			return -ENODEV;
		}
		status = hook(vm, epoch);
		if (status != 0) {
			return status;
		}
	}

	return 0;
}

static int32_t bsp_pm_vtimer_suspend(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, arm64_vtimer_suspend_vm);
}

static int32_t bsp_pm_vtimer_resume(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, arm64_vtimer_resume_vm);
}

static int32_t bsp_pm_vgic_suspend(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, arm64_vgicv3_suspend_vm);
}

static int32_t bsp_pm_vgic_resume(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, arm64_vgicv3_resume_vm);
}

static int32_t bsp_pm_register_retention_hooks(void)
{
	static const struct beau_pm_ops retention_hooks[] = {
		{
			.name = "vm-wdt",
			.priority = BSP_PM_HOOK_PRIO_WDT,
			.suspend = vm_wdt_pm_suspend,
			.resume = vm_wdt_pm_resume,
			.abort = vm_wdt_pm_resume,
		},
		{
			.name = "arm64-vtimer",
			.priority = BSP_PM_HOOK_PRIO_VTIMER,
			.suspend = bsp_pm_vtimer_suspend,
			.resume = bsp_pm_vtimer_resume,
			.abort = bsp_pm_vtimer_resume,
		},
		{
			.name = "arm64-vgicv3",
			.priority = BSP_PM_HOOK_PRIO_VGIC,
			.suspend = bsp_pm_vgic_suspend,
			.resume = bsp_pm_vgic_resume,
			.abort = bsp_pm_vgic_resume,
		},
	};
	uint16_t idx;
	int32_t status;

	if (bsp_pm_retention_hooks_registered) {
		return 0;
	}
	for (idx = 0U; idx < ARRAY_SIZE(retention_hooks); idx++) {
		status = hv_pm_register_hook(&retention_hooks[idx]);
		if (status != 0) {
			return status;
		}
	}
	bsp_pm_retention_hooks_registered = true;

	return 0;
}

int32_t bsp_pm_set_wakeup_irqs(const uint32_t *irqs, uint16_t count)
{
	uint16_t idx;
	int32_t status;

	if ((irqs == NULL) || (count == 0U) || (count > BSP_PM_MAX_WAKE_IRQS)) {
		return -EINVAL;
	}
	status = bsp_pm_register_retention_hooks();
	if (status != 0) {
		return status;
	}

	for (idx = 0U; idx < count; idx++) {
		bsp_pm_wakeup_irqs[idx] = irqs[idx];
	}
	bsp_pm_wakeup_irq_count = count;

	return 0;
}

int32_t bsp_pm_set_event_virq(uint32_t virq)
{
	if ((virq < 32U) || (virq >= IRQ_NUM_GIC_DOMAIN)) {
		return -EINVAL;
	}

	bsp_pm_event_virq = virq;
	return 0;
}

int32_t bsp_pm_request_suspend(void)
{
	struct beau_pm_snapshot snapshot;

	hv_pm_get_snapshot(&snapshot);
	return hv_pm_request_suspend(snapshot.controller_vmid);
}

int32_t bsp_pm_abort(int32_t reason)
{
	struct beau_pm_snapshot snapshot;

	hv_pm_get_snapshot(&snapshot);
	return hv_pm_abort(snapshot.epoch, reason);
}

int32_t bsp_pm_request_wake(uint32_t wake_source)
{
	uint16_t idx;

	for (idx = 0U; idx < bsp_pm_wakeup_irq_count; idx++) {
		if (bsp_pm_wakeup_irqs[idx] == wake_source) {
			return hv_pm_record_wake(wake_source, idx);
		}
	}

	return -EACCES;
}

static int32_t bsp_pm_publish_prepare_events(
	const struct beau_pm_snapshot *snapshot)
{
	uint16_t vmid;
	int32_t status = 0;

	if ((snapshot == NULL) || (bsp_pm_event_virq == 0U)) {
		return -ENODEV;
	}

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct acrn_vm *vm;
		struct acrn_vcpu *bsp;

		if ((snapshot->required_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}
		vm = get_vm_from_vmid(vmid);
		bsp = vcpu_from_vid(vm, BSP_CPU_ID);
		/* The transaction record is visible before this edge-triggered IRQ. */
		status = arm64_vgicv3_inject_irq(bsp, bsp_pm_event_virq, false);
		if (status != 0) {
			break;
		}
	}

	return status;
}

static bool bsp_pm_ioc_input_is_valid(const struct acrn_pm_ioc *ioc,
	uint16_t caller_vmid)
{
	return (ioc != NULL) && (ioc->abi_version == ACRN_PM_ABI_VERSION) &&
		(ioc->ioc_size == sizeof(*ioc)) &&
		(ioc->op <= ACRN_PM_RESUME_COMPLETE) &&
		((ioc->status == 0) || (ioc->op == ACRN_PM_ABORT)) &&
		(ioc->wake_reason == 0UL) &&
		(ioc->required_vm_mask == 0UL) && (ioc->pm_state == 0U) &&
		(ioc->vm_state == 0U) && (ioc->vmid == caller_vmid) &&
		(ioc->flags == 0U) && (ioc->event_virq == 0U) &&
		(ioc->reserved == 0UL);
}

static bool bsp_pm_ioc_epoch_is_current(uint64_t epoch,
	const struct beau_pm_snapshot *snapshot)
{
	return (epoch == 0UL) || (epoch == snapshot->epoch);
}

static void bsp_pm_fill_ioc(struct acrn_pm_ioc *ioc,
	const struct beau_pm_snapshot *snapshot, uint16_t caller_vmid,
	int32_t operation_status)
{
	const struct beau_vm_pm_record *record = &snapshot->vm[caller_vmid];
	uint64_t required_vm_mask = (snapshot->required_vm_mask != 0UL) ?
		snapshot->required_vm_mask : snapshot->policy_required_vm_mask;

	ioc->status = operation_status;
	ioc->epoch = snapshot->epoch;
	ioc->wake_reason = snapshot->wake_reason;
	ioc->required_vm_mask = required_vm_mask;
	ioc->pm_state = snapshot->state;
	ioc->vm_state = record->state;
	ioc->vmid = caller_vmid;
	ioc->flags = ACRN_PM_CAP_SYSTEM_SUSPEND;
	if ((required_vm_mask & (1UL << caller_vmid)) != 0UL) {
		ioc->flags |= ACRN_PM_FLAG_REQUIRED;
	}
	if ((record->epoch == snapshot->epoch) &&
		((record->state == VM_PM_PREPARE_SENT) ||
		 (record->state == VM_PM_SUSPEND_PENDING))) {
		ioc->flags |= ACRN_PM_EVENT_PREPARE;
	} else if ((record->epoch == snapshot->epoch) &&
		(record->state == VM_PM_RESUMING)) {
		ioc->flags |= ACRN_PM_EVENT_RESUME;
	}
	ioc->event_virq = bsp_pm_event_virq;
}

int32_t bsp_pm_control_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa)
{
	struct beau_pm_snapshot snapshot;
	struct acrn_pm_ioc ioc;
	uint16_t caller_vmid;
	int32_t status;
	int32_t copy_status;

	if ((vcpu == NULL) || (vcpu->vm == NULL) ||
		((ioc_gpa & (sizeof(struct acrn_pm_ioc) - 1UL)) != 0UL)) {
		return -EINVAL;
	}
	caller_vmid = vcpu->vm->vm_id;
	(void)memset(&ioc, 0U, sizeof(ioc));
	status = copy_from_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc));
	if (status != 0) {
		return status;
	}
	if (!bsp_pm_ioc_input_is_valid(&ioc, caller_vmid)) {
		return -EINVAL;
	}

	hv_pm_get_snapshot(&snapshot);
	switch (ioc.op) {
	case ACRN_PM_QUERY_CAPS:
		status = (ioc.epoch == 0UL) ? 0 : -EINVAL;
		break;
	case ACRN_PM_REQUEST_SUSPEND:
		if (caller_vmid != snapshot.controller_vmid) {
			status = -EPERM;
		} else if (ioc.epoch != 0UL) {
			status = -EINVAL;
		} else {
			status = hv_pm_request_suspend(caller_vmid);
			if (status == 0) {
				hv_pm_get_snapshot(&snapshot);
				status = bsp_pm_publish_prepare_events(&snapshot);
				if (status != 0) {
					(void)hv_pm_abort(snapshot.epoch, status);
				}
			}
		}
		break;
	case ACRN_PM_GET_EVENT:
	case ACRN_PM_GET_STATUS:
	case ACRN_PM_GET_WAKE_REASON:
		status = bsp_pm_ioc_epoch_is_current(ioc.epoch, &snapshot) ?
			0 : -EINVAL;
		break;
	case ACRN_PM_ABORT:
		if (caller_vmid != snapshot.controller_vmid) {
			status = -EPERM;
		} else if ((ioc.epoch == 0UL) || (ioc.epoch != snapshot.epoch)) {
			status = -EINVAL;
		} else {
			status = hv_pm_abort(ioc.epoch,
				(ioc.status != 0) ? ioc.status : -EAGAIN);
		}
		break;
	case ACRN_PM_RESUME_COMPLETE:
		status = ((ioc.epoch != 0UL) && (ioc.epoch == snapshot.epoch)) ?
			hv_pm_guest_resume_complete(caller_vmid, ioc.epoch) : -EINVAL;
		break;
	default:
		status = -EINVAL;
		break;
	}

	hv_pm_get_snapshot(&snapshot);
	bsp_pm_fill_ioc(&ioc, &snapshot, caller_vmid,
		((status == 0) && ((ioc.op == ACRN_PM_GET_EVENT) ||
		 (ioc.op == ACRN_PM_GET_STATUS))) ?
		 snapshot.vm[caller_vmid].status : status);
	copy_status = copy_to_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc));

	return (copy_status != 0) ? copy_status : status;
}
