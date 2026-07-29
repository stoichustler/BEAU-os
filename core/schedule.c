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
#include <logmsg.h>
#include <errno.h>
#include <hv_pm.h>
#include <asm/guest/vm_reset.h>
#include <asm/security.h>

static struct list_head thread_list = { &thread_list, &thread_list };
static uint32_t thread_count;
static spinlock_t thread_registry_lock = { .head = 0U, .tail = 0U };
static struct sched_platform_config sched_platform_config;

/* [20260726] Global scheduler registry ownership
 *
 * VM creator on pCPU A/B -> target scheduler lock -> registry lock -> publish
 * shell observer             -> registry lock -> bounded pointer snapshot
 *
 * Key rule:
 *   - the registry lock owns list topology and thread_count across all pCPUs;
 *   - target scheduler locks still own each thread's runqueue state;
 *   - observers never retain the registry lock across output or scheduling.
 */

/* [20260719] Target-pCPU quiesce completion
 *
 * recovery CPU                    target pCPU
 *      | request slot + VM bit         |
 *      +------------------------------>+-- schedule idle
 *      |                               +-- complete host stack switch
 *      |                               +-- clear VM bit
 *      |<--------- release ACK --------+
 *
 * Key rule:
 *   - scheduler_lock owns each target slot and VM request bit;
 *   - only the target idle context may publish completion;
 *   - the active slot rejects wake until a new vCPU launch releases the gate;
 *   - release/acquire generation matching rejects stale completion.
 */
struct sched_quiesce_slot {
	struct thread_object *obj;
	uint64_t request_generation;
	uint64_t ack_generation;
};

static struct sched_quiesce_slot
	sched_quiesce_slots[MAX_PCPU_NUM][CONFIG_MAX_VM_NUM];

_Static_assert(CONFIG_MAX_VM_NUM <= 64U,
	"scheduler quiesce VM mask supports at most 64 VMs");

uint32_t sched_get_thread_count(void)
{
	uint64_t flags;
	uint32_t count;

	spinlock_irqsave_obtain(&thread_registry_lock, &flags);
	count = thread_count;
	spinlock_irqrestore_release(&thread_registry_lock, flags);
	return count;
}

uint32_t sched_snapshot_threads(struct thread_object **threads, uint32_t max_threads,
	bool *truncated)
{
	struct list_head *pos;
	uint64_t flags;
	uint32_t count = 0U;

	if ((threads == NULL) || (max_threads == 0U)) {
		return 0U;
	}

	if (truncated != NULL) {
		*truncated = false;
	}
	spinlock_irqsave_obtain(&thread_registry_lock, &flags);
	list_for_each(pos, &thread_list) {
		if (count < max_threads) {
			threads[count++] = container_of(pos, struct thread_object, node);
		} else if (truncated != NULL) {
			*truncated = true;
		}
	}
	spinlock_irqrestore_release(&thread_registry_lock, flags);

	return count;
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

/* [20260724] Scheduler trace identity
 *
 * scheduler-owned thread object -> compact trace token -> shell dump decoder
 *
 * Key rule:
 *   - schedule() snapshots identity while it owns the scheduler lock;
 *   - vCPU identity uses immutable VM/vCPU metadata, host threads use a
 *     bounded two-character tag;
 *   - the trace record never retains a thread pointer that teardown can reuse.
 */
static uint32_t sched_trace_thread_token(const struct thread_object *obj)
{
	uint32_t token;

	if (obj->is_vcpu) {
		token = TRACE_SCHED_THREAD_VCPU_FLAG |
			(((uint32_t)obj->vm_id & TRACE_SCHED_THREAD_VM_MASK) <<
			TRACE_SCHED_THREAD_VM_SHIFT) |
			((uint32_t)obj->vcpu_id & TRACE_SCHED_THREAD_VCPU_MASK);
	} else {
		token = ((uint32_t)(uint8_t)obj->name[0] << TRACE_SCHED_THREAD_TAG_SHIFT) |
			((uint32_t)(uint8_t)obj->name[1] & TRACE_SCHED_THREAD_TAG_MASK);
	}

	return token;
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

static uint32_t sched_latency_hist_bucket(uint64_t wait_ticks)
{
	uint64_t wait_us = ticks_to_us(wait_ticks);

	if (wait_us < 100UL) {
		return 0U;
	}
	if (wait_us < 500UL) {
		return 1U;
	}
	if (wait_us < 1000UL) {
		return 2U;
	}
	if (wait_us < 5000UL) {
		return 3U;
	}
	if (wait_us < 10000UL) {
		return 4U;
	}
	if (wait_us < 15000UL) {
		return 5U;
	}
	if (wait_us < 20000UL) {
		return 6U;
	}

	return 7U;
}

const char *sched_latency_hist_bucket_name(uint32_t bucket)
{
	static const char * const names[SCHED_LATENCY_HIST_BUCKETS] = {
		"<100us",
		"<500us",
		"<1ms",
		"<5ms",
		"<10ms",
		"<15ms",
		"<20ms",
		">=20ms",
	};

	return (bucket < SCHED_LATENCY_HIST_BUCKETS) ? names[bucket] : "?";
}

static void sched_record_wait_latency(struct thread_object *obj, uint64_t wait_ticks)
{
	uint32_t bucket = sched_latency_hist_bucket(wait_ticks);

	if (obj->latency.wait_hist[bucket] != UINT64_MAX) {
		obj->latency.wait_hist[bucket]++;
	}
}

static inline void set_thread_status(struct thread_object *obj, enum thread_object_state status)
{
	obj->status = status;
}

void sched_set_platform_config(const struct sched_platform_config *config)
{
	if (config == NULL) {
		(void)memset(&sched_platform_config, 0U, sizeof(sched_platform_config));
	} else {
		(void)memcpy(&sched_platform_config, config, sizeof(sched_platform_config));
	}
}

const struct sched_platform_config *sched_get_platform_config(void)
{
	return &sched_platform_config;
}

static bool sched_cpupool_contains(const struct sched_cpupool_config *pool,
	uint16_t pcpu_id)
{
	return (pool != NULL) && pool->configured && pool->has_pcpu_mask &&
		((pool->pcpu_mask & AFFINITY_CPU(pcpu_id)) != 0UL);
}

const struct sched_cpupool_config *sched_get_pcpu_pool_config(uint16_t pcpu_id)
{
	const struct sched_cpupool_config *pool = NULL;

	if (sched_platform_config.configured) {
		if (sched_cpupool_contains(&sched_platform_config.exclusive, pcpu_id)) {
			pool = &sched_platform_config.exclusive;
		} else if (sched_cpupool_contains(&sched_platform_config.shared, pcpu_id)) {
			pool = &sched_platform_config.shared;
		}
	}

	return pool;
}

static struct acrn_scheduler *scheduler_from_policy(enum sched_policy_id policy)
{
	struct acrn_scheduler *scheduler = NULL;

	/*
	 * DTS scheduler policy comparison:
	 *
	 * noop:
	 *   Fit: an exclusive pCPU that is guaranteed to have only one runnable
	 *   non-idle object. It is the lowest-overhead policy, but it is fragile if
	 *   a second runnable object is later placed on the same pCPU.
	 *   Principle: remember one runnable object and always select it; otherwise
	 *   run idle.
	 *   Main parameters: none.
	 *
	 * iorr:
	 *   Fit: simple equal treatment for multiple same-class runnable objects.
	 *   It is easy to reason about, but less latency/fairness-aware than BVT or
	 *   the budget schedulers.
	 *   Principle: fixed time-slice round robin; the local scheduler tick charges
	 *   the current object and rotates it to the runqueue tail when its slice is
	 *   exhausted.
	 *   Main parameters: compile-time slice length in sched_iorr.c
	 *   (CONFIG_SLICE_MS, currently 10 ms) and the 1 ms scheduler tick.
	 *
	 * bvt:
	 *   Fit: exclusive pCPUs and general mixed workloads that need low overhead,
	 *   proportional share, and good event/wakeup response. This is the default
	 *   fit for BEAU exclusive cores.
	 *   Principle: charge actual runtime into actual virtual time (AVT), sort
	 *   runnable objects by effective virtual time (EVT), and run the lowest EVT.
	 *   Higher weight advances virtual time more slowly. Bounded warp gives a
	 *   temporary ordering boost for wake/event latency without changing long-term
	 *   fairness.
	 *   Main parameters: sched_params.bvt_weight, bvt_warp_value,
	 *   bvt_warp_limit, bvt_unwarp_period, plus Kconfig MCU and CSA values.
	 *
	 * cbs:
	 *   Fit: shared pCPUs with bursty or wake-heavy vCPUs where bandwidth should
	 *   be capped but boot and IRQ bursts should still make progress. This is the
	 *   default fit for BEAU shared Linux/service vCPUs.
	 *   Principle: each object is a Constant Bandwidth Server with budget Q and
	 *   period T. Runnable objects are EDF-ordered by deadline. Runtime consumes
	 *   remaining budget; depletion shifts the deadline and replenishes budget,
	 *   so long-term demand is limited to Q/T.
	 *   Main parameters: cpupool period/budget, or per-VM
	 *   sched_params.cbs_period_us and cbs_budget_us when provided.
	 *
	 * rtds:
	 *   Fit: shared pCPUs with periodic real-time style load where fixed period
	 *   boundaries are more important than burst absorption.
	 *   Principle: each object has a periodic budget server. EDF chooses the
	 *   earliest deadline among servers with remaining budget; depleted servers
	 *   wait for the next period boundary, with work-conserving slack only when no
	 *   budgeted server is ready.
	 *   Main parameters: cpupool period/budget, normalized by sched_rtds.c.
	 *
	 * prio:
	 *   Fit: small, controlled sets of runnable objects with a clear static
	 *   priority order. It is unsuitable as a general fairness policy because low
	 *   priority objects can starve.
	 *   Principle: keep the runqueue sorted by fixed priority and always pick the
	 *   highest-priority runnable object.
	 *   Main parameters: sched_params.prio.
	 */
	switch (policy) {
	case SCHED_POLICY_NOOP:
		scheduler = &sched_noop;
		break;
	case SCHED_POLICY_IORR:
		scheduler = &sched_iorr;
		break;
	case SCHED_POLICY_BVT:
		scheduler = &sched_bvt;
		break;
	case SCHED_POLICY_RTDS:
		scheduler = &sched_rtds;
		break;
	case SCHED_POLICY_CBS:
		scheduler = &sched_cbs;
		break;
	case SCHED_POLICY_PRIO:
		scheduler = &sched_prio;
		break;
	default:
		break;
	}

	return scheduler;
}

static struct acrn_scheduler *select_dts_pcpu_scheduler(uint16_t pcpu_id)
{
	const struct sched_cpupool_config *pool = sched_get_pcpu_pool_config(pcpu_id);
	struct acrn_scheduler *scheduler;

	/* [20260710] DTS scheduler ownership:
	 *
	 *   /hypervisor/sched
	 *       +-- exclusive-cpupool: pcpus + policy
	 *       +-- shared-cpupool:    pcpus + policy
	 *                  |
	 *                  v
	 *          one fixed scheduler per pCPU
	 *
	 * Kconfig only compiles scheduler backends in SCHED_DTS mode. The platform
	 * device tree owns the pCPU pool membership and the selected algorithm.
	 */
	if (!sched_platform_config.configured || (pool == NULL)) {
		panic("missing DTS scheduler cpupool for pCPU%hu", pcpu_id);
	}

	scheduler = scheduler_from_policy(pool->policy);
	if (scheduler == NULL) {
		panic("invalid DTS scheduler policy for pCPU%hu", pcpu_id);
	}

	return scheduler;
}

static struct acrn_scheduler *select_pcpu_scheduler(uint16_t pcpu_id)
{
	return select_dts_pcpu_scheduler(pcpu_id);
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
		sched_record_wait_latency(obj, wait_ticks);
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
	uint64_t flags;

	spinlock_irqsave_obtain(&thread_registry_lock, &flags);
	if (list_empty(&obj->node)) {
		list_add_tail(&obj->node, &thread_list);
		thread_count++;
	}
	spinlock_irqrestore_release(&thread_registry_lock, flags);
}

static void unregister_thread_object(struct thread_object *obj)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&thread_registry_lock, &flags);
	if (!list_empty(&obj->node)) {
		list_del_init(&obj->node);
		if (thread_count != 0U) {
			thread_count--;
		}
	}
	spinlock_irqrestore_release(&thread_registry_lock, flags);
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
	ctl->quiesce_request_vm_mask = 0UL;
	ctl->priority_pending = false;
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
	/*
	 * New objects enter through wake() even before first execution. That gives
	 * each scheduler one admission path for initial placement and later unblock.
	 */
	set_thread_status(obj, THREAD_STS_BLOCKED);
	obj->priority_pending = false;
	obj->freeze_epoch = 0UL;
	obj->deferred_wake = false;
	obj->latency.state_since = cpu_ticks();
	register_thread_object(obj);
	release_schedule_lock(obj->pcpu_id, rflag);
}

void deinit_thread_data(struct thread_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);
	uint64_t rflag;

	obtain_schedule_lock(obj->pcpu_id, &rflag);
	if (scheduler->deinit_data != NULL) {
		scheduler->deinit_data(obj);
	}
	unregister_thread_object(obj);
	release_schedule_lock(obj->pcpu_id, rflag);
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

__attribute__((weak)) bool sched_get_cbs_stats(__unused const struct thread_object *obj,
	__unused struct sched_cbs_stats *stats)
{
	return false;
}

__attribute__((weak)) bool sched_get_cbs_pcpu_stats(__unused uint16_t pcpu_id,
	__unused struct sched_cbs_pcpu_stats *stats)
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
	uint64_t flags;
	uint32_t runnable = 0U;

	if ((current == NULL) || is_idle_thread(current) || current->be_blocking ||
		!is_runnable_or_running(current)) {
		return false;
	}

	spinlock_irqsave_obtain(&thread_registry_lock, &flags);
	list_for_each(pos, &thread_list) {
		obj = container_of(pos, struct thread_object, node);
		if ((obj->pcpu_id != ctl->pcpu_id) || is_idle_thread(obj) || obj->be_blocking ||
			!is_runnable_or_running(obj)) {
			continue;
		}

		runnable++;
		if ((runnable > 1U) || (obj != current)) {
			break;
		}
	}
	spinlock_irqrestore_release(&thread_registry_lock, flags);

	return (runnable == 1U) && (current == obj);
}

static bool sched_idle_work_pending_locked(const struct sched_control *ctl)
{
	return has_system_suspend_request(ctl->pcpu_id) ||
		has_reset_vm_request(ctl->pcpu_id) ||
		(ctl->quiesce_request_vm_mask != 0UL);
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
	 * as soon as any other runnable object exists. Idle-owned management work
	 * is also excluded because schedule() must observe it and select idle even
	 * when the current vCPU is the only runnable thread.
	 */
	obtain_schedule_lock(pcpu_id, &rflag);
	if (!sched_idle_work_pending_locked(ctl) &&
		bitmap_test(NEED_RESCHEDULE, &ctl->flags) &&
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

	obtain_schedule_lock(pcpu_id, &rflag);
	if (ctl->priority_pending) {
		/*
		 * Consume one-shot priority requests before pick_next(). Request paths
		 * may already hold IRQ/vCPU locks, so they only set lockless flags and
		 * raise NEED_RESCHEDULE. The per-pCPU flag keeps ordinary schedule()
		 * calls from scanning the global thread list.
		 *
		 *   IRQ/vCPU event
		 *        |
		 *        v
		 *   request_thread_priority()
		 *        |
		 *        v
		 *   per-pCPU priority_pending
		 *        |
		 *        v
		 *   schedule() -> scheduler->prioritize() -> pick_next()
		 */
		ctl->priority_pending = false;
		uint64_t registry_flags;

		spinlock_irqsave_obtain(&thread_registry_lock, &registry_flags);
		list_for_each(pos, &thread_list) {
			obj = container_of(pos, struct thread_object, node);
			if ((obj->pcpu_id == pcpu_id) && obj->priority_pending) {
				obj->priority_pending = false;
				if ((scheduler->prioritize != NULL) &&
					is_runnable_or_running(obj) && !obj->be_blocking) {
					scheduler->prioritize(obj);
				}
			}
		}
		spinlock_irqrestore_release(&thread_registry_lock, registry_flags);
	}
	if (sched_idle_work_pending_locked(ctl)) {
		next = &per_cpu(idle, pcpu_id);
	} else if (scheduler->pick_next != NULL) {
		next = scheduler->pick_next(ctl);
	}
	bitmap_clear(NEED_RESCHEDULE, &ctl->flags);

	/* If we picked different sched object, switch context */
	if (prev != next) {
		const struct arm64_ptrauth_key *next_ptrauth_key = NULL;
		bool authenticate_return = false;
		bool ptrauth_active = false;

		#if CONFIG_ARM64_PTRAUTH
		ptrauth_active = arm64_ptrauth_active_current();
		if (ptrauth_active) {
			next_ptrauth_key = &next->ptrauth_key;
			authenticate_return = next->ptrauth_return_authenticated;
			next->ptrauth_return_authenticated = true;
		}
		#endif

		now = cpu_ticks();
		if (prev != NULL) {
			TRACE_4I(TRACE_SCHED_NEXT, sched_trace_thread_token(prev),
				sched_trace_thread_token(next), 0U, 0U);
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
		arch_switch_to(&prev->host_sp, &next->host_sp, next_ptrauth_key,
			authenticate_return, ptrauth_active);
	} else {
		release_schedule_lock(pcpu_id, rflag);
	}
}

static bool sleep_thread_locked(struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	bool wake_owned = !is_blocked(obj) && !obj->be_blocking;

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

	return wake_owned;
}

void sleep_thread(struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	uint64_t rflag;

	obtain_schedule_lock(pcpu_id, &rflag);
	(void)sleep_thread_locked(obj);
	release_schedule_lock(pcpu_id, rflag);
}

void sleep_thread_sync(struct thread_object *obj)
{
	sleep_thread(obj);
	while (!is_blocked(obj)) {
		asm_pause();
	}
}

bool request_thread_quiesce(struct thread_object *obj, uint64_t generation)
{
	struct sched_quiesce_slot *slot;
	struct thread_object *owner;
	struct sched_control *ctl;
	uint64_t request_generation;
	uint64_t ack_generation;
	uint64_t rflag;
	uint16_t pcpu_id;
	bool requested = false;

	if ((obj == NULL) || !obj->is_vcpu || (generation == 0UL) ||
		(obj->pcpu_id >= MAX_PCPU_NUM) || (obj->vm_id >= CONFIG_MAX_VM_NUM)) {
		return false;
	}

	pcpu_id = obj->pcpu_id;
	ctl = obj->sched_ctl;
	slot = &sched_quiesce_slots[pcpu_id][obj->vm_id];
	obtain_schedule_lock(pcpu_id, &rflag);
	owner = __atomic_load_n(&slot->obj, __ATOMIC_ACQUIRE);
	request_generation = __atomic_load_n(&slot->request_generation,
		__ATOMIC_ACQUIRE);
	ack_generation = __atomic_load_n(&slot->ack_generation, __ATOMIC_ACQUIRE);
	if ((ctl != NULL) && ((owner == NULL) || (owner == obj) ||
		(request_generation == ack_generation))) {
		__atomic_store_n(&slot->obj, obj, __ATOMIC_RELEASE);
		__atomic_store_n(&slot->request_generation, generation,
			__ATOMIC_RELEASE);
		bitmap_set_non_atomic(obj->vm_id, &ctl->quiesce_request_vm_mask);
		requested = true;
	}
	release_schedule_lock(pcpu_id, rflag);

	if (requested) {
		/* Reassert the SGI on every unacknowledged watchdog poll. */
		make_reschedule_request(pcpu_id);
	}

	return requested;
}

bool is_thread_quiesced(const struct thread_object *obj, uint64_t generation)
{
	const struct sched_quiesce_slot *slot;

	if ((obj == NULL) || !obj->is_vcpu || (generation == 0UL) ||
		(obj->pcpu_id >= MAX_PCPU_NUM) || (obj->vm_id >= CONFIG_MAX_VM_NUM)) {
		return false;
	}

	slot = &sched_quiesce_slots[obj->pcpu_id][obj->vm_id];
	return (__atomic_load_n(&slot->obj, __ATOMIC_ACQUIRE) == obj) &&
		(__atomic_load_n(&slot->request_generation, __ATOMIC_ACQUIRE) ==
		generation) &&
		(__atomic_load_n(&slot->ack_generation, __ATOMIC_ACQUIRE) ==
		generation);
}

void release_thread_quiesce(struct thread_object *obj)
{
	struct sched_quiesce_slot *slot;
	uint64_t rflag;
	uint16_t pcpu_id;

	if ((obj == NULL) || !obj->is_vcpu || (obj->pcpu_id >= MAX_PCPU_NUM) ||
		(obj->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	pcpu_id = obj->pcpu_id;
	slot = &sched_quiesce_slots[pcpu_id][obj->vm_id];
	obtain_schedule_lock(pcpu_id, &rflag);
	if (__atomic_load_n(&slot->obj, __ATOMIC_ACQUIRE) == obj) {
		bitmap_clear_non_atomic(obj->vm_id,
			&obj->sched_ctl->quiesce_request_vm_mask);
		__atomic_store_n(&slot->request_generation, 0UL, __ATOMIC_RELEASE);
		__atomic_store_n(&slot->ack_generation, 0UL, __ATOMIC_RELEASE);
		__atomic_store_n(&slot->obj, NULL, __ATOMIC_RELEASE);
	}
	release_schedule_lock(pcpu_id, rflag);
}

static bool sched_ack_quiesce_from_idle(uint16_t pcpu_id)
{
	struct sched_control *ctl = &per_cpu(sched_ctl, pcpu_id);
	struct thread_object *idle = &per_cpu(idle, pcpu_id);
	uint64_t request_vm_mask;
	uint64_t rflag;
	uint16_t vm_id;
	bool acked = false;

	obtain_schedule_lock(pcpu_id, &rflag);
	request_vm_mask = ctl->quiesce_request_vm_mask;
	vm_id = ffs64(request_vm_mask);
	while (vm_id < CONFIG_MAX_VM_NUM) {
		struct sched_quiesce_slot *slot =
			&sched_quiesce_slots[pcpu_id][vm_id];
		struct thread_object *obj = __atomic_load_n(&slot->obj,
			__ATOMIC_ACQUIRE);
		uint64_t generation = __atomic_load_n(&slot->request_generation,
			__ATOMIC_ACQUIRE);

		bitmap_clear_non_atomic(vm_id, &request_vm_mask);
		if ((ctl->curr_obj == idle) && (obj != NULL) && obj->is_vcpu &&
			(obj->pcpu_id == pcpu_id) && (obj->vm_id == vm_id) &&
			(generation != 0UL) && is_blocked(obj) && !obj->be_blocking &&
			(ctl->curr_obj != obj)) {
			/* Clear target work before release-publishing switch completion. */
			bitmap_clear_non_atomic(vm_id, &ctl->quiesce_request_vm_mask);
			__atomic_store_n(&slot->ack_generation, generation,
				__ATOMIC_RELEASE);
			acked = true;
		}
		vm_id = ffs64(request_vm_mask);
	}
	release_schedule_lock(pcpu_id, rflag);

	return acked;
}

static void wake_thread_locked(struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler;

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
}

static bool thread_quiesce_active_locked(const struct thread_object *obj)
{
	const struct sched_quiesce_slot *slot;

	if (!obj->is_vcpu || (obj->pcpu_id >= MAX_PCPU_NUM) ||
		(obj->vm_id >= CONFIG_MAX_VM_NUM)) {
		return false;
	}

	slot = &sched_quiesce_slots[obj->pcpu_id][obj->vm_id];
	return (__atomic_load_n(&slot->obj, __ATOMIC_ACQUIRE) == obj) &&
		(__atomic_load_n(&slot->request_generation, __ATOMIC_ACQUIRE) != 0UL);
}

void wake_thread(struct thread_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	uint64_t rflag;

	obtain_schedule_lock(pcpu_id, &rflag);
	if (!thread_quiesce_active_locked(obj)) {
		if (obj->freeze_epoch != 0UL) {
			obj->deferred_wake = true;
		} else {
			wake_thread_locked(obj);
		}
	}
	release_schedule_lock(pcpu_id, rflag);
}

/* [20260716] Transparent freeze scheduler gate
 *
 * Host PM                         target scheduler
 *    | freeze_thread(epoch)              |
 *    +---------------------------------->| block and publish epoch gate
 *    |                                   | wake -> deferred_wake only
 *    |<----------------------------------+ no longer current
 *    | thaw_thread(epoch, ownership)     |
 *    +---------------------------------->| clear gate, replay one wake
 *
 * Key rules:
 *   - the scheduler lock serializes freeze against every wake producer;
 *   - a gate never changes vCPU lifecycle state or architecture context;
 *   - thaw wakes only work owned by PM or observed while the gate was active.
 */
bool freeze_thread(struct thread_object *obj, uint64_t epoch,
	bool *wake_owned)
{
	uint16_t pcpu_id;
	uint64_t rflag;
	bool frozen = false;

	if ((obj == NULL) || (epoch == 0UL) || (wake_owned == NULL)) {
		return false;
	}
	pcpu_id = obj->pcpu_id;
	*wake_owned = false;

	obtain_schedule_lock(pcpu_id, &rflag);
	if ((obj->freeze_epoch == 0UL) || (obj->freeze_epoch == epoch)) {
		if (obj->freeze_epoch == 0UL) {
			obj->freeze_epoch = epoch;
			obj->deferred_wake = false;
		}
		*wake_owned = sleep_thread_locked(obj);
		frozen = true;
	}
	release_schedule_lock(pcpu_id, rflag);

	return frozen;
}

bool is_thread_frozen(struct thread_object *obj, uint64_t epoch)
{
	struct sched_control *ctl;
	uint16_t pcpu_id;
	uint64_t rflag;
	bool frozen;

	if ((obj == NULL) || (epoch == 0UL)) {
		return false;
	}
	pcpu_id = obj->pcpu_id;
	ctl = obj->sched_ctl;

	obtain_schedule_lock(pcpu_id, &rflag);
	frozen = (obj->freeze_epoch == epoch) &&
		(obj->status == THREAD_STS_BLOCKED) && !obj->be_blocking &&
		(ctl->curr_obj != obj);
	release_schedule_lock(pcpu_id, rflag);

	return frozen;
}

bool thaw_thread(struct thread_object *obj, uint64_t epoch, bool wake_owned)
{
	uint16_t pcpu_id;
	uint64_t rflag;
	bool thawed = false;
	bool replay_wake;

	if ((obj == NULL) || (epoch == 0UL)) {
		return false;
	}
	pcpu_id = obj->pcpu_id;

	obtain_schedule_lock(pcpu_id, &rflag);
	if (obj->freeze_epoch == epoch) {
		replay_wake = wake_owned || obj->deferred_wake;
		obj->freeze_epoch = 0UL;
		obj->deferred_wake = false;
		if (replay_wake) {
			wake_thread_locked(obj);
		}
		thawed = true;
	}
	release_schedule_lock(pcpu_id, rflag);

	return thawed;
}

void request_thread_priority(struct thread_object *obj)
{
	/*
	 * The flag is meaningful only for schedulers that opt in via .prioritize.
	 * NEED_RESCHEDULE is still useful for all schedulers because the target pCPU
	 * may need to leave idle or re-check pending vCPU requests.
	 */
	request_thread_priority_no_resched(obj);
	make_reschedule_request(obj->pcpu_id);
}

void request_thread_priority_no_resched(struct thread_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);

	if (scheduler->prioritize != NULL) {
		obj->priority_pending = true;
		per_cpu(sched_ctl, obj->pcpu_id).priority_pending = true;
	}
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

/* [20260717] Idle management scheduler handoff
 *
 * scheduler -> idle -> PM transaction / VM reset -> scheduler
 *
 * Key rule:
 *   - PM or VM topology state owns the active management transaction;
 *   - schedule() selects idle while management work is pending;
 *   - common idle owns the return handoff after the transaction completes.
 */
void default_idle(__unused struct thread_object *obj)
{
	uint16_t pcpu_id = get_pcpu_id();

	while (1) {
		if (sched_ack_quiesce_from_idle(pcpu_id)) {
			make_reschedule_request(pcpu_id);
		}
		if (need_system_suspend(pcpu_id)) {
			hv_pm_process_from_idle(pcpu_id);
			make_reschedule_request(pcpu_id);
		} else if (need_reschedule(pcpu_id)) {
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
	idle->host_stack_base = (uint64_t)&per_cpu(stack, pcpu_id)[0];
	idle->host_stack_size = CONFIG_STACK_SIZE;
	#if CONFIG_ARM64_PTRAUTH
	arm64_ptrauth_bind_idle_thread(idle);
	#endif
	idle_params.prio = PRIO_IDLE;
	init_thread_data(idle, &idle_params);

	run_thread(idle);

	cpu_dead();
}
