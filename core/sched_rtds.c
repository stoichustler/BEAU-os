/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Independent partitioned EDF budget scheduler for BEAU.
 *
 * This scheduler is based on public real-time scheduling theory: each thread
 * is modeled as a periodic budget server, and runnable servers are ordered by
 * earliest absolute deadline.
 *
 * Principle:
 *
 * Each non-idle thread owns one periodic server:
 *
 *     period = 4ms
 *     budget = 2ms
 *
 *     |<--------------- period --------------->|
 *     +===========+-----------------------------+
 *     |  budget   |          no budget          |
 *     +===========+-----------------------------+
 *     ^                                         ^
 *     current replenishment                     deadline / next replenishment
 *
 * Only actual execution consumes remaining_ticks. At each period boundary,
 * unused budget is discarded, remaining_ticks is reset to budget_ticks, and
 * deadline_ticks advances to the next period boundary.
 *
 * Queue model on each pCPU:
 *
 *  wake / runnable / period refresh
 *                 |
 *                 v
 *    +-------------------------+
 *    | remaining_ticks > 0 ?   |
 *    +-------------------------+
 *          |           |
 *          | yes       | no
 *          v           v
 *   +-------------+   +----------------+
 *   | ready_queue |   | depleted_queue |
 *   | EDF ordered |   | no budget      |
 *   +------+------+   +-------+--------+
 *          |                  |
 *          v                  |
 *   earliest deadline         |
 *          |                  |
 *          v                  |
 *   selected to run <---------+
 *          work-conserving slack only when ready_queue is empty
 *
 * The ready_queue is ordered by earliest absolute deadline. The depleted_queue
 * holds runnable threads that exhausted the current-period budget. BEAU keeps
 * RTDS partitioned: each pCPU owns its own queues and timer, and threads do
 * not migrate between pCPUs.
 *
 * Timer model:
 *
 *     running server
 *          |
 *          v
 *   min(budget exhaustion, period boundary, earliest replenishment)
 *          |
 *          v
 *   local one-shot timer -> account runtime -> request schedule()
 *
 * The timer callback does not directly pick the next thread. It records
 * accounting, refreshes queues, raises NEED_RESCHEDULE, and lets the common
 * scheduler path reprogram the next local one-shot deadline.
 *
 * Design model:
 * - BEAU already partitions schedulable objects by pCPU through thread->pcpu_id.
 *   RTDS follows that model and keeps one ready/depleted queue pair per pCPU.
 *   It does not migrate threads and does not use a global SMP queue.
 * - Each non-idle thread is a periodic server with period_ticks, budget_ticks,
 *   remaining_ticks and an absolute deadline_ticks. All initial deadlines are
 *   aligned to the same period grid so vCPU creation order does not create a
 *   persistent scheduling phase offset on shared pCPUs.
 * - The ready queue contains runnable servers with budget, sorted by earliest
 *   deadline. The depleted queue contains runnable servers with no budget until
 *   their next period boundary replenishes remaining_ticks.
 * - At a period boundary, unused budget is discarded and the server receives a
 *   fresh budget for the new period. This keeps CPU time bounded per period and
 *   avoids carrying old slack into later windows.
 * - The scheduler is work-conserving on a partitioned pCPU. If no server has
 *   budget but runnable depleted servers exist, RTDS runs the depleted server
 *   with the earliest replenishment deadline instead of idling the pCPU. That
 *   execution consumes otherwise idle CPU time and does not reduce the next
 *   period's configured budget.
 * - The one-shot timer is programmed for the earlier of the running server's
 *   budget/deadline boundary or the earliest depleted-server replenishment.
 *   The timer callback only records accounting and requests schedule(); the
 *   next one-shot deadline is programmed from scheduler paths outside the timer
 *   callback, matching BEAU timer API constraints.
 */

#include <list.h>
#include <per_cpu.h>
#include <schedule.h>
#include <ticks.h>
#include <util.h>

#define RTDS_DEFAULT_PERIOD_US	4000U
#define RTDS_DEFAULT_BUDGET_US	2000U
#define RTDS_MIN_PERIOD_US	500U
#define RTDS_MIN_BUDGET_US	500U

enum sched_rtds_queue {
	RTDS_QUEUE_NONE = 0,
	RTDS_QUEUE_READY,
	RTDS_QUEUE_DEPLETED,
};

struct sched_rtds_data {
	/* keep list as the first item */
	struct list_head list;
	/* Server period and budget are stored as CPU ticks after init_data(). */
	uint64_t period_ticks;
	uint64_t budget_ticks;
	/* Budget left in the current period; only execution time consumes it. */
	uint64_t remaining_ticks;
	/* Absolute end of the current period; also the next replenishment point. */
	uint64_t deadline_ticks;
	/* Non-zero only for the object currently being charged on a pCPU. */
	uint64_t last_start_ticks;
	enum sched_rtds_queue queue;
};

static void rtds_timer_handler(void *param);

static uint64_t rtds_next_period_boundary(uint64_t now, uint64_t period_ticks)
{
	uint64_t periods;

	if (period_ticks == 0UL) {
		return now;
	}

	/*
	 * Use a fixed tick epoch instead of the vCPU/thread creation time. This
	 * keeps all RTDS servers on the same pCPU aligned to the same period grid and
	 * makes equal-period reservations reproducible across boot timing changes.
	 */
	periods = (now / period_ticks) + 1UL;

	return periods * period_ticks;
}

static bool rtds_is_queued(const struct sched_rtds_data *data)
{
	return data->queue != RTDS_QUEUE_NONE;
}

static bool rtds_is_current(const struct thread_object *obj)
{
	return obj->sched_ctl->curr_obj == obj;
}

static bool rtds_is_active(const struct thread_object *obj)
{
	/*
	 * schedule() marks the outgoing thread runnable only after pick_next().
	 * A blocking current thread is advertised through be_blocking before that
	 * status change, so RTDS must not requeue it while selecting its replacement.
	 */
	return !is_idle_thread(obj) &&
		((rtds_is_current(obj) && !obj->be_blocking) ||
		 (obj->status == THREAD_STS_RUNNABLE));
}

static void rtds_queue_remove(struct thread_object *obj)
{
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;

	if (rtds_is_queued(data)) {
		list_del_init(&data->list);
		data->queue = RTDS_QUEUE_NONE;
	}
}

static void rtds_ready_insert(struct thread_object *obj)
{
	struct sched_rtds_control *rtds_ctl =
		(struct sched_rtds_control *)obj->sched_ctl->priv;
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;
	struct sched_rtds_data *iter_data;
	struct thread_object *iter_obj;
	struct list_head *pos;

	if (data->queue != RTDS_QUEUE_NONE) {
		rtds_queue_remove(obj);
	}

	list_for_each(pos, &rtds_ctl->ready_queue) {
		iter_obj = container_of(pos, struct thread_object, data);
		iter_data = (struct sched_rtds_data *)iter_obj->data;
		if (data->deadline_ticks < iter_data->deadline_ticks) {
			list_add_node(&data->list, pos->prev, pos);
			data->queue = RTDS_QUEUE_READY;
			return;
		}
	}

	list_add_tail(&data->list, &rtds_ctl->ready_queue);
	data->queue = RTDS_QUEUE_READY;
}

static void rtds_depleted_insert(struct thread_object *obj)
{
	struct sched_rtds_control *rtds_ctl =
		(struct sched_rtds_control *)obj->sched_ctl->priv;
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;

	if (data->queue == RTDS_QUEUE_DEPLETED) {
		return;
	}

	if (data->queue == RTDS_QUEUE_READY) {
		rtds_queue_remove(obj);
	}

	list_add_tail(&data->list, &rtds_ctl->depleted_queue);
	data->queue = RTDS_QUEUE_DEPLETED;
}

static void rtds_account_runtime(struct thread_object *obj, uint64_t now)
{
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;
	uint64_t delta;

	/*
	 * Accounting is lazy. The currently running server is charged at timer,
	 * sleep, and pick_next boundaries. Runnable-but-not-running servers keep
	 * their remaining budget until they actually execute or the period rolls.
	 */
	if (is_idle_thread(obj) || (obj->status != THREAD_STS_RUNNING) ||
		(data->last_start_ticks == 0UL) || (now <= data->last_start_ticks)) {
		return;
	}

	delta = now - data->last_start_ticks;
	data->last_start_ticks = now;

	if (delta >= data->remaining_ticks) {
		data->remaining_ticks = 0UL;
	} else {
		data->remaining_ticks -= delta;
	}
}

static bool rtds_advance_period(struct sched_rtds_data *data, uint64_t now)
{
	uint64_t periods_missed;
	bool advanced = false;

	if (now < data->deadline_ticks) {
		return advanced;
	}

	/*
	 * Deadline and replenishment are the same boundary in this simple
	 * periodic server. Jump directly over any fully missed periods so a
	 * long-sleeping thread does not walk period-by-period at wake time.
	 */
	periods_missed = ((now - data->deadline_ticks) / data->period_ticks) + 1UL;
	data->deadline_ticks += periods_missed * data->period_ticks;
	data->remaining_ticks = data->budget_ticks;

	return true;
}

static void rtds_refresh_thread(struct thread_object *obj, uint64_t now)
{
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;

	rtds_advance_period(data, now);

	if (!rtds_is_active(obj)) {
		return;
	}

	/*
	 * The queue state is derived from current budget after any period roll.
	 * Blocking threads are deliberately left out; sleep_thread() will mark
	 * their public state after the scheduler hook returns.
	 */
	if (data->remaining_ticks > 0UL) {
		rtds_ready_insert(obj);
	} else if (!obj->be_blocking) {
		rtds_depleted_insert(obj);
	}
}

static uint64_t rtds_next_depleted_deadline(struct sched_rtds_control *rtds_ctl)
{
	struct list_head *pos;
	struct thread_object *obj;
	struct sched_rtds_data *data;
	uint64_t next = 0UL;

	list_for_each(pos, &rtds_ctl->depleted_queue) {
		obj = container_of(pos, struct thread_object, data);
		data = (struct sched_rtds_data *)obj->data;
		if ((next == 0UL) || (data->deadline_ticks < next)) {
			next = data->deadline_ticks;
		}
	}

	return next;
}

static struct thread_object *rtds_pick_depleted_slack(struct sched_rtds_control *rtds_ctl)
{
	struct list_head *pos;
	struct thread_object *obj;
	struct thread_object *best = NULL;
	struct sched_rtds_data *data;
	struct sched_rtds_data *best_data = NULL;

	/*
	 * Work-conserving slack is deliberately bounded by availability of ready
	 * work. A depleted server can borrow only when no budgeted server is ready,
	 * and it is still ordered by earliest replenishment deadline.
	 */
	list_for_each(pos, &rtds_ctl->depleted_queue) {
		obj = container_of(pos, struct thread_object, data);
		data = (struct sched_rtds_data *)obj->data;
		if ((best == NULL) || (data->deadline_ticks < best_data->deadline_ticks)) {
			best = obj;
			best_data = data;
		}
	}

	if (best != NULL) {
		rtds_queue_remove(best);
	}

	return best;
}

static void rtds_program_local_timer(struct sched_control *ctl, struct thread_object *running)
{
	struct sched_rtds_control *rtds_ctl = (struct sched_rtds_control *)ctl->priv;
	struct sched_rtds_data *data;
	uint64_t now = cpu_ticks();
	uint64_t timeout = 0UL;
	uint64_t replenish_timeout;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "program RTDS timer on wrong cpu!");

	del_timer(&rtds_ctl->tick_timer);

	if ((running != NULL) && !is_idle_thread(running)) {
		data = (struct sched_rtds_data *)running->data;
		if (data->remaining_ticks > 0UL) {
			/*
			 * A running server must stop at whichever comes first:
			 * budget exhaustion or period boundary. Stopping at the
			 * boundary is required even when budget remains, because
			 * RTDS discards unused budget at the end of a period.
			 */
			if (data->last_start_ticks != 0UL) {
				timeout = data->last_start_ticks + data->remaining_ticks;
			} else {
				timeout = now + data->remaining_ticks;
			}

			if (data->deadline_ticks < timeout) {
				timeout = data->deadline_ticks;
			}
			if (timeout <= now) {
				timeout = now;
			}
		} else {
			/*
			 * A work-conserving depleted server has no current-period
			 * budget left. It may run only as slack until its period
			 * boundary, where normal replenishment and EDF ordering resume.
			 */
			timeout = (data->deadline_ticks > now) ? data->deadline_ticks : now;
		}
	}

	replenish_timeout = rtds_next_depleted_deadline(rtds_ctl);
	if ((replenish_timeout != 0UL) &&
		((timeout == 0UL) || (replenish_timeout < timeout))) {
		timeout = replenish_timeout;
	}

	if (timeout != 0UL) {
		update_timer(&rtds_ctl->tick_timer, timeout, 0UL);
		(void)add_timer(&rtds_ctl->tick_timer);
	}
}

static void rtds_program_timer(struct sched_control *ctl, struct thread_object *running)
{
	/*
	 * BEAU timers are per-pCPU objects, and add_timer()/del_timer() operate on
	 * the caller's local timer list. Wake paths can run from a different pCPU
	 * than the target thread, so only the owning pCPU may reprogram this RTDS
	 * scheduler timer. Remote wake still raises NEED_RESCHEDULE through the
	 * common scheduler path; the target pCPU will reprogram the timer when it
	 * enters schedule().
	 */
	if (ctl->pcpu_id == get_pcpu_id()) {
		rtds_program_local_timer(ctl, running);
	}
}

static void rtds_refresh_queues(struct sched_control *ctl, uint64_t now)
{
	struct sched_rtds_control *rtds_ctl = (struct sched_rtds_control *)ctl->priv;
	struct thread_object *obj;
	struct sched_rtds_data *data;
	struct list_head *pos, *tmp;

	/*
	 * Ready servers can miss a period boundary under overload. Refresh them
	 * before picking so the queue is ordered by the current period deadline,
	 * not by stale deadlines from work that already missed its window.
	 */
	list_for_each_safe(pos, tmp, &rtds_ctl->ready_queue) {
		obj = container_of(pos, struct thread_object, data);
		data = (struct sched_rtds_data *)obj->data;
		if (rtds_advance_period(data, now)) {
			rtds_queue_remove(obj);
			rtds_refresh_thread(obj, now);
		}
	}

	list_for_each_safe(pos, tmp, &rtds_ctl->depleted_queue) {
		obj = container_of(pos, struct thread_object, data);
		rtds_refresh_thread(obj, now);
	}
}

static void rtds_timer_handler(void *param)
{
	struct sched_control *ctl = (struct sched_control *)param;
	struct thread_object *current;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t rflags;
	uint64_t now = cpu_ticks();

	obtain_schedule_lock(pcpu_id, &rflags);
	sched_account_tick(ctl);

	current = ctl->curr_obj;
	if ((current != NULL) && !is_idle_thread(current)) {
		rtds_account_runtime(current, now);
		rtds_refresh_thread(current, now);
	}

	rtds_refresh_queues(ctl, now);
	make_reschedule_request(pcpu_id);

	release_schedule_lock(pcpu_id, rflags);
}

static int sched_rtds_init(struct sched_control *ctl)
{
	struct sched_rtds_control *rtds_ctl = &per_cpu(sched_rtds_ctl, ctl->pcpu_id);

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "init scheduler on wrong cpu!");

	ctl->priv = rtds_ctl;
	INIT_LIST_HEAD(&rtds_ctl->ready_queue);
	INIT_LIST_HEAD(&rtds_ctl->depleted_queue);
	initialize_timer(&rtds_ctl->tick_timer, rtds_timer_handler, ctl, 0UL, 0UL);

	return 0;
}

static void sched_rtds_deinit(struct sched_control *ctl)
{
	struct sched_rtds_control *rtds_ctl = (struct sched_rtds_control *)ctl->priv;

	del_timer(&rtds_ctl->tick_timer);
}

static void sched_rtds_init_data(struct thread_object *obj, __unused struct sched_params *params)
{
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;
	uint32_t period_us = RTDS_DEFAULT_PERIOD_US;
	uint32_t budget_us = RTDS_DEFAULT_BUDGET_US;
	uint64_t now = cpu_ticks();

	period_us = max(period_us, RTDS_MIN_PERIOD_US);
	budget_us = max(budget_us, RTDS_MIN_BUDGET_US);
	budget_us = min(budget_us, period_us);

	INIT_LIST_HEAD(&data->list);
	data->period_ticks = us_to_ticks(period_us);
	data->budget_ticks = us_to_ticks(budget_us);
	data->remaining_ticks = data->budget_ticks;
	data->deadline_ticks = rtds_next_period_boundary(now, data->period_ticks);
	data->last_start_ticks = 0UL;
	data->queue = RTDS_QUEUE_NONE;
}

static struct thread_object *sched_rtds_pick_next(struct sched_control *ctl)
{
	struct sched_rtds_control *rtds_ctl = (struct sched_rtds_control *)ctl->priv;
	struct thread_object *current = ctl->curr_obj;
	struct thread_object *next;
	uint64_t now = cpu_ticks();

	if ((current != NULL) && !is_idle_thread(current)) {
		/*
		 * Current is still marked RUNNING when pick_next() is called.
		 * Reinsert it according to budget/deadline so it competes with
		 * newly woken servers, then remove the selected next server below.
		 */
		rtds_account_runtime(current, now);
		rtds_refresh_thread(current, now);
	}

	rtds_refresh_queues(ctl, now);

	if (!list_empty(&rtds_ctl->ready_queue)) {
		next = get_first_item(&rtds_ctl->ready_queue, struct thread_object, data);
		rtds_queue_remove(next);
	} else {
		next = rtds_pick_depleted_slack(rtds_ctl);
		if (next == NULL) {
			next = &get_cpu_var(idle);
		}
	}

	if (!is_idle_thread(next)) {
		((struct sched_rtds_data *)next->data)->last_start_ticks = now;
	}
	rtds_program_timer(ctl, next);

	return next;
}

static void sched_rtds_sleep(struct thread_object *obj)
{
	if (rtds_is_current(obj)) {
		rtds_account_runtime(obj, cpu_ticks());
	}
	rtds_queue_remove(obj);
	((struct sched_rtds_data *)obj->data)->last_start_ticks = 0UL;
	rtds_program_timer(obj->sched_ctl, rtds_is_current(obj) ? NULL : obj->sched_ctl->curr_obj);
}

static void sched_rtds_wake(struct thread_object *obj)
{
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;
	uint64_t now = cpu_ticks();

	/*
	 * Wake does not grant special credit. If the server slept across one or
	 * more period boundaries, it joins with a fresh current-period budget;
	 * otherwise it resumes with the budget left from before it blocked.
	 */
	rtds_advance_period(data, now);
	data->last_start_ticks = 0UL;

	if (data->remaining_ticks > 0UL) {
		rtds_ready_insert(obj);
	} else {
		rtds_depleted_insert(obj);
	}

	rtds_program_timer(obj->sched_ctl, obj->sched_ctl->curr_obj);
}

static void sched_rtds_prioritize(struct thread_object *obj)
{
	struct sched_rtds_data *data = (struct sched_rtds_data *)obj->data;
	uint64_t now = cpu_ticks();

	/*
	 * Event priority is intentionally conservative: it only asks the pCPU to
	 * reconsider runnable work. It does not mint extra budget or move a
	 * depleted server ahead of its next replenishment boundary.
	 */
	rtds_advance_period(data, now);
	if (!rtds_is_current(obj) && (data->remaining_ticks > 0UL)) {
		rtds_ready_insert(obj);
	}
}

static void sched_rtds_suspend(struct sched_control *ctl)
{
	sched_rtds_deinit(ctl);
}

static void sched_rtds_snapshot(const struct thread_object *obj,
	struct sched_rtds_stats *stats)
{
	const struct sched_rtds_data *data = (const struct sched_rtds_data *)obj->data;
	uint64_t now = cpu_ticks();

	*stats = (struct sched_rtds_stats) {
		.period_ticks = data->period_ticks,
		.budget_ticks = data->budget_ticks,
		.remaining_ticks = data->remaining_ticks,
		.deadline_ticks = data->deadline_ticks,
		.last_start_ticks = data->last_start_ticks,
	};

	if ((obj->status == THREAD_STS_RUNNING) && !is_idle_thread(obj) &&
		(data->last_start_ticks != 0UL) && (now > data->last_start_ticks)) {
		uint64_t delta = now - data->last_start_ticks;

		stats->remaining_ticks = (delta >= stats->remaining_ticks) ?
			0UL : stats->remaining_ticks - delta;
	}
}

bool sched_get_rtds_stats(const struct thread_object *obj, struct sched_rtds_stats *stats)
{
	bool valid = false;
	uint64_t rflags;

	if ((obj != NULL) && (stats != NULL) && (obj->sched_ctl != NULL) &&
		(obj->sched_ctl->scheduler == &sched_rtds)) {
		obtain_schedule_lock(obj->pcpu_id, &rflags);
		sched_rtds_snapshot(obj, stats);
		release_schedule_lock(obj->pcpu_id, rflags);
		valid = true;
	}

	return valid;
}

struct acrn_scheduler sched_rtds = {
	.name		= "sched_rtds",
	.stat_desc	= "work-conserving partitioned-edf:period=4ms budget=2ms",
	.init		= sched_rtds_init,
	.init_data	= sched_rtds_init_data,
	.pick_next	= sched_rtds_pick_next,
	.sleep		= sched_rtds_sleep,
	.wake		= sched_rtds_wake,
	.prioritize	= sched_rtds_prioritize,
	.deinit		= sched_rtds_deinit,
	.suspend	= sched_rtds_suspend,
};
