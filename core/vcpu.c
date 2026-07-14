/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vcpu.h>
#include <vm.h>
#include <errno.h>
#include <per_cpu.h>
#include <sprintf.h>
#include <logmsg.h>
#include <schedule.h>
#include <notify.h>
#include <ticks.h>

#include <asm/notify.h>

/* [20260712] Common vCPU lifecycle and request delivery
 *
 * The common layer treats a vCPU as a schedulable thread bound to one pCPU.
 * Architecture code owns guest register/MMU/interrupt state; this file owns
 * object lifetime, pCPU binding, generic requests, and the bridge into the
 * scheduler.
 *
 *   VM cpu_affinity bit
 *          |
 *          v
 *   create_vcpu(vm, pcpu)
 *     - assign vcpu_id from creation order
 *     - bind thread_obj to per-pCPU sched_ctl
 *     - initialize events and arch-private vCPU state
 *          |
 *          v
 *      VCPU_INIT
 *          |
 *          v
 *   launch_vcpu()
 *     - set VCPU_RUNNING
 *     - wake vCPU thread
 *          |
 *          v
 *   scheduler picks thread_obj
 *          |
 *          v
 *   arch_vcpu_thread() enters guest
 *
 * Generic event delivery is deferred through pending_req. Producers set a bit,
 * ask the scheduler for priority treatment, and kick the pCPU if the target
 * vCPU is currently running remotely. The architecture vCPU thread consumes the
 * request at a safe guest-entry boundary.
 *
 *   injector / device / vGIC
 *          |
 *          v
 *   vcpu_make_request()
 *     - pending_req bit
 *     - request_thread_priority()
 *     - remote kick if needed
 *          |
 *          v
 *   target vCPU thread handles request before next EL1 entry
 *
 * Key rule:
 *   - common VM code owns object lifetime and scheduler binding;
 *   - architecture code owns saved guest context and guest-entry checks;
 *   - requests are published as bits first, then scheduler priority and remote
 *     kicks make the target observe them at a safe entry boundary.
 */
bool is_vcpu_bsp(const struct acrn_vcpu *vcpu)
{
	return (vcpu->vcpu_id == BSP_CPU_ID);
}

uint16_t pcpuid_from_vcpu(const struct acrn_vcpu *vcpu)
{
	return sched_get_pcpuid(&vcpu->thread_obj);
}

static bool vcpu_boot_log_enabled(const struct acrn_vcpu *vcpu)
{
	return (vcpu != NULL) && (vcpu->vm != NULL) &&
		(vcpu->vm->vm_id < (SERVICE_VM_NUM + PRE_VM_NUM));
}

uint64_t vcpumask2pcpumask(struct acrn_vm *vm, uint64_t vdmask)
{
	uint16_t vcpu_id;
	uint64_t dmask = 0UL;
	struct acrn_vcpu *vcpu;

	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		if ((vdmask & (1UL << vcpu_id)) != 0UL) {
			vcpu = vcpu_from_vid(vm, vcpu_id);
			bitmap_set_non_atomic(pcpuid_from_vcpu(vcpu), &dmask);
		}
	}

	return dmask;
}

struct acrn_vcpu *get_running_vcpu(uint16_t pcpu_id)
{
	struct thread_object *curr;
	struct acrn_vcpu *vcpu = NULL;
	uint16_t vm_id;

	if (pcpu_id >= MAX_PCPU_NUM) {
		return NULL;
	}

	curr = sched_get_current(pcpu_id);
	if (curr != NULL) {
		for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
			struct acrn_vcpu *candidate = per_cpu(vcpu_array, pcpu_id)[vm_id];

			if ((candidate != NULL) && (curr == &candidate->thread_obj)) {
				vcpu = candidate;
				break;
			}
		}
	}

	return vcpu;
}

struct acrn_vcpu *get_ever_run_vcpu(uint16_t pcpu_id)
{
	return per_cpu(ever_run_vcpu, pcpu_id);
}

static void init_vcpu_thread(struct acrn_vcpu *vcpu, uint16_t pcpu_id)
{
	struct acrn_vm *vm = vcpu->vm;
	char thread_name[16];

	snprintf(thread_name, 16U, "vm%hu:vcpu%hu", vm->vm_id, vcpu->vcpu_id);
	(void)strncpy_s(vcpu->thread_obj.name, 16U, thread_name, 16U);
	vcpu->thread_obj.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
	vcpu->thread_obj.thread_entry = arch_vcpu_thread;
	vcpu->thread_obj.pcpu_id = pcpu_id;
	vcpu->thread_obj.is_vcpu = true;
	vcpu->thread_obj.vm_id = vm->vm_id;
	vcpu->thread_obj.vcpu_id = vcpu->vcpu_id;
	vcpu->thread_obj.host_sp = arch_build_stack_frame(vcpu);
	vcpu->thread_obj.switch_out = arch_context_switch_out;
	vcpu->thread_obj.switch_in = arch_context_switch_in;
	init_thread_data(&vcpu->thread_obj, &get_vm_config(vm->vm_id)->sched_params);
}

void vcpu_set_state(struct acrn_vcpu *vcpu, enum vcpu_state new_state)
{
	vcpu->state = new_state;
}

void kick_vcpu(struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);

	if ((get_pcpu_id() != pcpu_id) && (get_running_vcpu(pcpu_id) == vcpu)) {
		arch_smp_call_kick_pcpu(pcpu_id);
	}
}

void vcpu_make_request(struct acrn_vcpu *vcpu, uint16_t eventid)
{
	bitmap_set(eventid, &vcpu->pending_req);
	/*
	 * Interrupt injection may already hold architecture interrupt locks. Ask the
	 * scheduler for best-effort event priority through a deferred request instead
	 * of editing scheduler runqueue state directly from this path.
	 */
	request_thread_priority(&vcpu->thread_obj);
	kick_vcpu(vcpu);
}

int32_t create_vcpu(struct acrn_vm *vm, uint16_t pcpu_id)
{
	struct acrn_vcpu *vcpu;
	uint16_t vcpu_id;
	int32_t i, ret;

	/*
	 * vcpu->vcpu_id = vm->hw.created_vcpus;
	 * vm->hw.created_vcpus++;
	 */
	vcpu_id = vm->hw.created_vcpus;
	if (vcpu_id < MAX_VCPUS_PER_VM) {
		/* Allocate memory for VCPU */
		vcpu = &(vm->hw.vcpu_array[vcpu_id]);
		(void)memset((void *)vcpu, 0U, sizeof(struct acrn_vcpu));

		vcpu->vcpu_id = vcpu_id;
		per_cpu(ever_run_vcpu, pcpu_id) = vcpu;

		vcpu->vm = vm;

		cpu_compiler_barrier();

		/*
		 * We maintain a per-pCPU array of vCPUs, and use vm_id as the index to the
		 * vCPU array
		 */
		per_cpu(vcpu_array, pcpu_id)[vm->vm_id] = vcpu;

		(void)memset((void *)&vcpu->req, 0U, sizeof(struct io_request));
		vm->hw.created_vcpus++;

		/* pcpuid_from_vcpu works after this call */
		init_vcpu_thread(vcpu, pcpu_id);

		/* init event */
		for (i = 0; i < MAX_VCPU_EVENT_NUM; i++) {
			init_event(&vcpu->events[i]);
		}

		ret = arch_init_vcpu(vcpu);

		if (ret == 0) {
			vcpu->state = VCPU_INIT;
		}
	} else {
		LOG_ERR("%s, vcpu id is invalid!\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}

void destroy_vcpu(struct acrn_vcpu *vcpu)
{
	arch_deinit_vcpu(vcpu);

	per_cpu(ever_run_vcpu, pcpuid_from_vcpu(vcpu)) = NULL;

	/* This operation must be atomic to avoid contention with posted interrupt handler */
	per_cpu(vcpu_array, pcpuid_from_vcpu(vcpu))[vcpu->vm->vm_id] = NULL;

	vcpu_set_state(vcpu, VCPU_OFFLINE);
}

void launch_vcpu(struct acrn_vcpu *vcpu)
{
	uint64_t kick_tsc = cpu_ticks();
	uint64_t kick_us;

	vcpu_set_state(vcpu, VCPU_RUNNING);

	wake_thread(&vcpu->thread_obj);

	if (vcpu_boot_log_enabled(vcpu)) {
		kick_us = ticks_to_us(cpu_ticks() - kick_tsc);
		LOG_INF("VM%u: kick vcpu%hu on pcpu%hu   0x%016lx +%6luus",
			vcpu->vm->vm_id, vcpu->vcpu_id, pcpuid_from_vcpu(vcpu),
			arch_vcpu_get_entry(vcpu), kick_us);
	}
}

void reset_vcpu(struct acrn_vcpu *vcpu)
{
	int i;

	vcpu->launched = false;
	vcpu->pending_req = 0UL;

	for (i = 0; i < MAX_VCPU_EVENT_NUM; i++) {
		reset_event(&vcpu->events[i]);
	}

	arch_reset_vcpu(vcpu);

	vcpu_set_state(vcpu, VCPU_INIT);
}

void zombie_vcpu(struct acrn_vcpu *vcpu)
{
	enum vcpu_state prev_state;
	uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);

	LOG_DBG("vcpu%hu paused", vcpu->vcpu_id);

	if ((vcpu->state == VCPU_RUNNING) || (vcpu->state == VCPU_INIT)) {
		prev_state = vcpu->state;

		vcpu_set_state(vcpu, VCPU_ZOMBIE);

		if (prev_state == VCPU_RUNNING) {
			if (pcpu_id == get_pcpu_id()) {
				sleep_thread(&vcpu->thread_obj);
			} else {
				sleep_thread_sync(&vcpu->thread_obj);
			}
		}
	}
}
