/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
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
