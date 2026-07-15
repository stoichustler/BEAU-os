/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <rtl.h>
#include <vm_config.h>

/* [20260715] Coordinated guest STR transaction
 *
 * PM controller       Guest OSes          BEAU PM owner          Platform
 *      |                    |                    |                    |
 *      | request(epoch)     |                    |                    |
 *      +---------------------------------------->| PREPARING          |
 *      |                    |<-- prepare IRQ ----|                    |
 *      |                    | freeze OS/devices  |                    |
 *      |                    | offline AP vCPUs   |                    |
 *      |                    | SYSTEM_SUSPEND     |                    |
 *      |                    +------------------->| save entry/context |
 *      |                    |     BSP blocked    | mark VM ready      |
 *      |                    |                    |                    |
 *      |                    | all required ready |                    |
 *      |                    |                    | FREEZING_HOST      |
 *      |                    |                    | quiesce hooks      |
 *      |                    |                    | stop secondary CPU |
 *      |                    |                    | save EL2 context   |
 *      |                    |                    +------------------->|
 *      |                    |                    |     suspended      |
 *      |                    |                    |<---- wake source --|
 *      |                    |                    | restore host       |
 *      |                    |<-- entry/x0 -------| resume providers   |
 *      |                    | resume OS/devices  |                    |
 *      |                    |-- resume complete->| resume consumers   |
 *      |                    |                    | RUNNING            |
 *
 * Key rules:
 *   - the BSP idle thread is the only owner allowed to freeze or restore EL2;
 *   - vPSCI exits publish guest readiness before blocking the calling BSP;
 *   - suspend callbacks run in dependency order and rollback in reverse order;
 *   - a pending wake event aborts before platform entry and is never dropped.
 */
static struct beau_pm_transaction pm_transaction = {
	.data = {
		.state = PM_RUNNING,
		.last_state = PM_RUNNING,
		.controller_vmid = HV_PM_DEFAULT_CONTROLLER_VM,
	},
};

static spinlock_t pm_hook_lock;
static struct beau_pm_ops pm_hooks[HV_PM_MAX_HOOKS];
static uint16_t pm_hook_count;
static bool pm_hooks_finalized;

static uint64_t hv_pm_next_epoch(uint64_t epoch)
{
	epoch++;
	return (epoch != 0UL) ? epoch : 1UL;
}

static uint64_t hv_pm_configured_vm_mask(void)
{
	uint64_t mask = 0UL;
	uint16_t vmid;

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		if (get_vm_config(vmid)->cpu_affinity != 0UL) {
			mask |= 1UL << vmid;
		}
	}

	return mask;
}

static void hv_pm_reset_epoch_locked(struct beau_pm_snapshot *data,
	uint16_t initiator_vmid, uint64_t required_vm_mask)
{
	uint16_t vmid;

	data->epoch = hv_pm_next_epoch(data->epoch);
	data->required_vm_mask = required_vm_mask;
	data->ready_vm_mask = 0UL;
	data->resume_pending_vm_mask = 0UL;
	data->completed_hook_mask = 0UL;
	data->wake_reason = 0UL;
	data->wake_bitmap = 0UL;
	data->initiator_vmid = initiator_vmid;
	data->io_gated = 1U;
	(void)memset(&data->last_error, 0U, sizeof(data->last_error));

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct beau_vm_pm_record *record = &data->vm[vmid];

		(void)memset(record, 0U, sizeof(*record));
		record->epoch = data->epoch;
		record->vmid = vmid;
		record->required = ((required_vm_mask & (1UL << vmid)) != 0UL) ? 1U : 0U;
		record->state = record->required ? VM_PM_PREPARE_SENT : VM_PM_RUNNING;
	}

	data->state = PM_PREPARING;
}

int32_t hv_pm_request_suspend(uint16_t initiator_vmid)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (initiator_vmid != data->controller_vmid) {
		status = -EACCES;
	} else if (data->state != PM_RUNNING) {
		status = -EBUSY;
	} else {
		hv_pm_reset_epoch_locked(data, initiator_vmid,
			hv_pm_configured_vm_mask());
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

int32_t hv_pm_abort(uint64_t epoch, int32_t reason)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	uint32_t failed_phase;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((epoch == 0UL) || (epoch != data->epoch) ||
		(data->state == PM_RUNNING)) {
		status = -EINVAL;
	} else {
		failed_phase = data->state;
		data->state = PM_ABORTING;
		data->last_epoch = data->epoch;
		data->last_state = PM_ABORTING;
		data->last_status = reason;
		data->last_error.epoch = data->epoch;
		data->last_error.phase = failed_phase;
		data->last_error.vmid = data->initiator_vmid;
		data->last_error.status = reason;
		data->ready_vm_mask = 0UL;
		data->resume_pending_vm_mask = 0UL;
		data->completed_hook_mask = 0UL;
		data->io_gated = 0U;
		data->state = PM_RUNNING;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

void hv_pm_get_snapshot(struct beau_pm_snapshot *snapshot)
{
	uint64_t flags;

	if (snapshot == NULL) {
		return;
	}

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	(void)memcpy_s(snapshot, sizeof(*snapshot), &pm_transaction.data,
		sizeof(pm_transaction.data));
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
}

bool hv_pm_io_is_gated(void)
{
	uint64_t flags;
	bool gated;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	gated = pm_transaction.data.io_gated != 0U;
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return gated;
}

int32_t hv_pm_register_hook(const struct beau_pm_ops *ops)
{
	uint64_t flags;
	uint16_t idx;
	uint16_t insert;
	int32_t status = 0;

	if ((ops == NULL) || (ops->name == NULL) || (ops->name[0] == '\0') ||
		(ops->priority == 0U)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&pm_hook_lock, &flags);
	if (pm_hooks_finalized) {
		status = -EPERM;
	} else if (pm_hook_count >= HV_PM_MAX_HOOKS) {
		status = -ENOMEM;
	} else {
		for (idx = 0U; idx < pm_hook_count; idx++) {
			if (strcmp(pm_hooks[idx].name, ops->name) == 0) {
				status = -EINVAL;
				break;
			}
		}
	}

	if (status == 0) {
		insert = pm_hook_count;
		while ((insert > 0U) &&
			(pm_hooks[insert - 1U].priority > ops->priority)) {
			pm_hooks[insert] = pm_hooks[insert - 1U];
			insert--;
		}
		pm_hooks[insert] = *ops;
		pm_hook_count++;
	}
	spinlock_irqrestore_release(&pm_hook_lock, flags);

	return status;
}

void hv_pm_finalize_hooks(void)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&pm_hook_lock, &flags);
	pm_hooks_finalized = true;
	spinlock_irqrestore_release(&pm_hook_lock, flags);
}

static int32_t hv_pm_get_hook_count(uint16_t *count)
{
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_hook_lock, &flags);
	if (!pm_hooks_finalized) {
		status = -EPERM;
	} else {
		*count = pm_hook_count;
	}
	spinlock_irqrestore_release(&pm_hook_lock, flags);

	return status;
}

static int32_t hv_pm_set_hook_completed(uint64_t epoch, uint16_t idx)
{
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((epoch == 0UL) || (epoch != pm_transaction.data.epoch) ||
		(pm_transaction.data.state == PM_RUNNING)) {
		status = -EINVAL;
	} else {
		pm_transaction.data.completed_hook_mask |= 1UL << idx;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

static void hv_pm_clear_hook_completed(uint16_t idx)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	pm_transaction.data.completed_hook_mask &= ~(1UL << idx);
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
}

static int32_t hv_pm_run_forward(uint64_t epoch, bool suspend_phase)
{
	uint16_t count;
	uint16_t idx;
	int32_t status = hv_pm_get_hook_count(&count);

	if (status != 0) {
		return status;
	}

	for (idx = 0U; idx < count; idx++) {
		beau_pm_hook_fn callback = suspend_phase ?
			pm_hooks[idx].suspend : pm_hooks[idx].prepare;

		if (callback != NULL) {
			status = callback(epoch);
			if (status != 0) {
				break;
			}
		}
		status = hv_pm_set_hook_completed(epoch, idx);
		if (status != 0) {
			break;
		}
	}

	return status;
}

int32_t hv_pm_run_prepare(uint64_t epoch)
{
	return hv_pm_run_forward(epoch, false);
}

int32_t hv_pm_run_suspend(uint64_t epoch)
{
	return hv_pm_run_forward(epoch, true);
}

static int32_t hv_pm_run_reverse(uint64_t epoch, bool abort_phase)
{
	struct beau_pm_snapshot snapshot;
	uint16_t count;
	uint16_t idx;
	int32_t first_error = 0;
	int32_t status = hv_pm_get_hook_count(&count);

	if (status != 0) {
		return status;
	}

	hv_pm_get_snapshot(&snapshot);
	if ((epoch == 0UL) || (epoch != snapshot.epoch)) {
		return -EINVAL;
	}

	for (idx = count; idx > 0U; idx--) {
		uint16_t hook_idx = idx - 1U;
		beau_pm_hook_fn callback;

		if ((snapshot.completed_hook_mask & (1UL << hook_idx)) == 0UL) {
			continue;
		}
		callback = abort_phase ? pm_hooks[hook_idx].abort :
			pm_hooks[hook_idx].resume;
		if (callback != NULL) {
			status = callback(epoch);
			if ((status != 0) && (first_error == 0)) {
				first_error = status;
			}
		}
		hv_pm_clear_hook_completed(hook_idx);
	}

	return first_error;
}

int32_t hv_pm_run_resume(uint64_t epoch)
{
	return hv_pm_run_reverse(epoch, false);
}

int32_t hv_pm_run_abort(uint64_t epoch)
{
	return hv_pm_run_reverse(epoch, true);
}

const char *hv_pm_state_to_str(enum beau_pm_system_state state)
{
	static const char *const names[] = {
		[PM_RUNNING] = "running",
		[PM_PREPARING] = "preparing",
		[PM_GUESTS_QUIESCED] = "guests-quiesced",
		[PM_FREEZING_HOST] = "freezing-host",
		[PM_SUSPENDED] = "suspended",
		[PM_RESTORING_HOST] = "restoring-host",
		[PM_RESUMING_GUESTS] = "resuming-guests",
		[PM_ABORTING] = "aborting",
		[PM_FAILED] = "failed",
	};

	return ((uint32_t)state < ARRAY_SIZE(names)) ? names[state] : "invalid";
}
