/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <bits.h>
#include <cpu.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <ticks.h>
#include <vm.h>
#include <asm/guest/vm_reset.h>

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

__attribute__((weak)) int32_t platform_pm_enter(__unused uint64_t epoch)
{
	return -ENOSYS;
}

static uint64_t hv_pm_next_epoch(uint64_t epoch)
{
	epoch++;
	return (epoch != 0UL) ? epoch : 1UL;
}

static void hv_pm_transition_locked(struct beau_pm_snapshot *data,
	enum beau_pm_system_state next)
{
	uint64_t now = cpu_ticks();
	uint32_t current = data->state;

	if ((current < HV_PM_PHASE_COUNT) &&
		(data->phase_start_ticks[current] != 0UL)) {
		data->phase_duration_ticks[current] =
			now - data->phase_start_ticks[current];
	}
	data->state = next;
	if ((uint32_t)next < HV_PM_PHASE_COUNT) {
		data->phase_start_ticks[next] = now;
	}
}

static void hv_pm_begin_abort_locked(struct beau_pm_snapshot *data,
	int32_t reason)
{
	uint32_t failed_phase = data->state;

	if (data->state != PM_ABORTING) {
		hv_pm_transition_locked(data, PM_ABORTING);
		data->last_epoch = data->epoch;
		data->last_state = PM_ABORTING;
		data->last_status = reason;
		data->last_error.epoch = data->epoch;
		data->last_error.phase = failed_phase;
		data->last_error.vmid = data->initiator_vmid;
		data->last_error.status = reason;
	}
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
	(void)memset(data->phase_start_ticks, 0U, sizeof(data->phase_start_ticks));
	(void)memset(data->phase_duration_ticks, 0U,
		sizeof(data->phase_duration_ticks));
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

	data->phase_start_ticks[PM_PREPARING] = cpu_ticks();
	data->state = PM_PREPARING;
}

int32_t hv_pm_set_policy(const struct beau_pm_policy *policy)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t valid_vm_mask = (1UL << CONFIG_MAX_VM_NUM) - 1UL;
	uint64_t flags;
	int32_t status = 0;

	if ((policy == NULL) || (policy->controller_vmid >= CONFIG_MAX_VM_NUM) ||
		((policy->required_vm_mask & ~valid_vm_mask) != 0UL) ||
		(policy->enabled > 1U) ||
		(policy->platform_mode > HV_PM_PLATFORM_STRICT) ||
		((policy->enabled == 0U) &&
		 (policy->platform_mode != HV_PM_PLATFORM_DISABLED)) ||
		((policy->enabled != 0U) &&
		 ((policy->required_vm_mask == 0UL) ||
		  ((policy->required_vm_mask & (1UL << policy->controller_vmid)) == 0UL) ||
		  (policy->prepare_timeout_ms == 0U) ||
		  (policy->resume_timeout_ms == 0U) ||
		  (policy->platform_mode == HV_PM_PLATFORM_DISABLED)))) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (data->state != PM_RUNNING) {
		status = -EBUSY;
	} else {
		data->controller_vmid = policy->controller_vmid;
		data->policy_required_vm_mask = policy->required_vm_mask;
		data->prepare_timeout_ms = policy->prepare_timeout_ms;
		data->resume_timeout_ms = policy->resume_timeout_ms;
		data->enabled = policy->enabled;
		data->platform_mode = policy->platform_mode;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

int32_t hv_pm_request_suspend(uint16_t initiator_vmid)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	hv_pm_finalize_hooks();
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (data->enabled == 0U) {
		status = -ENODEV;
	} else if (initiator_vmid != data->controller_vmid) {
		status = -EACCES;
	} else if (data->state != PM_RUNNING) {
		status = -EBUSY;
	} else {
		hv_pm_reset_epoch_locked(data, initiator_vmid,
			data->policy_required_vm_mask);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

int32_t hv_pm_abort(uint64_t epoch, int32_t reason)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((epoch == 0UL) || (epoch != data->epoch) ||
		(data->state == PM_RUNNING)) {
		status = -EINVAL;
	} else {
		hv_pm_begin_abort_locked(data, reason);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (status == 0) {
		make_system_suspend_request(BSP_CPU_ID);
	}

	return status;
}

void make_system_suspend_request(uint16_t pcpu_id)
{
	if (pcpu_id >= get_pcpu_nums()) {
		return;
	}

	bitmap_set(NEED_SYSTEM_SUSPEND, &per_cpu(pcpu_flag, pcpu_id));
	make_reschedule_request(pcpu_id);
}

bool has_system_suspend_request(uint16_t pcpu_id)
{
	return (pcpu_id < get_pcpu_nums()) &&
		bitmap_test(NEED_SYSTEM_SUSPEND, &per_cpu(pcpu_flag, pcpu_id));
}

bool need_system_suspend(uint16_t pcpu_id)
{
	return (pcpu_id < get_pcpu_nums()) &&
		bitmap_test_and_clear(NEED_SYSTEM_SUSPEND,
			&per_cpu(pcpu_flag, pcpu_id));
}

int32_t hv_pm_record_wake(uint32_t wake_source, uint16_t source_index)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	if (source_index >= 64U) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (data->state == PM_RUNNING) {
		status = -EINVAL;
	} else {
		if (data->wake_reason == 0UL) {
			data->wake_reason = wake_source;
		}
		data->wake_bitmap |= 1UL << source_index;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

int32_t hv_pm_mark_vm_suspended(uint16_t vmid, uint64_t epoch,
	uint64_t resume_entry, uint64_t resume_context)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	struct beau_vm_pm_record *record;
	uint64_t vm_mask;
	uint64_t flags;
	int32_t status = 0;
	bool queue_idle = false;

	if ((vmid >= CONFIG_MAX_VM_NUM) || (epoch == 0UL)) {
		return -EINVAL;
	}

	vm_mask = 1UL << vmid;
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	record = &data->vm[vmid];
	if ((data->state != PM_PREPARING) || (data->epoch != epoch) ||
		((data->required_vm_mask & vm_mask) == 0UL) ||
		((data->ready_vm_mask & vm_mask) != 0UL) ||
		(record->epoch != epoch) ||
		((record->state != VM_PM_PREPARE_SENT) &&
		 (record->state != VM_PM_SUSPEND_PENDING))) {
		status = -EINVAL;
	} else {
		record->resume_entry = resume_entry;
		record->context_id = resume_context;
		record->status = 0;
		record->state = VM_PM_SUSPENDED;
		data->ready_vm_mask |= vm_mask;
		data->resume_pending_vm_mask |= vm_mask;
		if (data->ready_vm_mask == data->required_vm_mask) {
			hv_pm_transition_locked(data, PM_GUESTS_QUIESCED);
			queue_idle = true;
		}
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (queue_idle) {
		make_system_suspend_request(BSP_CPU_ID);
	}

	return status;
}

int32_t hv_pm_resume_vm(uint16_t vmid, uint64_t epoch)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	struct beau_vm_pm_record *record;
	uint64_t resume_entry = 0UL;
	uint64_t resume_context = 0UL;
	uint64_t vm_mask;
	uint64_t flags;
	int32_t status = 0;

	if ((vmid >= CONFIG_MAX_VM_NUM) || (epoch == 0UL)) {
		return -EINVAL;
	}

	vm_mask = 1UL << vmid;
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	record = &data->vm[vmid];
	if ((data->epoch != epoch) ||
		((data->state != PM_RESUMING_GUESTS) &&
		 (data->state != PM_ABORTING)) ||
		((data->resume_pending_vm_mask & vm_mask) == 0UL) ||
		(record->epoch != epoch) || (record->state != VM_PM_SUSPENDED)) {
		status = -EINVAL;
	} else {
		resume_entry = record->resume_entry;
		resume_context = record->context_id;
		data->resume_pending_vm_mask &= ~vm_mask;
		record->state = VM_PM_RESUMING;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	if (status == 0) {
		status = arm64_vpsci_resume_vm(get_vm_from_vmid(vmid), epoch,
			resume_entry, resume_context);
		if (status != 0) {
			spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
			record->state = VM_PM_FAILED;
			record->status = status;
			spinlock_irqrestore_release(&pm_transaction.lock, flags);
		}
	}

	return status;
}

int32_t hv_pm_guest_resume_complete(uint16_t vmid, uint64_t epoch)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	struct beau_vm_pm_record *record;
	uint64_t vm_mask;
	uint64_t flags;
	int32_t status = 0;

	if ((vmid >= CONFIG_MAX_VM_NUM) || (epoch == 0UL)) {
		return -EINVAL;
	}

	vm_mask = 1UL << vmid;
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	record = &data->vm[vmid];
	if ((data->epoch != epoch) || (data->state != PM_RESUMING_GUESTS) ||
		((data->required_vm_mask & vm_mask) == 0UL) ||
		(record->epoch != epoch) || (record->state != VM_PM_RESUMING)) {
		status = -EINVAL;
	} else {
		record->state = VM_PM_RUNNING;
		record->status = 0;
		data->ready_vm_mask &= ~vm_mask;
		if ((data->ready_vm_mask == 0UL) &&
			(data->resume_pending_vm_mask == 0UL)) {
			data->last_epoch = epoch;
			data->last_state = PM_RESUMING_GUESTS;
			data->last_status = 0;
			data->io_gated = 0U;
			hv_pm_transition_locked(data, PM_RUNNING);
		}
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

enum hv_pm_idle_action {
	HV_PM_IDLE_NONE = 0U,
	HV_PM_IDLE_FREEZE,
	HV_PM_IDLE_ABORT,
};

static enum hv_pm_idle_action hv_pm_claim_from_idle(uint16_t pcpu_id,
	uint64_t *epoch, uint64_t *required_vm_mask)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	enum hv_pm_idle_action action = HV_PM_IDLE_NONE;
	uint64_t flags;

	if ((pcpu_id != BSP_CPU_ID) || (epoch == NULL) ||
		(required_vm_mask == NULL)) {
		return action;
	}

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (data->state == PM_ABORTING) {
		action = HV_PM_IDLE_ABORT;
	} else if ((data->state == PM_GUESTS_QUIESCED) &&
		(data->ready_vm_mask == data->required_vm_mask)) {
		hv_pm_transition_locked(data, PM_FREEZING_HOST);
		action = HV_PM_IDLE_FREEZE;
	} else if (data->state == PM_FREEZING_HOST) {
		action = HV_PM_IDLE_FREEZE;
	} else if (data->state == PM_GUESTS_QUIESCED) {
		hv_pm_begin_abort_locked(data, -EFAULT);
		action = HV_PM_IDLE_ABORT;
	}

	if (action != HV_PM_IDLE_NONE) {
		*epoch = data->epoch;
		*required_vm_mask = data->required_vm_mask;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return action;
}

static bool hv_pm_guest_bsps_are_blocked(uint64_t epoch,
	uint64_t required_vm_mask)
{
	uint16_t vmid;

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct acrn_vm *vm;
		struct acrn_vcpu *bsp;
		bool blocked;

		if ((required_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}

		vm = get_vm_from_vmid(vmid);
		get_vm_lock(vm);
		bsp = vcpu_from_vid(vm, BSP_CPU_ID);
		blocked = vm->arch_vm.pm.valid &&
			(vm->arch_vm.pm.epoch == epoch) && is_vcpu_running(bsp) &&
			(bsp->thread_obj.status == THREAD_STS_BLOCKED);
		put_vm_lock(vm);
		if (!blocked) {
			return false;
		}
	}

	return true;
}

static bool hv_pm_freeze_wait_expired(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint64_t start;

	hv_pm_get_snapshot(&snapshot);
	if ((snapshot.epoch != epoch) || (snapshot.state == PM_ABORTING)) {
		return true;
	}
	start = snapshot.phase_start_ticks[PM_FREEZING_HOST];

	return (start != 0UL) &&
		(ticks_to_ms(cpu_ticks() - start) >= snapshot.prepare_timeout_ms);
}

static bool hv_pm_abort_wait_expired(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint64_t start;

	hv_pm_get_snapshot(&snapshot);
	if ((snapshot.epoch != epoch) || (snapshot.state != PM_ABORTING)) {
		return true;
	}
	start = snapshot.phase_start_ticks[PM_ABORTING];

	return (start != 0UL) &&
		(ticks_to_ms(cpu_ticks() - start) >= snapshot.prepare_timeout_ms);
}

static bool hv_pm_entry_is_allowed(uint64_t epoch)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	bool allowed;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	allowed = (data->epoch == epoch) &&
		(data->state == PM_FREEZING_HOST) &&
		(data->wake_bitmap == 0UL);
	if ((data->epoch == epoch) && (data->state == PM_FREEZING_HOST) &&
		(data->wake_bitmap != 0UL)) {
		hv_pm_begin_abort_locked(data, -EAGAIN);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return allowed;
}

static int32_t hv_pm_set_epoch_state(uint64_t epoch,
	enum beau_pm_system_state expected, enum beau_pm_system_state next)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->epoch != epoch) || (data->state != expected)) {
		status = -EINVAL;
	} else {
		hv_pm_transition_locked(data, next);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

static int32_t hv_pm_publish_suspended(uint64_t epoch)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->epoch != epoch) || (data->state != PM_FREEZING_HOST)) {
		status = -EINVAL;
	} else if (data->wake_bitmap != 0UL) {
		hv_pm_begin_abort_locked(data, -EAGAIN);
		status = -EAGAIN;
	} else {
		hv_pm_transition_locked(data, PM_SUSPENDED);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

static int32_t hv_pm_resume_pending_contexts(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint16_t vmid;
	int32_t first_error = 0;
	int32_t status;

	/* Ascending VM ID is the initial provider-first resume order. */
	hv_pm_get_snapshot(&snapshot);
	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		if ((snapshot.resume_pending_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}
		status = hv_pm_resume_vm(vmid, epoch);
		if ((status != 0) && (first_error == 0)) {
			first_error = status;
		}
	}

	return first_error;
}

static void hv_pm_finish_abort(uint64_t epoch, int32_t rollback_status)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint16_t vmid;
	uint64_t flags;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->epoch == epoch) && (data->state == PM_ABORTING)) {
		if ((data->last_status == 0) && (rollback_status != 0)) {
			data->last_status = rollback_status;
		}
		data->ready_vm_mask = 0UL;
		data->resume_pending_vm_mask = 0UL;
		data->completed_hook_mask = 0UL;
		data->io_gated = 0U;
		for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
			if ((data->required_vm_mask & (1UL << vmid)) != 0UL) {
				data->vm[vmid].state = VM_PM_RUNNING;
			}
		}
		hv_pm_transition_locked(data, PM_RUNNING);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
}

static void hv_pm_fail_epoch(uint64_t epoch, int32_t status);

static void hv_pm_rollback_from_idle(uint64_t epoch, int32_t reason)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t hook_status;
	int32_t guest_status;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->epoch == epoch) && (data->state != PM_ABORTING) &&
		(data->state != PM_RUNNING)) {
		hv_pm_begin_abort_locked(data, reason);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	hook_status = hv_pm_run_abort(epoch);
	guest_status = hv_pm_resume_pending_contexts(epoch);
	if ((hook_status != 0) || (guest_status != 0)) {
		hv_pm_fail_epoch(epoch,
			(hook_status != 0) ? hook_status : guest_status);
	} else {
		hv_pm_finish_abort(epoch, 0);
	}
}

static void hv_pm_fail_epoch(uint64_t epoch, int32_t status)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	uint32_t failed_phase;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->epoch == epoch) && (data->state != PM_RUNNING)) {
		failed_phase = data->state;
		hv_pm_transition_locked(data, PM_FAILED);
		data->last_epoch = epoch;
		data->last_state = PM_FAILED;
		data->last_status = status;
		data->last_error.epoch = epoch;
		data->last_error.phase = failed_phase;
		data->last_error.vmid = data->initiator_vmid;
		data->last_error.status = status;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
}

void hv_pm_process_from_idle(uint16_t pcpu_id)
{
	uint64_t epoch = 0UL;
	uint64_t required_vm_mask = 0UL;
	enum hv_pm_idle_action action;
	int32_t status;

	action = hv_pm_claim_from_idle(pcpu_id, &epoch, &required_vm_mask);
	if (action == HV_PM_IDLE_NONE) {
		return;
	}
	if (action == HV_PM_IDLE_ABORT) {
		struct beau_pm_snapshot snapshot;

		hv_pm_get_snapshot(&snapshot);
		if (!hv_pm_guest_bsps_are_blocked(epoch,
			snapshot.resume_pending_vm_mask)) {
			if (hv_pm_abort_wait_expired(epoch)) {
				hv_pm_fail_epoch(epoch, -ETIMEDOUT);
			} else {
				asm_pause();
				make_system_suspend_request(BSP_CPU_ID);
			}
			return;
		}
		hv_pm_rollback_from_idle(epoch, -EIO);
		return;
	}

	/* The last vPSCI publisher may not have reached schedule() yet. */
	if (!hv_pm_guest_bsps_are_blocked(epoch, required_vm_mask)) {
		if (hv_pm_freeze_wait_expired(epoch)) {
			hv_pm_rollback_from_idle(epoch, -ETIMEDOUT);
		} else {
			asm_pause();
			make_system_suspend_request(BSP_CPU_ID);
		}
		return;
	}

	if (!hv_pm_entry_is_allowed(epoch)) {
		hv_pm_rollback_from_idle(epoch, -EAGAIN);
		return;
	}
	status = hv_pm_run_prepare(epoch);
	if ((status != 0) || !hv_pm_entry_is_allowed(epoch)) {
		hv_pm_rollback_from_idle(epoch,
			(status != 0) ? status : -EAGAIN);
		return;
	}
	status = hv_pm_run_suspend(epoch);
	if ((status != 0) || !hv_pm_entry_is_allowed(epoch)) {
		hv_pm_rollback_from_idle(epoch,
			(status != 0) ? status : -EAGAIN);
		return;
	}
	status = hv_pm_publish_suspended(epoch);
	if (status != 0) {
		hv_pm_rollback_from_idle(epoch, status);
		return;
	}

	status = platform_pm_enter(epoch);
	if (status != 0) {
		hv_pm_rollback_from_idle(epoch, status);
		return;
	}
	status = hv_pm_set_epoch_state(epoch, PM_SUSPENDED, PM_RESTORING_HOST);
	if (status == 0) {
		status = hv_pm_run_resume(epoch);
	}
	if (status == 0) {
		status = hv_pm_set_epoch_state(epoch, PM_RESTORING_HOST,
			PM_RESUMING_GUESTS);
	}
	if (status == 0) {
		status = hv_pm_resume_pending_contexts(epoch);
	}
	if (status != 0) {
		hv_pm_fail_epoch(epoch, status);
	}
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
