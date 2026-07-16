/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <vm_wdt.h>
#include <vm.h>
#include <bsp/pm.h>
#include <bsp/vpci.h>
#include <passthrough.h>
#include <virtio_proxy.h>
#include <asm/irq.h>
#include <asm/vtd.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vtimer.h>

#define BSP_PM_MAX_WAKE_IRQS	8U
#define BSP_PM_HOOK_PRIO_WDT	100U
#define BSP_PM_HOOK_PRIO_VIRTIO	200U
#define BSP_PM_HOOK_PRIO_PTIRQ	500U
#define BSP_PM_HOOK_PRIO_VPCI	600U
#define BSP_PM_HOOK_PRIO_SMMU	700U
#define BSP_PM_HOOK_PRIO_VTIMER	800U
#define BSP_PM_HOOK_PRIO_VGIC	900U

/* [20260716] BSP STR retention framework
 *
 * core PM epoch/scope
 *       |
 *       +-- prepare --> virtio drain and QueueNotify gate
 *       |
 *       +-- suspend --> WDT -> passthrough IRQ -> vPCI -> SMMU
 *                                      -> vtimer -> vGIC
 *       |
 *       +-- resume/abort <------------- reverse dependency order
 *
 * System scope                         VM scope
 *   all required VMs                     target VM only
 *   passthrough IRQ + SMMU retained       passthrough ownership rejected
 *   platform entry follows hooks          host and other VMs keep running
 *
 * Key rule:
 *   - core/pm.c owns the transaction; this file only binds device providers;
 *   - each adapter validates the epoch and derives scope from one snapshot;
 *   - suspend proceeds from producers toward delivery state, while reverse
 *     order restores consumers only after their providers are usable;
 *   - VM-only STR fails closed when DMA or backend sharing prevents an
 *     isolation proof without guest cooperation.
 */
static uint32_t bsp_pm_wakeup_irqs[BSP_PM_MAX_WAKE_IRQS];
static uint16_t bsp_pm_wakeup_irq_count;
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

static int32_t bsp_pm_run_required_vm_resume_hook(uint64_t epoch,
	bsp_pm_vm_hook_fn hook)
{
	struct beau_pm_snapshot snapshot;
	uint16_t vmid;
	int32_t first_error = 0;

	if ((epoch == 0UL) || (hook == NULL)) {
		return -EINVAL;
	}
	hv_pm_get_snapshot(&snapshot);
	if (snapshot.epoch != epoch) {
		return -EINVAL;
	}

	/* Match the hook-level LIFO rule inside a multi-VM provider as well. */
	for (vmid = CONFIG_MAX_VM_NUM; vmid > 0U; vmid--) {
		struct acrn_vm *vm;
		int32_t status;
		uint16_t target_vmid = vmid - 1U;

		if ((snapshot.required_vm_mask & (1UL << target_vmid)) == 0UL) {
			continue;
		}
		vm = get_vm_from_vmid(target_vmid);
		status = (vm != NULL) ? hook(vm, epoch) : -ENODEV;
		if ((status != 0) && (first_error == 0)) {
			first_error = status;
		}
	}

	return first_error;
}

static int32_t bsp_pm_vtimer_suspend(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, arm64_vtimer_suspend_vm);
}

static int32_t bsp_pm_vtimer_resume(uint64_t epoch)
{
	return bsp_pm_run_required_vm_resume_hook(epoch, arm64_vtimer_resume_vm);
}

static int32_t bsp_pm_vgic_suspend(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, arm64_vgicv3_suspend_vm);
}

static int32_t bsp_pm_vgic_resume(uint64_t epoch)
{
	return bsp_pm_run_required_vm_resume_hook(epoch, arm64_vgicv3_resume_vm);
}

static int32_t bsp_pm_vpci_suspend(uint64_t epoch)
{
	return bsp_pm_run_required_vm_hook(epoch, vpci_pm_suspend);
}

static int32_t bsp_pm_vpci_resume(uint64_t epoch)
{
	return bsp_pm_run_required_vm_resume_hook(epoch, vpci_pm_resume);
}

static int32_t bsp_pm_get_epoch_snapshot(uint64_t epoch,
	struct beau_pm_snapshot *snapshot)
{
	if ((epoch == 0UL) || (snapshot == NULL)) {
		return -EINVAL;
	}

	/* Epoch validation prevents a late callback from touching a newer STR. */
	hv_pm_get_snapshot(snapshot);
	return (snapshot->epoch == epoch) ? 0 : -EINVAL;
}

static int32_t bsp_pm_get_vm_target(uint64_t epoch,
	struct beau_pm_snapshot *snapshot, uint16_t *target_vmid)
{
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, snapshot);

	if (status != 0) {
		return status;
	}
	if ((snapshot->scope != HV_PM_SCOPE_VM) ||
		(snapshot->target_vmid >= CONFIG_MAX_VM_NUM) ||
		((snapshot->required_vm_mask & (1UL << snapshot->target_vmid)) == 0UL)) {
		return -EINVAL;
	}
	if (target_vmid != NULL) {
		*target_vmid = snapshot->target_vmid;
	}

	return 0;
}

static int32_t bsp_pm_wdt_suspend(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint16_t target_vmid;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}
	if (snapshot.scope == HV_PM_SCOPE_VM) {
		status = bsp_pm_get_vm_target(epoch, &snapshot, &target_vmid);
		return (status == 0) ?
			vm_wdt_pm_suspend_vm(target_vmid, epoch) : status;
	}

	return vm_wdt_pm_suspend(epoch);
}

static int32_t bsp_pm_wdt_resume(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint16_t target_vmid;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}
	if (snapshot.scope == HV_PM_SCOPE_VM) {
		status = bsp_pm_get_vm_target(epoch, &snapshot, &target_vmid);
		return (status == 0) ?
			vm_wdt_pm_resume_vm(target_vmid, epoch) : status;
	}

	return vm_wdt_pm_resume(epoch);
}

static int32_t bsp_pm_virtio_suspend(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint16_t target_vmid;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}
	if (snapshot.scope == HV_PM_SCOPE_VM) {
		status = bsp_pm_get_vm_target(epoch, &snapshot, &target_vmid);
		return (status == 0) ?
			virtio_proxy_pm_suspend_vm(target_vmid, epoch) : status;
	}

	return virtio_proxy_pm_suspend(epoch);
}

static int32_t bsp_pm_virtio_resume(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint16_t target_vmid;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}
	if (snapshot.scope == HV_PM_SCOPE_VM) {
		status = bsp_pm_get_vm_target(epoch, &snapshot, &target_vmid);
		return (status == 0) ?
			virtio_proxy_pm_resume_vm(target_vmid, epoch) : status;
	}

	return virtio_proxy_pm_resume(epoch);
}

static int32_t bsp_pm_passthrough_suspend(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint16_t target_vmid;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}
	if (snapshot.scope == HV_PM_SCOPE_VM) {
		status = bsp_pm_get_vm_target(epoch, &snapshot, &target_vmid);
		if (status != 0) {
			return status;
		}
		return passthrough_vm_has_owned_devices(target_vmid) ?
			-ENOTSUP : 0;
	}

	return passthrough_pm_suspend(epoch, snapshot.required_vm_mask);
}

static int32_t bsp_pm_passthrough_resume(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}

	return (snapshot.scope == HV_PM_SCOPE_VM) ? 0 :
		passthrough_pm_resume(epoch);
}

static int32_t bsp_pm_smmu_suspend(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}

	return (snapshot.scope == HV_PM_SCOPE_VM) ? 0 :
		arm_smmu_pm_suspend(epoch);
}

static int32_t bsp_pm_smmu_resume(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	int32_t status = bsp_pm_get_epoch_snapshot(epoch, &snapshot);

	if (status != 0) {
		return status;
	}

	return (snapshot.scope == HV_PM_SCOPE_VM) ? 0 :
		arm_smmu_pm_resume(epoch);
}

static int32_t bsp_pm_register_retention_hooks(void)
{
	/*
	 * Priorities encode dependencies. In particular vGIC is retained after
	 * vPCI/MSI and timers, then restored before they can deliver new IRQ state.
	 * vPCI disables bus mastering before SMMU switches DMA to abort mode; the
	 * passthrough hook keeps physical IRQs masked across that retention window.
	 */
	static const struct beau_pm_ops retention_hooks[] = {
		{
			.name = "vm-wdt",
			.priority = BSP_PM_HOOK_PRIO_WDT,
			.suspend = bsp_pm_wdt_suspend,
			.resume = bsp_pm_wdt_resume,
			.abort = bsp_pm_wdt_resume,
		},
		{
			.name = "virtio-proxy",
			.priority = BSP_PM_HOOK_PRIO_VIRTIO,
			.prepare = bsp_pm_virtio_suspend,
			.resume = bsp_pm_virtio_resume,
			.abort = bsp_pm_virtio_resume,
		},
		{
			.name = "passthrough-irqs",
			.priority = BSP_PM_HOOK_PRIO_PTIRQ,
			.suspend = bsp_pm_passthrough_suspend,
			.resume = bsp_pm_passthrough_resume,
			.abort = bsp_pm_passthrough_resume,
		},
		{
			.name = "vpci",
			.priority = BSP_PM_HOOK_PRIO_VPCI,
			.suspend = bsp_pm_vpci_suspend,
			.resume = bsp_pm_vpci_resume,
			.abort = bsp_pm_vpci_resume,
		},
		{
			.name = "arm-smmuv3",
			.priority = BSP_PM_HOOK_PRIO_SMMU,
			.suspend = bsp_pm_smmu_suspend,
			.resume = bsp_pm_smmu_resume,
			.abort = bsp_pm_smmu_resume,
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

	/* Only policy-listed physical sources may turn an IRQ into an STR wake. */
	for (idx = 0U; idx < count; idx++) {
		bsp_pm_wakeup_irqs[idx] = irqs[idx];
	}
	bsp_pm_wakeup_irq_count = count;

	return 0;
}

int32_t bsp_pm_request_suspend(void)
{
	struct beau_pm_snapshot snapshot;
	int32_t status = bsp_pm_register_retention_hooks();

	if (status != 0) {
		return status;
	}
	hv_pm_get_snapshot(&snapshot);
	return hv_pm_request_suspend(snapshot.controller_vmid);
}

int32_t bsp_pm_suspend_vm(uint16_t vmid)
{
	int32_t status;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return -EINVAL;
	}
	status = bsp_pm_register_retention_hooks();
	if (status != 0) {
		return status;
	}

	/*
	 * VM STR is transparent and EL2-owned. Refuse topologies that cannot be
	 * proven isolated without guest/backend cooperation:
	 *
	 *   target owns passthrough DMA  -> no VM-only quiesce proof
	 *   target serves other frontend -> backend freeze breaks a running VM
	 */
	if (passthrough_vm_has_owned_devices(vmid)) {
		return -ENOTSUP;
	}
	if (virtio_proxy_vm_has_backend_dependents(vmid)) {
		return -EBUSY;
	}

	return hv_pm_request_vm_suspend(vmid);
}

int32_t bsp_pm_resume_vm(uint16_t vmid)
{
	int32_t status;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return -EINVAL;
	}
	status = bsp_pm_register_retention_hooks();
	if (status != 0) {
		return status;
	}

	return hv_pm_request_vm_resume(vmid);
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
