/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cpu.h>
#include <debug/shell.h>
#include <errno.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>

#include "coremark_engine.h"

#define COREMARK_LINE_SIZE 176U
#define COREMARK_OUTPUT_SIZE ((COREMARK_CONTEXTS * COREMARK_LINE_SIZE) + 640U)
#define COREMARK_BVT_WEIGHT 1U
#define COREMARK_BVT_WARP_VALUE 8
#define COREMARK_BVT_WARP_LIMIT 1U
#define COREMARK_BVT_UNWARP_PERIOD 4U
#define COREMARK_MAX_WAIT_YIELDS 1000000U

enum coremark_worker_state {
	COREMARK_WORKER_IDLE = 0,
	COREMARK_WORKER_QUEUED,
	COREMARK_WORKER_RUNNING,
	COREMARK_WORKER_DONE,
	COREMARK_WORKER_START_TIMEOUT,
	COREMARK_WORKER_RUN_TIMEOUT,
	COREMARK_WORKER_ERROR,
};

struct coremark_worker {
	struct thread_object thread;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	uint16_t pcpu_id;
	volatile uint64_t request_generation;
	volatile uint64_t complete_generation;
	volatile uint64_t started_ticks;
	volatile uint64_t elapsed_ticks;
	volatile enum coremark_worker_state state;
	int32_t error;
	struct coremark_engine_context context;
};

struct coremark_control {
	spinlock_t lock;
	struct thread_object controller;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	bool initialized;
	bool active;
	uint16_t pcpu_num;
	uint64_t generation;
	uint32_t iterations;
};

static struct coremark_worker coremark_workers[COREMARK_CONTEXTS];
static struct coremark_control coremark_control = { .lock = { .head = 0U, .tail = 0U } };
static struct coremark_engine_context coremark_calibration;
static char coremark_output[COREMARK_OUTPUT_SIZE];

static const char *coremark_worker_state_name(enum coremark_worker_state state)
{
	switch (state) {
	case COREMARK_WORKER_IDLE:
		return "idle";
	case COREMARK_WORKER_QUEUED:
		return "queued";
	case COREMARK_WORKER_RUNNING:
		return "running";
	case COREMARK_WORKER_DONE:
		return "done";
	case COREMARK_WORKER_START_TIMEOUT:
		return "unavailable";
	case COREMARK_WORKER_RUN_TIMEOUT:
		return "late";
	case COREMARK_WORKER_ERROR:
		return "error";
	default:
		return "invalid";
	}
}

static bool coremark_worker_is_available(const struct coremark_worker *worker)
{
	enum coremark_worker_state state = __atomic_load_n(&worker->state, __ATOMIC_ACQUIRE);

	return ((state == COREMARK_WORKER_IDLE) || (state == COREMARK_WORKER_DONE) ||
		(state == COREMARK_WORKER_ERROR)) &&
		(__atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE) ==
		 __atomic_load_n(&worker->complete_generation, __ATOMIC_ACQUIRE));
}

static bool coremark_context_valid(const struct coremark_worker *worker)
{
	const core_results *result = &worker->context.result;

	return (result->crclist == 0xe714U) && (result->crcmatrix == 0x1fd7U) &&
		(result->crcstate == 0x8e3aU);
}

static void coremark_report(uint64_t generation)
{
	uint64_t total_ticks = 0UL;
	uint64_t total_iterations = 0UL;
	uint64_t rate_milli = 0UL;
	uint16_t completed = 0U;
	uint16_t requested = 0U;
	uint16_t pcpu_id;
	size_t offset;
	bool valid = true;
	bool duration_valid;

	(void)snprintf(coremark_output, sizeof(coremark_output),
		"┌─ COREMARK generation:%lu iterations:%s\r\n", generation,
		coremark_control.iterations == 0U ? "auto" : "fixed");
	offset = strnlen_s(coremark_output, sizeof(coremark_output));
	(void)snprintf(&coremark_output[offset], sizeof(coremark_output) - offset,
		"│ EL2 diagnostic; partial generations are not benchmark scores\r\n");
	offset = strnlen_s(coremark_output, sizeof(coremark_output));
	(void)snprintf(&coremark_output[offset], sizeof(coremark_output) - offset,
		"│ cpu state        elapsed.us iterations  list matrix state final status\r\n");
	offset = strnlen_s(coremark_output, sizeof(coremark_output));
	for (pcpu_id = 0U; pcpu_id < coremark_control.pcpu_num; pcpu_id++) {
		const struct coremark_worker *worker = &coremark_workers[pcpu_id];
		enum coremark_worker_state state = __atomic_load_n(&worker->state,
			__ATOMIC_ACQUIRE);
		uint64_t worker_generation = __atomic_load_n(&worker->request_generation,
			__ATOMIC_ACQUIRE);
		const core_results *result = &worker->context.result;
		const char *status = "unavailable";
		uint64_t elapsed_ticks = 0UL;
		uint32_t iterations = 0U;
		ee_u16 crclist = 0U;
		ee_u16 crcmatrix = 0U;
		ee_u16 crcstate = 0U;
		ee_u16 crcfinal = 0U;

		if (worker_generation == generation) {
			requested++;
			status = coremark_worker_state_name(state);
			elapsed_ticks = worker->elapsed_ticks;
			iterations = result->iterations;
			crclist = result->crclist;
			crcmatrix = result->crcmatrix;
			crcstate = result->crcstate;
			crcfinal = result->crc;
			if (state == COREMARK_WORKER_DONE) {
				completed++;
				total_iterations += result->iterations;
				if (worker->elapsed_ticks > total_ticks) {
					total_ticks = worker->elapsed_ticks;
				}
				if (!coremark_context_valid(worker)) {
					valid = false;
				}
			} else {
				valid = false;
			}
		}
		(void)snprintf(&coremark_output[offset], sizeof(coremark_output) - offset,
			"│ %3hu %-12s %10lu %10u  %04x %04x  %04x  %04x  %s\r\n",
			pcpu_id, status, ticks_to_us(elapsed_ticks), iterations, crclist,
			crcmatrix, crcstate, crcfinal,
			(state == COREMARK_WORKER_DONE) && coremark_context_valid(worker) ?
			"pass" : "fail");
		offset = strnlen_s(coremark_output, sizeof(coremark_output));
	}
	if ((requested != coremark_control.pcpu_num) || (completed != requested)) {
		valid = false;
	}
	if (total_ticks != 0UL) {
		rate_milli = (total_iterations * 1000000000UL) /
			ticks_to_us(total_ticks);
	}
	duration_valid = (completed == requested) && (requested == coremark_control.pcpu_num) &&
		(ticks_to_us(total_ticks) >= 10000000UL);
	(void)snprintf(&coremark_output[offset], sizeof(coremark_output) - offset,
		"│ complete:%hu/%hu selected:%hu/%hu elapsed.us:%lu rate:%lu.%03lu/s crc:%s duration:%s\r\n",
		completed, requested, requested, coremark_control.pcpu_num,
		ticks_to_us(total_ticks), rate_milli / 1000UL,
		rate_milli % 1000UL, valid ? "pass" : "fail",
		duration_valid ? "reportable" : "diagnostic");
	offset = strnlen_s(coremark_output, sizeof(coremark_output));
	if (offset < (sizeof(coremark_output) - 1U)) {
		(void)snprintf(&coremark_output[offset], sizeof(coremark_output) - offset,
			"└─\r\n");
	}
	(void)shell_async_puts(coremark_output);
}

/* [20260726] Bounded pCPU CoreMark ownership
 *
 * controller -> persistent worker context -> request generation -> pinned worker
 *      |                                                        |
 *      +--> bounded observation <--------- completion ----------+
 *              |
 *              +--> retain late context; report partial result
 *
 * Key rule:
 *   - a worker owns its result and data memory for its entire generation;
 *   - timeout changes report eligibility, never releases a worker context;
 *   - a later command skips a late worker until it publishes completion, preventing
 *     stack lifetime violations and an indefinitely busy shell command.
 */
static void coremark_worker_main(struct thread_object *thread)
{
	struct coremark_worker *worker = &coremark_workers[get_pcpu_id()];

	ASSERT(thread == &worker->thread, "CoreMark worker on wrong pCPU\n");
	while (true) {
		uint64_t request = __atomic_load_n(&worker->request_generation,
			__ATOMIC_ACQUIRE);

		if (request != __atomic_load_n(&worker->complete_generation,
			__ATOMIC_RELAXED)) {
			uint64_t started = cpu_ticks();

			__atomic_store_n(&worker->started_ticks, started, __ATOMIC_RELEASE);
			__atomic_store_n(&worker->state, COREMARK_WORKER_RUNNING,
				__ATOMIC_RELEASE);
			(void)iterate(&worker->context.result);
			worker->elapsed_ticks = cpu_ticks() - started;
			__atomic_store_n(&worker->complete_generation, request,
				__ATOMIC_RELEASE);
			__atomic_store_n(&worker->state, COREMARK_WORKER_DONE,
				__ATOMIC_RELEASE);
		}
		sleep_thread(thread);
		if (__atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE) !=
			__atomic_load_n(&worker->complete_generation, __ATOMIC_RELAXED)) {
			wake_thread(thread);
		}
		schedule();
	}
}

static bool coremark_dispatch_worker(struct coremark_worker *worker, uint64_t generation,
	uint32_t iterations)
{
	int32_t status;

	if (!coremark_worker_is_available(worker)) {
		return false;
	}
	status = coremark_engine_prepare(&worker->context, iterations);
	if (status < 0) {
		worker->error = status;
		__atomic_store_n(&worker->state, COREMARK_WORKER_ERROR, __ATOMIC_RELEASE);
		return false;
	}
	worker->error = 0;
	worker->elapsed_ticks = 0UL;
	__atomic_store_n(&worker->started_ticks, cpu_ticks(), __ATOMIC_RELAXED);
	__atomic_store_n(&worker->state, COREMARK_WORKER_QUEUED, __ATOMIC_RELEASE);
	__atomic_store_n(&worker->request_generation, generation, __ATOMIC_RELEASE);
	request_thread_priority(&worker->thread);
	wake_thread(&worker->thread);
	return true;
}

static bool coremark_generation_pending(uint64_t generation, uint64_t now)
{
	uint16_t pcpu_id;
	bool pending = false;

	for (pcpu_id = 0U; pcpu_id < coremark_control.pcpu_num; pcpu_id++) {
		struct coremark_worker *worker = &coremark_workers[pcpu_id];
		enum coremark_worker_state state = __atomic_load_n(&worker->state,
			__ATOMIC_ACQUIRE);

		if (__atomic_load_n(&worker->request_generation, __ATOMIC_ACQUIRE) != generation) {
			continue;
		}
		if (state == COREMARK_WORKER_QUEUED) {
			if (now - __atomic_load_n(&worker->started_ticks, __ATOMIC_ACQUIRE) >=
				us_to_ticks(CONFIG_COREMARK_START_TIMEOUT_MS * 1000U)) {
				__atomic_store_n(&worker->state, COREMARK_WORKER_START_TIMEOUT,
					__ATOMIC_RELEASE);
			} else {
				pending = true;
			}
		} else if (state == COREMARK_WORKER_RUNNING) {
			if (now - __atomic_load_n(&worker->started_ticks, __ATOMIC_ACQUIRE) >=
				us_to_ticks(CONFIG_COREMARK_RUN_TIMEOUT_MS * 1000U)) {
				__atomic_store_n(&worker->state, COREMARK_WORKER_RUN_TIMEOUT,
					__ATOMIC_RELEASE);
			} else {
				pending = true;
			}
		}
	}
	return pending;
}

static void coremark_wait_generation(uint64_t generation)
{
	uint32_t yields = 0U;

	while (coremark_generation_pending(generation, cpu_ticks()) &&
		(yields < COREMARK_MAX_WAIT_YIELDS)) {
		yields++;
		yield_current();
		schedule();
	}
	if (yields == COREMARK_MAX_WAIT_YIELDS) {
		(void)coremark_generation_pending(generation, UINT64_MAX);
	}
}

/* Kept only for the unmodified upstream core_main.c object. BEAU executes its
 * worker-owned engine instead, so core_main's stack-local multithread bridge is
 * never entered. */
ee_u8 core_start_parallel(__unused core_results *result)
{
	return 1U;
}

ee_u8 core_stop_parallel(__unused core_results *result)
{
	return 1U;
}

static void coremark_controller_main(struct thread_object *thread)
{
	while (true) {
		uint64_t generation;
		uint32_t iterations;
		uint64_t flags;
		uint16_t pcpu_id;

		spinlock_irqsave_obtain(&coremark_control.lock, &flags);
		if (!coremark_control.active) {
			spinlock_irqrestore_release(&coremark_control.lock, flags);
			sleep_thread(thread);
			schedule();
			continue;
		}
		generation = coremark_control.generation;
		iterations = coremark_control.iterations;
		spinlock_irqrestore_release(&coremark_control.lock, flags);

		if ((iterations == 0U) &&
			(coremark_engine_calibrate(&coremark_calibration, &iterations) < 0)) {
			iterations = 0U;
		}
		for (pcpu_id = 0U; pcpu_id < coremark_control.pcpu_num; pcpu_id++) {
			(void)coremark_dispatch_worker(&coremark_workers[pcpu_id], generation,
				iterations);
		}
		coremark_wait_generation(generation);
		coremark_report(generation);

		spinlock_irqsave_obtain(&coremark_control.lock, &flags);
		if (coremark_control.generation == generation) {
			coremark_control.active = false;
		}
		spinlock_irqrestore_release(&coremark_control.lock, flags);
	}
}

void coremark_init(void)
{
	struct sched_params params = { 0U };
	uint16_t pcpu_id;

	if (coremark_control.initialized) {
		return;
	}
	params.prio = PRIO_LOW;
	params.bvt_weight = COREMARK_BVT_WEIGHT;
	params.bvt_warp_value = COREMARK_BVT_WARP_VALUE;
	params.bvt_warp_limit = COREMARK_BVT_WARP_LIMIT;
	params.bvt_unwarp_period = COREMARK_BVT_UNWARP_PERIOD;
	coremark_control.pcpu_num = get_pcpu_nums();
	for (pcpu_id = 0U; pcpu_id < coremark_control.pcpu_num; pcpu_id++) {
		struct coremark_worker *worker = &coremark_workers[pcpu_id];

		worker->pcpu_id = pcpu_id;
		worker->state = COREMARK_WORKER_IDLE;
		(void)snprintf(worker->thread.name, sizeof(worker->thread.name),
			"coremark-%02hu", pcpu_id);
		worker->thread.pcpu_id = pcpu_id;
		worker->thread.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
		worker->thread.thread_entry = coremark_worker_main;
		worker->thread.host_sp = arch_setup_thread_stack(&worker->thread,
			worker->stack, CONFIG_STACK_SIZE);
		init_thread_data(&worker->thread, &params);
	}
	(void)strncpy_s(coremark_control.controller.name,
		sizeof(coremark_control.controller.name), "coremark-ctl",
		sizeof(coremark_control.controller.name));
	coremark_control.controller.pcpu_id = BSP_CPU_ID;
	coremark_control.controller.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	coremark_control.controller.thread_entry = coremark_controller_main;
	coremark_control.controller.host_sp = arch_setup_thread_stack(
		&coremark_control.controller, coremark_control.stack, CONFIG_STACK_SIZE);
	init_thread_data(&coremark_control.controller, &params);
	coremark_control.initialized = true;
}

int32_t shell_coremark(int32_t argc, char **argv)
{
	int64_t parsed;
	uint64_t flags;

	if ((argc < 1) || (argc > 2)) {
		return -EINVAL;
	}
	parsed = argc == 2 ? strtol_deci(argv[1]) : 0L;
	if ((parsed < 0L) || ((uint64_t)parsed > CONFIG_COREMARK_MAX_ITERATIONS)) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&coremark_control.lock, &flags);
	if (!coremark_control.initialized || coremark_control.active) {
		spinlock_irqrestore_release(&coremark_control.lock, flags);
		(void)shell_async_puts("coremark: busy\r\n");
		return -EBUSY;
	}
	coremark_control.generation++;
	if (coremark_control.generation == 0UL) {
		coremark_control.generation = 1UL;
	}
	coremark_control.iterations = (uint32_t)parsed;
	coremark_control.active = true;
	spinlock_irqrestore_release(&coremark_control.lock, flags);
	wake_thread(&coremark_control.controller);
	return 0;
}
