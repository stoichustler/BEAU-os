/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <acrn_hv_defs.h>
#include <ai_sched.h>
#include <bits.h>
#include <cpu.h>
#include <errno.h>
#include <guest_memory.h>
#include <schedule.h>
#include <spinlock.h>
#include <ticks.h>
#include <vconfig.h>
#include <vm.h>

/* This gate is separate from scheduler state: proposal acceptance never changes CBS. */
struct ai_sched_gate {
	spinlock_t lock;
	uint16_t advisor_vmid;
	uint64_t capability;
	uint64_t next_capability;
	uint64_t last_proposal_ticks[MAX_PCPU_NUM];
};

static struct ai_sched_gate ai_sched_gate = {
	.lock = { .head = 0U, .tail = 0U, },
	.advisor_vmid = ACRN_INVALID_VMID,
	.next_capability = 1UL,
};

static bool ai_sched_vm_uses_pcpu(const struct acrn_vm_config *config, uint16_t pcpu_id)
{
	uint16_t idx;

	if ((config == NULL) || (config->name[0] == '\0')) {
		return false;
	}
	if (config->cpu_affinity_num != 0U) {
		for (idx = 0U; idx < config->cpu_affinity_num; idx++) {
			if (config->cpu_affinity_order[idx] == pcpu_id) {
				return true;
			}
		}
		return false;
	}
	return (config->cpu_affinity & AFFINITY_CPU(pcpu_id)) != 0UL;
}

static int32_t ai_sched_pool(const struct acrn_ai_sched_ioc *ioc,
	const struct sched_cpupool_config **pool)
{
	if (ioc->pcpu_id >= get_pcpu_nums()) {
		return -EINVAL;
	}
	*pool = sched_get_pcpu_pool_config(ioc->pcpu_id);
	if ((*pool == NULL) || !(*pool)->ai_assist || ((*pool)->policy != SCHED_POLICY_CBS) ||
		((*pool)->period_us == 0U)) {
		return -ENOTSUP;
	}
	return 0;
}

static int32_t ai_sched_snapshot(struct acrn_ai_sched_ioc *ioc)
{
	const struct sched_cpupool_config *pool;
	uint16_t vmid;
	int32_t ret = ai_sched_pool(ioc, &pool);

	if (ret != 0) {
		return ret;
	}
	ioc->sample_ticks = cpu_ticks();
	ioc->policy = (uint32_t)pool->policy;
	ioc->period_us = pool->period_us;
	ioc->pool_budget_us = pool->budget_us;
	ioc->min_budget_us = pool->ai_min_budget_us;
	ioc->max_budget_us = pool->ai_max_budget_us;
	ioc->max_step_us = pool->ai_max_step_us;
	ioc->min_update_ms = pool->ai_min_update_ms;
	ioc->entry_count = 0U;
	ioc->active_vm_mask = 0UL;
	for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
		const struct acrn_vm_config *config = get_vm_config(vmid);
		struct acrn_ai_sched_entry *entry;

		if (!ai_sched_vm_uses_pcpu(config, ioc->pcpu_id)) {
			continue;
		}
		if (ioc->entry_count >= ACRN_AI_SCHED_MAX_ENTRIES) {
			return -EINVAL;
		}
		entry = &ioc->entries[ioc->entry_count++];
		entry->vmid = vmid;
		entry->budget_us = (config->sched_params.cbs_budget_us != 0U) ?
			config->sched_params.cbs_budget_us : pool->budget_us;
		if (vmid < 64U) {
			ioc->active_vm_mask |= 1UL << vmid;
		}
	}
	return 0;
}

static int32_t ai_sched_validate_proposal(struct acrn_ai_sched_ioc *ioc)
{
	const struct sched_cpupool_config *pool;
	uint64_t active_mask = 0UL;
	uint64_t seen_mask = 0UL;
	uint64_t utilization = 0UL;
	uint16_t idx;
	int32_t ret = ai_sched_pool(ioc, &pool);

	if ((ret != 0) || (ioc->sequence == 0UL) || (ioc->entry_count == 0U) ||
		(ioc->entry_count > ACRN_AI_SCHED_MAX_ENTRIES)) {
		return ret != 0 ? ret : -EINVAL;
	}
	for (idx = 0U; idx < CONFIG_MAX_VM_NUM; idx++) {
		if (ai_sched_vm_uses_pcpu(get_vm_config(idx), ioc->pcpu_id)) {
			if (idx >= 64U) {
				return -ENOTSUP;
			}
			active_mask |= 1UL << idx;
		}
	}
	for (idx = 0U; idx < ioc->entry_count; idx++) {
		const struct acrn_ai_sched_entry *entry = &ioc->entries[idx];
		uint32_t current;
		uint32_t delta;

		if ((entry->reserved != 0U) || (entry->vmid >= CONFIG_MAX_VM_NUM) ||
			(entry->vmid >= 64U) || ((active_mask & (1UL << entry->vmid)) == 0UL) ||
			((seen_mask & (1UL << entry->vmid)) != 0UL) ||
			(entry->budget_us < pool->ai_min_budget_us) ||
			(entry->budget_us > pool->ai_max_budget_us)) {
			return -EINVAL;
		}
		current = get_vm_config(entry->vmid)->sched_params.cbs_budget_us;
		if (current == 0U) {
			current = pool->budget_us;
		}
		delta = entry->budget_us > current ? entry->budget_us - current :
			current - entry->budget_us;
		if (delta > pool->ai_max_step_us) {
			return -EINVAL;
		}
		utilization += ((uint64_t)entry->budget_us * 1000000UL) / pool->period_us;
		if (utilization > 1000000UL) {
			return -EBUSY;
		}
		seen_mask |= 1UL << entry->vmid;
	}
	if (seen_mask != active_mask) {
		return -EINVAL;
	}
	ioc->active_vm_mask = active_mask;
	ioc->utilization_ppm = utilization;
	return 0;
}

void ai_sched_invalidate_vm(uint16_t vmid)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&ai_sched_gate.lock, &rflags);
	if (ai_sched_gate.advisor_vmid == vmid) {
		ai_sched_gate.advisor_vmid = ACRN_INVALID_VMID;
		ai_sched_gate.capability = 0UL;
		(void)memset(ai_sched_gate.last_proposal_ticks, 0U,
			sizeof(ai_sched_gate.last_proposal_ticks));
	}
	spinlock_irqrestore_release(&ai_sched_gate.lock, rflags);
}

int32_t hcall_ai_sched(struct acrn_vcpu *vcpu, __unused struct acrn_vm *target_vm,
	uint64_t param1, __unused uint64_t param2)
{
	struct acrn_ai_sched_ioc ioc;
	uint64_t rflags;
	int32_t ret;

	if ((vcpu == NULL) || !is_service_vm(vcpu->vm) ||
		(copy_from_gpa(vcpu->vm, &ioc, param1, sizeof(ioc)) != 0)) {
		return -EINVAL;
	}
	if ((ioc.abi_version != ACRN_AI_SCHED_ABI_VERSION) ||
		(ioc.ioc_size != sizeof(ioc)) || (ioc.reserved0 != 0U)) {
		ioc.status = ACRN_AI_SCHED_STATUS_BAD_PARAM;
		(void)copy_to_gpa(vcpu->vm, &ioc, param1, sizeof(ioc));
		return -EINVAL;
	}
	ioc.status = ACRN_AI_SCHED_STATUS_OK;
	spinlock_irqsave_obtain(&ai_sched_gate.lock, &rflags);
	if (ioc.op == ACRN_AI_SCHED_OP_REGISTER) {
		if ((ioc.capability != 0UL) || (ioc.sequence != 0UL) || (ioc.entry_count != 0U)) {
			ret = -EINVAL;
		} else {
			ai_sched_gate.advisor_vmid = vcpu->vm->vm_id;
			ai_sched_gate.capability = ++ai_sched_gate.next_capability ^ cpu_ticks();
			if (ai_sched_gate.capability == 0UL) {
				ai_sched_gate.capability = ++ai_sched_gate.next_capability;
			}
			(void)memset(ai_sched_gate.last_proposal_ticks, 0U,
				sizeof(ai_sched_gate.last_proposal_ticks));
			ioc.capability = ai_sched_gate.capability;
			ret = 0;
		}
	} else if ((ai_sched_gate.advisor_vmid != vcpu->vm->vm_id) ||
		(ai_sched_gate.capability == 0UL) ||
		(ioc.capability != ai_sched_gate.capability)) {
		ret = -EPERM;
	} else if (ioc.op == ACRN_AI_SCHED_OP_SNAPSHOT) {
		ret = ai_sched_snapshot(&ioc);
	} else if (ioc.op == ACRN_AI_SCHED_OP_PROPOSE) {
		ret = ai_sched_validate_proposal(&ioc);
		if (ret == 0) {
			uint64_t now = cpu_ticks();
			uint64_t minimum = us_to_ticks(ioc.min_update_ms * 1000U);

			if ((ai_sched_gate.last_proposal_ticks[ioc.pcpu_id] != 0UL) &&
				((now - ai_sched_gate.last_proposal_ticks[ioc.pcpu_id]) < minimum)) {
				ret = -EBUSY;
			} else {
				ai_sched_gate.last_proposal_ticks[ioc.pcpu_id] = now;
			}
		}
	} else {
		ret = -EINVAL;
	}
	if (ret == -EPERM) {
		ioc.status = ACRN_AI_SCHED_STATUS_DENIED;
	} else if (ret == -ENOTSUP) {
		ioc.status = ACRN_AI_SCHED_STATUS_UNSUPPORTED;
	} else if (ret != 0) {
		ioc.status = ACRN_AI_SCHED_STATUS_BAD_PARAM;
	}
	spinlock_irqrestore_release(&ai_sched_gate.lock, rflags);
	if (copy_to_gpa(vcpu->vm, &ioc, param1, sizeof(ioc)) != 0) {
		return -EINVAL;
	}
	return ret;
}
