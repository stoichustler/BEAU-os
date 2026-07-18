/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Partitioned Constant Bandwidth Server scheduler for BEAU.
 *
 * [20260711] CBS framework on BEAU:
 *
 *   platform.dts
 *   shared-cpupool {
 *       policy = "cbs";
 *       period = <T>;
 *       budget = <Q>;
 *   }
 *          |
 *          v
 *   one sched_cbs_control per pCPU
 *          |
 *          +--> EDF runqueue ordered by absolute deadline
 *          |
 *          +--> local one-shot timer
 *                 min(budget exhaustion, current deadline)
 *
 *   VM/vCPU thread_object
 *          |
 *          v
 *   sched_cbs_data { period, budget, remaining, deadline, last_start }
 *
 * BEAU keeps CBS partitioned by pCPU. There is no global EDF queue and no vCPU
 * migration; the pCPU selected by static VM affinity owns both server state and
 * the local scheduler timer.
 *
 * CBS models each non-idle scheduler object as a reservation server:
 *
 *     budget Q = 2ms, period T = 4ms
 *
 *     execution consumes remaining budget
 *           |
 *           v
 *     remaining == 0
 *           |
 *           v
 *     deadline += T, remaining = Q
 *
 * Runnable servers are ordered by absolute deadline. When an inactive server is
 * woken, the CBS admission rule decides whether to keep its old server state or
 * start a fresh period at now + T:
 *
 *     remaining / (deadline - now) > budget / period
 *
 * Knowledge notes:
 * - "Constant bandwidth" means a server's long-term demand is capped by Q/T.
 * - Deadline is both an EDF key and a bandwidth-control mechanism. When a
 *   server exhausts budget before its deadline, CBS postpones its deadline and
 *   replenishes budget instead of letting old priority create extra bandwidth.
 * - Late timer/accounting delivery carries excess runtime into later server
 *   windows, so interrupt-off or emulator delay does not create free CPU time.
 * - The inactive wake rule prevents a sleeping server from hoarding old budget
 *   and then using it at an earlier deadline after a bursty wakeup.
 * - Active refresh is intentionally narrower than wake refresh: a running or
 *   already-runnable server only handles depletion/deadline rollover; density
 *   checks are reserved for inactive demand entering the runqueue.
 *
 * Main flow:
 *
 *   wake inactive server
 *          |
 *          v
 *   CBS density rule
 *          |
 *          v
 *   insert into EDF runqueue
 *          |
 *          v
 *   schedule() / timer callback
 *          |
 *          +--> account running time
 *          +--> refresh active server window
 *          +--> pick earliest deadline
 *          +--> arm local one-shot timer
 *
 * [20260713] Soft-CBS boot-latency optimization:
 *
 * Reason:
 * - QEMU 4OS boot puts several secondary vCPUs on the shared CBS pool. During
 *   Linux boot, virtio, timers, and IRQ kicks generate bursty runnable demand.
 * - A previous hard-CBS experiment strictly throttled depleted servers and made
 *   boot-time kick-to-run latency worse. Current BEAU keeps soft CBS as the
 *   default for shared Linux/service vCPUs because it can absorb boot bursts by
 *   moving deadlines instead of idling until a hard replenishment point.
 * - Generic event kicks call request_thread_priority() even when the target vCPU
 *   is already RUNNABLE. Treating that as a fresh inactive wake can run the CBS
 *   density rule again and push the target's deadline later during a kick storm.
 *   Therefore wake() owns inactive admission; prioritize() only refreshes the
 *   active server window and reinserts the object by its existing CBS deadline.
 *
 * Current soft-CBS event framework:
 *
 *   BLOCKED vCPU wake
 *          |
 *          v
 *   sched_cbs_wake()
 *          |
 *          +--> inactive density rule
 *          +--> optional now+period replenishment
 *          +--> EDF runqueue insert
 *
 *   IRQ/timer kick for RUNNABLE vCPU
 *          |
 *          v
 *   request_thread_priority()
 *          |
 *          v
 *   schedule() consumes priority_pending
 *          |
 *          v
 *   sched_cbs_prioritize()
 *          |
 *          +--> active window refresh only
 *          +--> preserve existing deadline when still admissible
 *          +--> EDF runqueue reinsert
 *
 *   timer / pick_next / sleep
 *          |
 *          v
 *   cbs_account_runtime()
 *          |
 *          +--> charge runtime across late windows
 *          +--> count dep/repl/late for schedstat and vmstat
 *
 * Counter meaning:
 * - dep:  budget depletion events caused by charged runtime.
 * - repl: budget replenishments caused by depletion/deadline rollover.
 * - wake: replenishments caused specifically by inactive wake admission.
 * - late: accounting observed after budget exhaustion or deadline had already
 *         passed, which points to timer latency, IRQ-off time, or overloaded
 *         scheduling windows rather than a CBS policy decision alone.
 *
 * Admission and placement:
 * - Admission stats use DTS cpu_affinity_order when present, because it is the
 *   actual vCPU-index to pCPU mapping. The bitmap is only a legacy set view and
 *   cannot explain which shared pCPU is overloaded.
 * - QEMU platform affinity is also part of the optimization: spreading secondary
 *   vCPUs avoids making pCPU2 the bottleneck before changing CBS policy.
 *
 * CBS and RTDS comparison in this tree:
 *
 *   +--------------------+--------------------------+--------------------------+
 *   | Item               | CBS                      | RTDS                     |
 *   +--------------------+--------------------------+--------------------------+
 *   | Queue key          | Absolute deadline        | Absolute deadline        |
 *   | Period model       | Deadline can move        | Fixed period boundary    |
 *   | Wake behavior      | Density rule on inactive | No special wake credit   |
 *   | Budget exhaustion  | Replenish with later DDL | Wait/refresh at period   |
 *   | Burst handling     | Good for wake bursts     | Better for periodic load |
 *   | Bandwidth control  | Q/T via deadline shift   | Q/T via period refresh   |
 *   | BEAU fit           | Shared bursty vCPUs      | Shared periodic vCPUs    |
 *   +--------------------+--------------------------+--------------------------+
 */

#include <list.h>
#include <logmsg.h>
#include <per_cpu.h>
#include <schedule.h>
#include <ticks.h>
#include <util.h>
#include <vconfig.h>

#define CBS_DEFAULT_PERIOD_US	4000U
#define CBS_DEFAULT_BUDGET_US	2000U
#define CBS_MIN_PERIOD_US	500U
#define CBS_MIN_BUDGET_US	500U
#define CBS_UTIL_SCALE		1000000UL

struct sched_cbs_data {
	/* keep list as the first item */
	struct list_head list;
	uint64_t period_ticks;
	uint64_t budget_ticks;
	uint64_t remaining_ticks;
	uint64_t deadline_ticks;
	uint64_t last_start_ticks;
	uint64_t depleted_count;
	uint64_t replenish_count;
	uint64_t wake_replenish_count;
	uint64_t late_account_count;
};

static bool cbs_is_queued(const struct sched_cbs_data *data)
{
	return !list_empty(&data->list);
}

static bool cbs_is_current(const struct thread_object *obj)
{
	return obj->sched_ctl->curr_obj == obj;
}

static bool cbs_is_active(const struct thread_object *obj)
{
	/*
	 * schedule() marks the outgoing thread runnable after pick_next(). A
	 * blocking current object is advertised through be_blocking before that
	 * status transition, so CBS must not requeue it while selecting a successor.
	 */
	return !is_idle_thread(obj) &&
		((cbs_is_current(obj) && !obj->be_blocking) ||
		 (obj->status == THREAD_STS_RUNNABLE));
}

static void cbs_normalize_us(uint16_t pcpu_id, const struct sched_params *params,
	uint32_t *period_us, uint32_t *budget_us)
{
	const struct sched_cpupool_config *pool = sched_get_pcpu_pool_config(pcpu_id);
	uint32_t period = (params->cbs_period_us != 0U) ?
		params->cbs_period_us : CBS_DEFAULT_PERIOD_US;
	uint32_t budget = (params->cbs_budget_us != 0U) ?
		params->cbs_budget_us : CBS_DEFAULT_BUDGET_US;

	if ((pool != NULL) && (pool->policy == SCHED_POLICY_CBS)) {
		if ((params->cbs_period_us == 0U) && (pool->period_us != 0U)) {
			period = pool->period_us;
		}
		if ((params->cbs_budget_us == 0U) && (pool->budget_us != 0U)) {
			budget = pool->budget_us;
		}
	}

	if (period < CBS_MIN_PERIOD_US) {
		panic("CBS pCPU%hu period %u below minimum %u",
			pcpu_id, period, CBS_MIN_PERIOD_US);
	}
	if (budget < CBS_MIN_BUDGET_US) {
		panic("CBS pCPU%hu budget %u below minimum %u",
			pcpu_id, budget, CBS_MIN_BUDGET_US);
	}
	if (budget > period) {
		panic("CBS pCPU%hu budget %u > period %u",
			pcpu_id, budget, period);
	}

	*period_us = period;
	*budget_us = budget;
}

static void cbs_count_event(uint64_t *counter)
{
	if (*counter != UINT64_MAX) {
		(*counter)++;
	}
}

static void cbs_replenish_from_now(struct sched_cbs_data *data, uint64_t now,
	bool count)
{
	data->deadline_ticks = now + data->period_ticks;
	data->remaining_ticks = data->budget_ticks;
	if (count) {
		cbs_count_event(&data->replenish_count);
	}
}

static void cbs_replenish_next_window(struct sched_cbs_data *data, bool count)
{
	if (data->deadline_ticks > (UINT64_MAX - data->period_ticks)) {
		data->deadline_ticks = UINT64_MAX;
	} else {
		data->deadline_ticks += data->period_ticks;
	}
	data->remaining_ticks = data->budget_ticks;
	if (count) {
		cbs_count_event(&data->replenish_count);
	}
}

static void cbs_replenish_after_depletion(struct sched_cbs_data *data, uint64_t now,
	bool count)
{
	if (data->deadline_ticks == 0UL) {
		cbs_replenish_from_now(data, now, count);
	} else {
		do {
			cbs_replenish_next_window(data, count);
		} while ((data->deadline_ticks <= now) &&
			(data->deadline_ticks != UINT64_MAX));
	}
}

/*
 * Timer callbacks can arrive late while the current vCPU keeps running. Charge
 * the whole observed runtime by rolling through as many CBS windows as needed:
 *
 *   delta > remaining
 *        |
 *        +--> remaining = 0
 *        +--> deadline += period, remaining = budget
 *        +--> continue charging leftover delta
 */
static void cbs_charge_runtime(struct sched_cbs_data *data, uint64_t delta, bool count)
{
	while (delta != 0UL) {
		if (data->remaining_ticks == 0UL) {
			cbs_replenish_next_window(data, count);
		}

		if (delta < data->remaining_ticks) {
			data->remaining_ticks -= delta;
			break;
		}

		delta -= data->remaining_ticks;
		data->remaining_ticks = 0UL;
		if (count) {
			cbs_count_event(&data->depleted_count);
		}
	}
}

/*
 * Compare two fractions without truncating the result. The wake density rule is
 * algebraically a cross product, and DTS can raise period/budget enough that a
 * plain 64-bit multiply would become the hidden failure mode.
 */
static bool cbs_ratio_greater(uint64_t left_num, uint64_t left_den,
	uint64_t right_num, uint64_t right_den)
{
#if defined(__SIZEOF_INT128__)
	return (((__uint128_t)left_num) * right_den) >
		(((__uint128_t)right_num) * left_den);
#else
	uint64_t left;
	uint64_t right;
	bool left_overflow = (left_num != 0UL) && (right_den > (UINT64_MAX / left_num));
	bool right_overflow = (right_num != 0UL) && (left_den > (UINT64_MAX / right_num));

	if (left_overflow || right_overflow) {
		uint64_t left_q = left_num / left_den;
		uint64_t right_q = right_num / right_den;

		if (left_q != right_q) {
			return left_q > right_q;
		}

		return left_overflow && !right_overflow;
	}

	left = left_num * right_den;
	right = right_num * left_den;

	return left > right;
#endif
}

static bool cbs_needs_wake_replenish(const struct sched_cbs_data *data, uint64_t now)
{
	uint64_t window;

	if (data->deadline_ticks <= now) {
		return true;
	}

	window = data->deadline_ticks - now;

	return cbs_ratio_greater(data->remaining_ticks, window,
		data->budget_ticks, data->period_ticks);
}

/*
 * Active refresh is used for a server that is already running or already on the
 * runqueue. It handles time-window boundaries only; it must not run the CBS
 * inactive wake density rule, because that rule represents new demand entering
 * after an idle/sleep interval.
 */
static void cbs_refresh_active_window(struct sched_cbs_data *data, uint64_t now)
{
	if (data->remaining_ticks == 0UL) {
		cbs_replenish_after_depletion(data, now, true);
	} else if (data->deadline_ticks <= now) {
		cbs_replenish_from_now(data, now, true);
	}
}

/*
 * Inactive wake rule:
 *
 *   remaining / (deadline - now) > budget / period
 *        -> old deadline is too dense for the configured bandwidth
 *        -> start a fresh server window at now + period
 *
 * This is why CBS works well for burst/wakeup workloads without letting a
 * sleeping vCPU accumulate an early deadline and exceed its reservation.
 */
static void cbs_apply_inactive_wake_rule(struct sched_cbs_data *data, uint64_t now)
{
	bool replenished = false;

	if (data->remaining_ticks == 0UL) {
		cbs_replenish_after_depletion(data, now, true);
		replenished = true;
	} else if (cbs_needs_wake_replenish(data, now)) {
		cbs_replenish_from_now(data, now, true);
		replenished = true;
	}

	if (replenished) {
		cbs_count_event(&data->wake_replenish_count);
	}
}

static void cbs_queue_remove(struct thread_object *obj)
{
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;

	if (cbs_is_queued(data)) {
		list_del_init(&data->list);
	}
}

static void cbs_queue_insert(struct thread_object *obj)
{
	struct sched_cbs_control *cbs_ctl =
		(struct sched_cbs_control *)obj->sched_ctl->priv;
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;
	struct sched_cbs_data *iter_data;
	struct thread_object *iter_obj;
	struct list_head *pos;

	if (cbs_is_queued(data)) {
		cbs_queue_remove(obj);
	}

	list_for_each(pos, &cbs_ctl->runqueue) {
		iter_obj = container_of(pos, struct thread_object, data);
		iter_data = (struct sched_cbs_data *)iter_obj->data;
		if (data->deadline_ticks < iter_data->deadline_ticks) {
			list_add_node(&data->list, pos->prev, pos);
			return;
		}
	}

	list_add_tail(&data->list, &cbs_ctl->runqueue);
}

static void cbs_record_late_account(struct sched_cbs_data *data, uint64_t now)
{
	uint64_t due = data->deadline_ticks;
	uint64_t budget_due = 0UL;

	/*
	 * "late" is a diagnostic counter, not extra CBS policy. The local timer
	 * should normally fire at the earlier of budget exhaustion and deadline.
	 * If accounting observes time later than that earliest due point, the vCPU
	 * has run past the expected scheduling boundary because the timer or
	 * schedule() path arrived late.
	 */
	if (data->remaining_ticks == 0UL) {
		budget_due = data->last_start_ticks;
	} else if (data->last_start_ticks <= (UINT64_MAX - data->remaining_ticks)) {
		budget_due = data->last_start_ticks + data->remaining_ticks;
	}

	if ((budget_due != 0UL) && ((due == 0UL) || (budget_due < due))) {
		due = budget_due;
	}
	if ((due != 0UL) && (due < now)) {
		cbs_count_event(&data->late_account_count);
	}
}

static void cbs_account_runtime(struct thread_object *obj, uint64_t now)
{
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;
	uint64_t delta;

	if (is_idle_thread(obj) || (obj->status != THREAD_STS_RUNNING) ||
		(data->last_start_ticks == 0UL) || (now <= data->last_start_ticks)) {
		return;
	}

	delta = now - data->last_start_ticks;
	cbs_record_late_account(data, now);
	data->last_start_ticks = now;

	cbs_charge_runtime(data, delta, true);
}

static void cbs_refresh_thread(struct thread_object *obj, uint64_t now)
{
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;

	if (!cbs_is_active(obj)) {
		return;
	}

	cbs_refresh_active_window(data, now);
	cbs_queue_insert(obj);
}

static void cbs_refresh_queues(struct sched_control *ctl, uint64_t now)
{
	struct sched_cbs_control *cbs_ctl = (struct sched_cbs_control *)ctl->priv;
	struct thread_object *obj;
	struct sched_cbs_data *data;
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &cbs_ctl->runqueue) {
		obj = container_of(pos, struct thread_object, data);
		data = (struct sched_cbs_data *)obj->data;
		if ((data->remaining_ticks == 0UL) || (data->deadline_ticks <= now)) {
			cbs_queue_remove(obj);
			cbs_refresh_thread(obj, now);
		}
	}
}

static void cbs_program_local_timer(struct sched_control *ctl, struct thread_object *running)
{
	struct sched_cbs_control *cbs_ctl = (struct sched_cbs_control *)ctl->priv;
	struct sched_cbs_data *data;
	uint64_t now = cpu_ticks();
	uint64_t timeout = 0UL;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "program CBS timer on wrong cpu!");

	if ((running != NULL) && !is_idle_thread(running)) {
		data = (struct sched_cbs_data *)running->data;
		if (data->remaining_ticks > 0UL) {
			timeout = (data->last_start_ticks != 0UL) ?
				(data->last_start_ticks + data->remaining_ticks) :
				(now + data->remaining_ticks);
			if (data->deadline_ticks < timeout) {
				timeout = data->deadline_ticks;
			}
		} else {
			timeout = now;
		}
	}

	if (timeout <= now) {
		timeout = (timeout == 0UL) ? 0UL : now;
	}

	if (timeout == cbs_ctl->timer_deadline_ticks) {
		return;
	}

	if (cbs_ctl->timer_deadline_ticks != 0UL) {
		del_timer(&cbs_ctl->tick_timer);
		cbs_ctl->timer_deadline_ticks = 0UL;
	}

	if (timeout != 0UL) {
		update_timer(&cbs_ctl->tick_timer, timeout, 0UL);
		(void)add_timer(&cbs_ctl->tick_timer);
		cbs_ctl->timer_deadline_ticks = timeout;
	}
}

static void cbs_program_timer(struct sched_control *ctl, struct thread_object *running)
{
	/*
	 * Scheduler timers are local per-pCPU objects. Remote wakeups only raise a
	 * reschedule request; the owning pCPU reprograms CBS when it enters schedule().
	 */
	if (ctl->pcpu_id == get_pcpu_id()) {
		cbs_program_local_timer(ctl, running);
	}
}

static void cbs_timer_handler(void *param)
{
	struct sched_control *ctl = (struct sched_control *)param;
	struct thread_object *current;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t rflags;
	uint64_t now = cpu_ticks();

	obtain_schedule_lock(pcpu_id, &rflags);
	((struct sched_cbs_control *)ctl->priv)->timer_deadline_ticks = 0UL;
	sched_account_tick(ctl);

	current = ctl->curr_obj;
	if ((current != NULL) && !is_idle_thread(current)) {
		cbs_account_runtime(current, now);
		cbs_refresh_thread(current, now);
	}

	cbs_refresh_queues(ctl, now);
	make_reschedule_request(pcpu_id);

	release_schedule_lock(pcpu_id, rflags);
}

static bool cbs_vm_uses_pcpu(const struct acrn_vm_config *vm_config, uint16_t pcpu_id)
{
	uint16_t idx;

	if (vm_config->cpu_affinity_num != 0U) {
		for (idx = 0U; idx < vm_config->cpu_affinity_num; idx++) {
			if (vm_config->cpu_affinity_order[idx] == pcpu_id) {
				return true;
			}
		}
		return false;
	}

	return (vm_config->cpu_affinity & AFFINITY_CPU(pcpu_id)) != 0UL;
}

static uint64_t cbs_validate_pcpu_admission(uint16_t pcpu_id)
{
	const struct acrn_vm_config *vm_config;
	uint16_t vm_id;
	uint64_t utilization = 0UL;

	/*
	 * Admission is per pCPU because CBS state is partitioned. CBS+ shares this
	 * same Q/T bound; gang preference can reorder eligible work but cannot create
	 * extra reservation capacity.
	 */
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		uint32_t period_us;
		uint32_t budget_us;

		vm_config = get_vm_config(vm_id);
		if (!cbs_vm_uses_pcpu(vm_config, pcpu_id)) {
			continue;
		}

		cbs_normalize_us(pcpu_id, &vm_config->sched_params, &period_us, &budget_us);
		utilization += ((uint64_t)budget_us * CBS_UTIL_SCALE) / period_us;
		if (utilization > CBS_UTIL_SCALE) {
			panic("CBS admission failed on pCPU%hu vm%hu util=%lu ppm budget=%u period=%u",
				pcpu_id, vm_id, utilization, budget_us, period_us);
		}
	}

	return utilization;
}

static int sched_cbs_init(struct sched_control *ctl)
{
	struct sched_cbs_control *cbs_ctl = &per_cpu(sched_cbs_ctl, ctl->pcpu_id);

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "init scheduler on wrong cpu!");

	ctl->priv = cbs_ctl;
	INIT_LIST_HEAD(&cbs_ctl->runqueue);
	cbs_ctl->admission_utilization = cbs_validate_pcpu_admission(ctl->pcpu_id);
	cbs_ctl->timer_deadline_ticks = 0UL;
	initialize_timer(&cbs_ctl->tick_timer, cbs_timer_handler, ctl, 0UL, 0UL);

	return 0;
}

static void sched_cbs_deinit(struct sched_control *ctl)
{
	struct sched_cbs_control *cbs_ctl = (struct sched_cbs_control *)ctl->priv;

	del_timer(&cbs_ctl->tick_timer);
	cbs_ctl->timer_deadline_ticks = 0UL;
}

static void sched_cbs_init_data(struct thread_object *obj, struct sched_params *params)
{
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;
	uint32_t period_us;
	uint32_t budget_us;
	uint64_t now = cpu_ticks();

	cbs_normalize_us(obj->pcpu_id, params, &period_us, &budget_us);

	INIT_LIST_HEAD(&data->list);
	data->period_ticks = us_to_ticks(period_us);
	data->budget_ticks = us_to_ticks(budget_us);
	data->remaining_ticks = data->budget_ticks;
	data->deadline_ticks = now + data->period_ticks;
	data->last_start_ticks = 0UL;
	data->depleted_count = 0UL;
	data->replenish_count = 0UL;
	data->wake_replenish_count = 0UL;
	data->late_account_count = 0UL;
}

static struct thread_object *sched_cbs_pick_next(struct sched_control *ctl)
{
	struct sched_cbs_control *cbs_ctl = (struct sched_cbs_control *)ctl->priv;
	struct thread_object *current = ctl->curr_obj;
	struct thread_object *next;
	uint64_t now = cpu_ticks();

	if ((current != NULL) && !is_idle_thread(current)) {
		cbs_account_runtime(current, now);
		cbs_refresh_thread(current, now);
	}

	cbs_refresh_queues(ctl, now);

	if (!list_empty(&cbs_ctl->runqueue)) {
		next = get_first_item(&cbs_ctl->runqueue, struct thread_object, data);
		cbs_queue_remove(next);
	} else {
		next = &get_cpu_var(idle);
	}

	if (!is_idle_thread(next)) {
		((struct sched_cbs_data *)next->data)->last_start_ticks = now;
	}
	cbs_program_timer(ctl, next);

	return next;
}

static void sched_cbs_sleep(struct thread_object *obj)
{
	if (cbs_is_current(obj)) {
		cbs_account_runtime(obj, cpu_ticks());
	}
	cbs_queue_remove(obj);
	((struct sched_cbs_data *)obj->data)->last_start_ticks = 0UL;
	cbs_program_timer(obj->sched_ctl, cbs_is_current(obj) ? NULL : obj->sched_ctl->curr_obj);
}

static void sched_cbs_wake(struct thread_object *obj)
{
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;
	uint64_t now = cpu_ticks();

	cbs_apply_inactive_wake_rule(data, now);
	data->last_start_ticks = 0UL;
	cbs_queue_insert(obj);
	cbs_program_timer(obj->sched_ctl, obj->sched_ctl->curr_obj);
}

static void sched_cbs_prioritize(struct thread_object *obj)
{
	struct sched_cbs_data *data = (struct sched_cbs_data *)obj->data;
	uint64_t now = cpu_ticks();

	/*
	 * Event priority only asks CBS to reconsider already-runnable work. The
	 * inactive wake density rule belongs to wake(), where new demand enters
	 * from BLOCKED state; applying it again to a runnable vCPU can push its
	 * deadline later during a kick storm and lengthen interrupt delivery.
	 */
	cbs_refresh_active_window(data, now);
	if (!cbs_is_current(obj)) {
		cbs_queue_insert(obj);
	}
}

static void sched_cbs_suspend(struct sched_control *ctl)
{
	sched_cbs_deinit(ctl);
}

static void sched_cbs_snapshot(const struct thread_object *obj,
	struct sched_cbs_stats *stats)
{
	const struct sched_cbs_data *data = (const struct sched_cbs_data *)obj->data;
	uint64_t now = cpu_ticks();

	*stats = (struct sched_cbs_stats) {
		.period_ticks = data->period_ticks,
		.budget_ticks = data->budget_ticks,
		.remaining_ticks = data->remaining_ticks,
		.deadline_ticks = data->deadline_ticks,
		.last_start_ticks = data->last_start_ticks,
		.depleted_count = data->depleted_count,
		.replenish_count = data->replenish_count,
		.wake_replenish_count = data->wake_replenish_count,
		.late_account_count = data->late_account_count,
	};

	if ((obj->status == THREAD_STS_RUNNING) && !is_idle_thread(obj) &&
		(data->last_start_ticks != 0UL) && (now > data->last_start_ticks)) {
		struct sched_cbs_data live = *data;
		uint64_t delta = now - data->last_start_ticks;

		cbs_charge_runtime(&live, delta, false);
		stats->remaining_ticks = live.remaining_ticks;
		stats->deadline_ticks = live.deadline_ticks;
	}
}

bool sched_get_cbs_stats(const struct thread_object *obj, struct sched_cbs_stats *stats)
{
	bool valid = false;
	uint64_t rflags;

	if ((obj != NULL) && (stats != NULL) && (obj->sched_ctl != NULL) &&
		(obj->sched_ctl->scheduler == &sched_cbs)) {
		obtain_schedule_lock(obj->pcpu_id, &rflags);
		sched_cbs_snapshot(obj, stats);
		release_schedule_lock(obj->pcpu_id, rflags);
		valid = true;
	}

	return valid;
}

static uint32_t cbs_runqueue_count(const struct sched_cbs_control *cbs_ctl)
{
	const struct list_head *pos;
	uint32_t count = 0U;

	list_for_each(pos, &cbs_ctl->runqueue) {
		count++;
	}

	return count;
}

bool sched_get_cbs_pcpu_stats(uint16_t pcpu_id, struct sched_cbs_pcpu_stats *stats)
{
	struct sched_control *ctl;
	struct sched_cbs_control *cbs_ctl;
	bool valid = false;
	uint64_t rflags;

	if ((pcpu_id < get_pcpu_nums()) && (stats != NULL)) {
		ctl = &per_cpu(sched_ctl, pcpu_id);
		if (ctl->scheduler == &sched_cbs) {
			obtain_schedule_lock(pcpu_id, &rflags);
			cbs_ctl = (struct sched_cbs_control *)ctl->priv;
			if (cbs_ctl != NULL) {
				*stats = (struct sched_cbs_pcpu_stats) {
					.admission_utilization = cbs_ctl->admission_utilization,
					.runqueue_count = cbs_runqueue_count(cbs_ctl),
				};
				valid = true;
			}
			release_schedule_lock(pcpu_id, rflags);
		}
	}

	return valid;
}

struct acrn_scheduler sched_cbs = {
	.name		= "sched_cbs",
	.stat_desc	= "partitioned-edf-cbs:dts-defaults",
	.init		= sched_cbs_init,
	.init_data	= sched_cbs_init_data,
	.pick_next	= sched_cbs_pick_next,
	.sleep		= sched_cbs_sleep,
	.wake		= sched_cbs_wake,
	.prioritize	= sched_cbs_prioritize,
	.deinit		= sched_cbs_deinit,
	.suspend	= sched_cbs_suspend,
};
