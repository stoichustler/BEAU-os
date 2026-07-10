/*
 * Copyright (C) 2020-2022 Intel Corporation.
 * Copyright (c) 2026 Hustler Lo
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <list.h>
#include <per_cpu.h>
#include <schedule.h>
#include <ticks.h>

#define BVT_STRINGIFY_HELPER(x)	#x
#define BVT_STRINGIFY(x)	BVT_STRINGIFY_HELPER(x)

#define BVT_MCU_MS		CONFIG_SCHED_BVT_MCU_MS
/* context switch allowance */
#define BVT_CSA_MCU		CONFIG_SCHED_BVT_CSA_MCU

/*
 * limit the weight range to [1, 128]. It's enough to allocate CPU resources
 * for different types of vCPUs
 */
#define BVT_WEIGHT_MIN		1U
#define BVT_WEIGHT_MAX		128U

/*
 * the VT (Virtual Time) ratio is proportional to 1 / weight and making the VT
 * ratio an integer will ease translation between virtual time and physical
 * time.
 * Max (theoretical VT ratio - actual VT ratio) is
 *   1 (< 1 because of integer round down).
 * The minimum total VT ratios of VCPUs (at least two) is
 *   2 * 8 (Min per-vcpu VT ratio)
 * So the max VT ratio share error is about 1/16.
 * To reduce it, we can enlarge the BVT_VT_RATIO_MIN.
 * However increasing VT ratio will reduce the total time needed to overflow
 * AVT. AVT is of type int64_t. The max VT ratio is 1024. MCU is 1 ms.
 * So the time to overflow AVT is about:
 *   2^63  / (1024 * 1000) s, i.e. ~= 9 * 10^12(s) ~= 10^8 day
 * It's so large that we can ignore the AVT overflow case.
 */
#define BVT_VT_RATIO_MIN	8U
#define BVT_VT_RATIO_MAX	(BVT_WEIGHT_MAX * BVT_VT_RATIO_MIN / BVT_WEIGHT_MIN)
/*
 * A zero warp limit in static configuration still means "enable a bounded
 * boost" rather than "boost forever". One MCU is the minimum useful window.
 */
#define BVT_WARP_LIMIT_DEFAULT	1U

/*
 * BVT principle:
 *
 * BVT is a proportional-share scheduler built on virtual time. Real CPU
 * runtime is charged to each runnable thread as AVT (actual virtual time).
 * The charge rate is inverse to the thread weight, so a high-weight thread
 * advances AVT more slowly and therefore receives a larger long-term CPU
 * share. The runqueue is sorted by EVT (effective virtual time), and the
 * lowest EVT runs first.
 *
 *   real runtime
 *        |
 *        v
 *   +---------+     weight high      AVT grows slowly
 *   | threadA |  ------------------------------------+
 *   +---------+                                      |
 *                                                    v
 *                                                +-------+
 *                                                |  AVT  |
 *                                                +-------+
 *                                                    |
 *   +---------+     weight low       AVT grows fast  |
 *   | threadB |  ------------------------------------+
 *   +---------+
 *
 *   runqueue order:
 *
 *       lower EVT                       higher EVT
 *   +-------------+   +-------------+   +-------------+
 *   | next thread |-->| peer thread |-->| peer thread |
 *   +-------------+   +-------------+   +-------------+
 *
 * SVT is the minimum AVT among runnable threads. A waking thread that slept
 * for a long time is moved up to SVT instead of keeping an ancient AVT, which
 * prevents unbounded catch-up execution after long sleep.
 *
 * Bounded warp is an ordering boost, not fairness credit:
 *
 *       event / wake
 *            |
 *            v
 *      EVT = AVT - warp_value
 *            |
 *            v
 *      reinsert by lower EVT
 *
 * AVT remains the long-term fairness ledger. warp_left is consumed only while
 * the boosted thread actually runs; after the bounded window, EVT returns to
 * AVT and normal proportional sharing resumes.
 *
 * The local one-shot timer is programmed from the first two runnable EVTs. It
 * fires when the selected thread is expected to catch the next runnable peer,
 * plus the context-switch allowance, and then asks the common scheduler to
 * pick again.
 */
struct sched_bvt_data {
	/* keep list as the first item */
	struct list_head list;
	/* minimum charging unit in cycles */
	uint64_t mcu;
	/* a thread receives a share of cpu in proportion to its weight */
	uint8_t weight;
	/* virtual time advance variable, proportional to 1 / weight */
	uint64_t vt_ratio;
	/*
	 * BVT orders runnable threads by effective virtual time (EVT). A warp is a
	 * temporary negative offset applied as EVT = AVT - warp_value, giving a
	 * recently woken or event-targeted thread an earlier position in the runqueue.
	 * warp_left is charged in MCU units; last_unwarp_tsc enforces cooldown.
	 */
	bool warp_on;
	int32_t warp_value;
	uint32_t warp_limit;
	uint32_t unwarp_period;
	uint32_t warp_left;
	uint64_t last_unwarp_tsc;
	/* actual virtual time in units of mcu */
	int64_t avt;
	/* effective virtual time in units of mcu */
	int64_t evt;
	uint64_t residual;

	uint64_t start_tsc;
};

static void runqueue_add(struct thread_object *obj);
static void runqueue_remove(struct thread_object *obj);

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 */
static bool is_inqueue(struct thread_object *obj)
{
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;
	return !list_empty(&data->list);
}

static void update_evt(struct sched_bvt_data *data)
{
	/* AVT is the fairness ledger; EVT is only the ordering key. */
	data->evt = data->avt;
	if (data->warp_on) {
		data->evt -= (int64_t)data->warp_value;
	}
}

static bool bvt_can_warp(const struct sched_bvt_data *data, uint64_t now_tsc)
{
	bool can_warp = data->warp_value > 0;

	/*
	 * unwarp_period is configured in MCU units. A non-zero value prevents an
	 * interrupt-heavy vCPU from immediately re-entering warp after each charge.
	 */
	if (can_warp && (data->unwarp_period != 0U) && (data->last_unwarp_tsc != 0UL)) {
		can_warp = (now_tsc - data->last_unwarp_tsc) >=
			((uint64_t)data->unwarp_period * data->mcu);
	}

	return can_warp;
}

static void bvt_start_warp(struct thread_object *obj)
{
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;

	/* Repeated events during one active window do not stack extra credit. */
	if (data->warp_on) {
		return;
	}

	if (bvt_can_warp(data, cpu_ticks())) {
		data->warp_on = true;
		data->warp_left = (data->warp_limit != 0U) ?
			data->warp_limit : BVT_WARP_LIMIT_DEFAULT;
		update_evt(data);
		if (is_inqueue(obj)) {
			/* The runqueue is sorted by EVT, so a changed EVT must be reinserted. */
			runqueue_remove(obj);
			runqueue_add(obj);
		}
	}
}

static void bvt_update_warp_after_charge(struct sched_bvt_data *data,
	uint64_t charged_mcu, uint64_t now_tsc)
{
	if (!data->warp_on || (charged_mcu == 0U)) {
		return;
	}

	/*
	 * Warp duration is paid for by actual CPU execution, not wall time. A boosted
	 * thread that does not run yet keeps its window until the scheduler selects it.
	 */
	if (charged_mcu >= data->warp_left) {
		data->warp_on = false;
		data->warp_left = 0U;
		data->last_unwarp_tsc = now_tsc;
	} else {
		data->warp_left -= (uint32_t)charged_mcu;
	}
}

/*
 * @pre bvt_ctl != NULL
 */
static void update_svt(struct sched_bvt_control *bvt_ctl)
{
	struct sched_bvt_data *obj_data;
	struct thread_object *tmp_obj;

	if (!list_empty(&bvt_ctl->runqueue)) {
		tmp_obj = get_first_item(&bvt_ctl->runqueue, struct thread_object, data);
		obj_data = (struct sched_bvt_data *)tmp_obj->data;
		bvt_ctl->svt = obj_data->avt;
	}
}

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 * @pre obj->sched_ctl != NULL
 * @pre obj->sched_ctl->priv != NULL
 */
static void runqueue_add(struct thread_object *obj)
{
	struct sched_bvt_control *bvt_ctl =
		(struct sched_bvt_control *)obj->sched_ctl->priv;
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;
	struct list_head *pos;
	struct thread_object *iter_obj;
	struct sched_bvt_data *iter_data;

	/*
	 * the earliest evt has highest priority,
	 * the runqueue is ordered by priority.
	 */

	if (list_empty(&bvt_ctl->runqueue)) {
		list_add(&data->list, &bvt_ctl->runqueue);
	} else {
		list_for_each(pos, &bvt_ctl->runqueue) {
			iter_obj = container_of(pos, struct thread_object, data);
			iter_data = (struct sched_bvt_data *)iter_obj->data;
			if (iter_data->evt > data->evt) {
				list_add_node(&data->list, pos->prev, pos);
				break;
			}
		}
		if (!is_inqueue(obj)) {
			list_add_tail(&data->list, &bvt_ctl->runqueue);
		}
	}
}

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 */
static void runqueue_remove(struct thread_object *obj)
{
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;

	list_del_init(&data->list);
}

/*
 * @brief Get the SVT (scheduler virtual time) which indicates the
 * minimum AVT of any runnable threads.
 * @pre obj != NULL
 * @pre obj->data != NULL
 * @pre obj->sched_ctl != NULL
 * @pre obj->sched_ctl->priv != NULL
 */

static int64_t get_svt(struct thread_object *obj)
{
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)obj->sched_ctl->priv;

	return bvt_ctl->svt;
}

static void sched_tick_handler(void *param)
{
	struct sched_control  *ctl = (struct sched_control *)param;
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)ctl->priv;
	struct thread_object *current;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t rflags;

	obtain_schedule_lock(pcpu_id, &rflags);
	sched_account_tick(ctl);
	current = ctl->curr_obj;

	if (current != NULL ) {
		/* only non-idle thread need to consume run_countdown */
		if (!is_idle_thread(current)) {
			make_reschedule_request(pcpu_id);
		} else {
			if (!list_empty(&bvt_ctl->runqueue)) {
				make_reschedule_request(pcpu_id);
			}
		}
	}
	release_schedule_lock(pcpu_id, rflags);
}

/*
 *@pre: ctl->pcpu_id == get_pcpu_id()
 */
static int sched_bvt_init(struct sched_control *ctl)
{
	struct sched_bvt_control *bvt_ctl = &per_cpu(sched_bvt_ctl, ctl->pcpu_id);
	int ret = 0;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "init scheduler on wrong cpu!");

	ctl->priv = bvt_ctl;
	INIT_LIST_HEAD(&bvt_ctl->runqueue);

	/* The tick_timer is periodically */
	initialize_timer(&bvt_ctl->tick_timer, sched_tick_handler, ctl, 0, 0);

	return ret;
}

static void sched_bvt_deinit(struct sched_control *ctl)
{
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)ctl->priv;
	del_timer(&bvt_ctl->tick_timer);
}

static void sched_bvt_init_data(struct thread_object *obj, struct sched_params * params)
{
	struct sched_bvt_data *data;

	data = (struct sched_bvt_data *)obj->data;
	INIT_LIST_HEAD(&data->list);
	data->mcu = BVT_MCU_MS * TICKS_PER_MS;
	/*
	 * weight controls long-term share. warp_* controls short-term wakeup/event
	 * latency and must not alter AVT accounting, otherwise boosted guests would
	 * receive extra long-term CPU share.
	 */
	data->weight = clamp(params->bvt_weight, BVT_WEIGHT_MIN, BVT_WEIGHT_MAX);
	data->warp_value = params->bvt_warp_value;
	data->warp_limit = params->bvt_warp_limit;
	data->unwarp_period = params->bvt_unwarp_period;
	data->warp_on = false;	/* warp disabled by default */
	data->warp_left = 0U;
	data->last_unwarp_tsc = 0UL;
	data->vt_ratio = BVT_VT_RATIO_MAX / data->weight;
	data->residual = 0U;
}

static void sched_bvt_suspend(struct sched_control *ctl)
{
	sched_bvt_deinit(ctl);
}

static uint64_t v2p(uint64_t virt_time, uint64_t ratio)
{
	return (uint64_t)(virt_time / ratio);
}

static uint64_t p2v(uint64_t phy_time, uint64_t ratio)
{
	return (uint64_t)(phy_time * ratio);
}

static void sched_bvt_snapshot(const struct thread_object *obj,
	struct sched_bvt_stats *stats)
{
	const struct sched_bvt_data *data = (const struct sched_bvt_data *)obj->data;
	uint64_t now_tsc = cpu_ticks();
	uint64_t residual = data->residual;
	uint64_t delta_mcu = 0U;

	*stats = (struct sched_bvt_stats) {
		.weight = data->weight,
		.vt_ratio = data->vt_ratio,
		.avt = data->avt,
		.evt = data->evt,
	};

	if ((obj->status == THREAD_STS_RUNNING) && !is_idle_thread(obj) &&
		(now_tsc > data->start_tsc)) {
		uint64_t v_delta = p2v(now_tsc - data->start_tsc, data->vt_ratio) + residual;

		delta_mcu = (uint64_t)(v_delta / data->mcu);
		stats->avt += (int64_t)delta_mcu;
		stats->evt = stats->avt;
		if (data->warp_on) {
			stats->evt -= (int64_t)data->warp_value;
		}
	}
}

bool sched_get_bvt_stats(const struct thread_object *obj, struct sched_bvt_stats *stats)
{
	bool valid = false;
	uint64_t rflags;

	if ((obj != NULL) && (stats != NULL) && (obj->sched_ctl != NULL) &&
		(obj->sched_ctl->scheduler == &sched_bvt)) {
		obtain_schedule_lock(obj->pcpu_id, &rflags);
		sched_bvt_snapshot(obj, stats);
		release_schedule_lock(obj->pcpu_id, rflags);
		valid = true;
	}

	return valid;
}

static void update_vt(struct thread_object *obj)
{
	struct sched_bvt_data *data;
	uint64_t now_tsc = cpu_ticks();
	uint64_t v_delta, delta_mcu = 0U;

	data = (struct sched_bvt_data *)obj->data;

	/* Charge the current thread before comparing it with the runqueue again. */
	if (now_tsc > data->start_tsc) {
		v_delta = p2v(now_tsc - data->start_tsc, data->vt_ratio) + data->residual;
		delta_mcu = (uint64_t)(v_delta / data->mcu);
		data->residual = v_delta % data->mcu;
	}
	data->avt += delta_mcu;
	bvt_update_warp_after_charge(data, delta_mcu, now_tsc);
	update_evt(data);

	if (is_inqueue(obj)) {
		runqueue_remove(obj);
		runqueue_add(obj);
	}
}

static struct thread_object *sched_bvt_pick_next(struct sched_control *ctl)
{
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)ctl->priv;
	struct thread_object *first_obj = NULL, *second_obj = NULL;
	struct sched_bvt_data *first_data = NULL, *second_data = NULL;
	struct list_head *first, *sec;
	struct thread_object *next = NULL;
	struct thread_object *current = ctl->curr_obj;
	uint64_t now_tsc = cpu_ticks();
	uint64_t delta_mcu = 0U;
	uint64_t tick_period = BVT_MCU_MS * TICKS_PER_MS;
	uint64_t run_countdown;

	if (!is_idle_thread(current)) {
		update_vt(current);
	}
	/* always align the svt with the avt of the first thread object in runqueue.*/
	update_svt(bvt_ctl);

	del_timer(&bvt_ctl->tick_timer);

	if (!list_empty(&bvt_ctl->runqueue)) {
		first = bvt_ctl->runqueue.next;
		sec = (first->next == &bvt_ctl->runqueue) ? NULL : first->next;

		first_obj = container_of(first, struct thread_object, data);
		first_data = (struct sched_bvt_data *)first_obj->data;

		/* The run_countdown is used to describe how may mcu the next thread
		 * can run for. A one-shot timer is set to expire at
		 * current time + run_countdown. The next thread can run until the
		 * timer interrupts. But when there is only one object
		 * in runqueue, it can run forever. so, no timer is set.
		 */
		if (sec != NULL) {
			second_obj = container_of(sec, struct thread_object, data);
			second_data = (struct sched_bvt_data *)second_obj->data;
			delta_mcu = second_data->evt - first_data->evt;
			run_countdown = v2p(delta_mcu, first_data->vt_ratio) + BVT_CSA_MCU;
		} else {
			run_countdown = UINT64_MAX;
		}
		first_data->start_tsc = now_tsc;
		next = first_obj;
		if (run_countdown != UINT64_MAX) {
			update_timer(&bvt_ctl->tick_timer, cpu_ticks() + run_countdown * tick_period, 0);
			(void)add_timer(&bvt_ctl->tick_timer);
		}
	} else {
		next = &get_cpu_var(idle);
	}

	return next;
}

static void sched_bvt_sleep(struct thread_object *obj)
{
	runqueue_remove(obj);
}

static void sched_bvt_wake(struct thread_object *obj)
{
	struct sched_bvt_data *data;
	int64_t svt, threshold;

	data = (struct sched_bvt_data *)obj->data;
	svt = get_svt(obj);
	threshold = svt - BVT_CSA_MCU;
	/* adjusting AVT for a thread after a long sleep */
	data->avt = (data->avt > threshold) ? data->avt : svt;
	/*
	 * Wakeup boost and explicit event boost use the same bounded warp primitive.
	 * For a sleeping vCPU this reduces timer/IRQ wake latency without changing
	 * long-term AVT fairness.
	 */
	bvt_start_warp(obj);
	update_evt(data);
	/* add to runqueue in order */
	runqueue_add(obj);

}

static void sched_bvt_prioritize(struct thread_object *obj)
{
	/* Called from schedule() after a generic priority request is consumed. */
	bvt_start_warp(obj);
}

struct acrn_scheduler sched_bvt = {
	.name		= "sched_bvt",
	.stat_desc	= "mcu:" BVT_STRINGIFY(CONFIG_SCHED_BVT_MCU_MS)
			  "ms csa:" BVT_STRINGIFY(CONFIG_SCHED_BVT_CSA_MCU)
			  " weight:1-128 warp:on-event",
	.init		= sched_bvt_init,
	.init_data	= sched_bvt_init_data,
	.pick_next	= sched_bvt_pick_next,
	.sleep		= sched_bvt_sleep,
	.wake		= sched_bvt_wake,
	.prioritize	= sched_bvt_prioritize,
	.deinit		= sched_bvt_deinit,
	/* Now suspend is just to do del_timer and add_timer will be delayed to
	 * shedule after resume.
	 * So no need to add .resume now.
	 */
	.suspend	= sched_bvt_suspend,
};
