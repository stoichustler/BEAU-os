/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cpu.h>
#include <debug/shell.h>
#include <errno.h>
#include <event.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>

#include "rt-tests.h"

#define OSLAT_DEFAULT_DURATION_MS 1000U
#define OSLAT_MAX_DURATION_MS 60000U
#define OSLAT_START_TIMEOUT_MS 5000U
#define OSLAT_RUN_GRACE_MS 5000U
#define OSLAT_BUCKETS 16U
#define OSLAT_LINE_SIZE 160U
#define OSLAT_OUTPUT_SIZE ((MAX_PCPU_NUM * OSLAT_LINE_SIZE) + 768U)

enum oslat_state {
	OSLAT_IDLE = 0,
	OSLAT_QUEUED,
	OSLAT_RUNNING,
	OSLAT_DONE,
	OSLAT_UNAVAILABLE,
	OSLAT_ERROR,
};

struct oslat_worker {
	struct thread_object thread;
	struct sched_event request_event;
	struct sched_event completion_event;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	volatile uint64_t request_generation;
	volatile uint64_t complete_generation;
	volatile uint64_t queued_ticks;
	volatile uint64_t started_ticks;
	volatile enum oslat_state state;
	uint64_t samples;
	uint64_t min_ticks;
	uint64_t sum_ticks;
	uint64_t max_ticks;
	uint64_t histogram[OSLAT_BUCKETS];
};

struct oslat_control {
	spinlock_t lock;
	struct thread_object controller;
	struct sched_event request_event;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	bool initialized;
	bool active;
	uint16_t pcpu_num;
	uint64_t generation;
	uint32_t duration_ms;
};

static struct oslat_worker oslat_workers[MAX_PCPU_NUM];
static struct oslat_control oslat_control = { .lock = { .head = 0U, .tail = 0U } };
static char oslat_output[OSLAT_OUTPUT_SIZE];

static const char *oslat_state_name(enum oslat_state state)
{
	switch (state) {
	case OSLAT_IDLE:
		return "idle";
	case OSLAT_QUEUED:
		return "queued";
	case OSLAT_RUNNING:
		return "running";
	case OSLAT_DONE:
		return "done";
	case OSLAT_UNAVAILABLE:
		return "unavailable";
	case OSLAT_ERROR:
		return "error";
	default:
		return "invalid";
	}
}

static uint16_t oslat_bucket(uint64_t ticks)
{
	uint64_t us = ticks_to_us(ticks);
	uint16_t bucket = 0U;

	while ((us > 1UL) && (bucket < (OSLAT_BUCKETS - 1U))) {
		us >>= 1U;
		bucket++;
	}
	return bucket;
}

static void oslat_record(struct oslat_worker *worker, uint64_t gap)
{
	uint16_t bucket;

	if (gap == 0UL) {
		return;
	}
	if (gap < worker->min_ticks) {
		worker->min_ticks = gap;
	}
	if (gap > worker->max_ticks) {
		worker->max_ticks = gap;
	}
	if (worker->sum_ticks <= (UINT64_MAX - gap)) {
		worker->sum_ticks += gap;
	} else {
		worker->sum_ticks = UINT64_MAX;
	}
	if (worker->samples != UINT64_MAX) {
		worker->samples++;
	}
	bucket = oslat_bucket(gap);
	if (worker->histogram[bucket] != UINT64_MAX) {
		worker->histogram[bucket]++;
	}
}

static bool oslat_worker_available(const struct oslat_worker *worker)
{
	enum oslat_state state = __atomic_load_n(&worker->state, __ATOMIC_ACQUIRE);

	return ((state == OSLAT_IDLE) || (state == OSLAT_DONE) ||
		(state == OSLAT_UNAVAILABLE) || (state == OSLAT_ERROR)) &&
		(__atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE) ==
		 __atomic_load_n(&worker->complete_generation, __ATOMIC_ACQUIRE));
}

/* [20260727] EL2 polling-gap ownership
 *
 * shell -> controller -> QUEUED worker --CAS--> RUNNING polling loop
 *                          |                         |
 *                          +--> cancel                +--> completion event
 *
 * Key rule:
 *   - only the worker that wins QUEUED-to-RUNNING executes the bounded loop;
 *   - a queued timeout is cancelled before execution and returned to blocked;
 *   - the controller retains the benchmark gate until every started worker has
 *     published completion, so idle tools leave no runnable CPU load.
 */
static void oslat_worker_main(struct thread_object *thread)
{
	struct oslat_worker *worker = &oslat_workers[get_pcpu_id()];

	ASSERT(thread == &worker->thread, "oslat worker on wrong pCPU\n");
	while (true) {
		uint64_t request;
		uint64_t start;
		uint64_t deadline;
		uint64_t previous;
		enum oslat_state expected;

		wait_event(&worker->request_event);
		request = __atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE);
		if (request == __atomic_load_n(&worker->complete_generation,
			__ATOMIC_ACQUIRE)) {
			continue;
		}
		start = cpu_ticks();
		__atomic_store_n(&worker->started_ticks, start, __ATOMIC_RELAXED);
		expected = OSLAT_QUEUED;
		if (!__atomic_compare_exchange_n(&worker->state, &expected, OSLAT_RUNNING,
			false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			continue;
		}
		deadline = start + us_to_ticks(oslat_control.duration_ms * 1000U);
		previous = start;
		while (true) {
			uint64_t now = cpu_ticks();

			if (now >= deadline) {
				break;
			}
			oslat_record(worker, now - previous);
			previous = now;
		}
		__atomic_store_n(&worker->complete_generation, request, __ATOMIC_RELEASE);
		__atomic_store_n(&worker->state, OSLAT_DONE, __ATOMIC_RELEASE);
		signal_event(&worker->completion_event);
	}
}

static bool oslat_dispatch_worker(struct oslat_worker *worker, uint64_t generation)
{
	if (!oslat_worker_available(worker)) {
		return false;
	}
	worker->samples = 0UL;
	worker->min_ticks = UINT64_MAX;
	worker->sum_ticks = 0UL;
	worker->max_ticks = 0UL;
	(void)memset(worker->histogram, 0U, sizeof(worker->histogram));
	__atomic_store_n(&worker->queued_ticks, cpu_ticks(), __ATOMIC_RELAXED);
	__atomic_store_n(&worker->state, OSLAT_QUEUED, __ATOMIC_RELEASE);
	__atomic_store_n(&worker->request_generation, generation, __ATOMIC_RELEASE);
	reset_event(&worker->completion_event);
	signal_event(&worker->request_event);
	wake_thread(&worker->thread);
	request_thread_priority(&worker->thread);
	return true;
}

static bool oslat_cancel_worker(struct oslat_worker *worker, uint64_t generation)
{
	enum oslat_state expected = OSLAT_QUEUED;

	if (!__atomic_compare_exchange_n(&worker->state, &expected, OSLAT_UNAVAILABLE,
		false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return false;
	}
	__atomic_store_n(&worker->complete_generation, generation, __ATOMIC_RELEASE);
	sleep_thread(&worker->thread);
	return true;
}

static void oslat_wait_worker(struct oslat_worker *worker, uint64_t generation)
{
	uint64_t run_limit = us_to_ticks((oslat_control.duration_ms +
		OSLAT_RUN_GRACE_MS) * 1000U);

	while (true) {
		enum oslat_state state = __atomic_load_n(&worker->state, __ATOMIC_ACQUIRE);
		uint64_t now = cpu_ticks();

		if ((state == OSLAT_DONE) || (state == OSLAT_UNAVAILABLE) ||
			(state == OSLAT_ERROR)) {
			return;
		}
		if ((state == OSLAT_QUEUED) &&
			(now - __atomic_load_n(&worker->queued_ticks, __ATOMIC_ACQUIRE) >=
			us_to_ticks(OSLAT_START_TIMEOUT_MS * 1000U))) {
			if (oslat_cancel_worker(worker, generation)) {
				return;
			}
			continue;
		}
		if ((state == OSLAT_RUNNING) &&
			(now - __atomic_load_n(&worker->started_ticks, __ATOMIC_ACQUIRE) >=
			run_limit)) {
			wait_event(&worker->completion_event);
			continue;
		}
		yield_current();
		schedule();
	}
}

static void oslat_report(uint64_t generation)
{
	uint64_t histogram[OSLAT_BUCKETS] = { 0UL };
	uint16_t pcpu_id;
	uint16_t complete = 0U;
	size_t offset;

	(void)snprintf(oslat_output, sizeof(oslat_output),
		"┌─ OSLAT generation:%lu duration.ms:%u\r\n", generation,
		oslat_control.duration_ms);
	offset = strnlen_s(oslat_output, sizeof(oslat_output));
	(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset,
		"│ EL2 polling-gap diagnostic; runs only while this command is active\r\n");
	offset = strnlen_s(oslat_output, sizeof(oslat_output));
	(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset,
		"│ %3s %-12s %10s %10s %10s %10s\r\n", "cpu", "state", "samples",
		"min.us", "avg.us", "max.us");
	offset = strnlen_s(oslat_output, sizeof(oslat_output));
	(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset,
		"│ %3s %-12s %10s %10s %10s %10s\r\n", "───", "────────────",
		"──────────", "──────────", "──────────", "──────────");
	offset = strnlen_s(oslat_output, sizeof(oslat_output));
	for (pcpu_id = 0U; pcpu_id < oslat_control.pcpu_num; pcpu_id++) {
		const struct oslat_worker *worker = &oslat_workers[pcpu_id];
		enum oslat_state state = __atomic_load_n(&worker->state, __ATOMIC_ACQUIRE);
		uint64_t average = worker->samples == 0UL ? 0UL :
			worker->sum_ticks / worker->samples;
		uint16_t bucket;

		for (bucket = 0U; bucket < OSLAT_BUCKETS; bucket++) {
			histogram[bucket] += worker->histogram[bucket];
		}
		if ((__atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE) == generation) &&
			(state == OSLAT_DONE)) {
			complete++;
		}
		(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset,
			"│ %3hu %-12s %10lu %10lu %10lu %10lu\r\n", pcpu_id,
			oslat_state_name(state), worker->samples,
			ticks_to_us(worker->min_ticks == UINT64_MAX ? 0UL : worker->min_ticks),
			ticks_to_us(average), ticks_to_us(worker->max_ticks));
		offset = strnlen_s(oslat_output, sizeof(oslat_output));
	}
	(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset,
		"│ complete:%hu/%hu histogram.us:", complete, oslat_control.pcpu_num);
	offset = strnlen_s(oslat_output, sizeof(oslat_output));
	for (pcpu_id = 0U; pcpu_id < OSLAT_BUCKETS; pcpu_id++) {
		(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset,
			" %lu", histogram[pcpu_id]);
		offset = strnlen_s(oslat_output, sizeof(oslat_output));
	}
	if (offset < (sizeof(oslat_output) - 1U)) {
		(void)snprintf(&oslat_output[offset], sizeof(oslat_output) - offset, "\r\n└─\r\n");
	}
	(void)shell_async_puts(oslat_output);
}

static void oslat_controller_main(__unused struct thread_object *thread)
{
	while (true) {
		uint64_t generation;
		uint64_t flags;
		uint16_t pcpu_id;

		wait_event(&oslat_control.request_event);
		spinlock_irqsave_obtain(&oslat_control.lock, &flags);
		if (!oslat_control.active) {
			spinlock_irqrestore_release(&oslat_control.lock, flags);
			continue;
		}
		generation = oslat_control.generation;
		spinlock_irqrestore_release(&oslat_control.lock, flags);
		for (pcpu_id = 0U; pcpu_id < oslat_control.pcpu_num; pcpu_id++) {
			(void)oslat_dispatch_worker(&oslat_workers[pcpu_id], generation);
		}
		for (pcpu_id = 0U; pcpu_id < oslat_control.pcpu_num; pcpu_id++) {
			if (__atomic_load_n(&oslat_workers[pcpu_id].request_generation,
				__ATOMIC_ACQUIRE) == generation) {
				oslat_wait_worker(&oslat_workers[pcpu_id], generation);
			}
		}
		oslat_report(generation);
		spinlock_irqsave_obtain(&oslat_control.lock, &flags);
		if (oslat_control.generation == generation) {
			oslat_control.active = false;
		}
		spinlock_irqrestore_release(&oslat_control.lock, flags);
		rt_test_release();
	}
}

void oslat_init(void)
{
	struct sched_params params = { .prio = PRIO_LOW, .bvt_weight = 1U };
	uint16_t pcpu_id;

	if (oslat_control.initialized) {
		return;
	}
	oslat_control.pcpu_num = get_pcpu_nums();
	for (pcpu_id = 0U; pcpu_id < oslat_control.pcpu_num; pcpu_id++) {
		struct oslat_worker *worker = &oslat_workers[pcpu_id];

		worker->state = OSLAT_IDLE;
		init_event(&worker->request_event);
		init_event(&worker->completion_event);
		(void)snprintf(worker->thread.name, sizeof(worker->thread.name), "oslat-%02hu", pcpu_id);
		worker->thread.pcpu_id = pcpu_id;
		worker->thread.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
		worker->thread.thread_entry = oslat_worker_main;
		worker->thread.host_sp = arch_setup_thread_stack(&worker->thread, worker->stack,
			CONFIG_STACK_SIZE);
		init_thread_data(&worker->thread, &params);
	}
	init_event(&oslat_control.request_event);
	(void)strncpy_s(oslat_control.controller.name, sizeof(oslat_control.controller.name),
		"oslat-ctl", sizeof(oslat_control.controller.name));
	oslat_control.controller.pcpu_id = BSP_CPU_ID;
	oslat_control.controller.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	oslat_control.controller.thread_entry = oslat_controller_main;
	oslat_control.controller.host_sp = arch_setup_thread_stack(&oslat_control.controller,
		oslat_control.stack, CONFIG_STACK_SIZE);
	init_thread_data(&oslat_control.controller, &params);
	oslat_control.initialized = true;
}

int32_t shell_oslat(int32_t argc, char **argv)
{
	int64_t parsed;
	uint64_t flags;

	if ((argc < 1) || (argc > 2)) {
		return -EINVAL;
	}
	parsed = argc == 2 ? strtol_deci(argv[1]) : (int64_t)OSLAT_DEFAULT_DURATION_MS;
	if ((parsed <= 0L) || ((uint64_t)parsed > OSLAT_MAX_DURATION_MS)) {
		return -EINVAL;
	}
	if (!rt_test_try_acquire()) {
		return -EBUSY;
	}
	spinlock_irqsave_obtain(&oslat_control.lock, &flags);
	if (!oslat_control.initialized || oslat_control.active) {
		spinlock_irqrestore_release(&oslat_control.lock, flags);
		rt_test_release();
		return -EBUSY;
	}
	oslat_control.generation++;
	if (oslat_control.generation == 0UL) {
		oslat_control.generation = 1UL;
	}
	oslat_control.duration_ms = (uint32_t)parsed;
	oslat_control.active = true;
	spinlock_irqrestore_release(&oslat_control.lock, flags);
	signal_event(&oslat_control.request_event);
	wake_thread(&oslat_control.controller);
	return 0;
}
