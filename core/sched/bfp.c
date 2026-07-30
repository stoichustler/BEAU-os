/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <list.h>
#include <per_cpu.h>
#include <schedule.h>
#include <ticks.h>
#include <vconfig.h>

#define BFP_DEFAULT_PERIOD_US		4000U
#define BFP_DEFAULT_BUDGET_US		2000U
#define BFP_MIN_PERIOD_US		500U
#define BFP_MIN_BUDGET_US		500U
#define BFP_RTA_MAX_ITERATIONS		64U

enum sched_bfp_queue {
	BFP_QUEUE_NONE = 0,
	BFP_QUEUE_READY,
	BFP_QUEUE_DEPLETED,
};

struct sched_bfp_data {
	/* Keep list first so queue entries can use thread_object::data. */
	struct list_head list;
	uint64_t period_ticks;
	uint64_t budget_ticks;
	uint64_t remaining_ticks;
	uint64_t next_replenish_ticks;
	uint64_t last_start_ticks;
	uint64_t depleted_count;
	uint64_t replenish_count;
	uint64_t overrun_count;
	uint32_t priority;
	enum sched_bfp_queue queue;
};

struct bfp_admission_task {
	uint16_t vm_id;
	uint32_t priority;
	uint32_t period_us;
	uint32_t budget_us;
};

/* [20260731] Partitioned budgeted fixed-priority scheduling
 *
 * runnable vCPU
 *      |
 *      v
 * highest fixed priority with remaining budget
 *      |
 *      +--> budget exhausted --> depleted queue --> period replenishment
 *      |
 *      v
 * local one-shot timer --> account runtime --> request schedule()
 *
 * Key rule:
 *   - the target pCPU owns both queues, all server state, and the timer;
 *   - sleep and wake never mint budget;
 *   - a depleted vCPU cannot consume slack before its fixed replenishment.
 */
static bool bfp_is_queued(const struct sched_bfp_data *data)
{
	return data->queue != BFP_QUEUE_NONE;
}

static bool bfp_is_current(const struct thread_object *obj)
{
	return per_cpu(sched_ctl, obj->pcpu_id).curr_obj == obj;
}

static bool bfp_is_active(const struct thread_object *obj)
{
	return !is_idle_thread(obj) &&
		((bfp_is_current(obj) && !obj->be_blocking) ||
		(obj->status == THREAD_STS_RUNNABLE));
}

static void bfp_counter_add(uint64_t *counter, uint64_t value)
{
	if (*counter > (UINT64_MAX - value)) {
		*counter = UINT64_MAX;
	} else {
		*counter += value;
	}
}

static uint64_t bfp_next_period_boundary(uint64_t now, uint64_t period_ticks)
{
	uint64_t periods;

	if (period_ticks == 0UL) {
		return UINT64_MAX;
	}

	periods = now / period_ticks;
	if (periods >= (UINT64_MAX / period_ticks)) {
		return UINT64_MAX;
	}

	return (periods + 1UL) * period_ticks;
}

static void bfp_normalize_us(uint16_t pcpu_id, const struct sched_params *params,
	uint32_t *period_us, uint32_t *budget_us)
{
	const struct sched_cpupool_config *pool = sched_get_pcpu_pool_config(pcpu_id);
	uint32_t period = (params->bfp_period_us != 0U) ?
		params->bfp_period_us : BFP_DEFAULT_PERIOD_US;
	uint32_t budget = (params->bfp_budget_us != 0U) ?
		params->bfp_budget_us : BFP_DEFAULT_BUDGET_US;

	if ((pool != NULL) && (pool->policy == SCHED_POLICY_BFP)) {
		if ((params->bfp_period_us == 0U) && (pool->period_us != 0U)) {
			period = pool->period_us;
		}
		if ((params->bfp_budget_us == 0U) && (pool->budget_us != 0U)) {
			budget = pool->budget_us;
		}
	}

	if (period < BFP_MIN_PERIOD_US) {
		panic("BFP pCPU%hu period %u below minimum %u", pcpu_id, period,
			BFP_MIN_PERIOD_US);
	}
	if (budget < BFP_MIN_BUDGET_US) {
		panic("BFP pCPU%hu budget %u below minimum %u", pcpu_id, budget,
			BFP_MIN_BUDGET_US);
	}
	if (budget > period) {
		panic("BFP pCPU%hu budget %u > period %u", pcpu_id, budget, period);
	}

	*period_us = period;
	*budget_us = budget;
}

static bool bfp_vm_uses_pcpu(const struct acrn_vm_config *vm_config,
	uint16_t pcpu_id)
{
	uint16_t idx;

	if ((vm_config == NULL) || (vm_config->name[0] == '\0')) {
		return false;
	}

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

static uint16_t bfp_collect_admission_tasks(uint16_t pcpu_id,
	struct bfp_admission_task *tasks)
{
	const struct acrn_vm_config *vm_config;
	uint16_t vm_id;
	uint16_t count = 0U;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		uint32_t period_us;
		uint32_t budget_us;

		vm_config = get_vm_config(vm_id);
		if (!bfp_vm_uses_pcpu(vm_config, pcpu_id)) {
			continue;
		}
		if (vm_config->sched_params.prio <= PRIO_IDLE) {
			panic("BFP pCPU%hu vm%hu has invalid priority %u", pcpu_id, vm_id,
				vm_config->sched_params.prio);
		}
		bfp_normalize_us(pcpu_id, &vm_config->sched_params, &period_us,
			&budget_us);
		tasks[count].vm_id = vm_id;
		tasks[count].priority = vm_config->sched_params.prio;
		tasks[count].period_us = period_us;
		tasks[count].budget_us = budget_us;
		count++;
	}

	return count;
}

/* [20260731] Fixed-priority response-time admission
 *
 *   R_i(0) = C_i
 *
 *   R_i(k + 1) = C_i + sum(ceil(R_i(k) / T_j) * C_j)
 *                             j in hp-or-equal(i)
 *
 * Code mapping:
 *   - task->budget_us is C_i and response is R_i(k);
 *   - tasks[j].period_us and budget_us are T_j and C_j;
 *   - jobs is ceil(R_i(k) / T_j), calculated with quotient and remainder;
 *   - demand is R_i(k + 1), including all fixed-priority interference.
 *
 * Equal-priority tasks are included because BFP preserves FIFO order within a
 * priority level, so an arbitrary release can wait behind an equal-priority
 * peer. Admission succeeds only when the recurrence converges without demand
 * exceeding the implicit deadline D_i = T_i. Arithmetic overflow or failure to
 * converge within BFP_RTA_MAX_ITERATIONS rejects the configuration.
 *
 * Key rule:
 *   - admission proves the configured worst-case response bound before the
 *     pCPU publishes any BFP vCPU as runnable.
 */
static void bfp_validate_response_time(uint16_t pcpu_id,
	const struct bfp_admission_task *tasks, uint16_t task_count, uint16_t task_idx)
{
	const struct bfp_admission_task *task = &tasks[task_idx];
	uint64_t response = task->budget_us;
	uint32_t iteration;
	bool converged = false;

	for (iteration = 0U; iteration < BFP_RTA_MAX_ITERATIONS; iteration++) {
		uint64_t demand = task->budget_us;
		uint16_t idx;

		for (idx = 0U; idx < task_count; idx++) {
			uint64_t jobs;
			uint64_t interference;

			if ((idx == task_idx) ||
				(tasks[idx].priority < task->priority)) {
				continue;
			}

			jobs = response / tasks[idx].period_us;
			if ((response % tasks[idx].period_us) != 0UL) {
				jobs++;
			}
			if (jobs > (UINT64_MAX / tasks[idx].budget_us)) {
				panic("BFP pCPU%hu vm%hu response interference overflow",
					pcpu_id, task->vm_id);
			}
			interference = jobs * tasks[idx].budget_us;
			if (demand > (UINT64_MAX - interference)) {
				panic("BFP pCPU%hu vm%hu response demand overflow", pcpu_id,
					task->vm_id);
			}
			demand += interference;
		}

		if (demand > task->period_us) {
			panic("BFP admission failed pCPU%hu vm%hu response=%lu period=%u",
				pcpu_id, task->vm_id, demand, task->period_us);
		}
		if (demand == response) {
			converged = true;
			break;
		}
		response = demand;
	}

	if (!converged) {
		panic("BFP admission did not converge pCPU%hu vm%hu iterations=%u",
			pcpu_id, task->vm_id, BFP_RTA_MAX_ITERATIONS);
	}
}

static uint16_t bfp_validate_pcpu_admission(uint16_t pcpu_id)
{
	struct bfp_admission_task tasks[CONFIG_MAX_VM_NUM];
	uint16_t task_count;
	uint16_t idx;

	task_count = bfp_collect_admission_tasks(pcpu_id, tasks);
	for (idx = 0U; idx < task_count; idx++) {
		bfp_validate_response_time(pcpu_id, tasks, task_count, idx);
	}

	return task_count;
}

static void bfp_queue_remove(struct thread_object *obj)
{
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;

	if (bfp_is_queued(data)) {
		list_del_init(&data->list);
		data->queue = BFP_QUEUE_NONE;
	}
}

static void bfp_ready_insert(struct thread_object *obj, bool keep_turn)
{
	struct sched_bfp_control *bfp_ctl =
		(struct sched_bfp_control *)per_cpu(sched_ctl, obj->pcpu_id).priv;
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;
	struct sched_bfp_data *iter_data;
	struct thread_object *iter_obj;
	struct list_head *pos;

	bfp_queue_remove(obj);
	list_for_each(pos, &bfp_ctl->ready_queue) {
		iter_obj = container_of(pos, struct thread_object, data);
		iter_data = (struct sched_bfp_data *)iter_obj->data;
		if ((data->priority > iter_data->priority) ||
			(keep_turn && (data->priority == iter_data->priority))) {
			list_add_node(&data->list, pos->prev, pos);
			data->queue = BFP_QUEUE_READY;
			return;
		}
	}

	list_add_tail(&data->list, &bfp_ctl->ready_queue);
	data->queue = BFP_QUEUE_READY;
}

static void bfp_depleted_insert(struct thread_object *obj)
{
	struct sched_bfp_control *bfp_ctl =
		(struct sched_bfp_control *)per_cpu(sched_ctl, obj->pcpu_id).priv;
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;
	struct sched_bfp_data *iter_data;
	struct thread_object *iter_obj;
	struct list_head *pos;

	bfp_queue_remove(obj);
	list_for_each(pos, &bfp_ctl->depleted_queue) {
		iter_obj = container_of(pos, struct thread_object, data);
		iter_data = (struct sched_bfp_data *)iter_obj->data;
		if (data->next_replenish_ticks < iter_data->next_replenish_ticks) {
			list_add_node(&data->list, pos->prev, pos);
			data->queue = BFP_QUEUE_DEPLETED;
			data->depleted_count++;
			return;
		}
	}

	list_add_tail(&data->list, &bfp_ctl->depleted_queue);
	data->queue = BFP_QUEUE_DEPLETED;
	data->depleted_count++;
}

static bool bfp_advance_inactive_period(struct sched_bfp_data *data, uint64_t now)
{
	uint64_t periods;
	bool advanced = false;

	if ((data->next_replenish_ticks != UINT64_MAX) &&
		(now >= data->next_replenish_ticks)) {
		periods = ((now - data->next_replenish_ticks) / data->period_ticks) + 1UL;
		if (periods > ((UINT64_MAX - data->next_replenish_ticks) /
			data->period_ticks)) {
			data->next_replenish_ticks = UINT64_MAX;
			data->remaining_ticks = 0UL;
		} else {
			data->next_replenish_ticks += periods * data->period_ticks;
			data->remaining_ticks = data->budget_ticks;
		}
		bfp_counter_add(&data->replenish_count, periods);
		advanced = true;
	}

	return advanced;
}

static void bfp_charge_current_period(struct sched_bfp_data *data, uint64_t delta)
{
	if (delta > data->remaining_ticks) {
		data->remaining_ticks = 0UL;
		bfp_counter_add(&data->overrun_count, 1UL);
	} else {
		data->remaining_ticks -= delta;
	}
}

/* [20260731] Bounded late-accounting recovery
 *
 * last_start -------- old boundary -------- full periods -------- now
 *      |                    |                    |                 |
 *      +-- old budget ------+-- account O(1) ---+-- phase budget -+
 *
 * Key rule:
 *   - continuous execution is charged in every crossed period;
 *   - missed periods are reduced arithmetically instead of looped;
 *   - timer lateness can be diagnosed but never creates free budget.
 */
static void bfp_account_runtime(struct thread_object *obj, uint64_t now)
{
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;
	uint64_t start;
	uint64_t boundary;
	uint64_t elapsed;
	uint64_t full_periods;
	uint64_t phase;
	uint64_t replenishments;

	if (is_idle_thread(obj) || (obj->status != THREAD_STS_RUNNING) ||
		(data->last_start_ticks == 0UL) || (now <= data->last_start_ticks)) {
		return;
	}

	start = data->last_start_ticks;
	data->last_start_ticks = now;
	boundary = data->next_replenish_ticks;
	if ((boundary == UINT64_MAX) || (now < boundary)) {
		bfp_charge_current_period(data, now - start);
		return;
	}

	if (start < boundary) {
		bfp_charge_current_period(data, boundary - start);
	}

	elapsed = now - boundary;
	full_periods = elapsed / data->period_ticks;
	phase = elapsed % data->period_ticks;
	replenishments = full_periods + 1UL;
	if (replenishments > ((UINT64_MAX - boundary) / data->period_ticks)) {
		data->next_replenish_ticks = UINT64_MAX;
		data->remaining_ticks = 0UL;
		bfp_counter_add(&data->overrun_count, 1UL);
		return;
	}

	data->next_replenish_ticks = boundary +
		replenishments * data->period_ticks;
	bfp_counter_add(&data->replenish_count, replenishments);
	if ((full_periods != 0UL) && (data->budget_ticks < data->period_ticks)) {
		bfp_counter_add(&data->overrun_count, full_periods);
	}
	if (phase > data->budget_ticks) {
		data->remaining_ticks = 0UL;
		bfp_counter_add(&data->overrun_count, 1UL);
	} else {
		data->remaining_ticks = data->budget_ticks - phase;
	}
}

static void bfp_refresh_thread(struct thread_object *obj, uint64_t now,
	bool keep_turn)
{
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;

	(void)bfp_advance_inactive_period(data, now);
	if (!bfp_is_active(obj)) {
		bfp_queue_remove(obj);
	} else if (data->remaining_ticks != 0UL) {
		bfp_ready_insert(obj, keep_turn);
	} else {
		bfp_depleted_insert(obj);
	}
}

static void bfp_refresh_queues(struct sched_control *ctl, uint64_t now)
{
	struct sched_bfp_control *bfp_ctl = (struct sched_bfp_control *)ctl->priv;
	struct sched_bfp_data *data;
	struct thread_object *obj;
	struct list_head *pos;
	struct list_head *tmp;

	list_for_each(pos, &bfp_ctl->ready_queue) {
		obj = container_of(pos, struct thread_object, data);
		data = (struct sched_bfp_data *)obj->data;
		(void)bfp_advance_inactive_period(data, now);
	}

	list_for_each_safe(pos, tmp, &bfp_ctl->depleted_queue) {
		obj = container_of(pos, struct thread_object, data);
		data = (struct sched_bfp_data *)obj->data;
		if (bfp_advance_inactive_period(data, now)) {
			bfp_queue_remove(obj);
			if (bfp_is_active(obj) && (data->remaining_ticks != 0UL)) {
				bfp_ready_insert(obj, false);
			}
		}
	}
}

static uint64_t bfp_next_depleted_replenishment(
	const struct sched_bfp_control *bfp_ctl)
{
	const struct thread_object *obj;
	const struct sched_bfp_data *data;
	uint64_t deadline = 0UL;

	if (!list_empty(&bfp_ctl->depleted_queue)) {
		obj = get_first_item(&bfp_ctl->depleted_queue, struct thread_object, data);
		data = (const struct sched_bfp_data *)obj->data;
		deadline = data->next_replenish_ticks;
	}

	return deadline;
}

static void bfp_timer_handler(void *param)
{
	struct sched_control *ctl = (struct sched_control *)param;
	struct sched_bfp_control *bfp_ctl;
	struct thread_object *current;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t now = cpu_ticks();
	uint64_t rflags;

	ASSERT(ctl->pcpu_id == pcpu_id, "BFP timer on wrong cpu!");
	obtain_schedule_lock(pcpu_id, &rflags);
	bfp_ctl = (struct sched_bfp_control *)ctl->priv;
	bfp_ctl->timer_deadline_ticks = 0UL;
	sched_account_tick(ctl);
	current = ctl->curr_obj;
	if ((current != NULL) && !is_idle_thread(current)) {
		bfp_account_runtime(current, now);
	}
	bfp_refresh_queues(ctl, now);
	make_reschedule_request(pcpu_id);
	release_schedule_lock(pcpu_id, rflags);
}

static void bfp_program_local_timer(struct sched_control *ctl,
	struct thread_object *running)
{
	struct sched_bfp_control *bfp_ctl = (struct sched_bfp_control *)ctl->priv;
	struct sched_bfp_data *data;
	uint64_t now = cpu_ticks();
	uint64_t timeout = 0UL;
	uint64_t replenish_timeout;
	int32_t ret = 0;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "program BFP timer on wrong cpu!");
	if ((running != NULL) && !is_idle_thread(running)) {
		data = (struct sched_bfp_data *)running->data;
		if (data->remaining_ticks != 0UL) {
			timeout = (data->remaining_ticks <= (UINT64_MAX - now)) ?
				now + data->remaining_ticks : UINT64_MAX;
			if (data->next_replenish_ticks < timeout) {
				timeout = data->next_replenish_ticks;
			}
		}
	}

	replenish_timeout = bfp_next_depleted_replenishment(bfp_ctl);
	if ((replenish_timeout != 0UL) &&
		((timeout == 0UL) || (replenish_timeout < timeout))) {
		timeout = replenish_timeout;
	}

	if ((timeout == bfp_ctl->timer_deadline_ticks) &&
		timer_is_started(&bfp_ctl->tick_timer)) {
		return;
	}
	if (timer_is_started(&bfp_ctl->tick_timer)) {
		del_timer(&bfp_ctl->tick_timer);
	}
	bfp_ctl->timer_deadline_ticks = 0UL;
	if (timeout != 0UL) {
		ASSERT(timeout > now, "BFP timer deadline is not in the future!");
		update_timer(&bfp_ctl->tick_timer, timeout, 0UL);
		ret = add_timer(&bfp_ctl->tick_timer);
		if (ret != 0) {
			panic("BFP pCPU%hu failed to arm timer deadline=%lu ret=%d",
				ctl->pcpu_id, timeout, ret);
		}
		bfp_ctl->timer_deadline_ticks = timeout;
	}
}

static void bfp_program_timer(struct sched_control *ctl,
	struct thread_object *running)
{
	if (ctl->pcpu_id == get_pcpu_id()) {
		bfp_program_local_timer(ctl, running);
	}
}

static int32_t sched_bfp_init(struct sched_control *ctl)
{
	struct sched_bfp_control *bfp_ctl = &per_cpu(sched_bfp_ctl, ctl->pcpu_id);

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "init BFP on wrong cpu!");
	ctl->priv = bfp_ctl;
	INIT_LIST_HEAD(&bfp_ctl->ready_queue);
	INIT_LIST_HEAD(&bfp_ctl->depleted_queue);
	bfp_ctl->timer_deadline_ticks = 0UL;
	bfp_ctl->admitted_vcpus = bfp_validate_pcpu_admission(ctl->pcpu_id);
	initialize_timer(&bfp_ctl->tick_timer, bfp_timer_handler, ctl, 0UL, 0UL);

	return 0;
}

static void sched_bfp_deinit(struct sched_control *ctl)
{
	struct sched_bfp_control *bfp_ctl = (struct sched_bfp_control *)ctl->priv;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "deinit BFP on wrong cpu!");
	del_timer(&bfp_ctl->tick_timer);
	bfp_ctl->timer_deadline_ticks = 0UL;
}

static void sched_bfp_init_data(struct thread_object *obj,
	struct sched_params *params)
{
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;
	uint32_t period_us = 0U;
	uint32_t budget_us = 0U;
	uint64_t now = cpu_ticks();

	ASSERT(sizeof(*data) <= THREAD_DATA_SIZE, "BFP thread data is too large!");
	INIT_LIST_HEAD(&data->list);
	data->period_ticks = 0UL;
	data->budget_ticks = 0UL;
	data->remaining_ticks = 0UL;
	data->next_replenish_ticks = UINT64_MAX;
	data->last_start_ticks = 0UL;
	data->depleted_count = 0UL;
	data->replenish_count = 0UL;
	data->overrun_count = 0UL;
	data->priority = PRIO_IDLE;
	data->queue = BFP_QUEUE_NONE;

	if (is_idle_thread(obj)) {
		return;
	}
	if (!obj->is_vcpu) {
		panic("BFP pCPU%hu rejects host thread %s", obj->pcpu_id, obj->name);
	}
	if ((params == NULL) || (params->prio <= PRIO_IDLE)) {
		panic("BFP pCPU%hu vm%hu vcpu%hu invalid priority", obj->pcpu_id,
			obj->vm_id, obj->vcpu_id);
	}

	bfp_normalize_us(obj->pcpu_id, params, &period_us, &budget_us);
	data->period_ticks = us_to_ticks(period_us);
	data->budget_ticks = us_to_ticks(budget_us);
	if ((data->period_ticks == 0UL) || (data->budget_ticks == 0UL) ||
		(data->budget_ticks > data->period_ticks)) {
		panic("BFP pCPU%hu vm%hu vcpu%hu invalid tick budget", obj->pcpu_id,
			obj->vm_id, obj->vcpu_id);
	}
	data->remaining_ticks = data->budget_ticks;
	data->next_replenish_ticks =
		bfp_next_period_boundary(now, data->period_ticks);
	data->priority = params->prio;
}

static void sched_bfp_deinit_data(struct thread_object *obj)
{
	bfp_queue_remove(obj);
}

static struct thread_object *sched_bfp_pick_next(struct sched_control *ctl)
{
	struct sched_bfp_control *bfp_ctl = (struct sched_bfp_control *)ctl->priv;
	struct thread_object *current = ctl->curr_obj;
	struct thread_object *next = &get_cpu_var(idle);
	struct sched_bfp_data *data;
	uint64_t now = cpu_ticks();

	if ((current != NULL) && !is_idle_thread(current)) {
		bfp_account_runtime(current, now);
		bfp_refresh_thread(current, now, true);
	}
	bfp_refresh_queues(ctl, now);
	if (!list_empty(&bfp_ctl->ready_queue)) {
		next = get_first_item(&bfp_ctl->ready_queue, struct thread_object, data);
		bfp_queue_remove(next);
		data = (struct sched_bfp_data *)next->data;
		data->last_start_ticks = now;
	}
	bfp_program_local_timer(ctl, next);

	return next;
}

static void sched_bfp_sleep(struct thread_object *obj)
{
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;

	if (bfp_is_current(obj)) {
		bfp_account_runtime(obj, cpu_ticks());
	}
	bfp_queue_remove(obj);
	data->last_start_ticks = 0UL;
}

static void sched_bfp_wake(struct thread_object *obj)
{
	struct sched_bfp_data *data = (struct sched_bfp_data *)obj->data;
	uint64_t now = cpu_ticks();

	(void)bfp_advance_inactive_period(data, now);
	data->last_start_ticks = 0UL;
	if (data->remaining_ticks != 0UL) {
		bfp_ready_insert(obj, false);
	} else {
		bfp_depleted_insert(obj);
	}
	bfp_program_timer(&per_cpu(sched_ctl, obj->pcpu_id),
		per_cpu(sched_ctl, obj->pcpu_id).curr_obj);
}

static void sched_bfp_suspend(struct sched_control *ctl)
{
	struct sched_bfp_control *bfp_ctl = (struct sched_bfp_control *)ctl->priv;
	struct thread_object *current = ctl->curr_obj;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "suspend BFP on wrong cpu!");
	if ((current != NULL) && !is_idle_thread(current)) {
		bfp_account_runtime(current, cpu_ticks());
		((struct sched_bfp_data *)current->data)->last_start_ticks = 0UL;
	}
	del_timer(&bfp_ctl->tick_timer);
	bfp_ctl->timer_deadline_ticks = 0UL;
}

static void sched_bfp_resume(struct sched_control *ctl)
{
	struct thread_object *current = ctl->curr_obj;
	uint64_t now = cpu_ticks();

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "resume BFP on wrong cpu!");
	if ((current != NULL) && !is_idle_thread(current)) {
		struct sched_bfp_data *data = (struct sched_bfp_data *)current->data;

		(void)bfp_advance_inactive_period(data, now);
		data->last_start_ticks = now;
	}
	make_reschedule_request(ctl->pcpu_id);
}

struct acrn_scheduler sched_bfp = {
	.name = "bfp",
	.stat_desc = "hard-budget fixed-priority:inactive",
	.init = sched_bfp_init,
	.init_data = sched_bfp_init_data,
	.pick_next = sched_bfp_pick_next,
	.sleep = sched_bfp_sleep,
	.wake = sched_bfp_wake,
	.yield = NULL,
	.prioritize = NULL,
	.deinit_data = sched_bfp_deinit_data,
	.deinit = sched_bfp_deinit,
	.suspend = sched_bfp_suspend,
	.resume = sched_bfp_resume,
};
