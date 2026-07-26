/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <debug/shell.h>
#include <errno.h>
#include <per_cpu.h>
#include <schedule.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <timer.h>
#include <util.h>

#include "rt-tests.h"

#define RTTEST_INTERVAL_US 1000U
#define RTTEST_SAMPLE_COUNT 1000U
#define RTTEST_CBS_PERIOD_US 10000U
#define RTTEST_CBS_BUDGET_US 500U
#define RTTEST_LINE_SIZE 160U
#define RTTEST_OUTPUT_SIZE (((MAX_PCPU_NUM + 1U) * RTTEST_LINE_SIZE) + 1U)

struct rttest_cpu_context {
	struct hv_timer timer;
	struct thread_object thread;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	uint16_t pcpu_id;
	volatile bool pending;
	volatile bool wait_ready;
	volatile bool complete;
	volatile bool failed;
	uint32_t count;
	uint64_t min_ticks;
	uint64_t act_ticks;
	uint64_t sum_ticks;
	uint64_t max_ticks;
};

struct rttest_run_context {
	spinlock_t lock;
	bool initialized;
	bool running;
	uint16_t pcpu_num;
	uint32_t completed;
};

static struct rttest_cpu_context rttest_cpus[MAX_PCPU_NUM];
static struct rttest_run_context rttest_run = { .lock = { .head = 0U, .tail = 0U } };
static char rttest_output[RTTEST_OUTPUT_SIZE];
static spinlock_t rt_test_lock = { .head = 0U, .tail = 0U };
static bool rt_test_active;

bool rt_test_try_acquire(void)
{
	uint64_t flags;
	bool acquired = false;

	spinlock_irqsave_obtain(&rt_test_lock, &flags);
	if (!rt_test_active) {
		rt_test_active = true;
		acquired = true;
	}
	spinlock_irqrestore_release(&rt_test_lock, flags);
	return acquired;
}

void rt_test_release(void)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&rt_test_lock, &flags);
	rt_test_active = false;
	spinlock_irqrestore_release(&rt_test_lock, flags);
}

/*
 * rttest measures each pCPU's local EL2 CNTHP deadline to SOFTIRQ_TIMER
 * callback path while that CPU retains its configured partition scheduler.
 * It does not migrate vCPUs, change BVT/CBS policy, or measure a Linux
 * user-thread wakeup. The rt-tests-shaped summary fields are:
 * T = test index, (...) = pCPU, P = EL2 priority placeholder, I = interval in
 * microseconds, C = completed samples, and Min/Act/Avg/Max = minimum, last,
 * mean, and worst positive lateness in microseconds.
 *
 * C must reach RTTEST_SAMPLE_COUNT for a valid result. Lower Avg and Max
 * are better; a run passes only when Max is within the platform workload's
 * latency budget. The budget is product-specific and is not hard-coded here.
 */

static void rttest_append_result(const struct rttest_cpu_context *ctx, size_t *offset)
{
	uint64_t min_ticks = ctx->min_ticks == UINT64_MAX ? 0UL : ctx->min_ticks;
	uint64_t avg_ticks = ctx->count == 0U ? 0UL : ctx->sum_ticks / ctx->count;
	size_t remaining = RTTEST_OUTPUT_SIZE - *offset;

	if (remaining > 1U) {
		(void)snprintf(&rttest_output[*offset], remaining,
			"│   T:%2hu (%5hu) P:%2u I:%4u C:%7u Min:%7lu Act:%7lu Avg:%7lu Max:%7lu\r\n",
			ctx->pcpu_id, ctx->pcpu_id, 0U, RTTEST_INTERVAL_US, ctx->count,
			ticks_to_us(min_ticks), ticks_to_us(ctx->act_ticks), ticks_to_us(avg_ticks),
			ticks_to_us(ctx->max_ticks));
		*offset += strnlen_s(&rttest_output[*offset], remaining);
	}
}

static void rttest_print_results(void)
{
	size_t offset;
	size_t remaining;
	uint16_t pcpu_id;

	(void)snprintf(rttest_output, sizeof(rttest_output), "┌─  RTTEST\r\n");
	offset = strnlen_s(rttest_output, sizeof(rttest_output));
	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		rttest_append_result(&rttest_cpus[pcpu_id], &offset);
	}
	remaining = RTTEST_OUTPUT_SIZE - offset;
	if (remaining > 1U) {
		(void)snprintf(&rttest_output[offset], remaining, "└─\r\n");
	}
	/* Publish one complete item so asynchronous completion cannot split the prompt. */
	(void)shell_async_puts(rttest_output);
}

static void rttest_complete(struct rttest_cpu_context *ctx)
{
	uint64_t rflags;
	bool print_results = false;

	spinlock_irqsave_obtain(&rttest_run.lock, &rflags);
	if (!ctx->complete) {
		ctx->complete = true;
		rttest_run.completed++;
		print_results = rttest_run.completed == rttest_run.pcpu_num;
	}
	spinlock_irqrestore_release(&rttest_run.lock, rflags);

	if (print_results) {
		rttest_print_results();
		spinlock_irqsave_obtain(&rttest_run.lock, &rflags);
		rttest_run.running = false;
		spinlock_irqrestore_release(&rttest_run.lock, rflags);
		rt_test_release();
	}
}

static void rttest_timer(void *data)
{
	struct rttest_cpu_context *ctx = (struct rttest_cpu_context *)data;
	uint64_t now = cpu_ticks();
	uint64_t deadline = ctx->timer.timeout;
	uint64_t latency = now > deadline ? now - deadline : 0UL;

	if (latency < ctx->min_ticks) {
		ctx->min_ticks = latency;
	}
	if (latency > ctx->max_ticks) {
		ctx->max_ticks = latency;
	}
	ctx->act_ticks = latency;
	if (ctx->sum_ticks <= (UINT64_MAX - latency)) {
		ctx->sum_ticks += latency;
	} else {
		ctx->sum_ticks = UINT64_MAX;
	}
	ctx->count++;

	if (ctx->count == RTTEST_SAMPLE_COUNT) {
		/* timer_softirq() sees this after the callback and does not reinsert it. */
		ctx->timer.mode = TICK_MODE_ONESHOT;
		rttest_complete(ctx);
		if (ctx->wait_ready) {
			wake_thread(&ctx->thread);
		}
	}
}

static bool rttest_start_local(struct rttest_cpu_context *ctx)
{
	uint64_t now = cpu_ticks();
	int32_t ret;

	initialize_timer(&ctx->timer, rttest_timer, ctx,
		now + us_to_ticks(RTTEST_INTERVAL_US), us_to_ticks(RTTEST_INTERVAL_US));
	ret = add_timer(&ctx->timer);
	if (ret != 0) {
		ctx->failed = true;
		rttest_complete(ctx);
	}

	return ret == 0;
}

static void rttest_worker(struct thread_object *thread)
{
	struct rttest_cpu_context *ctx = &rttest_cpus[get_pcpu_id()];

	ASSERT(thread == &ctx->thread, "rttest worker on wrong pCPU\n");
	while (true) {
		if (ctx->pending) {
			ctx->pending = false;
			if (rttest_start_local(ctx)) {
				sleep_thread(thread);
				ctx->wait_ready = true;
				if (ctx->complete) {
					wake_thread(thread);
				}
				schedule();
			}
		}
		sleep_thread(thread);
		schedule();
	}
}

void arm64_rttest_init(void)
{
	struct sched_params params = {
		.prio = PRIO_LOW,
		.bvt_weight = 1U,
		.cbs_period_us = RTTEST_CBS_PERIOD_US,
		.cbs_budget_us = RTTEST_CBS_BUDGET_US,
	};
	uint16_t pcpu_id;

	if (rttest_run.initialized) {
		return;
	}

	rttest_run.pcpu_num = get_pcpu_nums();
	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		struct rttest_cpu_context *ctx = &rttest_cpus[pcpu_id];

		ctx->pcpu_id = pcpu_id;
		(void)snprintf(ctx->thread.name, sizeof(ctx->thread.name), "rttest-%02hu", pcpu_id);
		ctx->thread.pcpu_id = pcpu_id;
		ctx->thread.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
		ctx->thread.thread_entry = rttest_worker;
		ctx->thread.switch_out = NULL;
		ctx->thread.switch_in = NULL;
		ctx->thread.host_sp = arch_setup_thread_stack(&ctx->thread, ctx->stack,
			CONFIG_STACK_SIZE);
		init_thread_data(&ctx->thread, &params);
	}
	rttest_run.initialized = true;
}

int32_t shell_rttest(int32_t argc, __unused char **argv)
{
	uint64_t rflags;
	uint16_t pcpu_id;

	if (argc != 1) {
		return -EINVAL;
	}
	if (!rt_test_try_acquire()) {
		return -EBUSY;
	}

	spinlock_irqsave_obtain(&rttest_run.lock, &rflags);
	if (!rttest_run.initialized || rttest_run.running) {
		spinlock_irqrestore_release(&rttest_run.lock, rflags);
		rt_test_release();
		return -EBUSY;
	}
	rttest_run.running = true;
	rttest_run.completed = 0U;
	spinlock_irqrestore_release(&rttest_run.lock, rflags);

	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		struct rttest_cpu_context *ctx = &rttest_cpus[pcpu_id];

		ctx->pending = true;
		ctx->wait_ready = false;
		ctx->complete = false;
		ctx->failed = false;
		ctx->count = 0U;
		ctx->min_ticks = UINT64_MAX;
		ctx->act_ticks = 0UL;
		ctx->sum_ticks = 0UL;
		ctx->max_ticks = 0UL;
	}
	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		wake_thread(&rttest_cpus[pcpu_id].thread);
	}

	/* Console input may execute from a timer softirq, so completion is asynchronous. */
	return 0;
}
