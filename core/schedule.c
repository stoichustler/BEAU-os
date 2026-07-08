/*
 * Copyright (C) 2018-2025 Intel Corporation.
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <rtl.h>
#include <list.h>
#include <bits.h>
#include <cpu.h>
#include <per_cpu.h>
#include <schedule.h>
#include <sprintf.h>
#include <irq.h>
#include <trace.h>
#include <ticks.h>
#include <vm_config.h>
#include <logmsg.h>
#include <asm/guest/vm_reset.h>

static struct list_head thread_list;
static uint32_t thread_count;
static bool thread_list_initialized;

static void init_thread_list_once(void)
{
	if (!thread_list_initialized) {
		INIT_LIST_HEAD(&thread_list);
		thread_list_initialized = true;
	}
}

const struct list_head *sched_get_thread_list(void)
{
	init_thread_list_once();
	return &thread_list;
}

uint32_t sched_get_thread_count(void)
{
	return thread_count;
}

bool is_idle_thread(const struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	return (obj == &per_cpu(idle, pcpu_id));
}

static inline bool is_blocked(const struct thread_object *obj)
{
	return obj->status == THREAD_STS_BLOCKED;
}

static inline bool is_running(const struct thread_object *obj)
{
	return obj->status == THREAD_STS_RUNNING;
}

static inline bool is_runnable_or_running(const struct thread_object *obj)
{
	return (obj->status == THREAD_STS_RUNNING) || (obj->status == THREAD_STS_RUNNABLE);
}

/*
 * Runtime accounting is tied to scheduler ownership:
 *
 *   switch-in at T0        switch-out at T1
 *   RUNNING state_since -> runtime += T1 - T0
 *
 * Live readers add the current RUNNING delta without mutating scheduler state.
 */
static void sched_add_runtime(struct thread_object *obj, uint64_t ticks)
{
	if (ticks > (UINT64_MAX - obj->latency.runtime_ticks)) {
		obj->latency.runtime_ticks = UINT64_MAX;
	} else {
		obj->latency.runtime_ticks += ticks;
	}
}

static inline void set_thread_status(struct thread_object *obj, enum thread_object_state status)
{
	obj->status = status;
}

/*
 * Hybrid scheduler ownership model:
 *
 * BEAU keeps one scheduler instance per physical CPU. A thread never carries a
 * scheduler choice of its own; its scheduler is derived from thread->pcpu_id
 * and the selected per-pCPU sched_control. This keeps context-switch, wake, and
 * timer ownership local to one partitioned runqueue.
 *
 * For the ARM64 mixed-criticality scenario, static VM affinity is the policy
 * input:
 * - pCPUs assigned to more than one configured VM are shared cores and use
 *   RTDS, so each runnable vCPU is represented as a periodic budget server.
 * - pCPUs assigned to one configured VM are exclusive cores and use BVT, keeping
 *   the lower-overhead fair-share scheduler for helper threads and private vCPU
 *   execution.
 *
 * This function intentionally uses configured affinity rather than runtime VM
 * state. The scheduler must be selected during pCPU initialization, before all
 * VM threads necessarily exist, and must remain stable for every thread whose
 * private scheduler data was initialized against that pCPU.
 */
static bool sched_pcpu_is_shared(uint16_t pcpu_id)
{
	const struct acrn_vm_config *vm_config;
	uint16_t vm_id;
	uint32_t users = 0U;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm_config = get_vm_config(vm_id);
		if ((vm_config->cpu_affinity & (1UL << pcpu_id)) == 0UL) {
			continue;
		}

		users++;
		if (users > 1U) {
			return true;
		}
	}

	return false;
}

static struct acrn_scheduler *select_pcpu_scheduler(uint16_t pcpu_id)
{
	struct acrn_scheduler *scheduler = NULL;

#if defined(CONFIG_SCHED_BVT) && defined(CONFIG_SCHED_RTDS)
	/*
	 * The hybrid policy is deliberately resolved once per pCPU, not per VM or
	 * per vCPU. BVT and RTDS store different private data in thread_object->data,
	 * so changing a pCPU's scheduler after threads have been initialized would
	 * reinterpret that data with the wrong layout.
	 */
	scheduler = sched_pcpu_is_shared(pcpu_id) ? &sched_rtds : &sched_bvt;
#else
#ifdef CONFIG_SCHED_NOOP
	scheduler = &sched_noop;
#endif
#ifdef CONFIG_SCHED_IORR
	scheduler = &sched_iorr;
#endif
#ifdef CONFIG_SCHED_BVT
	scheduler = &sched_bvt;
#endif
#ifdef CONFIG_SCHED_RTDS
	scheduler = &sched_rtds;
#endif
#ifdef CONFIG_SCHED_PRIO
	scheduler = &sched_prio;
#endif
#endif

	return scheduler;
}

static void sched_mark_runnable(struct thread_object *obj, uint64_t now)
{
	obj->latency.state_since = now;
	obj->latency.runnable_since = now;
}

static void sched_mark_blocked(struct thread_object *obj, uint64_t now)
{
	obj->latency.state_since = now;
	obj->latency.runnable_since = 0UL;
}

static void sched_mark_running(struct thread_object *obj, uint64_t now)
{
	uint64_t wait_ticks = 0UL;

	if (obj->latency.runnable_since != 0UL) {
		wait_ticks = now - obj->latency.runnable_since;
		obj->latency.last_wait_ticks = wait_ticks;
		if (wait_ticks > obj->latency.max_wait_ticks) {
			obj->latency.max_wait_ticks = wait_ticks;
		}
	}

	obj->latency.switches++;
	obj->latency.state_since = now;
	obj->latency.runnable_since = 0UL;
}

static void sched_mark_not_running(struct thread_object *obj, uint64_t now, bool runnable)
{
	if (obj->latency.state_since != 0UL) {
		sched_add_runtime(obj, now - obj->latency.state_since);
	}
	obj->latency.state_since = now;
	obj->latency.runnable_since = runnable ? now : 0UL;
}

static void register_thread_object(struct thread_object *obj)
{
	init_thread_list_once();
	if (list_empty(&obj->node)) {
		list_add_tail(&obj->node, &thread_list);
		thread_count++;
	}
}

void obtain_schedule_lock(uint16_t pcpu_id, uint64_t *rflag)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	spinlock_irqsave_obtain(&ctl->scheduler_lock, rflag);
}

void release_schedule_lock(uint16_t pcpu_id, uint64_t rflag)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	spinlock_irqrestore_release(&ctl->scheduler_lock, rflag);
}

static struct acrn_scheduler *get_scheduler(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	return ctl->scheduler;
}

/**
 * @pre obj != NULL
 */
uint16_t sched_get_pcpuid(const struct thread_object *obj)
{
	return obj->pcpu_id;
}

void init_sched(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	spinlock_init(&ctl->scheduler_lock);
	ctl->flags = 0UL;
	ctl->curr_obj = NULL;
	ctl->pcpu_id = pcpu_id;
	ctl->scheduler_ticks = 0UL;
	ctl->context_switches = 0UL;
	ctl->reschedule_requests = 0UL;
	/*
	 * Scheduler selection happens before idle/vCPU/helper threads are attached
	 * to this pCPU. The chosen scheduler owns ctl->priv and the interpretation
	 * of every future thread_object->data bound to this pCPU.
	 */
	ctl->scheduler = select_pcpu_scheduler(pcpu_id);
	ASSERT(ctl->scheduler != NULL, "no scheduler configured!");
	if (ctl->scheduler->init != NULL) {
		ctl->scheduler->init(ctl);
	}
}

void deinit_sched(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	if (ctl->scheduler->deinit != NULL) {
		ctl->scheduler->deinit(ctl);
	}
}

void suspend_sched(void)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, BSP_CPU_ID);

	if (ctl->scheduler->suspend != NULL) {
		ctl->scheduler->suspend(ctl);
	}
}

void resume_sched(void)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, BSP_CPU_ID);

	if (ctl->scheduler->resume != NULL) {
		ctl->scheduler->resume(ctl);
	}
}

void init_thread_data(struct thread_object *obj, struct sched_params *params)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);
	uint64_t rflag;

	INIT_LIST_HEAD(&obj->node);
	(void)memset(&obj->latency, 0U, sizeof(obj->latency));
	obtain_schedule_lock(obj->pcpu_id, &rflag);
	/*
	 * Thread private scheduler data is initialized by the scheduler selected
	 * for the target pCPU. In the hybrid mode this means a vCPU on a shared core
	 * receives RTDS state, while the same VM's vCPU on an exclusive core may
	 * receive BVT state. Moving a thread across pCPUs would require rebuilding
	 * this private data, so the current framework remains partitioned.
	 */
	if (scheduler->init_data != NULL) {
		scheduler->init_data(obj, params);
	}
	/* initial as BLOCKED status, so we can wake it up to run */
	set_thread_status(obj, THREAD_STS_BLOCKED);
	obj->priority_pending = false;
	obj->latency.state_since = cpu_ticks();
	register_thread_object(obj);
	release_schedule_lock(obj->pcpu_id, rflag);
}

void deinit_thread_data(struct thread_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);

	if (scheduler->deinit_data != NULL) {
		scheduler->deinit_data(obj);
	}
}

struct thread_object *sched_get_current(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	return ctl->curr_obj;
}

const char *sched_get_scheduler_name(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	return (ctl->scheduler != NULL) ? ctl->scheduler->name : "none";
}

const char *sched_get_scheduler_stat_desc(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	return (ctl->scheduler != NULL) ? ctl->scheduler->stat_desc : "";
}

uint64_t sched_get_ticks(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	return ctl->scheduler_ticks;
}

uint64_t sched_get_context_switches(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	return ctl->context_switches;
}

uint64_t sched_get_reschedule_requests(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	return ctl->reschedule_requests;
}

void sched_get_latency(const struct thread_object *obj, struct sched_latency_stats *stats)
{
	enum thread_object_state status;
	uint64_t now;

	if ((obj != NULL) && (stats != NULL)) {
		*stats = obj->latency;
		status = obj->status;
		now = cpu_ticks();

		if ((status == THREAD_STS_RUNNABLE) && (stats->runnable_since != 0UL)) {
			uint64_t wait_ticks = now - stats->runnable_since;

			if (wait_ticks > stats->max_wait_ticks) {
				stats->max_wait_ticks = wait_ticks;
			}
		} else if ((status == THREAD_STS_RUNNING) && (stats->state_since != 0UL)) {
			uint64_t run_ticks = now - stats->state_since;

			if (run_ticks > (UINT64_MAX - stats->runtime_ticks)) {
				stats->runtime_ticks = UINT64_MAX;
			} else {
				stats->runtime_ticks += run_ticks;
			}
		}
	}
}

__attribute__((weak)) bool sched_get_bvt_stats(__unused const struct thread_object *obj,
	__unused struct sched_bvt_stats *stats)
{
	return false;
}

__attribute__((weak)) bool sched_get_rtds_stats(__unused const struct thread_object *obj,
	__unused struct sched_rtds_stats *stats)
{
	return false;
}

void sched_account_tick(struct sched_control *ctl)
{
	if (ctl != NULL) {
		ctl->scheduler_ticks++;
	}
}

void make_reschedule_request(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	ctl->reschedule_requests++;
	bitmap_set(NEED_RESCHEDULE, &ctl->flags);
	if (get_pcpu_id() != pcpu_id) {
		arch_send_reschedule_request(pcpu_id);
	}
}

bool need_reschedule(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);

	return bitmap_test(NEED_RESCHEDULE, &ctl->flags);
}

static bool sched_current_is_only_runnable_locked(struct sched_control *ctl)
{
	struct thread_object *current = ctl->curr_obj;
	struct thread_object *obj;
	struct list_head *pos;
	uint32_t runnable = 0U;

	if ((current == NULL) || is_idle_thread(current) || current->be_blocking ||
		!is_runnable_or_running(current)) {
		return false;
	}

	list_for_each(pos, &thread_list) {
		obj = container_of(pos, struct thread_object, node);
		if ((obj->pcpu_id != ctl->pcpu_id) || is_idle_thread(obj) || obj->be_blocking ||
			!is_runnable_or_running(obj)) {
			continue;
		}

		runnable++;
		if ((runnable > 1U) || (obj != current)) {
			return false;
		}
	}

	return runnable == 1U;
}

bool sched_clear_reschedule_if_current_only(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	uint64_t rflag;
	bool cleared = false;

	/*
	 * Architecture code uses this only for forward-progress windows where a
	 * pending guest interrupt is already resident in the current vCPU state.
	 * If the current thread is the only runnable non-idle object, schedule()
	 * would select it again after clearing NEED_RESCHEDULE; doing that under
	 * the scheduler lock avoids consuming a bounded guest IRQ rescue window on
	 * a no-op tick. Shared pCPU fairness is preserved because the helper fails
	 * as soon as any other runnable object exists.
	 */
	obtain_schedule_lock(pcpu_id, &rflag);
	if (bitmap_test(NEED_RESCHEDULE, &ctl->flags) &&
		sched_current_is_only_runnable_locked(ctl)) {
		/*
		 * A scheduler tick can request a reschedule even when the runqueue
		 * contains only the current thread. schedule() would just clear the
		 * flag and pick the same object; do that cheaply when architecture
		 * code must return to the current guest to retire a pending IRQ.
		 */
		bitmap_clear(NEED_RESCHEDULE, &ctl->flags);
		cleared = true;
	}
	release_schedule_lock(pcpu_id, rflag);

	return cleared;
}

void schedule(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	struct acrn_scheduler *scheduler = ctl->scheduler;
	struct thread_object *next = &per_cpu(idle, pcpu_id);
	struct thread_object *prev = ctl->curr_obj;
	struct thread_object *obj;
	struct list_head *pos;
	uint64_t rflag;
	uint64_t now;
	char name[16];

	obtain_schedule_lock(pcpu_id, &rflag);
	/*
	 * Consume one-shot priority requests before pick_next(). The request may be
	 * raised from IRQ/vCPU paths that already hold unrelated locks, so those paths
	 * only set a flag and request reschedule. The scheduler-specific callback runs
	 * here under the scheduler lock, preserving a single lock owner for runqueue
	 * ordering changes.
	 */
	list_for_each(pos, &thread_list) {
		obj = container_of(pos, struct thread_object, node);
		if ((obj->pcpu_id == pcpu_id) && obj->priority_pending) {
			obj->priority_pending = false;
			if ((scheduler->prioritize != NULL) && is_runnable_or_running(obj) && !obj->be_blocking) {
				scheduler->prioritize(obj);
			}
		}
	}
	if (scheduler->pick_next != NULL) {
		next = scheduler->pick_next(ctl);
	}
	bitmap_clear(NEED_RESCHEDULE, &ctl->flags);

	/* If we picked different sched object, switch context */
	if (prev != next) {
		now = cpu_ticks();
		if (prev != NULL) {
			memcpy(name, prev->name, 4);
			memcpy(name + 4, next->name, 4);
			memset(name + 8, 0, sizeof(name) - 8);
			TRACE_16STR(TRACE_SCHED_NEXT, name);
			if (prev->switch_out != NULL) {
				prev->switch_out(prev);
			}
			sched_mark_not_running(prev, now, !prev->be_blocking);
			set_thread_status(prev, prev->be_blocking ? THREAD_STS_BLOCKED : THREAD_STS_RUNNABLE);
			prev->be_blocking = false;
		}

		if (next->switch_in != NULL) {
			next->switch_in(next);
		}
		sched_mark_running(next, now);
		set_thread_status(next, THREAD_STS_RUNNING);

		ctl->curr_obj = next;
		ctl->context_switches++;
		release_schedule_lock(pcpu_id, rflag);
		arch_switch_to(&prev->host_sp, &next->host_sp);
	} else {
		release_schedule_lock(pcpu_id, rflag);
	}
}

void sleep_thread(struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	uint64_t rflag;

	obtain_schedule_lock(pcpu_id, &rflag);
	if (scheduler->sleep != NULL) {
		scheduler->sleep(obj);
	}
	if (is_running(obj)) {
		make_reschedule_request(pcpu_id);
		obj->be_blocking = true;
	} else {
		sched_mark_blocked(obj, cpu_ticks());
		set_thread_status(obj, THREAD_STS_BLOCKED);
	}
	release_schedule_lock(pcpu_id, rflag);
}

void sleep_thread_sync(struct thread_object *obj)
{
	sleep_thread(obj);
	while (!is_blocked(obj)) {
		asm_pause();
	}
}

void wake_thread(struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler;
	uint64_t rflag;

	obtain_schedule_lock(pcpu_id, &rflag);
	if (is_blocked(obj) || obj->be_blocking) {
		scheduler = get_scheduler(pcpu_id);
		if (scheduler->wake != NULL) {
			scheduler->wake(obj);
		}
		if (is_blocked(obj)) {
			sched_mark_runnable(obj, cpu_ticks());
			set_thread_status(obj, THREAD_STS_RUNNABLE);
			make_reschedule_request(pcpu_id);
		}
		obj->be_blocking = false;
	}
	release_schedule_lock(pcpu_id, rflag);
}

void request_thread_priority(struct thread_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);

	/*
	 * The flag is meaningful only for schedulers that opt in via .prioritize.
	 * NEED_RESCHEDULE is still useful for all schedulers because the target pCPU
	 * may need to leave idle or re-check pending vCPU requests.
	 */
	if (scheduler->prioritize != NULL) {
		obj->priority_pending = true;
	}
	make_reschedule_request(obj->pcpu_id);
}

void yield_current(void)
{
	make_reschedule_request(get_pcpu_id());
}

void run_thread(struct thread_object *obj)
{
	uint64_t rflag;

	obtain_schedule_lock(obj->pcpu_id, &rflag);
	get_cpu_var(sched_ctl).curr_obj = obj;
	sched_mark_running(obj, cpu_ticks());
	set_thread_status(obj, THREAD_STS_RUNNING);
	release_schedule_lock(obj->pcpu_id, rflag);

	if (obj->thread_entry != NULL) {
		obj->thread_entry(obj);
	}
}

void default_idle(__unused struct thread_object *obj)
{
	uint16_t pcpu_id = get_pcpu_id();

	while (1) {
		if (need_reschedule(pcpu_id)) {
			schedule();
		} else if (need_offline(pcpu_id)) {
			cpu_dead();
		} else if (need_reset_vm(pcpu_id)) {
			reset_vm_from_idle(pcpu_id);
		} else if (need_shutdown_vm(pcpu_id)) {
			shutdown_vm_from_idle(pcpu_id);
		} else {
			cpu_do_idle();
		}
	}
}

void run_idle_thread(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct thread_object *idle = &per_cpu(idle, pcpu_id);
	struct sched_params idle_params = {0};
	char idle_name[16];

	snprintf(idle_name, 16U, "idle-%02hu", pcpu_id);
	(void)strncpy_s(idle->name, 16U, idle_name, 16U);
	idle->pcpu_id = pcpu_id;
	idle->thread_entry = default_idle;
	idle->switch_out = NULL;
	idle->switch_in = NULL;
	idle_params.prio = PRIO_IDLE;
	init_thread_data(idle, &idle_params);

	run_thread(idle);

	/* Control should not come here */
	cpu_dead();
}
