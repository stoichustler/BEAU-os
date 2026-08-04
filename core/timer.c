/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <per_cpu.h>
#include <softirq.h>
#include <trace.h>
#include <irq.h>
#include <ticks.h>
#include <timer.h>

#define MAX_TIMER_ACTIONS	32U

/*
 * Common timer principle:
 *
 * BEAU keeps one software timer list per pCPU and programs the architecture
 * hardware timer only with the earliest deadline on the local list.
 *
 *   per-pCPU timer_list, sorted by timeout
 *
 *      head
 *       |
 *       v
 *   +---------+   +---------+   +---------+
 *   | timer A |-->| timer B |-->| timer C |
 *   | t=100   |   | t=200   |   | t=500   |
 *   +---------+   +---------+   +---------+
 *       |
 *       v
 *   arch_set_timer_count(100)
 *
 * When the architecture timer IRQ fires, architecture code raises SOFTIRQ_TIMER.
 * The softirq runs callbacks for expired timers, advances periodic timers past
 * missed periods, reinserts active periodic timers, and finally reprograms the
 * hardware timer to the new list head.
 *
 *   hardware timer IRQ
 *          |
 *          v
 *   SOFTIRQ_TIMER
 *     - remove expired head timers
 *     - run callback
 *     - one-shot: stop by clearing timeout
 *     - periodic: advance to the next future deadline and reinsert
 *     - program hardware timer from new head
 *
 * Timers are local objects: add_timer() and del_timer() operate on the current
 * pCPU list with local IRQs disabled. Remote users must arrange execution on
 * the target pCPU instead of editing another pCPU's timer list directly.
 */
bool timer_expired(const struct hv_timer *timer, uint64_t now, uint64_t *delta)
{
	bool ret = true;
	uint64_t delt = 0UL;

	if  ((timer->timeout != 0UL) && (now < timer->timeout)) {
		ret = false;
		delt = timer->timeout - now;
	}

	if (delta != NULL) {
		*delta = delt;
	}

	return ret;
}

bool timer_is_started(const struct hv_timer *timer)
{
	return (!list_empty(&timer->node));
}

static void run_timer(const struct hv_timer *timer)
{
	if ((timer->func != NULL) && (timer->timeout != 0UL)) {
		timer->func(timer->priv_data);
	}

	TRACE_2L(TRACE_TIMER_ACTION_PCKUP, timer->timeout, 0UL);
}

static void advance_periodic_timer(struct hv_timer *timer, uint64_t now)
{
	uint64_t next;
	uint64_t missed;

	next = timer->timeout + timer->period_in_cycle;
	if (next <= now) {
		missed = ((now - next) / timer->period_in_cycle) + 1UL;
		next += missed * timer->period_in_cycle;
		if (next <= now) {
			next = now + timer->period_in_cycle;
		}
	}

	timer->timeout = next;
}

static inline void update_architecture_timer(struct per_cpu_timers *cpu_timer)
{
	struct hv_timer *timer = NULL;

	/* find the next event timer */
	if (!list_empty(&cpu_timer->timer_list)) {
		timer = container_of((&cpu_timer->timer_list)->next,
			struct hv_timer, node);

		/* it is okay to program a expired time */
		arch_set_timer_count(timer->timeout);
	}
}

/*
 * return true if we add the timer on the timer_list head
 */
static bool local_add_timer(struct per_cpu_timers *cpu_timer,
			struct hv_timer *timer)
{
	struct list_head *pos, *prev;
	struct hv_timer *tmp;
	uint64_t tsc = timer->timeout;

	prev = &cpu_timer->timer_list;
	list_for_each(pos, &cpu_timer->timer_list) {
		tmp = container_of(pos, struct hv_timer, node);
		if (tmp->timeout < tsc) {
			prev = &tmp->node;
		} else {
			break;
		}
	}

	list_add(&timer->node, prev);

	return (prev == &cpu_timer->timer_list);
}

int32_t add_timer(struct hv_timer *timer)
{
	struct per_cpu_timers *cpu_timer;
	uint16_t pcpu_id;
	int32_t ret = 0;
	uint64_t rflags;

	if ((timer == NULL) || (timer->func == NULL) || (timer->timeout == 0UL)) {
		ret = -EINVAL;
	} else {
		ASSERT(list_empty(&timer->node), "add timer again!\n");

		/* limit minimal periodic timer cycle period */
		if (timer->mode == TICK_MODE_PERIODIC) {
			timer->period_in_cycle = max(timer->period_in_cycle, us_to_ticks(MIN_TIMER_PERIOD_US));
		}

		pcpu_id  = get_pcpu_id();
		cpu_timer = &per_cpu(cpu_timers, pcpu_id);

		local_irq_save(&rflags);
		/* update the architecture timer if we're on the timer_list head */
		if (local_add_timer(cpu_timer, timer)) {
			update_architecture_timer(cpu_timer);
		}
		local_irq_restore(rflags);

		TRACE_2L(TRACE_TIMER_ACTION_ADDED, timer->timeout, 0UL);
	}

	return ret;

}

void initialize_timer(struct hv_timer *timer,
			timer_handle_t func, void *priv_data,
			uint64_t timeout, uint64_t period_in_cycle)
{
	if (timer != NULL) {
		timer->func = func;
		timer->priv_data = priv_data;
		timer->timeout = timeout;
		if (period_in_cycle > 0UL) {
			timer->mode = TICK_MODE_PERIODIC;
			timer->period_in_cycle = period_in_cycle;
		} else {
			timer->mode = TICK_MODE_ONESHOT;
			timer->period_in_cycle = 0UL;
		}
		INIT_LIST_HEAD(&timer->node);
	}
}

void update_timer(struct hv_timer *timer, uint64_t timeout, uint64_t period)
{
	if (timer != NULL) {
		timer->timeout = timeout;
		if (period > 0UL) {
			timer->mode = TICK_MODE_PERIODIC;
			timer->period_in_cycle = period;
		} else {
			timer->mode = TICK_MODE_ONESHOT;
			timer->period_in_cycle = 0UL;
		}
	}
}

void del_timer(struct hv_timer *timer)
{
	uint64_t rflags;

	local_irq_save(&rflags);
	if ((timer != NULL) && !list_empty(&timer->node)) {
		list_del_init(&timer->node);
	}
	local_irq_restore(rflags);
}

static void init_percpu_timer(uint16_t pcpu_id)
{
	struct per_cpu_timers *cpu_timer;

	cpu_timer = &per_cpu(cpu_timers, pcpu_id);
	INIT_LIST_HEAD(&cpu_timer->timer_list);
}

static void timer_softirq(uint16_t pcpu_id)
{
	struct per_cpu_timers *cpu_timer;
	struct hv_timer *timer;
	const struct list_head *pos, *n;
	uint32_t tries = MAX_TIMER_ACTIONS;
	uint64_t current_tsc = cpu_ticks();

	/* handle passed timer */
	cpu_timer = &per_cpu(cpu_timers, pcpu_id);

	/* This is to make sure we are not blocked due to delay inside func()
	 * force to exit irq handler after we serviced >31 timers
	 * caller used to local_add_timer() for periodic timer, if there is a delay
	 * inside func(), it will infinitely loop here, because new added timer
	 * already passed due to previously func()'s delay.
	 */
	list_for_each_safe(pos, n, &cpu_timer->timer_list) {
		timer = container_of(pos, struct hv_timer, node);
		/* timer expried */
		tries--;
		if ((timer->timeout <= current_tsc) && (tries != 0U)) {
			del_timer(timer);

			run_timer(timer);

			/*
			 * A callback can install a new local deadline for itself. That list
			 * membership owns the new timeout; recycling the callback's old
			 * one-shot state would otherwise clear the newly armed deadline.
			 */
			if (timer_is_started(timer)) {
				continue;
			}

			if (timer->mode == TICK_MODE_PERIODIC) {
				/*
				 * Periodic timers are wall-clock ticks, not a backlog queue.
				 * If softirq handling was delayed across multiple periods,
				 * skip missed periods instead of programming an expired
				 * architecture-timer deadline and taking immediate IRQs again.
				 */
				current_tsc = cpu_ticks();
				advance_periodic_timer(timer, current_tsc);
				(void)local_add_timer(cpu_timer, timer);
			} else {
				timer->timeout = 0UL;
			}
		} else {
			break;
		}
	}

	/* update nearest timer */
	update_architecture_timer(cpu_timer);
}

void timer_init(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	init_percpu_timer(pcpu_id);

	if (pcpu_id == BSP_CPU_ID) {
		register_softirq(SOFTIRQ_TIMER, timer_softirq);
	}

	arch_init_timer();
}
