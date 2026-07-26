/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cpu.h>
#include <debug/shell.h>
#include <errno.h>
#include <event.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>

#include "rt-tests.h"

#define IPILAT_DEFAULT_SAMPLES 100U
#define IPILAT_MAX_SAMPLES 10000U
#define IPILAT_CALL_TIMEOUT_US 10000U
#define IPILAT_LINE_SIZE 176U
#define IPILAT_OUTPUT_SIZE ((MAX_PCPU_NUM * IPILAT_LINE_SIZE) + 512U)

enum ipilat_state {
	IPILAT_LOCAL = 0,
	IPILAT_DONE,
	IPILAT_INACTIVE,
	IPILAT_BUSY,
	IPILAT_TIMEOUT,
	IPILAT_ERROR,
};

struct ipilat_stats {
	uint64_t count;
	uint64_t min_ticks;
	uint64_t sum_ticks;
	uint64_t max_ticks;
};

struct ipilat_target {
	uint16_t pcpu_id;
	volatile uint64_t generation;
	volatile uint64_t sent_ticks;
	volatile uint64_t arrival_ticks;
	volatile bool acknowledged;
	enum ipilat_state state;
	uint32_t busy;
	uint32_t timeout;
	struct ipilat_stats arrival;
	struct ipilat_stats roundtrip;
};

struct ipilat_control {
	spinlock_t lock;
	struct thread_object controller;
	struct sched_event request_event;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	bool initialized;
	bool active;
	uint16_t pcpu_num;
	uint64_t generation;
	uint32_t samples;
};

static struct ipilat_target ipilat_targets[MAX_PCPU_NUM];
static struct ipilat_control ipilat_control = { .lock = { .head = 0U, .tail = 0U } };
static char ipilat_output[IPILAT_OUTPUT_SIZE];

static const char *ipilat_state_name(enum ipilat_state state)
{
	switch (state) {
	case IPILAT_LOCAL:
		return "local";
	case IPILAT_DONE:
		return "done";
	case IPILAT_INACTIVE:
		return "inactive";
	case IPILAT_BUSY:
		return "busy";
	case IPILAT_TIMEOUT:
		return "timeout";
	case IPILAT_ERROR:
		return "error";
	default:
		return "invalid";
	}
}

static void ipilat_stats_add(struct ipilat_stats *stats, uint64_t delta)
{
	if (stats->count == 0UL) {
		stats->min_ticks = delta;
		stats->max_ticks = delta;
	} else {
		if (delta < stats->min_ticks) {
			stats->min_ticks = delta;
		}
		if (delta > stats->max_ticks) {
			stats->max_ticks = delta;
		}
	}
	if (stats->sum_ticks <= (UINT64_MAX - delta)) {
		stats->sum_ticks += delta;
	} else {
		stats->sum_ticks = UINT64_MAX;
	}
	if (stats->count != UINT64_MAX) {
		stats->count++;
	}
}

/* [20260727] Bounded cross-pCPU SGI observation
 *
 * BSP controller -> common SMP-call slot -> target SGI callback -> ack
 *       |                                                   |
 *       +---------- bounded return / busy / timeout --------+
 *
 * Key rule:
 *   - the common SMP-call owner remains authoritative; an occupied slot is
 *     reported as busy and never delayed or retried by this diagnostic;
 *   - target IRQ context only timestamps and release-publishes acknowledgement;
 *   - static target records outlive every callback, including a timed-out call.
 */
static void ipilat_target_callback(void *data)
{
	struct ipilat_target *target = (struct ipilat_target *)data;

	if ((target != NULL) && (get_pcpu_id() == target->pcpu_id) &&
		(__atomic_load_n(&target->generation, __ATOMIC_ACQUIRE) ==
		 ipilat_control.generation)) {
		__atomic_store_n(&target->arrival_ticks, cpu_ticks(), __ATOMIC_RELAXED);
		__atomic_store_n(&target->acknowledged, true, __ATOMIC_RELEASE);
	}
}

static void ipilat_run_target(struct ipilat_target *target, uint64_t generation,
	uint32_t samples)
{
	uint32_t sample;

	target->state = IPILAT_DONE;
	target->busy = 0U;
	target->timeout = 0U;
	target->arrival = (struct ipilat_stats) { 0UL };
	target->roundtrip = (struct ipilat_stats) { 0UL };
	__atomic_store_n(&target->generation, generation, __ATOMIC_RELEASE);
	for (sample = 0U; sample < samples; sample++) {
		int32_t ret;
		uint64_t sent = cpu_ticks();
		uint64_t returned;

		__atomic_store_n(&target->acknowledged, false, __ATOMIC_RELAXED);
		__atomic_store_n(&target->sent_ticks, sent, __ATOMIC_RELEASE);
		ret = smp_try_call_function_timeout(1UL << target->pcpu_id,
			ipilat_target_callback, target, IPILAT_CALL_TIMEOUT_US);
		returned = cpu_ticks();
		if (ret == -EBUSY) {
			target->busy++;
			target->state = IPILAT_BUSY;
			continue;
		}
		if (ret != 0) {
			target->timeout++;
			target->state = IPILAT_TIMEOUT;
			continue;
		}
		if (!__atomic_load_n(&target->acknowledged, __ATOMIC_ACQUIRE)) {
			target->state = IPILAT_INACTIVE;
			continue;
		}
		ipilat_stats_add(&target->arrival,
			__atomic_load_n(&target->arrival_ticks, __ATOMIC_ACQUIRE) - sent);
		ipilat_stats_add(&target->roundtrip, returned - sent);
	}
}

static uint64_t ipilat_average(const struct ipilat_stats *stats)
{
	return stats->count == 0UL ? 0UL : stats->sum_ticks / stats->count;
}

static void ipilat_report(uint64_t generation)
{
	uint16_t pcpu_id;
	size_t offset;

	(void)snprintf(ipilat_output, sizeof(ipilat_output),
		"┌─ IPILAT generation:%lu samples:%u wait-budget.us:%u\r\n", generation,
		ipilat_control.samples, IPILAT_CALL_TIMEOUT_US);
	offset = strnlen_s(ipilat_output, sizeof(ipilat_output));
	(void)snprintf(&ipilat_output[offset], sizeof(ipilat_output) - offset,
		"│ BSP-to-target EL2 SMP-call arrival and round-trip latency\r\n");
	offset = strnlen_s(ipilat_output, sizeof(ipilat_output));
	(void)snprintf(&ipilat_output[offset], sizeof(ipilat_output) - offset,
		"│ %3s %-10s %5s  %-20s       %-20s  %4s %7s\r\n",
		"cpu", "state", "count", "arr.us min/avg/max", "rtt.us min/avg/max", "busy",
		"timeout");
	offset = strnlen_s(ipilat_output, sizeof(ipilat_output));
	(void)snprintf(&ipilat_output[offset], sizeof(ipilat_output) - offset,
		"│ %3s %-10s %5s  %-20s       %-20s  %4s %7s\r\n",
		"───", "──────────", "─────", "────────────────────", "────────────────────",
		"────", "───────");
	offset = strnlen_s(ipilat_output, sizeof(ipilat_output));
	for (pcpu_id = 0U; pcpu_id < ipilat_control.pcpu_num; pcpu_id++) {
		const struct ipilat_target *target = &ipilat_targets[pcpu_id];

		(void)snprintf(&ipilat_output[offset], sizeof(ipilat_output) - offset,
			"│ %3hu %-10s %5lu  %6lu/%6lu/%6lu       %6lu/%6lu/%6lu  %4u %7u\r\n",
			pcpu_id, ipilat_state_name(target->state), target->arrival.count,
			ticks_to_us(target->arrival.min_ticks), ticks_to_us(ipilat_average(&target->arrival)),
			ticks_to_us(target->arrival.max_ticks), ticks_to_us(target->roundtrip.min_ticks),
			ticks_to_us(ipilat_average(&target->roundtrip)), ticks_to_us(target->roundtrip.max_ticks),
			target->busy, target->timeout);
		offset = strnlen_s(ipilat_output, sizeof(ipilat_output));
	}
	if (offset < (sizeof(ipilat_output) - 1U)) {
		(void)snprintf(&ipilat_output[offset], sizeof(ipilat_output) - offset, "└─\r\n");
	}
	(void)shell_async_puts(ipilat_output);
}

static void ipilat_controller_main(__unused struct thread_object *thread)
{
	while (true) {
		uint64_t flags;
		uint64_t generation;
		uint16_t pcpu_id;

		wait_event(&ipilat_control.request_event);
		spinlock_irqsave_obtain(&ipilat_control.lock, &flags);
		if (!ipilat_control.active) {
			spinlock_irqrestore_release(&ipilat_control.lock, flags);
			continue;
		}
		generation = ipilat_control.generation;
		spinlock_irqrestore_release(&ipilat_control.lock, flags);
		for (pcpu_id = 0U; pcpu_id < ipilat_control.pcpu_num; pcpu_id++) {
			struct ipilat_target *target = &ipilat_targets[pcpu_id];

			target->pcpu_id = pcpu_id;
			if (pcpu_id == BSP_CPU_ID) {
				target->state = IPILAT_LOCAL;
				target->arrival = (struct ipilat_stats) { 0UL };
				target->roundtrip = (struct ipilat_stats) { 0UL };
				target->busy = 0U;
				target->timeout = 0U;
			} else if (is_pcpu_active(pcpu_id)) {
				ipilat_run_target(target, generation, ipilat_control.samples);
			} else {
				target->state = IPILAT_INACTIVE;
			}
		}
		ipilat_report(generation);
		spinlock_irqsave_obtain(&ipilat_control.lock, &flags);
		if (ipilat_control.generation == generation) {
			ipilat_control.active = false;
		}
		spinlock_irqrestore_release(&ipilat_control.lock, flags);
		rt_test_release();
	}
}

void ipilat_init(void)
{
	struct sched_params params = { .prio = PRIO_LOW, .bvt_weight = 1U };

	if (ipilat_control.initialized) {
		return;
	}
	ipilat_control.pcpu_num = get_pcpu_nums();
	init_event(&ipilat_control.request_event);
	(void)strncpy_s(ipilat_control.controller.name, sizeof(ipilat_control.controller.name),
		"ipilat-ctl", sizeof(ipilat_control.controller.name));
	ipilat_control.controller.pcpu_id = BSP_CPU_ID;
	ipilat_control.controller.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	ipilat_control.controller.thread_entry = ipilat_controller_main;
	ipilat_control.controller.host_sp = arch_setup_thread_stack(&ipilat_control.controller,
		ipilat_control.stack, CONFIG_STACK_SIZE);
	init_thread_data(&ipilat_control.controller, &params);
	ipilat_control.initialized = true;
}

int32_t shell_ipilat(int32_t argc, char **argv)
{
	int64_t parsed;
	uint64_t flags;

	if ((argc < 1) || (argc > 2)) {
		return -EINVAL;
	}
	parsed = argc == 2 ? strtol_deci(argv[1]) : (int64_t)IPILAT_DEFAULT_SAMPLES;
	if ((parsed <= 0L) || ((uint64_t)parsed > IPILAT_MAX_SAMPLES)) {
		return -EINVAL;
	}
	if (!rt_test_try_acquire()) {
		return -EBUSY;
	}
	spinlock_irqsave_obtain(&ipilat_control.lock, &flags);
	if (!ipilat_control.initialized || ipilat_control.active) {
		spinlock_irqrestore_release(&ipilat_control.lock, flags);
		rt_test_release();
		return -EBUSY;
	}
	ipilat_control.generation++;
	if (ipilat_control.generation == 0UL) {
		ipilat_control.generation = 1UL;
	}
	ipilat_control.samples = (uint32_t)parsed;
	ipilat_control.active = true;
	spinlock_irqrestore_release(&ipilat_control.lock, flags);
	signal_event(&ipilat_control.request_event);
	wake_thread(&ipilat_control.controller);
	return 0;
}
