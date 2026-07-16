/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <atomic.h>
#include <barrier.h>
#include <logmsg.h>
#include <per_cpu.h>
#include <schedule.h>
#include <sprintf.h>
#include <util.h>
#include <debug/symbol.h>
#include <asm/boot/ld_sym.h>
#include <asm/coredump.h>

#define ARM64_COREDUMP_MAGIC		0x42434450U
#define ARM64_COREDUMP_VERSION		1U
#define ARM64_COREDUMP_MAX_FRAMES	16U
#define ARM64_COREDUMP_STACK_WORDS	16U
#define ARM64_COREDUMP_SLOT_COUNT	(MAX_PCPU_NUM + 1U)

enum arm64_coredump_stop {
	ARM64_COREDUMP_STOP_COMPLETE = 0U,
	ARM64_COREDUMP_STOP_NO_STACK,
	ARM64_COREDUMP_STOP_FP_MISALIGNED,
	ARM64_COREDUMP_STOP_FP_OUTSIDE,
	ARM64_COREDUMP_STOP_FP_ORDER,
	ARM64_COREDUMP_STOP_LR_OUTSIDE,
	ARM64_COREDUMP_STOP_DEPTH,
};

struct arm64_coredump_stack {
	uint64_t start;
	uint64_t end;
	char owner[32U];
	bool valid;
};

struct arm64_frame_record {
	uint64_t previous_fp;
	uint64_t lr;
};

struct arm64_coredump_snapshot {
	uint32_t magic;
	uint32_t version;
	uint32_t sequence;
	uint32_t checksum;
	uint16_t pcpu_id;
	uint16_t frame_count;
	uint16_t stack_word_count;
	uint16_t stop;
	struct arm64_coredump_context context;
	uint64_t stack_start;
	uint64_t stack_end;
	uint64_t stack_word_start;
	uint64_t stack_words[ARM64_COREDUMP_STACK_WORDS];
	uint64_t frames[ARM64_COREDUMP_MAX_FRAMES];
	char owner[32U];
};

static struct arm64_coredump_snapshot
	arm64_coredump_slots[ARM64_COREDUMP_SLOT_COUNT];
static struct arm64_coredump_snapshot arm64_coredump_readback;
static int32_t arm64_coredump_sequence;

/* [20260717] Bounded EL2 coredump ownership
 *
 *   panic/exception PC, LR, SP, FP
 *       |
 *       +--> resolve registered thread, pCPU, or boot stack
 *       |
 *       +--> copy bounded raw words + validated frame records
 *       |
 *       +--> checksum snapshot, publish magic last
 *       |
 *       +--> severity-selected log; shell may print or erase the copy later
 *
 * Key rule:
 *   - capture never follows an address outside known EL2 stack storage;
 *   - each pCPU owns one fixed slot, so panic capture needs no allocator or
 *     lock and cannot wait behind a failed CPU;
 *   - readers accept only a complete versioned snapshot with a valid checksum;
 *   - shell erase clears the publish marker before touching stored contents.
 */

static bool arm64_coredump_range_contains(uint64_t start, uint64_t end,
	uint64_t address, uint64_t size)
{
	return (start < end) && (address >= start) && (address <= end) &&
		(size <= (end - address));
}

static bool arm64_coredump_hv_range_valid(uint64_t address, uint64_t size)
{
	return arm64_coredump_range_contains((uint64_t)&ld_ram_start,
		(uint64_t)&ld_ram_end, address, size);
}

static bool arm64_coredump_stack_candidate(struct arm64_coredump_stack *stack,
	uint64_t start, uint64_t size, uint64_t sp, const char *owner)
{
	uint64_t end;

	if ((stack == NULL) || (owner == NULL) || (size == 0UL) ||
		(start > (UINT64_MAX - size))) {
		return false;
	}
	end = start + size;
	if (((start & (CPU_STACK_ALIGN - 1UL)) != 0UL) ||
		((end & (CPU_STACK_ALIGN - 1UL)) != 0UL) ||
		!arm64_coredump_hv_range_valid(start, size) ||
		!arm64_coredump_range_contains(start, end, sp, sizeof(uint64_t))) {
		return false;
	}

	stack->start = start;
	stack->end = end;
	stack->valid = true;
	(void)snprintf(stack->owner, sizeof(stack->owner), "%s", owner);
	return true;
}

static void arm64_coredump_resolve_stack(uint16_t pcpu_id, uint64_t sp,
	struct arm64_coredump_stack *stack)
{
	struct thread_object *thread;
	char owner[32U];

	(void)memset(stack, 0U, sizeof(*stack));
	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}

	thread = sched_get_current(pcpu_id);
	if ((thread != NULL) &&
		arm64_coredump_hv_range_valid((uint64_t)thread, sizeof(*thread))) {
		(void)snprintf(owner, sizeof(owner), "thread:%.16s", thread->name);
		if (arm64_coredump_stack_candidate(stack, thread->host_stack_base,
			thread->host_stack_size, sp, owner)) {
			return;
		}
	}

	if (arm64_coredump_stack_candidate(stack,
		(uint64_t)&per_cpu(stack, pcpu_id)[0], CONFIG_STACK_SIZE, sp,
		"pcpu")) {
		return;
	}

	(void)arm64_coredump_stack_candidate(stack,
		(uint64_t)&_boot_stack_start,
		(uint64_t)&_boot_stack_end - (uint64_t)&_boot_stack_start,
		sp, "boot");
}

static bool arm64_coredump_text_address(uint64_t address)
{
	return (address >= (uint64_t)&_text_start) &&
		(address < (uint64_t)&_text_end);
}

static void arm64_coredump_append_frame(struct arm64_coredump_snapshot *snapshot,
	uint64_t pc)
{
	if ((snapshot->frame_count < ARM64_COREDUMP_MAX_FRAMES) &&
		((snapshot->frame_count == 0U) ||
		 (snapshot->frames[snapshot->frame_count - 1U] != pc))) {
		snapshot->frames[snapshot->frame_count++] = pc;
	}
}

static void arm64_coredump_capture_stack(struct arm64_coredump_snapshot *snapshot,
	const struct arm64_coredump_stack *stack)
{
	uint64_t available;
	uint32_t count;
	uint32_t index;

	snapshot->stack_start = stack->start;
	snapshot->stack_end = stack->end;
	snapshot->stack_word_start = snapshot->context.sp;
	(void)snprintf(snapshot->owner, sizeof(snapshot->owner), "%s", stack->owner);
	available = stack->end - snapshot->context.sp;
	count = (uint32_t)min(available / sizeof(uint64_t),
		(uint64_t)ARM64_COREDUMP_STACK_WORDS);
	for (index = 0U; index < count; index++) {
		snapshot->stack_words[index] =
			((const uint64_t *)snapshot->context.sp)[index];
	}
	snapshot->stack_word_count = (uint16_t)count;
}

static void arm64_coredump_capture_frames(struct arm64_coredump_snapshot *snapshot,
	const struct arm64_coredump_stack *stack)
{
	uint64_t fp = snapshot->context.fp;
	uint32_t records = 0U;

	if (arm64_coredump_text_address(snapshot->context.pc)) {
		arm64_coredump_append_frame(snapshot, snapshot->context.pc);
	}
	if (arm64_coredump_text_address(snapshot->context.lr)) {
		arm64_coredump_append_frame(snapshot, snapshot->context.lr);
	}

	while ((snapshot->frame_count < ARM64_COREDUMP_MAX_FRAMES) &&
		(records < ARM64_COREDUMP_MAX_FRAMES)) {
		const struct arm64_frame_record *frame;
		uint64_t next_fp;

		if (fp == 0UL) {
			snapshot->stop = ARM64_COREDUMP_STOP_COMPLETE;
			return;
		}
		if ((fp & (CPU_STACK_ALIGN - 1UL)) != 0UL) {
			snapshot->stop = ARM64_COREDUMP_STOP_FP_MISALIGNED;
			return;
		}
		if ((fp < snapshot->context.sp) ||
			!arm64_coredump_range_contains(stack->start, stack->end, fp,
				sizeof(*frame))) {
			snapshot->stop = ARM64_COREDUMP_STOP_FP_OUTSIDE;
			return;
		}

		frame = (const struct arm64_frame_record *)fp;
		next_fp = frame->previous_fp;
		records++;
		if (frame->lr != 0UL) {
			if (!arm64_coredump_text_address(frame->lr)) {
				snapshot->stop = ARM64_COREDUMP_STOP_LR_OUTSIDE;
				return;
			}
			arm64_coredump_append_frame(snapshot, frame->lr);
		}
		if (next_fp == 0UL) {
			snapshot->stop = ARM64_COREDUMP_STOP_COMPLETE;
			return;
		}
		if (next_fp <= fp) {
			snapshot->stop = ARM64_COREDUMP_STOP_FP_ORDER;
			return;
		}
		fp = next_fp;
	}

	snapshot->stop = ARM64_COREDUMP_STOP_DEPTH;
}

static uint32_t arm64_coredump_checksum(const struct arm64_coredump_snapshot *snapshot)
{
	const uint8_t *bytes = (const uint8_t *)snapshot;
	uint32_t checksum = 2166136261U;
	uint32_t index;

	for (index = 0U; index < sizeof(*snapshot); index++) {
		if ((index < sizeof(snapshot->magic)) ||
			((index >= offsetof(struct arm64_coredump_snapshot, checksum)) &&
			 (index < (offsetof(struct arm64_coredump_snapshot, checksum) +
			  sizeof(snapshot->checksum))))) {
			continue;
		}
		checksum ^= bytes[index];
		checksum *= 16777619U;
	}

	return checksum;
}

static bool arm64_coredump_snapshot_valid(
	const struct arm64_coredump_snapshot *snapshot)
{
	uint32_t index;

	if ((snapshot->magic != ARM64_COREDUMP_MAGIC) ||
		(snapshot->version != ARM64_COREDUMP_VERSION) ||
		(snapshot->frame_count > ARM64_COREDUMP_MAX_FRAMES) ||
		(snapshot->stack_word_count > ARM64_COREDUMP_STACK_WORDS) ||
		(snapshot->stop > ARM64_COREDUMP_STOP_DEPTH) ||
		(snapshot->owner[sizeof(snapshot->owner) - 1U] != '\0') ||
		(snapshot->checksum != arm64_coredump_checksum(snapshot))) {
		return false;
	}

	if (snapshot->stack_end > snapshot->stack_start) {
		if ((snapshot->stop == ARM64_COREDUMP_STOP_NO_STACK) ||
			(snapshot->owner[0] == '\0') ||
			!arm64_coredump_hv_range_valid(snapshot->stack_start,
				snapshot->stack_end - snapshot->stack_start) ||
			!arm64_coredump_range_contains(snapshot->stack_start,
				snapshot->stack_end, snapshot->context.sp,
				sizeof(uint64_t)) ||
			(snapshot->stack_word_start != snapshot->context.sp) ||
			!arm64_coredump_range_contains(snapshot->stack_start,
				snapshot->stack_end, snapshot->stack_word_start,
				(uint64_t)snapshot->stack_word_count * sizeof(uint64_t))) {
			return false;
		}
	} else if ((snapshot->stack_start != 0UL) ||
		(snapshot->stack_end != 0UL) || (snapshot->stack_word_start != 0UL) ||
		(snapshot->stack_word_count != 0U) || (snapshot->owner[0] != '\0') ||
		(snapshot->stop != ARM64_COREDUMP_STOP_NO_STACK)) {
		return false;
	}

	for (index = 0U; index < snapshot->frame_count; index++) {
		if (!arm64_coredump_text_address(snapshot->frames[index])) {
			return false;
		}
	}

	return true;
}

static const char *arm64_coredump_stop_name(uint16_t stop)
{
	const char *name;

	switch (stop) {
	case ARM64_COREDUMP_STOP_COMPLETE:
		name = "complete";
		break;
	case ARM64_COREDUMP_STOP_NO_STACK:
		name = "no-trusted-stack";
		break;
	case ARM64_COREDUMP_STOP_FP_MISALIGNED:
		name = "fp-misaligned";
		break;
	case ARM64_COREDUMP_STOP_FP_OUTSIDE:
		name = "fp-outside-stack";
		break;
	case ARM64_COREDUMP_STOP_FP_ORDER:
		name = "fp-not-monotonic";
		break;
	case ARM64_COREDUMP_STOP_LR_OUTSIDE:
		name = "lr-outside-text";
		break;
	case ARM64_COREDUMP_STOP_DEPTH:
	default:
		name = "depth-limit";
		break;
	}

	return name;
}

static void arm64_coredump_emit(const struct arm64_coredump_snapshot *snapshot,
	uint32_t severity)
{
	uint32_t index;
	char symbol[96U];

	do_logmsg(severity,
		"coredump.header version:%u sequence:%u pcpu:%hu checksum:0x%08x",
		snapshot->version, snapshot->sequence, snapshot->pcpu_id,
		snapshot->checksum);
	do_logmsg(severity,
		"coredump.regs pc:0x%016lx lr:0x%016lx sp:0x%016lx fp:0x%016lx",
		snapshot->context.pc, snapshot->context.lr,
		snapshot->context.sp, snapshot->context.fp);
	if (snapshot->stack_end > snapshot->stack_start) {
		do_logmsg(severity,
			"coredump.stack owner:%s range:[0x%016lx-0x%016lx) used:%lu free:%lu",
			snapshot->owner, snapshot->stack_start, snapshot->stack_end,
			snapshot->stack_end - snapshot->context.sp,
			snapshot->context.sp - snapshot->stack_start);
	}
	for (index = 0U; index < snapshot->stack_word_count; index += 4U) {
		uint64_t words[4U] = { 0UL, 0UL, 0UL, 0UL };
		uint32_t word;

		for (word = 0U; (word < 4U) &&
			((index + word) < snapshot->stack_word_count); word++) {
			words[word] = snapshot->stack_words[index + word];
		}
		do_logmsg(severity,
			"coredump.stack:0x%016lx %016lx %016lx %016lx %016lx",
			snapshot->stack_word_start + ((uint64_t)index * sizeof(uint64_t)),
			words[0], words[1], words[2], words[3]);
	}
	for (index = 0U; index < snapshot->frame_count; index++) {
		dbg_format_symbol(snapshot->frames[index], symbol, sizeof(symbol));
		do_logmsg(severity, "coredump.frame:%02u pc:0x%016lx %s",
			index, snapshot->frames[index], symbol);
	}
	do_logmsg(severity, "coredump.stop:%s",
		arm64_coredump_stop_name(snapshot->stop));
}

static uint32_t arm64_coredump_slot(uint16_t pcpu_id)
{
	return (pcpu_id < MAX_PCPU_NUM) ? pcpu_id : MAX_PCPU_NUM;
}

static struct arm64_coredump_snapshot *arm64_coredump_capture(
	const struct arm64_coredump_context *context, uint16_t pcpu_id)
{
	struct arm64_coredump_snapshot *snapshot;
	struct arm64_coredump_stack stack;

	snapshot = &arm64_coredump_slots[arm64_coredump_slot(pcpu_id)];
	snapshot->magic = 0U;
	cpu_write_memory_barrier();
	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->version = ARM64_COREDUMP_VERSION;
	snapshot->sequence = (uint32_t)atomic_inc_return(&arm64_coredump_sequence);
	snapshot->pcpu_id = pcpu_id;
	snapshot->context = *context;
	arm64_coredump_resolve_stack(pcpu_id, context->sp, &stack);
	if (stack.valid) {
		arm64_coredump_capture_stack(snapshot, &stack);
		arm64_coredump_capture_frames(snapshot, &stack);
	} else {
		snapshot->stop = ARM64_COREDUMP_STOP_NO_STACK;
	}
	snapshot->checksum = arm64_coredump_checksum(snapshot);
	cpu_write_memory_barrier();
	snapshot->magic = ARM64_COREDUMP_MAGIC;

	return snapshot;
}

static const struct arm64_coredump_snapshot *arm64_coredump_latest(void)
{
	const struct arm64_coredump_snapshot *latest = NULL;
	uint32_t slot;

	for (slot = 0U; slot < ARM64_COREDUMP_SLOT_COUNT; slot++) {
		const struct arm64_coredump_snapshot *snapshot =
			&arm64_coredump_slots[slot];

		cpu_read_memory_barrier();
		if (arm64_coredump_snapshot_valid(snapshot) &&
			((latest == NULL) || (snapshot->sequence > latest->sequence))) {
			latest = snapshot;
		}
	}

	return latest;
}

void arm64_coredump_log(const struct arm64_coredump_context *context,
	uint16_t pcpu_id, uint32_t severity)
{
	struct arm64_coredump_snapshot *snapshot;

	if (context == NULL) {
		return;
	}
	snapshot = arm64_coredump_capture(context, pcpu_id);
	arm64_coredump_emit(snapshot, severity);
}

bool arm64_coredump_print_stored(uint32_t severity)
{
	const struct arm64_coredump_snapshot *stored = arm64_coredump_latest();

	if (stored == NULL) {
		return false;
	}
	(void)memcpy_s(&arm64_coredump_readback, sizeof(arm64_coredump_readback),
		stored, sizeof(*stored));
	cpu_read_memory_barrier();
	if (!arm64_coredump_snapshot_valid(&arm64_coredump_readback)) {
		return false;
	}
	arm64_coredump_emit(&arm64_coredump_readback, severity);
	return true;
}

void arm64_coredump_erase_stored(void)
{
	uint32_t slot;

	for (slot = 0U; slot < ARM64_COREDUMP_SLOT_COUNT; slot++) {
		arm64_coredump_slots[slot].magic = 0U;
	}
	cpu_write_memory_barrier();
	(void)memset(arm64_coredump_slots, 0U, sizeof(arm64_coredump_slots));
}
