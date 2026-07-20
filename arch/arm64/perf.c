/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <cpu.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <timer.h>
#include <ticks.h>
#include <vcpu.h>
#include <vm.h>
#include <asm/boot/ld_sym.h>
#include <asm/irq.h>
#include <asm/notify.h>
#include <asm/perf.h>

#define ARM64_PERF_MIN_FREQUENCY_HZ	1U
#define ARM64_PERF_MAX_FREQUENCY_HZ	1000U
#define ARM64_PERF_MAX_DURATION_MS	60000U
#define ARM64_PERF_USEC_PER_SEC		1000000U
#define ARM64_PERF_USEC_PER_MSEC		1000U
#define ARM64_PERF_STOP_TIMEOUT_US	1000U

struct arm64_perf_stack {
	uint64_t start;
	uint64_t end;
	const struct thread_object *thread;
	bool valid;
};

struct arm64_perf_frame_record {
	uint64_t previous_fp;
	uint64_t lr;
};

struct arm64_perf_cpu_ring {
	struct arm64_perf_sample records[ARM64_PERF_RECORDS_PER_CPU];
	volatile uint64_t pending_epoch;
	volatile uint64_t pending_generation;
	volatile uint64_t due_ticks;
	volatile bool writer_active;
	uint64_t attempts;
	uint64_t captured;
	uint64_t no_stack;
	uint64_t missed;
	uint64_t overwritten;
	uint32_t head;
	uint32_t count;
} __aligned(64);

struct arm64_perf_context {
	struct hv_timer timer;
	uint64_t stop_ticks;
	volatile uint64_t epoch;
	volatile uint64_t generation;
	uint32_t duration_ms;
	uint32_t frequency_hz;
	uint32_t period_us;
	uint16_t controller_pcpu;
	uint16_t pcpu_num;
	volatile bool running;
	bool timer_started;
};

static struct arm64_perf_cpu_ring arm64_perf_rings[MAX_PCPU_NUM];
static struct arm64_perf_context arm64_perf_ctx;

/* [20260721] Bounded EL2 sampling ownership
 *
 * controller timer IRQ -> sample saved controller context
 *          |
 *          v
 * timer callback -> publish per-pCPU generation -> SGI kick
 *                                                    |
 *                                                    v
 * remote IRQ entry -> local fixed ring -> stopped shell dump
 *
 * Key rule:
 *   - the interrupted pCPU is the only writer of its ring;
 *   - a release-published generation precedes the SGI that consumes it;
 *   - Host frame walking stays inside the stack containing the saved SP;
 *   - guest-origin samples record ownership but never dereference guest state.
 */

static bool arm64_perf_range_contains(uint64_t start, uint64_t end,
	uint64_t address, uint64_t size)
{
	return (start < end) && (address >= start) && (address <= end) &&
		(size <= (end - address));
}

static bool arm64_perf_hv_range_valid(uint64_t address, uint64_t size)
{
	return arm64_perf_range_contains((uint64_t)&ld_ram_start,
		(uint64_t)&ld_ram_end, address, size);
}

static bool arm64_perf_text_address(uint64_t address)
{
	return (address >= (uint64_t)&_text_start) &&
		(address < (uint64_t)&_text_end);
}

static bool arm64_perf_stack_candidate(struct arm64_perf_stack *stack,
	uint64_t start, uint64_t size, uint64_t sp,
	const struct thread_object *thread)
{
	uint64_t end;

	if ((stack == NULL) || (size == 0UL) || (start > (UINT64_MAX - size))) {
		return false;
	}
	end = start + size;
	if (((start & (CPU_STACK_ALIGN - 1UL)) != 0UL) ||
		((end & (CPU_STACK_ALIGN - 1UL)) != 0UL) ||
		!arm64_perf_hv_range_valid(start, size) ||
		!arm64_perf_range_contains(start, end, sp, sizeof(uint64_t))) {
		return false;
	}

	stack->start = start;
	stack->end = end;
	stack->thread = thread;
	stack->valid = true;
	return true;
}

static void arm64_perf_resolve_stack(uint16_t pcpu_id, uint64_t sp,
	struct arm64_perf_stack *stack)
{
	struct thread_object *thread;

	(void)memset(stack, 0U, sizeof(*stack));
	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}

	thread = sched_get_current(pcpu_id);
	if ((thread != NULL) &&
		arm64_perf_hv_range_valid((uint64_t)thread, sizeof(*thread)) &&
		arm64_perf_stack_candidate(stack, thread->host_stack_base,
			thread->host_stack_size, sp, thread)) {
		return;
	}
	if (arm64_perf_stack_candidate(stack,
		(uint64_t)&per_cpu(stack, pcpu_id)[0], CONFIG_STACK_SIZE, sp, NULL)) {
		return;
	}
	(void)arm64_perf_stack_candidate(stack, (uint64_t)&_boot_stack_start,
		(uint64_t)&_boot_stack_end - (uint64_t)&_boot_stack_start,
		sp, NULL);
}

static void arm64_perf_append_frame(struct arm64_perf_sample *sample, uint64_t pc)
{
	if ((sample->frame_count < ARM64_PERF_MAX_FRAMES) &&
		arm64_perf_text_address(pc) &&
		((sample->frame_count == 0U) ||
		 (sample->frames[sample->frame_count - 1U] != pc))) {
		sample->frames[sample->frame_count++] = pc;
	}
}

static void arm64_perf_capture_host(const struct intr_excp_ctx *ctx,
	struct arm64_perf_sample *sample)
{
	struct arm64_perf_stack stack;
	uint64_t fp = ctx->regs.x29;
	uint32_t records = 0U;

	arm64_perf_append_frame(sample, ctx->regs.elr);
	arm64_perf_resolve_stack(sample->pcpu_id, ctx->regs.sp, &stack);
	if (!stack.valid) {
		sample->unwind_stop = ARM64_PERF_UNWIND_NO_STACK;
		return;
	}
	if (stack.thread != NULL) {
		(void)strncpy_s(sample->thread_name, sizeof(sample->thread_name),
			stack.thread->name, sizeof(sample->thread_name) - 1U);
	}
	arm64_perf_append_frame(sample, ctx->regs.lr);

	while ((sample->frame_count < ARM64_PERF_MAX_FRAMES) &&
		(records < ARM64_PERF_MAX_FRAMES)) {
		const struct arm64_perf_frame_record *frame;
		uint64_t next_fp;

		if ((fp == 0UL) || (fp == SP_BOTTOM_MAGIC)) {
			sample->unwind_stop = ARM64_PERF_UNWIND_COMPLETE;
			return;
		}
		if ((fp & (CPU_STACK_ALIGN - 1UL)) != 0UL) {
			sample->unwind_stop = ARM64_PERF_UNWIND_FP_MISALIGNED;
			return;
		}
		if ((fp < ctx->regs.sp) ||
			!arm64_perf_range_contains(stack.start, stack.end, fp,
				sizeof(*frame))) {
			sample->unwind_stop = ARM64_PERF_UNWIND_FP_OUTSIDE;
			return;
		}

		frame = (const struct arm64_perf_frame_record *)fp;
		next_fp = frame->previous_fp;
		records++;
		if ((frame->lr != 0UL) && !arm64_perf_text_address(frame->lr)) {
			sample->unwind_stop = ARM64_PERF_UNWIND_LR_OUTSIDE;
			return;
		}
		if (frame->lr != 0UL) {
			arm64_perf_append_frame(sample, frame->lr);
		}
		if ((next_fp == 0UL) || (next_fp == SP_BOTTOM_MAGIC)) {
			sample->unwind_stop = ARM64_PERF_UNWIND_COMPLETE;
			return;
		}
		if (next_fp <= fp) {
			sample->unwind_stop = ARM64_PERF_UNWIND_FP_ORDER;
			return;
		}
		fp = next_fp;
	}

	sample->unwind_stop = ARM64_PERF_UNWIND_DEPTH;
}

static void arm64_perf_capture_guest(struct arm64_perf_sample *sample)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(sample->pcpu_id);

	sample->owner = ARM64_PERF_OWNER_GUEST;
	sample->vm_id = ARM64_PERF_INVALID_ID;
	sample->vcpu_id = ARM64_PERF_INVALID_ID;
	if ((vcpu != NULL) && (vcpu->vm != NULL)) {
		sample->vm_id = vcpu->vm->vm_id;
		sample->vcpu_id = vcpu->vcpu_id;
	}
}

static void arm64_perf_publish_sample(struct arm64_perf_cpu_ring *ring,
	const struct arm64_perf_sample *sample)
{
	uint32_t head;
	uint32_t count;
	uint32_t next;

	head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
	count = __atomic_load_n(&ring->count, __ATOMIC_RELAXED);
	ring->records[head] = *sample;
	next = head + 1U;
	if (next >= ARM64_PERF_RECORDS_PER_CPU) {
		next = 0U;
	}
	cpu_write_memory_barrier();
	__atomic_store_n(&ring->head, next, __ATOMIC_RELEASE);
	if (count < ARM64_PERF_RECORDS_PER_CPU) {
		__atomic_store_n(&ring->count, count + 1U, __ATOMIC_RELEASE);
	} else {
		(void)__atomic_add_fetch(&ring->overwritten, 1UL, __ATOMIC_RELAXED);
	}
	(void)__atomic_add_fetch(&ring->captured, 1UL, __ATOMIC_RELAXED);
}

void arm64_perf_sample_irq(const struct intr_excp_ctx *ctx, bool guest_context)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_perf_cpu_ring *ring;
	struct arm64_perf_sample sample;
	uint64_t epoch;
	uint64_t generation;
	uint64_t now;

	if ((ctx == NULL) || (pcpu_id >= arm64_perf_ctx.pcpu_num) ||
		!__atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE)) {
		return;
	}
	ring = &arm64_perf_rings[pcpu_id];
	generation = __atomic_load_n(&ring->pending_generation, __ATOMIC_ACQUIRE);
	if (generation == 0UL) {
		return;
	}
	now = cpu_ticks();
	if (now < __atomic_load_n(&ring->due_ticks, __ATOMIC_RELAXED)) {
		return;
	}
	epoch = __atomic_load_n(&ring->pending_epoch, __ATOMIC_RELAXED);
	if (!__atomic_compare_exchange_n(&ring->pending_generation, &generation, 0UL,
		false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return;
	}

	(void)__atomic_add_fetch(&ring->attempts, 1UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->writer_active, true, __ATOMIC_RELEASE);
	if (!__atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE) ||
		(epoch != __atomic_load_n(&arm64_perf_ctx.epoch, __ATOMIC_RELAXED))) {
		__atomic_store_n(&ring->writer_active, false, __ATOMIC_RELEASE);
		return;
	}

	(void)memset(&sample, 0U, sizeof(sample));
	sample.tsc = now;
	sample.epoch = epoch;
	sample.generation = generation;
	sample.pcpu_id = pcpu_id;
	sample.vm_id = ARM64_PERF_INVALID_ID;
	sample.vcpu_id = ARM64_PERF_INVALID_ID;
	sample.owner = ARM64_PERF_OWNER_HOST;
	if (guest_context) {
		arm64_perf_capture_guest(&sample);
	} else {
		arm64_perf_capture_host(ctx, &sample);
		if (sample.unwind_stop == ARM64_PERF_UNWIND_NO_STACK) {
			(void)__atomic_add_fetch(&ring->no_stack, 1UL, __ATOMIC_RELAXED);
		}
	}
	arm64_perf_publish_sample(ring, &sample);
	__atomic_store_n(&ring->writer_active, false, __ATOMIC_RELEASE);
}

static void arm64_perf_publish_generation(uint16_t pcpu_id,
	uint64_t epoch, uint64_t generation, uint64_t due_ticks)
{
	struct arm64_perf_cpu_ring *ring = &arm64_perf_rings[pcpu_id];

	if (__atomic_load_n(&ring->pending_generation, __ATOMIC_ACQUIRE) != 0UL) {
		(void)__atomic_add_fetch(&ring->missed, 1UL, __ATOMIC_RELAXED);
	}
	__atomic_store_n(&ring->pending_epoch, epoch, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->due_ticks, due_ticks, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->pending_generation, generation, __ATOMIC_RELEASE);
}

static uint64_t arm64_perf_next_due(const struct hv_timer *timer, uint64_t now)
{
	uint64_t next = timer->timeout + timer->period_in_cycle;

	if ((next <= timer->timeout) || (next <= now)) {
		next = now + timer->period_in_cycle;
	}
	return next;
}

static void arm64_perf_timer(void *data)
{
	struct arm64_perf_context *perf = data;
	uint64_t now = cpu_ticks();
	uint64_t epoch;
	uint64_t generation;
	uint16_t pcpu_id;

	if ((perf == NULL) ||
		!__atomic_load_n(&perf->running, __ATOMIC_ACQUIRE)) {
		if (perf != NULL) {
			perf->timer.mode = TICK_MODE_ONESHOT;
			perf->timer_started = false;
		}
		return;
	}

	if (now >= perf->stop_ticks) {
		perf->timer.mode = TICK_MODE_ONESHOT;
		perf->timer_started = false;
		__atomic_store_n(&perf->running, false, __ATOMIC_RELEASE);
		for (pcpu_id = 0U; pcpu_id < perf->pcpu_num; pcpu_id++) {
			__atomic_store_n(&arm64_perf_rings[pcpu_id].pending_generation,
				0UL, __ATOMIC_RELEASE);
		}
		return;
	}

	epoch = __atomic_load_n(&perf->epoch, __ATOMIC_RELAXED);
	generation = __atomic_load_n(&perf->generation, __ATOMIC_RELAXED);
	for (pcpu_id = 0U; pcpu_id < perf->pcpu_num; pcpu_id++) {
		if (pcpu_id != perf->controller_pcpu) {
			arm64_perf_publish_generation(pcpu_id, epoch, generation, now);
			arch_smp_call_kick_pcpu(pcpu_id);
		}
	}
	generation = (generation == UINT64_MAX) ? 1UL : generation + 1UL;
	__atomic_store_n(&perf->generation, generation, __ATOMIC_RELAXED);
	arm64_perf_publish_generation(perf->controller_pcpu, epoch, generation,
		arm64_perf_next_due(&perf->timer, now));
}

static void arm64_perf_reset_ring(struct arm64_perf_cpu_ring *ring)
{
	(void)memset(ring->records, 0U, sizeof(ring->records));
	__atomic_store_n(&ring->pending_epoch, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->pending_generation, 0UL, __ATOMIC_RELEASE);
	__atomic_store_n(&ring->due_ticks, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->writer_active, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ring->attempts, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->captured, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->no_stack, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->missed, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->overwritten, 0UL, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->head, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->count, 0U, __ATOMIC_RELEASE);
}

static bool arm64_perf_writers_idle(void)
{
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < arm64_perf_ctx.pcpu_num; pcpu_id++) {
		if (__atomic_load_n(&arm64_perf_rings[pcpu_id].writer_active,
			__ATOMIC_ACQUIRE)) {
			return false;
		}
	}
	return true;
}

static int32_t arm64_perf_wait_writers(void)
{
	uint64_t deadline = cpu_ticks() + us_to_ticks(ARM64_PERF_STOP_TIMEOUT_US);

	do {
		if (arm64_perf_writers_idle()) {
			return 0;
		}
		cpu_relax();
	} while (cpu_ticks() < deadline);
	return -ETIMEDOUT;
}

int32_t arm64_perf_clear(void)
{
	int32_t ret;
	uint16_t pcpu_id;

	if (__atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE)) {
		return -EBUSY;
	}
	ret = arm64_perf_wait_writers();
	if (ret == 0) {
		for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
			arm64_perf_reset_ring(&arm64_perf_rings[pcpu_id]);
		}
	}
	return ret;
}

int32_t arm64_perf_record(uint32_t duration_ms, uint32_t frequency_hz)
{
	uint32_t duration_us;
	uint32_t period_us;
	uint64_t now;
	uint64_t period_ticks;
	uint64_t duration_ticks;
	uint64_t epoch;
	int32_t ret;

	if ((frequency_hz < ARM64_PERF_MIN_FREQUENCY_HZ) ||
		(frequency_hz > ARM64_PERF_MAX_FREQUENCY_HZ) ||
		(duration_ms == 0U) || (duration_ms > ARM64_PERF_MAX_DURATION_MS)) {
		return -EINVAL;
	}
	duration_us = duration_ms * ARM64_PERF_USEC_PER_MSEC;
	period_us = ARM64_PERF_USEC_PER_SEC / frequency_hz;
	if (duration_us < period_us) {
		return -EINVAL;
	}
	if (__atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE)) {
		return -EBUSY;
	}
	ret = arm64_perf_clear();
	if (ret != 0) {
		return ret;
	}

	arm64_perf_ctx.pcpu_num = get_pcpu_nums();
	arm64_perf_ctx.controller_pcpu = get_pcpu_id();
	if ((arm64_perf_ctx.pcpu_num == 0U) ||
		(arm64_perf_ctx.pcpu_num > MAX_PCPU_NUM) ||
		(arm64_perf_ctx.controller_pcpu >= arm64_perf_ctx.pcpu_num)) {
		return -EINVAL;
	}
	period_ticks = us_to_ticks(period_us);
	duration_ticks = us_to_ticks(duration_us);
	now = cpu_ticks();
	if ((period_ticks == 0UL) || (duration_ticks == 0UL) ||
		(now > (UINT64_MAX - duration_ticks)) ||
		(now > (UINT64_MAX - period_ticks))) {
		return -EINVAL;
	}

	arm64_perf_ctx.duration_ms = duration_ms;
	arm64_perf_ctx.frequency_hz = frequency_hz;
	arm64_perf_ctx.period_us = period_us;
	arm64_perf_ctx.stop_ticks = now + duration_ticks;
	epoch = __atomic_load_n(&arm64_perf_ctx.epoch, __ATOMIC_RELAXED);
	epoch = (epoch == UINT64_MAX) ? 1UL : epoch + 1UL;
	__atomic_store_n(&arm64_perf_ctx.epoch, epoch, __ATOMIC_RELAXED);
	arm64_perf_ctx.generation = 1UL;
	initialize_timer(&arm64_perf_ctx.timer, arm64_perf_timer,
		&arm64_perf_ctx, now + period_ticks, period_ticks);
	arm64_perf_publish_generation(arm64_perf_ctx.controller_pcpu, epoch, 1UL,
		now + period_ticks);
	__atomic_store_n(&arm64_perf_ctx.running, true, __ATOMIC_RELEASE);
	ret = add_timer(&arm64_perf_ctx.timer);
	if (ret != 0) {
		__atomic_store_n(&arm64_perf_ctx.running, false, __ATOMIC_RELEASE);
		__atomic_store_n(
			&arm64_perf_rings[arm64_perf_ctx.controller_pcpu].pending_generation,
			0UL, __ATOMIC_RELEASE);
		return ret;
	}
	arm64_perf_ctx.timer_started = true;
	return 0;
}

int32_t arm64_perf_stop(void)
{
	uint16_t pcpu_id;

	if (__atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE) &&
		(get_pcpu_id() != arm64_perf_ctx.controller_pcpu)) {
		return -EPERM;
	}
	__atomic_store_n(&arm64_perf_ctx.running, false, __ATOMIC_RELEASE);
	if (arm64_perf_ctx.timer_started) {
		del_timer(&arm64_perf_ctx.timer);
		arm64_perf_ctx.timer_started = false;
	}
	for (pcpu_id = 0U; pcpu_id < arm64_perf_ctx.pcpu_num; pcpu_id++) {
		__atomic_store_n(&arm64_perf_rings[pcpu_id].pending_generation,
			0UL, __ATOMIC_RELEASE);
	}
	return arm64_perf_wait_writers();
}

void arm64_perf_get_status(struct arm64_perf_status *status)
{
	bool running;

	if (status == NULL) {
		return;
	}
	(void)memset(status, 0U, sizeof(*status));
	running = __atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE);
	status->epoch = __atomic_load_n(&arm64_perf_ctx.epoch, __ATOMIC_RELAXED);
	status->generation = __atomic_load_n(&arm64_perf_ctx.generation,
		__ATOMIC_RELAXED);
	status->stop_ticks = arm64_perf_ctx.stop_ticks;
	status->duration_ms = arm64_perf_ctx.duration_ms;
	status->frequency_hz = arm64_perf_ctx.frequency_hz;
	status->period_us = arm64_perf_ctx.period_us;
	status->controller_pcpu = arm64_perf_ctx.controller_pcpu;
	status->pcpu_num = arm64_perf_ctx.pcpu_num;
	status->running = running;
	status->readable = !running && arm64_perf_writers_idle();
}

void arm64_perf_get_cpu_status(uint16_t pcpu_id,
	struct arm64_perf_cpu_status *status)
{
	const struct arm64_perf_cpu_ring *ring;

	if (status == NULL) {
		return;
	}
	(void)memset(status, 0U, sizeof(*status));
	if (pcpu_id >= arm64_perf_ctx.pcpu_num) {
		return;
	}
	ring = &arm64_perf_rings[pcpu_id];
	status->attempts = __atomic_load_n(&ring->attempts, __ATOMIC_RELAXED);
	status->captured = __atomic_load_n(&ring->captured, __ATOMIC_RELAXED);
	status->no_stack = __atomic_load_n(&ring->no_stack, __ATOMIC_RELAXED);
	status->missed = __atomic_load_n(&ring->missed, __ATOMIC_RELAXED);
	status->overwritten = __atomic_load_n(&ring->overwritten, __ATOMIC_RELAXED);
	status->pending_generation = __atomic_load_n(&ring->pending_generation,
		__ATOMIC_ACQUIRE);
	status->count = __atomic_load_n(&ring->count, __ATOMIC_ACQUIRE);
	status->writer_active = __atomic_load_n(&ring->writer_active,
		__ATOMIC_ACQUIRE);
}

bool arm64_perf_get_sample(uint16_t pcpu_id, uint32_t index,
	struct arm64_perf_sample *sample)
{
	const struct arm64_perf_cpu_ring *ring;
	uint32_t oldest;
	uint32_t slot;

	if ((sample == NULL) || (pcpu_id >= arm64_perf_ctx.pcpu_num) ||
		__atomic_load_n(&arm64_perf_ctx.running, __ATOMIC_ACQUIRE) ||
		!arm64_perf_writers_idle()) {
		return false;
	}
	ring = &arm64_perf_rings[pcpu_id];
	if (index >= __atomic_load_n(&ring->count, __ATOMIC_ACQUIRE)) {
		return false;
	}
	oldest = (__atomic_load_n(&ring->count, __ATOMIC_RELAXED) ==
		ARM64_PERF_RECORDS_PER_CPU) ?
		__atomic_load_n(&ring->head, __ATOMIC_ACQUIRE) : 0U;
	slot = oldest + index;
	if (slot >= ARM64_PERF_RECORDS_PER_CPU) {
		slot -= ARM64_PERF_RECORDS_PER_CPU;
	}
	cpu_read_memory_barrier();
	*sample = ring->records[slot];
	return true;
}

const char *arm64_perf_unwind_stop_name(uint16_t stop)
{
	const char *name;

	switch (stop) {
	case ARM64_PERF_UNWIND_COMPLETE:
		name = "complete";
		break;
	case ARM64_PERF_UNWIND_NO_STACK:
		name = "no-stack";
		break;
	case ARM64_PERF_UNWIND_FP_MISALIGNED:
		name = "fp-misaligned";
		break;
	case ARM64_PERF_UNWIND_FP_OUTSIDE:
		name = "fp-outside";
		break;
	case ARM64_PERF_UNWIND_FP_ORDER:
		name = "fp-order";
		break;
	case ARM64_PERF_UNWIND_LR_OUTSIDE:
		name = "lr-outside";
		break;
	case ARM64_PERF_UNWIND_DEPTH:
	default:
		name = "depth";
		break;
	}
	return name;
}
