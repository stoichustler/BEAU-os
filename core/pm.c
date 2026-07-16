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
#include <vcpu.h>
#include <vm.h>
#include <logmsg.h>

/* [20260716] BEAU-owned transparent suspend
 *
 * request       PM transaction       schedulers          retention/platform
 *    |                 |                  |                       |
 *    +---------------->| gate target I/O  |                       |
 *    |                 | prepare/drain -------------------------->|
 *    |                 | freeze(epoch) -->| block target threads  |
 *    |                 |<-- switch-out ACK|                       |
 *    |                 | suspend target devices ----------------->|
 *    |                 |                                          |
 *    |                 +-- VM scope: wait for explicit resume     |
 *    |                 +-- system: retain EL2 / wait for wake --->|
 *    |                 |<----------------------- restore Host EL2 |
 *    |                 | resume devices ------------------------->|
 *    |                 | thaw(epoch) ---->| replay deferred wakes |
 *    |                 | clear I/O gate   |                       |
 *
 * Key rules:
 *   - Guest software never participates in the transaction and sees no PM ABI;
 *   - one transaction owns either one VM or the policy-defined system VM set;
 *   - scheduler epoch gates preserve vCPU lifecycle states and serialize wakes;
 *   - device retention starts only after every gated vCPU has switched out;
 *   - restore hooks and vCPU thaw finish before target I/O is ungated;
 *   - an unprovable restore leaves PM_FAILED with guests and I/O still frozen.
 *
 * State flow:
 *
 *   RUNNING -> PREPARING -> FREEZING_HOST -> SUSPENDED
 *      ^                                       |
 *      |                         resume/wake   v
 *      +------------------------------- RESTORING_HOST
 *      ^
 *      |
 *   ABORTING <---------- explicit abort/error in any active phase
 *      |
 *      +--> PM_FAILED if rollback cannot prove restored isolation
 */
static struct beau_pm_transaction pm_transaction = {
	.data = {
		.state = PM_RUNNING,
		.last_state = PM_RUNNING,
		.controller_vmid = HV_PM_DEFAULT_CONTROLLER_VM,
		.target_vmid = CONFIG_MAX_VM_NUM,
		.scope = HV_PM_SCOPE_NONE,
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

static void hv_pm_record_error_locked(struct beau_pm_snapshot *data,
	uint64_t epoch, uint32_t phase, int32_t status, uint16_t vmid)
{
	data->last_epoch = epoch;
	data->last_state = phase;
	data->last_status = status;
	data->last_error.epoch = epoch;
	data->last_error.phase = phase;
	data->last_error.status = status;
	data->last_error.vmid = vmid;
}

static void hv_pm_fail_epoch(uint64_t epoch, int32_t status)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	bool failed = false;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->epoch == epoch) && (data->state != PM_RUNNING)) {
		uint32_t failed_phase = data->state;

		hv_pm_record_error_locked(data, epoch, failed_phase, status,
			data->initiator_vmid);
		hv_pm_transition_locked(data, PM_FAILED);
		failed = true;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (failed) {
		LOG_ERR("STR: PM_FAILED epoch:%lu status:%d", epoch, status);
	}
}

static bool hv_pm_required_vms_running(uint64_t required_vm_mask)
{
	uint16_t vmid;

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct acrn_vm *vm;

		if ((required_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}
		vm = get_vm_from_vmid(vmid);
		if ((vm == NULL) || (vm->state != VM_RUNNING) ||
			(vm->hw.created_vcpus == 0U)) {
			return false;
		}
	}

	return true;
}

static void hv_pm_reset_epoch_locked(struct beau_pm_snapshot *data,
	uint16_t initiator_vmid, uint64_t required_vm_mask,
	enum beau_pm_scope scope, uint16_t target_vmid)
{
	uint16_t vmid;

	data->epoch = hv_pm_next_epoch(data->epoch);
	data->required_vm_mask = required_vm_mask;
	data->completed_hook_mask = 0UL;
	data->wake_reason = 0UL;
	data->wake_bitmap = 0UL;
	data->initiator_vmid = initiator_vmid;
	data->target_vmid = target_vmid;
	data->scope = scope;
	data->io_gated_vm_mask = required_vm_mask;
	data->io_gated = 1U;
	(void)memset(data->phase_start_ticks, 0U, sizeof(data->phase_start_ticks));
	(void)memset(data->phase_duration_ticks, 0U,
		sizeof(data->phase_duration_ticks));
	(void)memset(&data->last_error, 0U, sizeof(data->last_error));

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct beau_vm_pm_record *record = &data->vm[vmid];

		(void)memset(record, 0U, sizeof(*record));
		record->epoch = data->epoch;
		record->vmid = vmid;
		record->required =
			((data->required_vm_mask & (1UL << vmid)) != 0UL) ? 1U : 0U;
		record->state = VM_PM_RUNNING;
	}
	hv_pm_transition_locked(data, PM_PREPARING);
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
	} else if (data->topology_change_vm_mask != 0UL) {
		status = -EBUSY;
	} else if (platform_pm_preflight(data->platform_mode) != 0) {
		status = -ENOTSUP;
	} else if (!hv_pm_required_vms_running(data->policy_required_vm_mask)) {
		status = -ENODEV;
	} else {
		hv_pm_reset_epoch_locked(data, initiator_vmid,
			data->policy_required_vm_mask, HV_PM_SCOPE_SYSTEM,
			CONFIG_MAX_VM_NUM);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (status == 0) {
		make_system_suspend_request(BSP_CPU_ID);
	}

	return status;
}

int32_t hv_pm_request_vm_suspend(uint16_t vmid)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return -EINVAL;
	}

	hv_pm_finalize_hooks();
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (data->enabled == 0U) {
		status = -ENODEV;
	} else if (data->state != PM_RUNNING) {
		status = -EBUSY;
	} else if (data->topology_change_vm_mask != 0UL) {
		status = -EBUSY;
	} else if (!hv_pm_required_vms_running(1UL << vmid)) {
		status = -ENODEV;
	} else {
		hv_pm_reset_epoch_locked(data, vmid, 1UL << vmid,
			HV_PM_SCOPE_VM, vmid);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (status == 0) {
		make_system_suspend_request(BSP_CPU_ID);
	}

	return status;
}

int32_t hv_pm_request_vm_resume(uint16_t vmid)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->scope != HV_PM_SCOPE_VM) ||
		(data->target_vmid != vmid)) {
		status = -EINVAL;
	} else if (data->state != PM_SUSPENDED) {
		status = -EBUSY;
	} else {
		hv_pm_transition_locked(data, PM_RESTORING_HOST);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (status == 0) {
		make_system_suspend_request(BSP_CPU_ID);
	}

	return status;
}

int32_t hv_pm_abort(uint64_t epoch, int32_t reason)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((epoch == 0UL) || (data->epoch != epoch) ||
		(data->state == PM_RUNNING) || (data->state == PM_FAILED)) {
		status = -EINVAL;
	} else if (data->state != PM_ABORTING) {
		uint32_t failed_phase = data->state;

		hv_pm_record_error_locked(data, epoch, failed_phase, reason,
			data->initiator_vmid);
		hv_pm_transition_locked(data, PM_ABORTING);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (status == 0) {
		make_system_suspend_request(BSP_CPU_ID);
	}

	return status;
}

void make_system_suspend_request(uint16_t pcpu_id)
{
	if (pcpu_id < get_pcpu_nums()) {
		bitmap_set(NEED_SYSTEM_SUSPEND, &per_cpu(pcpu_flag, pcpu_id));
		make_reschedule_request(pcpu_id);
	}
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
	uint64_t epoch = 0UL;
	int32_t status = 0;
	bool abort = false;

	if (source_index >= 64U) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->scope != HV_PM_SCOPE_SYSTEM) ||
		(data->state == PM_RUNNING) || (data->state == PM_FAILED)) {
		status = -EINVAL;
	} else {
		if (data->wake_reason == 0UL) {
			data->wake_reason = wake_source;
		}
		data->wake_bitmap |= 1UL << source_index;
		epoch = data->epoch;
		abort = (data->state != PM_SUSPENDED);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (abort && (status == 0)) {
		(void)hv_pm_abort(epoch, -EAGAIN);
	}

	return status;
}

int32_t hv_pm_begin_vm_topology_change(uint16_t vmid)
{
	struct beau_pm_snapshot *data = &pm_transaction.data;
	uint64_t bit;
	uint64_t flags;
	int32_t status = 0;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return -EINVAL;
	}
	bit = 1UL << vmid;
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((data->state != PM_RUNNING) ||
		((data->topology_change_vm_mask & bit) != 0UL)) {
		status = -EBUSY;
	} else {
		data->topology_change_vm_mask |= bit;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

void hv_pm_end_vm_topology_change(uint16_t vmid)
{
	uint64_t flags;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return;
	}
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	pm_transaction.data.topology_change_vm_mask &= ~(1UL << vmid);
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
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

bool hv_pm_vm_io_is_gated(uint16_t vmid)
{
	uint64_t flags;
	bool gated = false;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return false;
	}
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	gated = (pm_transaction.data.io_gated_vm_mask & (1UL << vmid)) != 0UL;
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
	if ((pm_transaction.data.epoch != epoch) ||
		(pm_transaction.data.state == PM_RUNNING) ||
		(pm_transaction.data.state == PM_FAILED)) {
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
	/*
	 * The priority-sorted array is a dependency order, not merely a callback
	 * list. Recording each success makes reverse traversal a precise unwind of
	 * the resources acquired or retained by this epoch.
	 */
	for (idx = 0U; idx < count; idx++) {
		beau_pm_hook_fn callback = suspend_phase ?
			pm_hooks[idx].suspend : pm_hooks[idx].prepare;

		if (callback == NULL) {
			continue;
		}
		status = callback(epoch);
		if (status == 0) {
			status = hv_pm_set_hook_completed(epoch, idx);
		}
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
	if ((epoch == 0UL) || (snapshot.epoch != epoch)) {
		return -EINVAL;
	}
	for (idx = count; idx > 0U; idx--) {
		uint16_t hook_idx = idx - 1U;
		beau_pm_hook_fn callback;
		int32_t callback_status = 0;

		if ((snapshot.completed_hook_mask & (1UL << hook_idx)) == 0UL) {
			continue;
		}
		callback = abort_phase ? pm_hooks[hook_idx].abort :
			pm_hooks[hook_idx].resume;
		if (callback != NULL) {
			callback_status = callback(epoch);
			if ((callback_status != 0) && (first_error == 0)) {
				first_error = callback_status;
			}
		}
		if (callback_status == 0) {
			hv_pm_clear_hook_completed(hook_idx);
		}
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

static bool hv_pm_epoch_has_state(uint64_t epoch,
	enum beau_pm_system_state state)
{
	uint64_t flags;
	bool match;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	match = (pm_transaction.data.epoch == epoch) &&
		(pm_transaction.data.state == state);
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return match;
}

static int32_t hv_pm_set_epoch_state(uint64_t epoch,
	enum beau_pm_system_state expected, enum beau_pm_system_state next)
{
	uint64_t flags;
	int32_t status = 0;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((pm_transaction.data.epoch != epoch) ||
		(pm_transaction.data.state != expected)) {
		status = -EINVAL;
	} else {
		hv_pm_transition_locked(&pm_transaction.data, next);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	return status;
}

static void hv_pm_publish_vcpu_masks(uint64_t epoch, uint16_t vmid,
	uint64_t gated, uint64_t active, uint64_t frozen, uint64_t wake_owned)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((pm_transaction.data.epoch == epoch) &&
		(vmid < CONFIG_MAX_VM_NUM)) {
		struct beau_vm_pm_record *record = &pm_transaction.data.vm[vmid];

		record->gated_vcpu_mask = gated;
		record->active_vcpu_mask = active;
		record->frozen_vcpu_mask = frozen;
		record->wake_owned_vcpu_mask = wake_owned;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
}

static int32_t hv_pm_gate_vm(uint64_t epoch, uint16_t vmid)
{
	struct acrn_vm *vm = get_vm_from_vmid(vmid);
	uint64_t gated = 0UL;
	uint64_t active = 0UL;
	uint64_t wake_owned_mask = 0UL;
	uint16_t vcpu_id;
	uint64_t flags;

	if ((vm == NULL) || (vm->state != VM_RUNNING)) {
		return -ENODEV;
	}
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((pm_transaction.data.epoch != epoch) ||
		(pm_transaction.data.state != PM_FREEZING_HOST)) {
		spinlock_irqrestore_release(&pm_transaction.lock, flags);
		return -EAGAIN;
	}
	pm_transaction.data.vm[vmid].prior_vm_state = vm->state;
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	/*
	 * STR freezes scheduler threads without rewriting guest-visible vCPU
	 * lifecycle state. active records threads that still need a switch-out;
	 * wake_owned records blocked threads whose pending wake must be replayed by
	 * thaw_thread(), preserving the sleep/wake edge across the freeze window.
	 */
	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		struct acrn_vcpu *vcpu = vcpu_from_vid(vm, vcpu_id);
		enum vcpu_state state = vcpu_get_state(vcpu);
		bool wake_owned;

		if (state == VCPU_OFFLINE) {
			continue;
		}
		if (state == VCPU_PAUSED) {
			return -EBUSY;
		}
		if (!freeze_thread(&vcpu->thread_obj, epoch, &wake_owned)) {
			return -EBUSY;
		}
		gated |= 1UL << vcpu_id;
		if (wake_owned) {
			wake_owned_mask |= 1UL << vcpu_id;
		}
		if (vcpu_get_state(vcpu) == VCPU_RUNNING) {
			active |= 1UL << vcpu_id;
		}
		hv_pm_publish_vcpu_masks(epoch, vmid, gated, active, 0UL,
			wake_owned_mask);
		make_reschedule_request(pcpuid_from_vcpu(vcpu));
	}

	return 0;
}

static int32_t hv_pm_freeze_guests(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	uint64_t start = cpu_ticks();
	uint16_t vmid;
	int32_t status;

	hv_pm_get_snapshot(&snapshot);
	if ((snapshot.epoch != epoch) ||
		(snapshot.state != PM_FREEZING_HOST)) {
		return -EINVAL;
	}
	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		if ((snapshot.required_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}
		status = hv_pm_gate_vm(epoch, vmid);
		if (status != 0) {
			return status;
		}
	}

	for (;;) {
		bool all_frozen = true;

		hv_pm_get_snapshot(&snapshot);
		if ((snapshot.epoch != epoch) ||
			(snapshot.state != PM_FREEZING_HOST)) {
			return -EAGAIN;
		}
		for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
			struct beau_vm_pm_record *record = &snapshot.vm[vmid];
			struct acrn_vm *vm;
			uint64_t frozen = 0UL;
			uint16_t vcpu_id;

			if ((snapshot.required_vm_mask & (1UL << vmid)) == 0UL) {
				continue;
			}
			vm = get_vm_from_vmid(vmid);
			for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
				if ((record->gated_vcpu_mask & (1UL << vcpu_id)) == 0UL) {
					continue;
				}
				if (is_thread_frozen(&vcpu_from_vid(vm, vcpu_id)->thread_obj,
					epoch)) {
					frozen |= 1UL << vcpu_id;
				} else {
					all_frozen = false;
				}
			}
			hv_pm_publish_vcpu_masks(epoch, vmid,
				record->gated_vcpu_mask, record->active_vcpu_mask, frozen,
				record->wake_owned_vcpu_mask);
			if (frozen == record->gated_vcpu_mask) {
				uint64_t flags;

				spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
				pm_transaction.data.vm[vmid].state = VM_PM_FROZEN;
				spinlock_irqrestore_release(&pm_transaction.lock, flags);
			}
		}
		if (all_frozen) {
			break;
		}
		if (ticks_to_ms(cpu_ticks() - start) >= snapshot.prepare_timeout_ms) {
			return -ETIMEDOUT;
		}
		asm_pause();
	}

	LOG_INF("STR: PM_GUESTS_FROZEN epoch:%lu vm-mask:0x%lx", epoch,
		snapshot.required_vm_mask);
	return 0;
}

static int32_t hv_pm_thaw_guests(uint64_t epoch, bool require_frozen)
{
	struct beau_pm_snapshot snapshot;
	uint16_t vmid;
	uint64_t flags;

	hv_pm_get_snapshot(&snapshot);
	if (snapshot.epoch != epoch) {
		return -EINVAL;
	}
	if (require_frozen) {
		for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
			struct beau_vm_pm_record *record = &snapshot.vm[vmid];
			struct acrn_vm *vm;
			uint16_t vcpu_id;

			if ((snapshot.required_vm_mask & (1UL << vmid)) == 0UL) {
				continue;
			}
			vm = get_vm_from_vmid(vmid);
			if ((vm == NULL) ||
				(vm->state != (enum vm_state)record->prior_vm_state) ||
				(record->frozen_vcpu_mask != record->gated_vcpu_mask)) {
				return -EFAULT;
			}
			for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
				if (((record->frozen_vcpu_mask & (1UL << vcpu_id)) != 0UL) &&
					!is_thread_frozen(&vcpu_from_vid(vm, vcpu_id)->thread_obj,
						epoch)) {
					return -EBUSY;
				}
			}
		}
	}
	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		if (((snapshot.required_vm_mask & (1UL << vmid)) != 0UL) &&
			(snapshot.vm[vmid].gated_vcpu_mask != 0UL) &&
			(get_vm_from_vmid(vmid) == NULL)) {
			return -ENODEV;
		}
	}

	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		struct beau_vm_pm_record *record = &snapshot.vm[vmid];
		struct acrn_vm *vm;
		uint16_t vcpu_id;

		if ((snapshot.required_vm_mask & (1UL << vmid)) == 0UL) {
			continue;
		}
		vm = get_vm_from_vmid(vmid);
		if (vm == NULL) {
			if (record->gated_vcpu_mask != 0UL) {
				return -ENODEV;
			}
			continue;
		}
		for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
			if ((record->gated_vcpu_mask & (1UL << vcpu_id)) == 0UL) {
				continue;
			}
			if (!thaw_thread(&vcpu_from_vid(vm, vcpu_id)->thread_obj, epoch,
				((record->wake_owned_vcpu_mask & (1UL << vcpu_id)) != 0UL))) {
				return -EFAULT;
			}
		}
		spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
		pm_transaction.data.vm[vmid].state = VM_PM_RUNNING;
		spinlock_irqrestore_release(&pm_transaction.lock, flags);
	}

	/*
	 * Keep MMIO/HVC paths gated until every target vCPU has been thawed. If
	 * a restore hook or thread thaw fails, PM_FAILED still blocks target I/O
	 * instead of letting a half-restored VM touch retained device state.
	 */
	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if (pm_transaction.data.epoch != epoch) {
		spinlock_irqrestore_release(&pm_transaction.lock, flags);
		return -EINVAL;
	}
	pm_transaction.data.io_gated_vm_mask &= ~snapshot.required_vm_mask;
	if (pm_transaction.data.io_gated_vm_mask == 0UL) {
		pm_transaction.data.io_gated = 0U;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	LOG_INF("STR: PM_GUESTS_THAWED epoch:%lu vm-mask:0x%lx", epoch,
		snapshot.required_vm_mask);

	return 0;
}

static void hv_pm_complete_running(uint64_t epoch, int32_t last_status)
{
	uint64_t flags;
	bool completed = false;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((pm_transaction.data.epoch == epoch) &&
		(pm_transaction.data.state != PM_FAILED)) {
		uint32_t completed_phase = pm_transaction.data.state;

		pm_transaction.data.last_epoch = epoch;
		pm_transaction.data.last_state = completed_phase;
		pm_transaction.data.last_status = last_status;
		pm_transaction.data.required_vm_mask = 0UL;
		pm_transaction.data.completed_hook_mask = 0UL;
		pm_transaction.data.io_gated = 0U;
		pm_transaction.data.io_gated_vm_mask = 0UL;
		pm_transaction.data.target_vmid = CONFIG_MAX_VM_NUM;
		pm_transaction.data.scope = HV_PM_SCOPE_NONE;
		hv_pm_transition_locked(&pm_transaction.data, PM_RUNNING);
		completed = true;
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);
	if (completed) {
		LOG_INF("STR: PM_RUNNING epoch:%lu", epoch);
	}
}

static int32_t hv_pm_rollback_from_idle(uint64_t epoch, int32_t reason)
{
	uint64_t flags;
	int32_t hook_status;
	int32_t thaw_status;

	spinlock_irqsave_obtain(&pm_transaction.lock, &flags);
	if ((pm_transaction.data.epoch == epoch) &&
		(pm_transaction.data.state != PM_ABORTING) &&
		(pm_transaction.data.state != PM_RUNNING)) {
		uint32_t failed_phase = pm_transaction.data.state;

		hv_pm_record_error_locked(&pm_transaction.data, epoch, failed_phase,
			reason, pm_transaction.data.initiator_vmid);
		hv_pm_transition_locked(&pm_transaction.data, PM_ABORTING);
	}
	spinlock_irqrestore_release(&pm_transaction.lock, flags);

	hook_status = hv_pm_run_abort(epoch);
	if (hook_status != 0) {
		hv_pm_fail_epoch(epoch, hook_status);
		return hook_status;
	}
	thaw_status = hv_pm_thaw_guests(epoch, false);
	if (thaw_status != 0) {
		hv_pm_fail_epoch(epoch, thaw_status);
		return thaw_status;
	}
	hv_pm_complete_running(epoch, reason);

	return 0;
}

static int32_t hv_pm_restore_from_idle(uint64_t epoch)
{
	int32_t status;

	LOG_INF("STR: PM_RESUMING epoch:%lu", epoch);
	status = hv_pm_run_resume(epoch);
	if (status == 0) {
		status = hv_pm_thaw_guests(epoch, true);
	}
	if (status != 0) {
		hv_pm_fail_epoch(epoch, status);
		return status;
	}
	hv_pm_complete_running(epoch, 0);

	return 0;
}

void hv_pm_process_from_idle(uint16_t pcpu_id)
{
	struct beau_pm_snapshot snapshot;
	uint64_t epoch;
	int32_t status;
	bool host_restored = false;

	/*
	 * Idle context is the execution barrier for STR: no caller stack or guest
	 * exit path remains part of the transaction. APs retain only banked local
	 * state; the BSP alone advances the global state machine and platform path.
	 */
	if (pcpu_id != BSP_CPU_ID) {
		arch_pm_process_secondary_from_idle(pcpu_id);
		return;
	}
	hv_pm_get_snapshot(&snapshot);
	epoch = snapshot.epoch;
	if (snapshot.state == PM_ABORTING) {
		(void)hv_pm_rollback_from_idle(epoch, snapshot.last_status);
		return;
	}
	if (snapshot.state == PM_RESTORING_HOST) {
		(void)hv_pm_restore_from_idle(epoch);
		return;
	}
	if ((epoch == 0UL) || (snapshot.state != PM_PREPARING)) {
		return;
	}

	status = hv_pm_run_prepare(epoch);
	if ((status != 0) || !hv_pm_epoch_has_state(epoch, PM_PREPARING)) {
		(void)hv_pm_rollback_from_idle(epoch,
			(status != 0) ? status : -EAGAIN);
		return;
	}
	status = hv_pm_set_epoch_state(epoch, PM_PREPARING, PM_FREEZING_HOST);
	if (status == 0) {
		status = hv_pm_freeze_guests(epoch);
	}
	if (status == 0) {
		status = hv_pm_run_suspend(epoch);
	}
	if ((status != 0) || !hv_pm_epoch_has_state(epoch, PM_FREEZING_HOST)) {
		(void)hv_pm_rollback_from_idle(epoch,
			(status != 0) ? status : -EAGAIN);
		return;
	}
	hv_pm_get_snapshot(&snapshot);
	if (snapshot.wake_bitmap != 0UL) {
		(void)hv_pm_rollback_from_idle(epoch, -EAGAIN);
		return;
	}
	status = hv_pm_set_epoch_state(epoch, PM_FREEZING_HOST, PM_SUSPENDED);
	if (status != 0) {
		(void)hv_pm_rollback_from_idle(epoch, status);
		return;
	}
	hv_pm_get_snapshot(&snapshot);
	if (snapshot.scope == HV_PM_SCOPE_VM) {
		LOG_INF("STR: PM_VM_SUSPENDED epoch:%lu vm:%hu",
			epoch, snapshot.target_vmid);
		return;
	}
	if (snapshot.scope != HV_PM_SCOPE_SYSTEM) {
		(void)hv_pm_rollback_from_idle(epoch, -EINVAL);
		return;
	}

	status = platform_pm_enter(epoch, &host_restored);
	if ((status != 0) || !host_restored) {
		if (!host_restored) {
			hv_pm_fail_epoch(epoch, (status != 0) ? status : -EFAULT);
		} else {
			(void)hv_pm_rollback_from_idle(epoch, status);
		}
		return;
	}
	status = hv_pm_set_epoch_state(epoch, PM_SUSPENDED, PM_RESTORING_HOST);
	if (status != 0) {
		hv_pm_fail_epoch(epoch, status);
		return;
	}
	(void)hv_pm_restore_from_idle(epoch);
}

const char *hv_pm_state_to_str(enum beau_pm_system_state state)
{
	static const char *const names[] = {
		[PM_RUNNING] = "running",
		[PM_PREPARING] = "preparing",
		[PM_FREEZING_HOST] = "freezing-host",
		[PM_SUSPENDED] = "suspended",
		[PM_RESTORING_HOST] = "restoring-host",
		[PM_ABORTING] = "aborting",
		[PM_FAILED] = "failed",
	};

	return ((uint32_t)state < ARRAY_SIZE(names)) ? names[state] : "invalid";
}

const char *hv_pm_scope_to_str(enum beau_pm_scope scope)
{
	static const char *const names[] = {
		[HV_PM_SCOPE_NONE] = "none",
		[HV_PM_SCOPE_SYSTEM] = "system",
		[HV_PM_SCOPE_VM] = "vm",
	};

	return ((uint32_t)scope < ARRAY_SIZE(names)) ? names[scope] : "invalid";
}
