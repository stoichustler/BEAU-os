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

#include "dhrystone_port.h"

#define DHRYSTONE_LINE_SIZE 144U
#define DHRYSTONE_OUTPUT_SIZE ((MAX_PCPU_NUM * DHRYSTONE_LINE_SIZE) + 512U)
/* Match the highest static VM BVT weight so a completed diagnostic does not
 * retain a disproportionately large virtual-time debt before its next run. */
#define DHRYSTONE_BVT_WEIGHT 128U
#define DHRYSTONE_BVT_WARP_VALUE 8
#define DHRYSTONE_BVT_WARP_LIMIT 1U
#define DHRYSTONE_BVT_UNWARP_PERIOD 4U
#define DHRYSTONE_MAX_WAIT_YIELDS 1000000U

enum dhrystone_worker_state {
	DHRYSTONE_WORKER_IDLE = 0,
	DHRYSTONE_WORKER_QUEUED,
	DHRYSTONE_WORKER_RUNNING,
	DHRYSTONE_WORKER_DONE,
	DHRYSTONE_WORKER_START_TIMEOUT,
	DHRYSTONE_WORKER_ERROR,
};

struct dhrystone_worker {
	struct thread_object thread;
	struct sched_event request_event;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	volatile uint64_t request_generation;
	volatile uint64_t complete_generation;
	volatile uint64_t queued_ticks;
	volatile uint64_t started_ticks;
	volatile enum dhrystone_worker_state state;
	volatile bool late;
	int32_t error;
	struct dhrystone_result result;
};

struct dhrystone_control {
	spinlock_t lock;
	struct thread_object controller;
	struct sched_event request_event;
	struct sched_event completion_event;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	bool initialized;
	bool active;
	uint16_t pcpu_num;
	uint64_t generation;
	uint32_t initial_runs;
};

static struct dhrystone_worker dhrystone_workers[MAX_PCPU_NUM];
static struct dhrystone_control dhrystone_control = { .lock = { .head = 0U, .tail = 0U } };
static char dhrystone_output[DHRYSTONE_OUTPUT_SIZE];

static const char *dhrystone_state_name(enum dhrystone_worker_state state)
{
	switch (state) {
	case DHRYSTONE_WORKER_IDLE:
		return "idle";
	case DHRYSTONE_WORKER_QUEUED:
		return "queued";
	case DHRYSTONE_WORKER_RUNNING:
		return "running";
	case DHRYSTONE_WORKER_DONE:
		return "done";
	case DHRYSTONE_WORKER_START_TIMEOUT:
		return "unavailable";
	case DHRYSTONE_WORKER_ERROR:
		return "error";
	default:
		return "invalid";
	}
}

static bool dhrystone_worker_available(const struct dhrystone_worker *worker)
{
	enum dhrystone_worker_state state = __atomic_load_n(&worker->state,
		__ATOMIC_ACQUIRE);

	return ((state == DHRYSTONE_WORKER_IDLE) || (state == DHRYSTONE_WORKER_DONE) ||
		(state == DHRYSTONE_WORKER_START_TIMEOUT) ||
		(state == DHRYSTONE_WORKER_ERROR)) &&
		(__atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE) ==
		 __atomic_load_n(&worker->complete_generation, __ATOMIC_ACQUIRE));
}

static void dhrystone_report(uint64_t generation)
{
	uint16_t pcpu_id;
	uint16_t selected = 0U;
	uint16_t complete = 0U;
	size_t offset;
	bool valid = true;

	(void)snprintf(dhrystone_output, sizeof(dhrystone_output),
		"┌─ DHRYSTONE generation:%lu initial-runs:%s\r\n", generation,
		dhrystone_control.initial_runs == 0U ? "auto" : "fixed");
	offset = strnlen_s(dhrystone_output, sizeof(dhrystone_output));
	(void)snprintf(&dhrystone_output[offset], sizeof(dhrystone_output) - offset,
		"│ EL2 diagnostic; pCPU executions are serialized by source state\r\n");
	offset = strnlen_s(dhrystone_output, sizeof(dhrystone_output));
	(void)snprintf(&dhrystone_output[offset], sizeof(dhrystone_output) - offset,
		"│ cpu state        elapsed.us       runs       dps validation\r\n");
	offset = strnlen_s(dhrystone_output, sizeof(dhrystone_output));
	for (pcpu_id = 0U; pcpu_id < dhrystone_control.pcpu_num; pcpu_id++) {
		const struct dhrystone_worker *worker = &dhrystone_workers[pcpu_id];
		enum dhrystone_worker_state state = __atomic_load_n(&worker->state,
			__ATOMIC_ACQUIRE);
		uint64_t request = __atomic_load_n(&worker->request_generation,
			__ATOMIC_ACQUIRE);
		uint64_t elapsed_us = 0UL;
		uint64_t rate_milli = 0UL;
		uint32_t runs = 0U;
		const char *name = "skipped";
		bool late = __atomic_load_n(&worker->late, __ATOMIC_ACQUIRE);

		if (request == generation) {
			selected++;
			name = late ? "late" : dhrystone_state_name(state);
			elapsed_us = ticks_to_us(worker->result.elapsed_ticks);
			runs = worker->result.runs;
			if (elapsed_us != 0UL) {
				rate_milli = ((uint64_t)runs * 1000000000UL) / elapsed_us;
			}
			if ((state == DHRYSTONE_WORKER_DONE) && worker->result.valid) {
				complete++;
			} else {
				valid = false;
			}
		} else {
			valid = false;
		}
		if (request == generation) {
			(void)snprintf(&dhrystone_output[offset], sizeof(dhrystone_output) - offset,
				"│ %3hu %-12s %10lu %10u %9lu.%03lu %s\r\n", pcpu_id, name,
				elapsed_us, runs, rate_milli / 1000UL, rate_milli % 1000UL,
				(state == DHRYSTONE_WORKER_DONE) && worker->result.valid ?
				"pass" : "fail");
		} else {
			(void)snprintf(&dhrystone_output[offset], sizeof(dhrystone_output) - offset,
				"│ %3hu %-12s %10s %10s %13s %s\r\n", pcpu_id, name,
				"-", "-", "n/a", "n/a");
		}
		offset = strnlen_s(dhrystone_output, sizeof(dhrystone_output));
	}
	if ((selected != dhrystone_control.pcpu_num) || (complete != selected)) {
		valid = false;
	}
	(void)snprintf(&dhrystone_output[offset], sizeof(dhrystone_output) - offset,
		"│ complete:%hu/%hu selected:%hu/%hu validation:%s\r\n", complete,
		selected, selected, dhrystone_control.pcpu_num, valid ? "pass" : "partial");
	offset = strnlen_s(dhrystone_output, sizeof(dhrystone_output));
	if (offset < (sizeof(dhrystone_output) - 1U)) {
		(void)snprintf(&dhrystone_output[offset], sizeof(dhrystone_output) - offset,
			"└─\r\n");
	}
	(void)shell_async_puts(dhrystone_output);
}

/* [20260726] Serialized Dhrystone worker ownership
 *
 * controller request + wake -> queued worker -> CAS running -> original state
 *         |                       |                    |                 |
 *         |                       +--> cancel          +--> completion ---+
 *         +-------------------- bounded observer -------------------------+
 *
 * Key rule:
 *   - dry.c retains one set of upstream globals and is entered by only one worker;
 *   - only a successful QUEUED-to-RUNNING transition grants dry.c ownership;
 *   - the worker BVT weight matches static VM priority, bounding retained
 *     virtual-time debt between explicit benchmark generations;
 *   - a start timeout cancels QUEUED before execution, so it cannot become
 *     background benchmark work after the command reports;
 *   - a running timeout retains command ownership until completion publishes,
 *     preventing a later command from overlapping upstream global state.
 */
static void dhrystone_worker_main(struct thread_object *thread)
{
	struct dhrystone_worker *worker = &dhrystone_workers[get_pcpu_id()];

	ASSERT(thread == &worker->thread, "Dhrystone worker on wrong pCPU\n");
	while (true) {
		uint64_t request;
		uint64_t started;
		enum dhrystone_worker_state expected;

		wait_event(&worker->request_event);
		request = __atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE);
		if (request != __atomic_load_n(&worker->complete_generation,
			__ATOMIC_RELAXED)) {
			started = cpu_ticks();
			__atomic_store_n(&worker->started_ticks, started, __ATOMIC_RELAXED);
			expected = DHRYSTONE_WORKER_QUEUED;
			if (!__atomic_compare_exchange_n(&worker->state, &expected,
				DHRYSTONE_WORKER_RUNNING, false, __ATOMIC_ACQ_REL,
				__ATOMIC_ACQUIRE)) {
				continue;
			}
			worker->error = 0;
			(void)memset(&worker->result, 0U, sizeof(worker->result));
			worker->error = dhrystone_run(dhrystone_control.initial_runs,
				&worker->result);
			__atomic_store_n(&worker->complete_generation, request,
				__ATOMIC_RELEASE);
			__atomic_store_n(&worker->state,
				worker->error == 0 ? DHRYSTONE_WORKER_DONE : DHRYSTONE_WORKER_ERROR,
				__ATOMIC_RELEASE);
			signal_event(&dhrystone_control.completion_event);
		}
	}
}

static bool dhrystone_dispatch_worker(struct dhrystone_worker *worker,
	uint64_t generation)
{
	if (!dhrystone_worker_available(worker)) {
		return false;
	}
	worker->error = 0;
	(void)memset(&worker->result, 0U, sizeof(worker->result));
	__atomic_store_n(&worker->late, false, __ATOMIC_RELAXED);
	__atomic_store_n(&worker->queued_ticks, cpu_ticks(), __ATOMIC_RELAXED);
	__atomic_store_n(&worker->state, DHRYSTONE_WORKER_QUEUED, __ATOMIC_RELEASE);
	__atomic_store_n(&worker->request_generation, generation, __ATOMIC_RELEASE);
	signal_event(&worker->request_event);
	wake_thread(&worker->thread);
	request_thread_priority(&worker->thread);
	return true;
}

static bool dhrystone_cancel_queued_worker(struct dhrystone_worker *worker,
	uint64_t generation)
{
	enum dhrystone_worker_state expected = DHRYSTONE_WORKER_QUEUED;

	if (!__atomic_compare_exchange_n(&worker->state, &expected,
		DHRYSTONE_WORKER_START_TIMEOUT, false, __ATOMIC_ACQ_REL,
		__ATOMIC_ACQUIRE)) {
		return false;
	}
	__atomic_store_n(&worker->complete_generation, generation, __ATOMIC_RELEASE);
	/* The successful CAS excludes dry.c entry, so this removes deferred work. */
	sleep_thread(&worker->thread);
	return true;
}

static bool dhrystone_wait_worker(struct dhrystone_worker *worker, uint64_t generation)
{
	uint32_t yields = 0U;

	while (true) {
		enum dhrystone_worker_state state = __atomic_load_n(&worker->state,
			__ATOMIC_ACQUIRE);
		uint64_t now = cpu_ticks();

		if (state == DHRYSTONE_WORKER_DONE) {
			return true;
		}
		if ((state == DHRYSTONE_WORKER_ERROR) ||
			(state == DHRYSTONE_WORKER_START_TIMEOUT)) {
			return false;
		}
		if ((state == DHRYSTONE_WORKER_QUEUED) &&
			(now - __atomic_load_n(&worker->queued_ticks, __ATOMIC_ACQUIRE) >=
			us_to_ticks(CONFIG_DHRYSTONE_START_TIMEOUT_MS * 1000U))) {
			if (dhrystone_cancel_queued_worker(worker, generation)) {
				return false;
			}
			continue;
		}
		if ((state == DHRYSTONE_WORKER_RUNNING) &&
			((now - __atomic_load_n(&worker->started_ticks, __ATOMIC_ACQUIRE) >=
			us_to_ticks(CONFIG_DHRYSTONE_RUN_TIMEOUT_MS * 1000U)) ||
			(yields >= DHRYSTONE_MAX_WAIT_YIELDS))) {
			__atomic_store_n(&worker->late, true, __ATOMIC_RELEASE);
			wait_event(&dhrystone_control.completion_event);
			yields = 0U;
			continue;
		}
		yields++;
		yield_current();
		schedule();
	}
}

static void dhrystone_controller_main(__unused struct thread_object *thread)
{
	while (true) {
		uint64_t generation;
		uint64_t flags;
		uint16_t pcpu_id;

		wait_event(&dhrystone_control.request_event);
		spinlock_irqsave_obtain(&dhrystone_control.lock, &flags);
		if (!dhrystone_control.active) {
			spinlock_irqrestore_release(&dhrystone_control.lock, flags);
			continue;
		}
		generation = dhrystone_control.generation;
		spinlock_irqrestore_release(&dhrystone_control.lock, flags);

		for (pcpu_id = 0U; pcpu_id < dhrystone_control.pcpu_num; pcpu_id++) {
			struct dhrystone_worker *worker = &dhrystone_workers[pcpu_id];

			reset_event(&dhrystone_control.completion_event);
			if (dhrystone_dispatch_worker(worker, generation)) {
				(void)dhrystone_wait_worker(worker, generation);
			}
		}
		dhrystone_report(generation);

		spinlock_irqsave_obtain(&dhrystone_control.lock, &flags);
		if (dhrystone_control.generation == generation) {
			dhrystone_control.active = false;
		}
		spinlock_irqrestore_release(&dhrystone_control.lock, flags);
	}
}

void dhrystone_init(void)
{
	struct sched_params params = { 0U };
	uint16_t pcpu_id;

	if (dhrystone_control.initialized) {
		return;
	}
	params.prio = PRIO_LOW;
	params.bvt_weight = DHRYSTONE_BVT_WEIGHT;
	params.bvt_warp_value = DHRYSTONE_BVT_WARP_VALUE;
	params.bvt_warp_limit = DHRYSTONE_BVT_WARP_LIMIT;
	params.bvt_unwarp_period = DHRYSTONE_BVT_UNWARP_PERIOD;
	dhrystone_control.pcpu_num = get_pcpu_nums();
	for (pcpu_id = 0U; pcpu_id < dhrystone_control.pcpu_num; pcpu_id++) {
		struct dhrystone_worker *worker = &dhrystone_workers[pcpu_id];

		worker->state = DHRYSTONE_WORKER_IDLE;
		worker->late = false;
		init_event(&worker->request_event);
		(void)snprintf(worker->thread.name, sizeof(worker->thread.name),
			"dhrystone-%02hu", pcpu_id);
		worker->thread.pcpu_id = pcpu_id;
		worker->thread.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
		worker->thread.thread_entry = dhrystone_worker_main;
		worker->thread.host_sp = arch_setup_thread_stack(&worker->thread,
			worker->stack, CONFIG_STACK_SIZE);
		init_thread_data(&worker->thread, &params);
	}
	init_event(&dhrystone_control.request_event);
	init_event(&dhrystone_control.completion_event);
	(void)strncpy_s(dhrystone_control.controller.name,
		sizeof(dhrystone_control.controller.name), "dhrystone-ctl",
		sizeof(dhrystone_control.controller.name));
	dhrystone_control.controller.pcpu_id = BSP_CPU_ID;
	dhrystone_control.controller.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	dhrystone_control.controller.thread_entry = dhrystone_controller_main;
	dhrystone_control.controller.host_sp = arch_setup_thread_stack(
		&dhrystone_control.controller, dhrystone_control.stack, CONFIG_STACK_SIZE);
	init_thread_data(&dhrystone_control.controller, &params);
	dhrystone_control.initialized = true;
}

int32_t shell_dhrystone(int32_t argc, char **argv)
{
	int64_t parsed;
	uint64_t flags;

	if ((argc < 1) || (argc > 2)) {
		return -EINVAL;
	}
	parsed = argc == 2 ? strtol_deci(argv[1]) : 0L;
	if ((parsed < 0L) || ((uint64_t)parsed > CONFIG_DHRYSTONE_MAX_INITIAL_RUNS)) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&dhrystone_control.lock, &flags);
	if (!dhrystone_control.initialized || dhrystone_control.active) {
		spinlock_irqrestore_release(&dhrystone_control.lock, flags);
		(void)shell_async_puts("dhrystone: busy\r\n");
		return -EBUSY;
	}
	dhrystone_control.generation++;
	if (dhrystone_control.generation == 0UL) {
		dhrystone_control.generation = 1UL;
	}
	dhrystone_control.initial_runs = (uint32_t)parsed;
	dhrystone_control.active = true;
	spinlock_irqrestore_release(&dhrystone_control.lock, flags);
	signal_event(&dhrystone_control.request_event);
	wake_thread(&dhrystone_control.controller);
	return 0;
}
