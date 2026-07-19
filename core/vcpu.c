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
#include <atomic.h>

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

static bool vcpu_transition_allowed(enum vcpu_state old_state,
	enum vcpu_state new_state)
{
	bool allowed = false;

	switch (old_state) {
	case VCPU_OFFLINE:
		allowed = new_state == VCPU_INIT;
		break;
	case VCPU_INIT:
		allowed = (new_state == VCPU_RUNNING) ||
			(new_state == VCPU_PAUSED);
		break;
	case VCPU_RUNNING:
		allowed = (new_state == VCPU_PAUSED) ||
			(new_state == VCPU_POWERED_OFF);
		break;
	case VCPU_PAUSED:
		allowed = (new_state == VCPU_INIT) ||
			(new_state == VCPU_OFFLINE);
		break;
	case VCPU_POWERED_OFF:
		allowed = (new_state == VCPU_RUNNING) ||
			(new_state == VCPU_PAUSED);
		break;
	default:
		break;
	}

	return allowed;
}

enum vcpu_state vcpu_get_state(const struct acrn_vcpu *vcpu)
{
	return (vcpu != NULL) ?
		(enum vcpu_state)__atomic_load_n((const volatile uint32_t *)&vcpu->state,
			__ATOMIC_ACQUIRE) : VCPU_OFFLINE;
}

bool vcpu_try_transition_state(struct acrn_vcpu *vcpu,
	enum vcpu_state old_state, enum vcpu_state new_state)
{
	bool transitioned = false;

	if ((vcpu != NULL) && vcpu_transition_allowed(old_state, new_state)) {
		transitioned = atomic_cmpxchg32((volatile uint32_t *)&vcpu->state,
			(uint32_t)old_state, (uint32_t)new_state) == (uint32_t)old_state;
	}

	return transitioned;
}

const char *vcpu_state_to_str(enum vcpu_state state)
{
	const char *name;

	switch (state) {
	case VCPU_OFFLINE:
		name = "offline";
		break;
	case VCPU_INIT:
		name = "init";
		break;
	case VCPU_RUNNING:
		name = "running";
		break;
	case VCPU_PAUSED:
		name = "paused";
		break;
	case VCPU_POWERED_OFF:
		name = "poweroff";
		break;
	default:
		name = "unknown";
		break;
	}

	return name;
}

bool is_vcpu_running(const struct acrn_vcpu *vcpu)
{
	return vcpu_get_state(vcpu) == VCPU_RUNNING;
}

bool is_vcpu_powered_off(const struct acrn_vcpu *vcpu)
{
	return vcpu_get_state(vcpu) == VCPU_POWERED_OFF;
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
	uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);
	bool remote_running = (get_pcpu_id() != pcpu_id) && (get_running_vcpu(pcpu_id) == vcpu);

	bitmap_set(eventid, &vcpu->pending_req);
	/*
	 * Interrupt injection may already hold architecture interrupt locks. Ask the
	 * scheduler for best-effort event priority through a deferred request instead
	 * of editing scheduler runqueue state directly from this path.
	 *
	 * If the target vCPU is already running on another pCPU, a direct vCPU kick is
	 * sufficient to make it observe pending_req at the next guest-entry boundary.
	 * Raising a scheduler reschedule IPI in that case creates a second cross-core
	 * interrupt for the same event and does not improve runqueue latency.
	 */
	if (remote_running) {
		request_thread_priority_no_resched(&vcpu->thread_obj);
		arch_smp_call_kick_pcpu(pcpu_id);
	} else {
		request_thread_priority(&vcpu->thread_obj);
	}
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

		if ((ret == 0) && (vcpu_get_state(vcpu) != VCPU_INIT)) {
			ret = -EINVAL;
		}
	} else {
		LOG_ERR("%s, vcpu id is invalid!\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}

void destroy_vcpu(struct acrn_vcpu *vcpu)
{
	if (!vcpu_try_transition_state(vcpu, VCPU_PAUSED, VCPU_OFFLINE)) {
		LOG_ERR("VM%u: vCPU%hu destroy from %s denied", vcpu->vm->vm_id,
			vcpu->vcpu_id, vcpu_state_to_str(vcpu_get_state(vcpu)));
		return;
	}

	arch_deinit_vcpu(vcpu);

	per_cpu(ever_run_vcpu, pcpuid_from_vcpu(vcpu)) = NULL;

	/* This operation must be atomic to avoid contention with posted interrupt handler */
	per_cpu(vcpu_array, pcpuid_from_vcpu(vcpu))[vcpu->vm->vm_id] = NULL;
}

bool launch_vcpu(struct acrn_vcpu *vcpu)
{
	uint64_t kick_tsc = cpu_ticks();
	uint64_t kick_us;
	enum vcpu_state state = vcpu_get_state(vcpu);
	bool launched;

	launched = vcpu_try_transition_state(vcpu, state, VCPU_RUNNING);
	if (!launched) {
		return false;
	}

	release_thread_quiesce(&vcpu->thread_obj);
	wake_thread(&vcpu->thread_obj);

	if (vcpu_boot_log_enabled(vcpu)) {
		kick_us = ticks_to_us(cpu_ticks() - kick_tsc);
		LOG_INF("VM%u: kick vCPU%hu on pCPU%hu   0x%016lx +%6luus",
			vcpu->vm->vm_id, vcpu->vcpu_id, pcpuid_from_vcpu(vcpu),
			arch_vcpu_get_entry(vcpu), kick_us);
	}

	return true;
}

void reset_vcpu(struct acrn_vcpu *vcpu)
{
	int i;
	bool claimed;

	claimed = vcpu_try_transition_state(vcpu, VCPU_PAUSED, VCPU_INIT) ||
		vcpu_try_transition_state(vcpu, VCPU_OFFLINE, VCPU_INIT);
	if (!claimed) {
		LOG_ERR("VM%u: vCPU%hu reset from %s denied", vcpu->vm->vm_id,
			vcpu->vcpu_id, vcpu_state_to_str(vcpu_get_state(vcpu)));
		return;
	}

	vcpu->launched = false;
	vcpu->pending_req = 0UL;

	for (i = 0; i < MAX_VCPU_EVENT_NUM; i++) {
		reset_event(&vcpu->events[i]);
	}

	arch_reset_vcpu(vcpu);
}

static bool vcpu_pause(struct acrn_vcpu *vcpu)
{
	bool was_running = false;
	bool pause_thread = false;

	if (vcpu == NULL) {
		return false;
	}

	while (true) {
		enum vcpu_state state = vcpu_get_state(vcpu);

		if (state == VCPU_PAUSED) {
			pause_thread = true;
			break;
		}
		if ((state == VCPU_OFFLINE) ||
			!vcpu_try_transition_state(vcpu, state, VCPU_PAUSED)) {
			if (state == VCPU_OFFLINE) {
				break;
			}
			continue;
		}

		was_running = state == VCPU_RUNNING;
		pause_thread = true;
		break;
	}
	if (pause_thread) {
		/* A repeated pause reasserts scheduler removal after a delayed SGI. */
		sleep_thread(&vcpu->thread_obj);
	}

	return was_running;
}

void pause_vcpu(struct acrn_vcpu *vcpu)
{
	(void)vcpu_pause(vcpu);
}

void pause_vcpu_sync(struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id;
	bool was_running;

	if (vcpu == NULL) {
		return;
	}

	pcpu_id = pcpuid_from_vcpu(vcpu);
	was_running = vcpu_pause(vcpu);
	if (was_running && (pcpu_id != get_pcpu_id())) {
		while (!is_vcpu_paused(vcpu)) {
			asm_pause();
		}
	}
}

bool is_vcpu_paused(const struct acrn_vcpu *vcpu)
{
	return (vcpu != NULL) && (vcpu_get_state(vcpu) == VCPU_PAUSED) &&
		(vcpu->thread_obj.status == THREAD_STS_BLOCKED);
}

bool is_vcpu_quiesced(const struct acrn_vcpu *vcpu, uint64_t generation)
{
	return (vcpu != NULL) && (vcpu_get_state(vcpu) == VCPU_PAUSED) &&
		is_thread_quiesced(&vcpu->thread_obj, generation);
}

bool request_vcpu_quiesce(struct acrn_vcpu *vcpu, uint64_t generation)
{
	if ((vcpu == NULL) || (generation == 0UL)) {
		return false;
	}
	if (is_vcpu_quiesced(vcpu, generation)) {
		return true;
	}

	(void)vcpu_pause(vcpu);
	return (vcpu_get_state(vcpu) == VCPU_PAUSED) &&
		request_thread_quiesce(&vcpu->thread_obj, generation);
}

bool poweroff_vcpu(struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id;
	bool powered_off;

	if (vcpu == NULL) {
		return false;
	}

	pcpu_id = pcpuid_from_vcpu(vcpu);
	powered_off = vcpu_try_transition_state(vcpu, VCPU_RUNNING,
		VCPU_POWERED_OFF);
	if (powered_off) {
		LOG_DBG("vcpu%hu powered off", vcpu->vcpu_id);
		sleep_thread(&vcpu->thread_obj);
	}

	if (pcpu_id != get_pcpu_id()) {
		while (is_vcpu_powered_off(vcpu) &&
			(vcpu->thread_obj.status != THREAD_STS_BLOCKED)) {
			asm_pause();
		}
	}

	return powered_off;
}
