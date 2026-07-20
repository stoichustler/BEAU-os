/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <barrier.h>
#include <cpu.h>
#include <errno.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <vcpu.h>
#include <vm.h>
#include <debug/symbol.h>
#include <asm/boot/ld_sym.h>
#include <asm/guest/vcpu.h>
#include "ddb_internal.h"

#define DDB_CPU_OWNER_SIZE	16U

struct ddb_frame_record {
	uint64_t previous_fp;
	uint64_t lr;
};

struct ddb_stack_range {
	uint64_t start;
	uint64_t end;
	const char *owner;
	bool valid;
};

struct ddb_cpu_sample {
	uint64_t pc;
	uint64_t sp;
	uint64_t fp;
	uint64_t lr;
	char owner[DDB_CPU_OWNER_SIZE];
	uint16_t pcpu_id;
	uint16_t vm_id;
	uint16_t vcpu_id;
	bool guest;
	bool valid;
};

struct ddb_cpu_mailbox {
	uint64_t publish_version;
	uint64_t completed_sequence;
	struct ddb_cpu_sample sample;
} __aligned(64);

struct ddb_cpu_request {
	uint64_t sequence;
};

static struct ddb_cpu_mailbox ddb_cpu_mailboxes[MAX_PCPU_NUM];
static struct ddb_cpu_request ddb_cpu_request;
static uint64_t ddb_cpu_next_sequence;

static bool ddb_range_contains(uint64_t start, uint64_t end,
	uint64_t address, uint64_t size)
{
	return (start < end) && (address >= start) && (address <= end) &&
		(size <= (end - address));
}

static bool ddb_hv_range_valid(uint64_t address, uint64_t size)
{
	return ddb_range_contains((uint64_t)&ld_ram_start,
		(uint64_t)&ld_ram_end, address, size);
}

static bool ddb_stack_candidate(struct ddb_stack_range *stack,
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
		!ddb_hv_range_valid(start, size) ||
		!ddb_range_contains(start, end, sp, sizeof(uint64_t))) {
		return false;
	}

	stack->start = start;
	stack->end = end;
	stack->owner = owner;
	stack->valid = true;
	return true;
}

static void ddb_resolve_stack(const struct ddb_session *session,
	struct ddb_stack_range *stack)
{
	struct thread_object *thread;
	uint64_t sp = session->ctx->regs.sp;

	(void)memset(stack, 0U, sizeof(*stack));
	thread = sched_get_current(session->pcpu_id);
	if ((thread != NULL) && ddb_hv_range_valid((uint64_t)thread,
		sizeof(*thread)) && ddb_stack_candidate(stack,
		thread->host_stack_base, thread->host_stack_size, sp, "thread")) {
		return;
	}
	if (ddb_stack_candidate(stack,
		(uint64_t)&per_cpu(stack, session->pcpu_id)[0], CONFIG_STACK_SIZE,
		sp, "pcpu")) {
		return;
	}
	(void)ddb_stack_candidate(stack, (uint64_t)&_boot_stack_start,
		(uint64_t)&_boot_stack_end - (uint64_t)&_boot_stack_start,
		sp, "boot");
}

static bool ddb_text_address(uint64_t address)
{
	return (address >= (uint64_t)&_text_start) &&
		(address < (uint64_t)&_text_end);
}

static void ddb_print_frame(uint32_t index, uint64_t address)
{
	char symbol[96U];

	dbg_format_symbol(address, symbol, sizeof(symbol));
	ddb_printf("#%02u 0x%016lx %s\n", index, address, symbol);
}

/* [20260720] FreeBSD-derived ARM64 frame unwind
 *
 *   saved EL2 FP -> trusted stack range -> read {previous FP, LR}
 *                                             |
 *                                             +--> valid -> symbolize
 *                                             `--> invalid -> bounded stop
 *
 * Design origin:
 *   - the frame-pointer unwind mechanism derives from FreeBSD ARM64
 *     db_trace.c (sys/arm64/arm64/db_trace.c);
 *   - BEAU adds trusted EL2 stack, monotonic FP, Host text, and depth checks.
 *
 * Key rule:
 *   - each frame must remain inside the stack containing the saved Host SP;
 *   - an invalid frame terminates the diagnostic without following it.
 */
int32_t ddb_cmd_backtrace(struct ddb_session *session, uint32_t argc,
	__unused char **argv)
{
	struct ddb_stack_range stack;
	uint64_t fp = session->ctx->regs.x29;
	uint64_t last_address = session->ctx->regs.elr;
	uint32_t depth = 0U;

	if (argc != 1U) {
		ddb_puts("usage: bt\n");
		return -EINVAL;
	}
	ddb_resolve_stack(session, &stack);
	if (!stack.valid) {
		ddb_puts("ddb: no trusted EL2 stack contains the saved SP\n");
		return -EFAULT;
	}

	ddb_printf("stack:%s [0x%016lx,0x%016lx)\n",
		stack.owner, stack.start, stack.end);
	ddb_print_frame(depth++, last_address);
	while (depth < DDB_STACK_DEPTH) {
		struct ddb_frame_record frame;
		uint64_t fault_address = 0UL;
		uint64_t fault_esr = 0UL;

		if (fp == 0UL) {
			ddb_puts("stop: complete\n");
			return 0;
		}
		if ((fp & (CPU_STACK_ALIGN - 1UL)) != 0UL) {
			ddb_printf("stop: misaligned fp 0x%016lx\n", fp);
			return -EFAULT;
		}
		if ((fp < session->ctx->regs.sp) ||
			!ddb_range_contains(stack.start, stack.end, fp, sizeof(frame))) {
			ddb_printf("stop: fp outside stack 0x%016lx\n", fp);
			return -EFAULT;
		}
		if (ddb_read_memory(fp, &frame, sizeof(frame),
			&fault_address, &fault_esr) != 0) {
			ddb_printf("stop: unreadable frame 0x%016lx esr:0x%016lx\n",
				fault_address, fault_esr);
			return -EFAULT;
		}
		if ((frame.lr != 0UL) && (frame.lr != last_address)) {
			if (!ddb_text_address(frame.lr)) {
				ddb_printf("stop: lr outside text 0x%016lx\n", frame.lr);
				return -EFAULT;
			}
			ddb_print_frame(depth++, frame.lr);
			last_address = frame.lr;
		}
		if ((frame.previous_fp == 0UL) ||
			(frame.previous_fp == SP_BOTTOM_MAGIC)) {
			ddb_puts("stop: complete\n");
			return 0;
		}
		if (frame.previous_fp <= fp) {
			ddb_printf("stop: nonmonotonic fp 0x%016lx -> 0x%016lx\n",
				fp, frame.previous_fp);
			return -EFAULT;
		}
		fp = frame.previous_fp;
	}

	ddb_puts("stop: depth limit\n");
	return 0;
}

/* [20260720] Bounded live pCPU sampling
 *
 *   DDB owner -> publish generation -> SMP try-call -> pCPU mailbox
 *                                      |
 *                                      +--> guest: saved exit registers
 *                                      `--> host: callback SP/FP/LR sample
 *
 * Key rule:
 *   - callbacks publish odd/even mailbox versions around one fixed snapshot;
 *   - a timeout cannot make a late callback valid for a newer generation;
 *   - remote CPUs are sampled and released, never parked by the debugger.
 */
static void ddb_cpu_capture_owner(struct ddb_cpu_sample *sample)
{
	struct thread_object *owner = sched_get_current(sample->pcpu_id);

	if (owner != NULL) {
		size_t length = strnlen_s(owner->name, sizeof(sample->owner) - 1U);

		(void)memcpy_s(sample->owner, sizeof(sample->owner), owner->name,
			length);
		sample->owner[length] = '\0';
	}
}

static void ddb_cpu_capture_callback(void *data)
{
	const struct ddb_cpu_request *request = data;
	uint16_t pcpu_id = get_pcpu_id();
	struct ddb_cpu_mailbox *mailbox;
	struct ddb_cpu_sample sample;
	struct acrn_vcpu *vcpu;
	uint64_t version;
	uint64_t sequence;

	if ((request == NULL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	sequence = __atomic_load_n(&request->sequence, __ATOMIC_ACQUIRE);
	if (sequence == 0UL) {
		return;
	}

	(void)memset(&sample, 0U, sizeof(sample));
	sample.pcpu_id = pcpu_id;
	ddb_cpu_capture_owner(&sample);
	vcpu = get_running_vcpu(pcpu_id);
	if ((vcpu != NULL) && (vcpu->vm != NULL)) {
		sample.guest = true;
		sample.vm_id = vcpu->vm->vm_id;
		sample.vcpu_id = vcpu->vcpu_id;
		sample.pc = vcpu->arch.regs.elr;
		sample.sp = vcpu->arch.regs.sp;
		sample.fp = vcpu->arch.regs.x29;
		sample.lr = vcpu->arch.regs.lr;
	} else {
		__asm__ volatile(
			"mov %0, sp\n"
			"mov %1, x29\n"
			"mov %2, x30\n"
			: "=r" (sample.sp), "=r" (sample.fp), "=r" (sample.lr));
		sample.pc = sample.lr;
	}
	sample.valid = true;

	mailbox = &ddb_cpu_mailboxes[pcpu_id];
	version = (__atomic_load_n(&mailbox->publish_version,
		__ATOMIC_RELAXED) + 1UL) | 1UL;
	__atomic_store_n(&mailbox->publish_version, version, __ATOMIC_RELEASE);
	cpu_write_memory_barrier();
	(void)memcpy_s(&mailbox->sample, sizeof(mailbox->sample),
		&sample, sizeof(sample));
	cpu_write_memory_barrier();
	__atomic_store_n(&mailbox->completed_sequence, sequence,
		__ATOMIC_RELEASE);
	cpu_write_memory_barrier();
	__atomic_store_n(&mailbox->publish_version, version + 1UL,
		__ATOMIC_RELEASE);
}

static bool ddb_cpu_copy_sample(uint16_t pcpu_id, uint64_t sequence,
	struct ddb_cpu_sample *sample)
{
	const struct ddb_cpu_mailbox *mailbox = &ddb_cpu_mailboxes[pcpu_id];
	uint64_t version_before;
	uint64_t version_after;

	version_before = __atomic_load_n(&mailbox->publish_version,
		__ATOMIC_ACQUIRE);
	if (((version_before & 1UL) != 0UL) ||
		(__atomic_load_n(&mailbox->completed_sequence,
			__ATOMIC_ACQUIRE) != sequence)) {
		return false;
	}
	(void)memcpy_s(sample, sizeof(*sample), &mailbox->sample,
		sizeof(mailbox->sample));
	cpu_read_memory_barrier();
	version_after = __atomic_load_n(&mailbox->publish_version,
		__ATOMIC_ACQUIRE);

	return (version_before == version_after) &&
		((version_after & 1UL) == 0UL) &&
		(__atomic_load_n(&mailbox->completed_sequence,
			__ATOMIC_ACQUIRE) == sequence) && sample->valid &&
		(sample->pcpu_id == pcpu_id);
}

static void ddb_cpu_print_sample(const struct ddb_cpu_sample *sample,
	const char *kind)
{
	char symbol[96U];

	if (sample->guest) {
		ddb_printf("cpu%hu %-11s owner:%11s vm%hu:v%hu pc:0x%016lx sp:0x%016lx\n",
			sample->pcpu_id, kind, sample->owner, sample->vm_id,
			sample->vcpu_id, sample->pc, sample->sp);
	} else {
		dbg_format_symbol(sample->pc, symbol, sizeof(symbol));
		ddb_printf("cpu%hu %-11s owner:%11s pc:0x%016lx %s sp:0x%016lx fp:0x%016lx\n",
			sample->pcpu_id, kind, sample->owner, sample->pc, symbol,
			sample->sp, sample->fp);
	}
}

int32_t ddb_cmd_cpu(struct ddb_session *session, uint32_t argc,
	__unused char **argv)
{
	uint64_t active_mask;
	uint64_t remote_mask;
	uint64_t sequence;
	uint16_t pcpu_id;
	int32_t call_status = 0;
	struct ddb_cpu_sample local;

	if (argc != 1U) {
		ddb_puts("usage: cpu\n");
		return -EINVAL;
	}
	active_mask = get_active_pcpu_bitmap();
	remote_mask = active_mask & ~(1UL << session->pcpu_id);
	sequence = __atomic_add_fetch(&ddb_cpu_next_sequence, 1UL,
		__ATOMIC_SEQ_CST);
	if (sequence == 0UL) {
		sequence = __atomic_add_fetch(&ddb_cpu_next_sequence, 1UL,
			__ATOMIC_SEQ_CST);
	}
	__atomic_store_n(&ddb_cpu_request.sequence, sequence, __ATOMIC_RELEASE);

	(void)memset(&local, 0U, sizeof(local));
	local.pcpu_id = session->pcpu_id;
	local.pc = session->ctx->regs.elr;
	local.sp = session->ctx->regs.sp;
	local.fp = session->ctx->regs.x29;
	local.lr = session->ctx->regs.lr;
	local.valid = true;
	ddb_cpu_capture_owner(&local);

	if (remote_mask != 0UL) {
		call_status = smp_try_call_function_timeout(remote_mask,
			ddb_cpu_capture_callback, &ddb_cpu_request,
			DDB_SMP_TIMEOUT_US);
	}
	ddb_printf("active:0x%016lx smp-status:%d\n", active_mask, call_status);
	ddb_cpu_print_sample(&local, "breakpoint");
	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		struct ddb_cpu_sample sample;

		if ((pcpu_id == session->pcpu_id) ||
			((remote_mask & (1UL << pcpu_id)) == 0UL)) {
			continue;
		}
		(void)memset(&sample, 0U, sizeof(sample));
		if (ddb_cpu_copy_sample(pcpu_id, sequence, &sample)) {
			ddb_cpu_print_sample(&sample,
				sample.guest ? "guest-exit" : "host-sample");
		} else {
			ddb_printf("cpu%hu unavailable\n", pcpu_id);
		}
	}
	__atomic_store_n(&ddb_cpu_request.sequence, 0UL, __ATOMIC_RELEASE);
	return 0;
}
