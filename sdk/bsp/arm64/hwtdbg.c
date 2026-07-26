/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <barrier.h>
#include <cpu.h>
#include <errno.h>
#include <guest_memory.h>
#include <irq.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>
#include <vconfig.h>
#include <vcpu.h>
#include <vm.h>
#include <virtio_proxy.h>
#include <hwtdbg.h>
#include <vm_crash.h>
#include <debug/symbol.h>
#include <asm/boot/ld_sym.h>
#include <asm/guest/vcpu.h>
#include <asm/guest/stage2.h>
#include <asm/sysreg.h>
#include <asm/trap.h>
#include "../shell_priv.h"

#define HWTDBG_MAGIC			0x48575444U
#define HWTDBG_VERSION			3U
#define HWTDBG_EVENT_SLOTS		4U
#define HWTDBG_RAS_MAGIC		0x48575241U
#define HWTDBG_RAS_EVENT_SLOTS		4U
#define HWTDBG_FAULT_MAGIC		0x48574654U
#define HWTDBG_FAULT_VERSION		2U
#define HWTDBG_FAULT_EVENT_SLOTS	2U
#define HWTDBG_INVALID_ID		0xffffU
#define HWTDBG_RAS_RECORD_GPA_VALID	(1U << 2U)
#define HWTDBG_STACK_DEPTH		16U
#define HWTDBG_LIVE_TIMEOUT_US		1000U
#define HWTDBG_REGS_PER_LINE_MAX	4U
#define HWTDBG_REG_KEY_FMT		"%5s:0x%016lx"
#define HWTDBG_CAPTURE_LIVE_BUSY	(1U << 0U)
#define HWTDBG_CAPTURE_LIVE_TIMEOUT	(1U << 1U)
#define HWTDBG_VM_NUM			((CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ? \
	CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM)

enum hwtdbg_stack_stop {
	HWTDBG_STACK_COMPLETE = 0U,
	HWTDBG_STACK_EMPTY,
	HWTDBG_STACK_MISALIGNED,
	HWTDBG_STACK_TRANSLATE_FAILED,
	HWTDBG_STACK_COPY_FAILED,
	HWTDBG_STACK_OUTSIDE,
	HWTDBG_STACK_ORDER,
	HWTDBG_STACK_LR_OUTSIDE,
	HWTDBG_STACK_DEPTH_LIMIT,
};

struct hwtdbg_frame {
	uint64_t fp;
	uint64_t lr;
};

struct hwtdbg_stack_snapshot {
	struct hwtdbg_frame frame[HWTDBG_STACK_DEPTH];
	uint32_t count;
	enum hwtdbg_stack_stop stop;
	bool live;
};

struct hwtdbg_vcpu_snapshot {
	struct cpu_regs regs;
	struct arm64_vcpu_guest_ctx gctx;
	struct sched_latency_stats latency;
	struct hwtdbg_stack_snapshot guest_stack;
	struct hwtdbg_stack_snapshot host_stack;
	uint64_t pending_req;
	uint64_t irqs_pending;
	uint64_t irqs_pending_mask;
	char pcpu_owner_name[16U];
	uint16_t vcpu_id;
	uint16_t pcpu_id;
	uint16_t pcpu_owner_vm_id;
	uint16_t pcpu_owner_vcpu_id;
	enum vcpu_state vcpu_state;
	enum thread_object_state thread_state;
	enum thread_object_state pcpu_owner_state;
	bool current;
	bool live;
	bool pcpu_valid;
	bool pcpu_owner_present;
	bool pcpu_owner_is_vcpu;
	bool pcpu_need_reschedule;
};

struct hwtdbg_virtio_snapshot {
	uint64_t timeout_count;
	uint64_t completed_count;
	uint64_t notify_count;
	uint64_t error_count;
	uint16_t device_count;
	uint16_t pending_active;
	uint16_t pending_limit;
	uint16_t unhealthy_count;
};

struct hwtdbg_event {
	uint32_t magic;
	uint16_t version;
	uint16_t vm_id;
	uint32_t checksum;
	bool valid;
	uint64_t sequence;
	uint64_t captured_tsc;
	struct hwtdbg_timeout_context timeout;
	enum hwtdbg_recovery_result recovery;
	uint64_t recovery_attempt;
	uint64_t recovery_updated_tsc;
	uint64_t recovery_wait_vcpus;
	int32_t reset_ret;
	uint32_t capture_flags;
	uint64_t live_requested_mask;
	uint64_t live_captured_mask;
	uint64_t live_timeout_mask;
	enum vm_state vm_state;
	uint16_t vcpu_count;
	char vm_name[MAX_VM_NAME_LEN];
	struct hwtdbg_virtio_snapshot virtio;
	struct hwtdbg_vcpu_snapshot vcpu[MAX_VCPUS_PER_VM];
};

struct hwtdbg_ras_record {
	uint64_t status;
	uint64_t pa;
	uint64_t gpa;
	uint64_t misc0;
	uint16_t index;
	uint16_t flags;
};

struct hwtdbg_ras_event {
	uint32_t magic;
	uint16_t version;
	uint16_t pcpu_id;
	uint32_t checksum;
	bool valid;
	bool guest_context;
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint16_t hw_vmid;
	uint64_t sequence;
	uint64_t captured_tsc;
	uint64_t vttbr;
	struct cpu_regs regs;
	uint64_t erridr;
	uint64_t disr;
	uint32_t ras_flags;
	uint32_t record_count;
	uint16_t valid_count;
	struct hwtdbg_ras_record record[ARM64_RAS_MAX_RECORDS];
	struct hwtdbg_vcpu_snapshot vcpu;
};

struct hwtdbg_fault_event {
	uint32_t magic;
	uint16_t version;
	uint16_t vm_id;
	uint32_t checksum;
	bool valid;
	uint16_t vcpu_id;
	uint16_t pcpu_id;
	enum hwtdbg_guest_fault_reason reason;
	int32_t exit_ret;
	uint64_t sequence;
	uint64_t captured_tsc;
	enum vm_state vm_state;
	enum vcpu_state vcpu_state;
	enum thread_object_state thread_state;
	bool wdt_valid;
	struct vm_wdt_snapshot wdt;
	struct cpu_regs regs;
	struct arm64_vcpu_guest_ctx gctx;
};

struct hwtdbg_live_mailbox {
	uint64_t publish_version;
	struct hwtdbg_vcpu_snapshot result;
	uint64_t completed_sequence;
} __aligned(64);

struct hwtdbg_live_request {
	uint16_t vm_id;
	uint64_t sequence;
};

/* [20260718] Watchdog timeout evidence ownership:
 *
 *   WDT transition -> reserve invalid event -> durable VM/vCPU snapshot
 *       -> publish live request generation
 *       -> one bounded SMP try-call over the target pCPU mask
 *       -> remote callbacks publish odd/even-versioned per-pCPU mailboxes
 *       -> copy stable matching generations -> virtio snapshot
 *       -> checksum -> write barrier -> publish valid
 *
 * Recovery never waits for the shell. The shell copies one valid event while
 * holding hwtdbg_lock, then formats the private readback without any lock.
 * A late callback can update only its mailbox, never a reused or published
 * event slot.
 */
static struct hwtdbg_event hwtdbg_events[HWTDBG_VM_NUM][HWTDBG_EVENT_SLOTS];
static uint8_t hwtdbg_next_slot[HWTDBG_VM_NUM];
static uint64_t hwtdbg_next_sequence;
static spinlock_t hwtdbg_lock = { .head = 0U, .tail = 0U, };
static struct hwtdbg_live_mailbox hwtdbg_mailbox[MAX_PCPU_NUM];
static struct hwtdbg_live_request hwtdbg_live_request;
static struct hwtdbg_event hwtdbg_readback;
static struct hwtdbg_ras_event
	hwtdbg_ras_events[MAX_PCPU_NUM][HWTDBG_RAS_EVENT_SLOTS];
static uint8_t hwtdbg_ras_next_slot[MAX_PCPU_NUM];
static uint64_t hwtdbg_ras_next_sequence;
static struct hwtdbg_ras_event hwtdbg_ras_readback;
static struct hwtdbg_fault_event
	hwtdbg_fault_events[CONFIG_MAX_VM_NUM][HWTDBG_FAULT_EVENT_SLOTS];
static uint8_t hwtdbg_fault_next_slot[CONFIG_MAX_VM_NUM];
static spinlock_t hwtdbg_fault_locks[CONFIG_MAX_VM_NUM];
static uint64_t hwtdbg_fault_next_sequence;
static struct hwtdbg_fault_event hwtdbg_fault_readback;
static struct vm_crash_record vm_crash_readback[BEAU_VM_CRASH_HISTORY_SLOTS];

static bool hwtdbg_range_contains(uint64_t start, uint64_t end,
	uint64_t address, uint64_t bytes)
{
	return (address >= start) && (address < end) && (bytes <= (end - address));
}

static bool hwtdbg_text_address(uint64_t address)
{
	return (address >= (uint64_t)&_text_start) &&
		(address < (uint64_t)&_text_end);
}

/* [20260721] HWTDBG stack unwind stop contract
 *
 * captured frame -> validate alignment and readable range -> read next frame
 *       |                         |                         |
 *       |                         +--> stop with evidence    +--> continue
 *       v
 * preserve every frame accepted before the stop reason
 *
 * Stop labels identify the first boundary that prevented a further read:
 *   - complete: the initial frame, next FP, or stack-bottom marker ends the
 *     chain normally;
 *   - empty: neither an initial FP nor LR was available;
 *   - misaligned: FP cannot name an AArch64 frame record;
 *   - gva-unavailable/copy-failed: guest translation or frame copy failed;
 *   - outside: SP or FP is outside the recorded host stack bounds;
 *   - nonmonotonic: the next FP would revisit or move below a prior frame;
 *   - lr-outside: a nonzero host LR is not in BEAU text;
 *   - depth-limit: the bounded snapshot reached HWTDBG_STACK_DEPTH.
 *
 * Key rule:
 *   - the snapshot owns only copies of validated frame records;
 *   - a failed validation keeps earlier evidence but never dereferences the
 *     failing frame, preventing an invalid guest or host stack read.
 */
static const char *hwtdbg_stack_stop_str(enum hwtdbg_stack_stop stop)
{
	const char *str;

	switch (stop) {
	case HWTDBG_STACK_COMPLETE:
		str = "complete";
		break;
	case HWTDBG_STACK_EMPTY:
		str = "empty";
		break;
	case HWTDBG_STACK_MISALIGNED:
		str = "misaligned";
		break;
	case HWTDBG_STACK_TRANSLATE_FAILED:
		str = "gva-unavailable";
		break;
	case HWTDBG_STACK_COPY_FAILED:
		str = "copy-failed";
		break;
	case HWTDBG_STACK_OUTSIDE:
		str = "outside";
		break;
	case HWTDBG_STACK_ORDER:
		str = "nonmonotonic";
		break;
	case HWTDBG_STACK_LR_OUTSIDE:
		str = "lr-outside";
		break;
	case HWTDBG_STACK_DEPTH_LIMIT:
		str = "depth-limit";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static int32_t hwtdbg_translate_live_gva(struct acrn_vcpu *vcpu,
	uint64_t gva, uint64_t *gpa)
{
	uint64_t old_par;
	uint64_t par;
	int32_t ret = -EINVAL;

	if ((vcpu == NULL) || (gpa == NULL) ||
		(get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return ret;
	}

	old_par = read_par_el1();
	arm64_at_s1e1r(gva);
	par = read_par_el1();
	write_par_el1(old_par);
	if ((par & PAR_EL1_F) == 0UL) {
		*gpa = (par & PAR_EL1_PA_MASK) | (gva & (PAGE_SIZE - 1UL));
		ret = 0;
	}

	return ret;
}

static void hwtdbg_capture_guest_stack(struct acrn_vcpu *vcpu,
	const struct cpu_regs *regs, bool live, struct hwtdbg_stack_snapshot *stack)
{
	struct hwtdbg_frame frame;
	uint64_t fp = regs->x29;
	uint64_t lr = regs->lr;
	uint32_t index;

	stack->live = live;
	stack->stop = HWTDBG_STACK_EMPTY;
	if ((fp == 0UL) && (lr == 0UL)) {
		return;
	}

	for (index = 0U; index < HWTDBG_STACK_DEPTH; index++) {
		uint64_t gpa = fp;

		stack->frame[stack->count].fp = fp;
		stack->frame[stack->count].lr = lr;
		stack->count++;
		if (fp == 0UL) {
			stack->stop = HWTDBG_STACK_COMPLETE;
			return;
		}
		if ((fp & (CPU_STACK_ALIGN - 1UL)) != 0UL) {
			stack->stop = HWTDBG_STACK_MISALIGNED;
			return;
		}
		if (live) {
			if (hwtdbg_translate_live_gva(vcpu, fp, &gpa) != 0) {
				stack->stop = HWTDBG_STACK_TRANSLATE_FAILED;
				return;
			}
		} else if (!arm64_guest_gpa_range_valid(vcpu->vm, fp, sizeof(frame))) {
			stack->stop = HWTDBG_STACK_TRANSLATE_FAILED;
			return;
		}
		if (copy_from_gpa(vcpu->vm, &frame, gpa, sizeof(frame)) != 0) {
			stack->stop = HWTDBG_STACK_COPY_FAILED;
			return;
		}
		if (frame.fp == 0UL) {
			stack->stop = HWTDBG_STACK_COMPLETE;
			return;
		}
		if (frame.fp <= fp) {
			stack->stop = HWTDBG_STACK_ORDER;
			return;
		}
		fp = frame.fp;
		lr = frame.lr;
	}
	stack->stop = HWTDBG_STACK_DEPTH_LIMIT;
}

static void hwtdbg_unwind_host_stack(uint64_t sp, uint64_t fp, uint64_t lr,
	uint64_t stack_start, uint64_t stack_end, bool live,
	struct hwtdbg_stack_snapshot *stack)
{
	uint32_t index;

	stack->live = live;
	stack->stop = HWTDBG_STACK_OUTSIDE;
	if (!hwtdbg_range_contains(stack_start, stack_end, sp, sizeof(uint64_t))) {
		return;
	}
	if ((fp == 0UL) && (lr == 0UL)) {
		stack->stop = HWTDBG_STACK_EMPTY;
		return;
	}

	for (index = 0U; index < HWTDBG_STACK_DEPTH; index++) {
		const struct hwtdbg_frame *frame;
		uint64_t next_fp;

		stack->frame[stack->count].fp = fp;
		stack->frame[stack->count].lr = lr;
		stack->count++;
		if ((lr != 0UL) && !hwtdbg_text_address(lr)) {
			stack->stop = HWTDBG_STACK_LR_OUTSIDE;
			return;
		}
		if (fp == 0UL) {
			stack->stop = HWTDBG_STACK_COMPLETE;
			return;
		}
		if ((fp & (CPU_STACK_ALIGN - 1UL)) != 0UL) {
			stack->stop = HWTDBG_STACK_MISALIGNED;
			return;
		}
		if ((fp < sp) || !hwtdbg_range_contains(stack_start, stack_end,
			fp, sizeof(*frame))) {
			stack->stop = HWTDBG_STACK_OUTSIDE;
			return;
		}
		frame = (const struct hwtdbg_frame *)fp;
		next_fp = frame->fp;
		lr = frame->lr;
		if ((next_fp == 0UL) || (next_fp == SP_BOTTOM_MAGIC)) {
			stack->stop = HWTDBG_STACK_COMPLETE;
			return;
		}
		if (next_fp <= fp) {
			stack->stop = HWTDBG_STACK_ORDER;
			return;
		}
		fp = next_fp;
	}
	stack->stop = HWTDBG_STACK_DEPTH_LIMIT;
}

static void hwtdbg_capture_saved_host_stack(const struct acrn_vcpu *vcpu,
	struct hwtdbg_stack_snapshot *stack)
{
	const struct stack_frame *frame;
	uint64_t stack_start = vcpu->thread_obj.host_stack_base;
	uint64_t stack_end = stack_start + vcpu->thread_obj.host_stack_size;

	if ((stack_start == 0UL) || (vcpu->thread_obj.host_stack_size == 0UL)) {
		stack_start = (uint64_t)&vcpu->stack[0];
		stack_end = (uint64_t)&vcpu->stack[CONFIG_STACK_SIZE];
	}
	if (!hwtdbg_range_contains(stack_start, stack_end,
		vcpu->thread_obj.host_sp, sizeof(*frame))) {
		stack->stop = HWTDBG_STACK_OUTSIDE;
		return;
	}
	frame = (const struct stack_frame *)vcpu->thread_obj.host_sp;
	hwtdbg_unwind_host_stack(vcpu->thread_obj.host_sp, frame->x29,
		frame->lr, stack_start, stack_end, false, stack);
}

static void hwtdbg_capture_live_host_stack(const struct acrn_vcpu *vcpu,
	struct hwtdbg_stack_snapshot *stack)
{
	uint64_t stack_start = vcpu->thread_obj.host_stack_base;
	uint64_t stack_end = stack_start + vcpu->thread_obj.host_stack_size;
	uint64_t sp;
	uint64_t fp;
	uint64_t lr;

	if ((stack_start == 0UL) || (vcpu->thread_obj.host_stack_size == 0UL)) {
		stack_start = (uint64_t)&vcpu->stack[0];
		stack_end = (uint64_t)&vcpu->stack[CONFIG_STACK_SIZE];
	}
	asm volatile ("mov %0, sp" : "=r" (sp));
	asm volatile ("mov %0, x29" : "=r" (fp));
	asm volatile ("mov %0, x30" : "=r" (lr));
	hwtdbg_unwind_host_stack(sp, fp, lr, stack_start, stack_end, true, stack);
}

static void hwtdbg_snapshot_pcpu_owner(const struct acrn_vcpu *vcpu,
	struct hwtdbg_vcpu_snapshot *snapshot)
{
	struct thread_object *owner;

	if (snapshot->pcpu_id >= MAX_PCPU_NUM) {
		return;
	}

	snapshot->pcpu_valid = true;
	owner = sched_get_current(snapshot->pcpu_id);
	snapshot->pcpu_need_reschedule = need_reschedule(snapshot->pcpu_id);
	if (owner == NULL) {
		return;
	}

	snapshot->pcpu_owner_present = true;
	snapshot->pcpu_owner_state = owner->status;
	snapshot->pcpu_owner_is_vcpu = owner->is_vcpu;
	snapshot->pcpu_owner_vm_id = owner->vm_id;
	snapshot->pcpu_owner_vcpu_id = owner->vcpu_id;
	(void)memcpy_s(snapshot->pcpu_owner_name,
		sizeof(snapshot->pcpu_owner_name), owner->name,
		sizeof(snapshot->pcpu_owner_name) - 1U);
	snapshot->pcpu_owner_name[sizeof(snapshot->pcpu_owner_name) - 1U] = '\0';
	snapshot->current = (snapshot->vcpu_state != VCPU_OFFLINE) &&
		(owner == &vcpu->thread_obj);
}

static void hwtdbg_capture_vcpu_durable(struct acrn_vcpu *vcpu,
	struct hwtdbg_vcpu_snapshot *snapshot)
{
	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->vcpu_id = vcpu->vcpu_id;
	snapshot->pcpu_id = vcpu->thread_obj.pcpu_id;
	snapshot->vcpu_state = vcpu_get_state(vcpu);
	snapshot->thread_state = vcpu->thread_obj.status;
	hwtdbg_snapshot_pcpu_owner(vcpu, snapshot);
	sched_get_latency(&vcpu->thread_obj, &snapshot->latency);
	(void)memcpy_s(&snapshot->regs, sizeof(snapshot->regs),
		&vcpu->arch.regs, sizeof(vcpu->arch.regs));
	(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
		&vcpu->arch.gctx, sizeof(vcpu->arch.gctx));
	snapshot->pending_req = vcpu->pending_req;
	snapshot->irqs_pending = vcpu->arch.irqs_pending;
	snapshot->irqs_pending_mask = vcpu->arch.irqs_pending_mask;
	if (snapshot->vcpu_state != VCPU_OFFLINE) {
		hwtdbg_capture_guest_stack(vcpu, &snapshot->regs, false,
			&snapshot->guest_stack);
		hwtdbg_capture_saved_host_stack(vcpu, &snapshot->host_stack);
	}
}

static void hwtdbg_capture_vcpu_live(struct acrn_vcpu *vcpu,
	struct hwtdbg_vcpu_snapshot *snapshot)
{
	hwtdbg_capture_vcpu_durable(vcpu, snapshot);
	(void)memset(&snapshot->guest_stack, 0U, sizeof(snapshot->guest_stack));
	(void)memset(&snapshot->host_stack, 0U, sizeof(snapshot->host_stack));
	hwtdbg_capture_guest_stack(vcpu, &snapshot->regs, true,
		&snapshot->guest_stack);
	hwtdbg_capture_live_host_stack(vcpu, &snapshot->host_stack);
	snapshot->live = true;
}

static void hwtdbg_live_capture_callback(__unused void *data)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct hwtdbg_live_mailbox *mailbox;
	struct acrn_vcpu *vcpu;
	uint64_t publish_version;
	uint64_t sequence;
	uint16_t vm_id;

	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}
	sequence = __atomic_load_n(&hwtdbg_live_request.sequence, __ATOMIC_ACQUIRE);
	vm_id = __atomic_load_n(&hwtdbg_live_request.vm_id, __ATOMIC_ACQUIRE);
	if ((sequence == 0UL) ||
		(sequence != __atomic_load_n(&hwtdbg_live_request.sequence,
			__ATOMIC_ACQUIRE))) {
		return;
	}
	vcpu = get_running_vcpu(pcpu_id);
	if ((vcpu == NULL) || (vcpu->vm == NULL) ||
		(vcpu->vm->vm_id != vm_id)) {
		return;
	}

	mailbox = &hwtdbg_mailbox[pcpu_id];
	publish_version = (__atomic_load_n(&mailbox->publish_version,
		__ATOMIC_RELAXED) + 1UL) | 1UL;
	__atomic_store_n(&mailbox->publish_version, publish_version,
		__ATOMIC_RELEASE);
	cpu_write_memory_barrier();
	hwtdbg_capture_vcpu_live(vcpu, &mailbox->result);
	cpu_write_memory_barrier();
	__atomic_store_n(&mailbox->completed_sequence, sequence, __ATOMIC_RELEASE);
	cpu_write_memory_barrier();
	__atomic_store_n(&mailbox->publish_version, publish_version + 1UL,
		__ATOMIC_RELEASE);
}

static uint32_t hwtdbg_checksum(const struct hwtdbg_event *event)
{
	const uint8_t *bytes = (const uint8_t *)event;
	uint32_t checksum = 2166136261U;
	uint32_t index;

	for (index = 0U; index < sizeof(*event); index++) {
		bool checksum_byte = (index >= offsetof(struct hwtdbg_event, checksum)) &&
			(index < (offsetof(struct hwtdbg_event, checksum) +
			 sizeof(event->checksum)));
		bool valid_byte = (index >= offsetof(struct hwtdbg_event, valid)) &&
			(index < (offsetof(struct hwtdbg_event, valid) +
			 sizeof(event->valid)));

		if (!checksum_byte && !valid_byte) {
			checksum ^= bytes[index];
			checksum *= 16777619U;
		}
	}

	return checksum;
}

static uint32_t hwtdbg_ras_checksum(const struct hwtdbg_ras_event *event)
{
	const uint8_t *bytes = (const uint8_t *)event;
	uint32_t checksum = 2166136261U;
	uint32_t index;

	for (index = 0U; index < sizeof(*event); index++) {
		bool checksum_byte = (index >= offsetof(struct hwtdbg_ras_event, checksum)) &&
			(index < (offsetof(struct hwtdbg_ras_event, checksum) +
			 sizeof(event->checksum)));
		bool valid_byte = (index >= offsetof(struct hwtdbg_ras_event, valid)) &&
			(index < (offsetof(struct hwtdbg_ras_event, valid) +
			 sizeof(event->valid)));

		if (!checksum_byte && !valid_byte) {
			checksum ^= bytes[index];
			checksum *= 16777619U;
		}
	}

	return checksum;
}

static uint32_t hwtdbg_fault_checksum(const struct hwtdbg_fault_event *event)
{
	const uint8_t *bytes = (const uint8_t *)event;
	uint32_t checksum = 2166136261U;
	uint32_t index;

	for (index = 0U; index < sizeof(*event); index++) {
		bool checksum_byte = (index >= offsetof(struct hwtdbg_fault_event, checksum)) &&
			(index < (offsetof(struct hwtdbg_fault_event, checksum) +
			 sizeof(event->checksum)));
		bool valid_byte = (index >= offsetof(struct hwtdbg_fault_event, valid)) &&
			(index < (offsetof(struct hwtdbg_fault_event, valid) +
			 sizeof(event->valid)));

		if (!checksum_byte && !valid_byte) {
			checksum ^= bytes[index];
			checksum *= 16777619U;
		}
	}

	return checksum;
}

static struct hwtdbg_ras_event *hwtdbg_reserve_ras_event(uint16_t pcpu_id,
	uint64_t *sequence)
{
	struct hwtdbg_ras_event *event;
	uint8_t slot;

	if ((pcpu_id >= MAX_PCPU_NUM) || (sequence == NULL)) {
		return NULL;
	}
	slot = hwtdbg_ras_next_slot[pcpu_id];
	hwtdbg_ras_next_slot[pcpu_id] = (uint8_t)((slot + 1U) %
		HWTDBG_RAS_EVENT_SLOTS);
	*sequence = __atomic_add_fetch(&hwtdbg_ras_next_sequence, 1UL,
		__ATOMIC_RELAXED);
	if (*sequence == 0UL) {
		*sequence = __atomic_add_fetch(&hwtdbg_ras_next_sequence, 1UL,
			__ATOMIC_RELAXED);
	}
	event = &hwtdbg_ras_events[pcpu_id][slot];
	__atomic_store_n(&event->valid, false, __ATOMIC_RELEASE);
	cpu_write_memory_barrier();
	(void)memset(event, 0U, sizeof(*event));
	return event;
}

static void hwtdbg_publish_ras_event(struct hwtdbg_ras_event *event)
{
	event->checksum = 0U;
	event->checksum = hwtdbg_ras_checksum(event);
	cpu_write_memory_barrier();
	__atomic_store_n(&event->valid, true, __ATOMIC_RELEASE);
}

static struct hwtdbg_event *hwtdbg_reserve_event(uint16_t vm_id,
	uint64_t *sequence)
{
	struct hwtdbg_event *event;
	uint64_t rflags;
	uint8_t slot;

	spinlock_irqsave_obtain(&hwtdbg_lock, &rflags);
	slot = hwtdbg_next_slot[vm_id];
	hwtdbg_next_slot[vm_id] = (uint8_t)((slot + 1U) % HWTDBG_EVENT_SLOTS);
	hwtdbg_next_sequence++;
	if (hwtdbg_next_sequence == 0UL) {
		hwtdbg_next_sequence++;
	}
	*sequence = hwtdbg_next_sequence;
	event = &hwtdbg_events[vm_id][slot];
	event->valid = false;
	cpu_write_memory_barrier();
	spinlock_irqrestore_release(&hwtdbg_lock, rflags);

	(void)memset(event, 0U, sizeof(*event));
	return event;
}

static struct hwtdbg_fault_event *hwtdbg_reserve_fault_event(uint16_t vm_id,
	uint64_t *sequence)
{
	struct hwtdbg_fault_event *event;
	uint8_t slot;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (sequence == NULL)) {
		return NULL;
	}

	slot = hwtdbg_fault_next_slot[vm_id];
	hwtdbg_fault_next_slot[vm_id] = (uint8_t)((slot + 1U) %
		HWTDBG_FAULT_EVENT_SLOTS);
	*sequence = __atomic_add_fetch(&hwtdbg_fault_next_sequence, 1UL,
		__ATOMIC_RELAXED);
	if (*sequence == 0UL) {
		*sequence = __atomic_add_fetch(&hwtdbg_fault_next_sequence, 1UL,
			__ATOMIC_RELAXED);
	}
	event = &hwtdbg_fault_events[vm_id][slot];
	__atomic_store_n(&event->valid, false, __ATOMIC_RELEASE);
	cpu_write_memory_barrier();

	(void)memset(event, 0U, sizeof(*event));
	return event;
}

/* [20260723] Guest fault evidence publication
 *
 * synchronous guest exit
 *     |
 *     v
 * copy only durable EL2-owned vCPU state
 *     |
 *     +--> no guest-memory read, no remote sampling, no recovery action
 *     |
 *     v
 * checksum -> release-publish valid fault slot -> pause failing vCPU
 *
 * Key rule:
 *   - the exit CPU records the frame before pausing its vCPU, so the shell
 *     observes the failing EL1 context rather than a later reset context;
 *   - the fault ring is diagnostic-only and never changes watchdog ownership
 *     or VM restart policy.
 */
void hwtdbg_capture_guest_fault(struct acrn_vcpu *vcpu,
	enum hwtdbg_guest_fault_reason reason, int32_t exit_ret)
{
	struct hwtdbg_fault_event *event;
	uint64_t sequence;
	uint64_t rflags;
	uint16_t vm_id;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return;
	}

	vm_id = vcpu->vm->vm_id;
	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return;
	}
	spinlock_irqsave_obtain(&hwtdbg_fault_locks[vm_id], &rflags);
	event = hwtdbg_reserve_fault_event(vm_id, &sequence);
	if (event == NULL) {
		spinlock_irqrestore_release(&hwtdbg_fault_locks[vm_id], rflags);
		return;
	}

	event->magic = HWTDBG_FAULT_MAGIC;
	event->version = HWTDBG_FAULT_VERSION;
	event->vm_id = vcpu->vm->vm_id;
	event->vcpu_id = vcpu->vcpu_id;
	event->pcpu_id = get_pcpu_id();
	event->reason = reason;
	event->exit_ret = exit_ret;
	event->sequence = sequence;
	event->captured_tsc = cpu_ticks();
	event->vm_state = vcpu->vm->state;
	event->vcpu_state = vcpu_get_state(vcpu);
	event->thread_state = vcpu->thread_obj.status;
	event->wdt_valid = vm_wdt_get_snapshot(event->vm_id, &event->wdt) == 0;
	(void)memcpy_s(&event->regs, sizeof(event->regs),
		&vcpu->arch.regs, sizeof(vcpu->arch.regs));
	(void)memcpy_s(&event->gctx, sizeof(event->gctx),
		&vcpu->arch.gctx, sizeof(vcpu->arch.gctx));

	event->checksum = hwtdbg_fault_checksum(event);
	cpu_write_memory_barrier();
	__atomic_store_n(&event->valid, true, __ATOMIC_RELEASE);
	spinlock_irqrestore_release(&hwtdbg_fault_locks[vm_id], rflags);
}

static void hwtdbg_capture_virtio(uint16_t vm_id,
	struct hwtdbg_virtio_snapshot *snapshot)
{
	uint16_t count = virtio_proxy_device_count(vm_id);
	uint16_t index;

	snapshot->device_count = count;
	for (index = 0U; index < count; index++) {
		struct virtio_proxy_stats stats;

		if (!virtio_proxy_get_stats(vm_id, index, &stats)) {
			continue;
		}
		snapshot->timeout_count += stats.timeout_count;
		snapshot->completed_count += stats.completed_count;
		snapshot->notify_count += stats.notify_count;
		snapshot->pending_active += stats.pending_active;
		snapshot->pending_limit += stats.pending_limit;
		if (!stats.backend_healthy) {
			snapshot->unhealthy_count++;
		}
		if (stats.last_hcall_ret != 0) {
			snapshot->error_count++;
		}
	}
}

static uint64_t hwtdbg_build_live_mask(struct acrn_vm *vm,
	const struct hwtdbg_event *event)
{
	uint64_t mask = 0UL;
	uint16_t vcpu_id;

	for (vcpu_id = 0U; vcpu_id < event->vcpu_count; vcpu_id++) {
		struct acrn_vcpu *vcpu = vcpu_from_vid(vm, vcpu_id);
		uint16_t pcpu_id = vcpu->thread_obj.pcpu_id;

		if (is_vcpu_running(vcpu) && (pcpu_id < MAX_PCPU_NUM) &&
			(sched_get_current(pcpu_id) == &vcpu->thread_obj)) {
			mask |= 1UL << pcpu_id;
		}
	}

	return mask;
}

static void hwtdbg_copy_live_results(struct hwtdbg_event *event)
{
	struct hwtdbg_vcpu_snapshot result;
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		struct hwtdbg_live_mailbox *mailbox = &hwtdbg_mailbox[pcpu_id];
		uint64_t bit = 1UL << pcpu_id;
		uint64_t version_before;
		uint64_t version_after;

		if ((event->live_requested_mask & bit) == 0UL) {
			continue;
		}
		version_before = __atomic_load_n(&mailbox->publish_version,
			__ATOMIC_ACQUIRE);
		if (((version_before & 1UL) != 0UL) ||
			(__atomic_load_n(&mailbox->completed_sequence,
				__ATOMIC_ACQUIRE) != event->sequence)) {
			continue;
		}
		(void)memcpy_s(&result, sizeof(result), &mailbox->result,
			sizeof(mailbox->result));
		cpu_read_memory_barrier();
		version_after = __atomic_load_n(&mailbox->publish_version,
			__ATOMIC_ACQUIRE);
		if ((version_before != version_after) ||
			((version_after & 1UL) != 0UL) ||
			(__atomic_load_n(&mailbox->completed_sequence,
				__ATOMIC_ACQUIRE) != event->sequence)) {
			continue;
		}
		if ((result.vcpu_id < event->vcpu_count) &&
			(result.pcpu_id == pcpu_id)) {
			(void)memcpy_s(&event->vcpu[result.vcpu_id],
				sizeof(event->vcpu[result.vcpu_id]),
				&result, sizeof(result));
			event->live_captured_mask |= bit;
		}
	}
	event->live_timeout_mask = event->live_requested_mask &
		~event->live_captured_mask;
}

static void hwtdbg_publish_event(struct hwtdbg_event *event)
{
	uint64_t rflags;

	event->checksum = 0U;
	event->checksum = hwtdbg_checksum(event);
	spinlock_irqsave_obtain(&hwtdbg_lock, &rflags);
	cpu_write_memory_barrier();
	event->valid = true;
	spinlock_irqrestore_release(&hwtdbg_lock, rflags);
}

/* [20260721] RAS evidence publication
 *
 * SError frame + PE-local RAS state -> validate active guest VTTBR
 *                                      |
 *                                      +--> optional static PA-to-GPA proof
 *       |
 *       v
 * per-pCPU invalid slot -> checksum -> release-publish valid slot
 *
 * Key rule:
 *   - the interrupted pCPU owns its RAS ring slot and never waits on the WDT
 *     lock, so SError capture cannot deadlock behind normal diagnostics;
 *   - VMID and GPA are published only after current-vCPU and reversible RAM
 *     mapping validation, preventing host addresses from being blamed on a VM.
 */
void hwtdbg_capture_ras(struct acrn_vcpu *vcpu, const struct cpu_regs *regs,
	const struct arm64_ras_snapshot *snapshot)
{
	struct hwtdbg_ras_event *event;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t sequence;
	uint32_t index;

	if ((regs == NULL) || (snapshot == NULL) ||
		((snapshot->flags & ARM64_RAS_SNAPSHOT_SUPPORTED) == 0U)) {
		return;
	}
	event = hwtdbg_reserve_ras_event(pcpu_id, &sequence);
	if (event == NULL) {
		return;
	}
	event->magic = HWTDBG_RAS_MAGIC;
	event->version = HWTDBG_VERSION;
	event->pcpu_id = pcpu_id;
	event->vm_id = HWTDBG_INVALID_ID;
	event->vcpu_id = HWTDBG_INVALID_ID;
	event->hw_vmid = HWTDBG_INVALID_ID;
	event->sequence = sequence;
	event->captured_tsc = cpu_ticks();
	event->erridr = snapshot->erridr;
	event->disr = snapshot->disr;
	event->ras_flags = snapshot->flags;
	event->record_count = snapshot->record_count;
	event->valid_count = snapshot->valid_count;
	(void)memcpy_s(&event->regs, sizeof(event->regs), regs, sizeof(*regs));

	if ((vcpu != NULL) && (vcpu->vm != NULL) &&
		(vcpu->vm->root_stg2ptp != NULL) &&
		(get_running_vcpu(pcpu_id) == vcpu)) {
		event->vttbr = read_vttbr_el2();
		if (event->vttbr == arm64_stage2_vttbr(vcpu->vm)) {
			event->guest_context = true;
			event->vm_id = vcpu->vm->vm_id;
			event->vcpu_id = vcpu->vcpu_id;
			event->hw_vmid = (uint16_t)((event->vttbr >>
				ARM64_STAGE2_VMID_SHIFT) & ARM64_STAGE2_VMID_MASK);
			hwtdbg_capture_vcpu_live(vcpu, &event->vcpu);
		}
	}

	for (index = 0U; index < snapshot->valid_count; index++) {
		const struct arm64_ras_record *record = &snapshot->record[index];
		struct hwtdbg_ras_record *saved = &event->record[index];

		saved->status = record->status;
		saved->pa = record->address;
		saved->misc0 = record->misc0;
		saved->index = record->index;
		saved->flags = record->flags;
		if (event->guest_context &&
			((record->flags & ARM64_RAS_RECORD_ADDRESS_VALID) != 0U) &&
			arm64_guest_hpa_to_gpa(vcpu->vm, record->address, &saved->gpa)) {
			saved->flags |= HWTDBG_RAS_RECORD_GPA_VALID;
		}
	}
	hwtdbg_publish_ras_event(event);
}

uint64_t hwtdbg_capture_timeout(uint16_t vm_id,
	const struct hwtdbg_timeout_context *timeout)
{
	struct hwtdbg_event *event;
	struct acrn_vm *vm;
	uint64_t remote_mask;
	uint64_t sequence;
	uint16_t vcpu_id;
	int32_t live_status = 0;

	if ((timeout == NULL) || (vm_id >= HWTDBG_VM_NUM)) {
		return 0UL;
	}
	vm = get_vm_from_vmid(vm_id);
	if (vm == NULL) {
		return 0UL;
	}

	event = hwtdbg_reserve_event(vm_id, &sequence);
	event->magic = HWTDBG_MAGIC;
	event->version = HWTDBG_VERSION;
	event->vm_id = vm_id;
	event->sequence = sequence;
	event->captured_tsc = cpu_ticks();
	event->timeout = *timeout;
	event->vm_state = vm->state;
	event->vcpu_count = (vm->hw.created_vcpus < MAX_VCPUS_PER_VM) ?
		vm->hw.created_vcpus : MAX_VCPUS_PER_VM;
	(void)strncpy_s(event->vm_name, sizeof(event->vm_name), vm->name,
		sizeof(event->vm_name));
	event->recovery = timeout->restart_enabled &&
		(timeout->restart_count >= CONFIG_VM_WDT_RESTART_MAX) ?
		HWTDBG_RECOVERY_EXHAUSTED : HWTDBG_RECOVERY_NOT_REQUESTED;

	for (vcpu_id = 0U; vcpu_id < event->vcpu_count; vcpu_id++) {
		hwtdbg_capture_vcpu_durable(vcpu_from_vid(vm, vcpu_id),
			&event->vcpu[vcpu_id]);
	}

	event->live_requested_mask = hwtdbg_build_live_mask(vm, event);
	__atomic_store_n(&hwtdbg_live_request.vm_id, vm_id, __ATOMIC_RELEASE);
	__atomic_store_n(&hwtdbg_live_request.sequence, sequence, __ATOMIC_RELEASE);
	if ((event->live_requested_mask & (1UL << get_pcpu_id())) != 0UL) {
		hwtdbg_live_capture_callback(NULL);
	}
	remote_mask = event->live_requested_mask & ~(1UL << get_pcpu_id());
	if (remote_mask != 0UL) {
		live_status = smp_try_call_function_timeout(remote_mask,
			hwtdbg_live_capture_callback, NULL, HWTDBG_LIVE_TIMEOUT_US);
		if (live_status == -EBUSY) {
			event->capture_flags |= HWTDBG_CAPTURE_LIVE_BUSY;
		} else if (live_status == -ETIMEDOUT) {
			event->capture_flags |= HWTDBG_CAPTURE_LIVE_TIMEOUT;
		}
	}
	hwtdbg_copy_live_results(event);

	hwtdbg_capture_virtio(vm_id, &event->virtio);
	hwtdbg_publish_event(event);

	return sequence;
}

void hwtdbg_update_recovery(uint16_t vm_id, uint64_t sequence,
	enum hwtdbg_recovery_result result, uint64_t attempt,
	uint64_t wait_vcpus, int32_t reset_ret)
{
	uint64_t rflags;
	uint32_t slot;

	if ((vm_id >= HWTDBG_VM_NUM) || (sequence == 0UL)) {
		return;
	}

	spinlock_irqsave_obtain(&hwtdbg_lock, &rflags);
	for (slot = 0U; slot < HWTDBG_EVENT_SLOTS; slot++) {
		struct hwtdbg_event *event = &hwtdbg_events[vm_id][slot];

		if (event->valid && (event->magic == HWTDBG_MAGIC) &&
			(event->sequence == sequence)) {
			event->valid = false;
			cpu_write_memory_barrier();
			event->recovery = result;
			if (attempt != 0UL) {
				event->recovery_attempt = attempt;
			}
			event->recovery_wait_vcpus = wait_vcpus;
			event->reset_ret = reset_ret;
			event->recovery_updated_tsc = cpu_ticks();
			event->checksum = 0U;
			event->checksum = hwtdbg_checksum(event);
			cpu_write_memory_barrier();
			event->valid = true;
			break;
		}
	}
	spinlock_irqrestore_release(&hwtdbg_lock, rflags);
}

static const char *hwtdbg_yes_no(bool value)
{
	return value ? "Y" : "N";
}

static const char *hwtdbg_timeout_kind_str(enum hwtdbg_timeout_kind kind)
{
	return (kind == HWTDBG_TIMEOUT_FIRST_KICK) ? "1st-kick" : "runtime";
}

/* [20260721] HWTDBG watchdog cause contract
 *
 * watchdog samples -> classify the timeout transition -> freeze HWTDBG event
 *       |                       |
 *       |                       +--> cause is diagnostic evidence only
 *       v
 * core/vm_wdt.c retains recovery ownership
 *
 * Cause labels describe the evidence selected by the watchdog: heartbeat is
 * a received kick, timeout is an expired heartbeat age, vcpustall is a
 * runnable vCPU delayed beyond the watchdog threshold, irqstorm is excessive
 * IRQ progress, console is a persistently undrained console queue, and virtio
 * is persistently stalled proxy work. N/A represents no classified cause.
 *
 * Key rule:
 *   - core/vm_wdt.c owns sampling and recovery decisions;
 *   - HWTDBG records the selected cause without deriving a new policy, so
 *     shell diagnostics cannot alter timeout or VM recovery behavior.
 */
static const char *hwtdbg_cause_str(enum vm_wdt_cause cause)
{
	const char *str;

	switch (cause) {
	case VM_WDT_CAUSE_HEARTBEAT:
		str = "heartbeat";
		break;
	case VM_WDT_CAUSE_TIMEOUT:
		str = "timeout";
		break;
	case VM_WDT_CAUSE_VCPU_STALL:
		str = "vcpustall";
		break;
	case VM_WDT_CAUSE_IRQ_STORM:
		str = "irqstorm";
		break;
	case VM_WDT_CAUSE_CONSOLE_STUCK:
		str = "console";
		break;
	case VM_WDT_CAUSE_VIRTIO_STUCK:
		str = "virtio";
		break;
	case VM_WDT_CAUSE_NONE:
	default:
		str = "N/A";
		break;
	}

	return str;
}

/* [20260721] HWTDBG recovery result contract
 *
 * timeout event -> quiescing -> resetting -> launched -> verified
 *                    |             |           |           |
 *                    +--> timeout  +--> failed +--> late kick or timeout
 *                    +--> invalid state or attempt budget exhausted
 *
 * not-requested means recovery was disabled; quiescing waits for vCPU
 * acknowledgement; resetting records the reset request; launched records a
 * successful reset launch; verified records the post-reset heartbeat. A late
 * kick, exhausted attempt budget, invalid VM state, reset failure, quiesce
 * timeout, or verification timeout is a terminal diagnostic outcome.
 *
 * Key rule:
 *   - core/vm_wdt.c owns recovery state and updates this event by sequence;
 *   - HWTDBG publishes each result only after its durable event exists, so a
 *     delayed recovery update cannot create or modify unrelated timeout data.
 */
static const char *hwtdbg_recovery_str(enum hwtdbg_recovery_result result)
{
	const char *str;

	switch (result) {
	case HWTDBG_RECOVERY_QUIESCING:
		str = "quiescing";
		break;
	case HWTDBG_RECOVERY_RESETTING:
		str = "resetting";
		break;
	case HWTDBG_RECOVERY_LAUNCHED:
		str = "launched";
		break;
	case HWTDBG_RECOVERY_VERIFIED:
		str = "verified";
		break;
	case HWTDBG_RECOVERY_LATE_KICK:
		str = "late-kick";
		break;
	case HWTDBG_RECOVERY_EXHAUSTED:
		str = "exhausted";
		break;
	case HWTDBG_RECOVERY_INVALID_STATE:
		str = "invalid-state";
		break;
	case HWTDBG_RECOVERY_RESET_FAILED:
		str = "reset-failed";
		break;
	case HWTDBG_RECOVERY_QUIESCE_TIMEOUT:
		str = "quiesce-timeout";
		break;
	case HWTDBG_RECOVERY_VERIFY_TIMEOUT:
		str = "verify-timeout";
		break;
	case HWTDBG_RECOVERY_NOT_REQUESTED:
	default:
		str = "not-requested";
		break;
	}

	return str;
}

static const char *hwtdbg_thread_state_str(enum thread_object_state state)
{
	const char *str;

	switch (state) {
	case THREAD_STS_RUNNING:
		str = "running";
		break;
	case THREAD_STS_RUNNABLE:
		str = "runnable";
		break;
	case THREAD_STS_BLOCKED:
		str = "blocked";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *hwtdbg_vm_state_str(enum vm_state state)
{
	const char *str;

	switch (state) {
	case VM_POWERED_OFF:
		str = "poweroff";
		break;
	case VM_CREATED:
		str = "created";
		break;
	case VM_RUNNING:
		str = "running";
		break;
	case VM_READY_TO_POWEROFF:
		str = "ready-off";
		break;
	case VM_PAUSED:
		str = "paused";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static void hwtdbg_format_reg(char *buf, size_t size, const char *name,
	uint64_t value)
{
	(void)snprintf(buf, size, HWTDBG_REG_KEY_FMT, name, value);
}

static void hwtdbg_reg_line(uint32_t count, ...)
{
	char reg[HWTDBG_REGS_PER_LINE_MAX][32U];
	va_list args;
	uint32_t index;

	if ((count == 0U) || (count > HWTDBG_REGS_PER_LINE_MAX)) {
		return;
	}
	va_start(args, count);
	for (index = 0U; index < count; index++) {
		const char *name = __builtin_va_arg(args, const char *);
		uint64_t value = __builtin_va_arg(args, uint64_t);

		hwtdbg_format_reg(reg[index], sizeof(reg[index]), name, value);
	}
	va_end(args);

	if (count == 1U) {
		shell_item_line("%s", reg[0U]);
	} else if (count == 2U) {
		shell_item_line("%s %s", reg[0U], reg[1U]);
	} else if (count == 3U) {
		shell_item_line("%s %s %s", reg[0U], reg[1U], reg[2U]);
	} else {
		shell_item_line("%s %s %s %s", reg[0U], reg[1U], reg[2U], reg[3U]);
	}
	shell_output_checkpoint();
}

static void hwtdbg_print_regs(const struct cpu_regs *regs)
{
	hwtdbg_reg_line(3U, "elr", regs->elr, "spsr", regs->spsr,
		"esr", regs->esr);
	hwtdbg_reg_line(2U, "far", regs->far, "hpfar", regs->hpfar);
	hwtdbg_reg_line(4U, "x00", regs->x0, "x01", regs->x1,
		"x02", regs->x2, "x03", regs->x3);
	hwtdbg_reg_line(4U, "x04", regs->x4, "x05", regs->x5,
		"x06", regs->x6, "x07", regs->x7);
	hwtdbg_reg_line(4U, "x08", regs->x8, "x09", regs->x9,
		"x10", regs->x10, "x11", regs->x11);
	hwtdbg_reg_line(4U, "x12", regs->x12, "x13", regs->x13,
		"x14", regs->x14, "x15", regs->x15);
	hwtdbg_reg_line(4U, "x16", regs->x16, "x17", regs->x17,
		"x18", regs->x18, "x19", regs->x19);
	hwtdbg_reg_line(4U, "x20", regs->x20, "x21", regs->x21,
		"x22", regs->x22, "x23", regs->x23);
	hwtdbg_reg_line(4U, "x24", regs->x24, "x25", regs->x25,
		"x26", regs->x26, "x27", regs->x27);
	hwtdbg_reg_line(4U, "x28", regs->x28, "x29", regs->x29,
		"lr", regs->lr, "sp", regs->sp);
}

/* [20260722] HWT minidump guest-context boundary
 *
 * vCPU durable register image + saved EL1/EL2 context
 *     |
 *     v
 * checksum-published WDT event -> shell readback
 *
 * Key rule:
 *   - the WDT event owns a copied gctx, never a pointer to mutable vCPU state;
 *   - gctx is the saved vCPU image and is labelled as such, preventing a
 *     running guest's live EL1 hardware state from being misrepresented as a
 *     globally frozen dump;
 *   - bounded register-only capture avoids guest-memory reads in the timeout
 *     path, so diagnostics cannot delay or compromise WDT recovery.
 */
static void hwtdbg_print_minidump_text(const char *label, const char *value)
{
	shell_item_line("%-18s : %s", label, value);
}

static void hwtdbg_print_minidump_u64(const char *label, uint64_t value)
{
	shell_item_line("%-18s : 0x%016lx", label, value);
}

static void hwtdbg_print_minidump_u32(const char *label, uint32_t value)
{
	shell_item_line("%-18s : 0x%08x", label, value);
}

static void hwtdbg_print_gctx(const struct arm64_vcpu_guest_ctx *gctx)
{
	hwtdbg_print_minidump_text("SOURCE", "saved-vcpu-context");
	hwtdbg_print_minidump_u64("EL2.VTTBR", gctx->vttbr_el2);
	hwtdbg_print_minidump_u64("EL2.VTCR", gctx->vtcr_el2);
	hwtdbg_print_minidump_u64("EL2.HCR", gctx->hcr_el2);
	hwtdbg_print_minidump_u64("EL2.CNTVOFF", gctx->cntvoff_el2);
	hwtdbg_print_minidump_u64("TIMER.CNTP_CVAL", gctx->cntp_cval_el0);
	hwtdbg_print_minidump_u32("TIMER.CNTP_CTL", gctx->cntp_ctl_el0);
	hwtdbg_print_minidump_u64("TIMER.CNTV_CVAL", gctx->cntv_cval_el0);
	hwtdbg_print_minidump_u32("TIMER.CNTV_CTL", gctx->cntv_ctl_el0);
	hwtdbg_print_minidump_u32("TIMER.VIRQ", gctx->timer_virq);
	hwtdbg_print_minidump_text("TIMER.CNTV_MASKED",
		hwtdbg_yes_no(gctx->cntv_el2_masked));
	hwtdbg_print_minidump_u64("EL1.SCTLR", gctx->sctlr_el1);
	hwtdbg_print_minidump_u64("EL1.TTBR0", gctx->ttbr0_el1);
	hwtdbg_print_minidump_u64("EL1.TTBR1", gctx->ttbr1_el1);
	hwtdbg_print_minidump_u64("EL1.TCR", gctx->tcr_el1);
	hwtdbg_print_minidump_u64("EL1.MAIR", gctx->mair_el1);
	hwtdbg_print_minidump_u64("EL1.AMAIR", gctx->amair_el1);
	hwtdbg_print_minidump_u64("EL1.VBAR", gctx->vbar_el1);
	hwtdbg_print_minidump_u64("EL1.CONTEXTIDR", gctx->contextidr_el1);
	hwtdbg_print_minidump_u64("EL1.CPACR", gctx->cpacr_el1);
	hwtdbg_print_minidump_u64("EL1.TPIDR_EL0", gctx->tpidr_el0);
	hwtdbg_print_minidump_u64("EL1.TPIDRRO_EL0", gctx->tpidrro_el0);
	hwtdbg_print_minidump_u64("EL1.TPIDR_EL1", gctx->tpidr_el1);
	hwtdbg_print_minidump_u64("EL1.SP_EL0", gctx->sp_el0);
	hwtdbg_print_minidump_u64("EL1.ELR", gctx->elr_el1);
	hwtdbg_print_minidump_u64("EL1.SPSR", gctx->spsr_el1);
	hwtdbg_print_minidump_u64("EL1.ESR", gctx->esr_el1);
	hwtdbg_print_minidump_u64("EL1.FAR", gctx->far_el1);
}

static void hwtdbg_print_stack(const struct hwtdbg_stack_snapshot *stack,
	bool symbolize)
{
	uint32_t index;

	shell_item_line("source:%s count:%u stop:%s",
		stack->live ? "live" : "durable", stack->count,
		hwtdbg_stack_stop_str(stack->stop));
	for (index = 0U; index < stack->count; index++) {
		if (symbolize) {
			char symbol[96U];

			dbg_format_symbol(stack->frame[index].lr, symbol, sizeof(symbol));
			shell_item_line("[%02u] fp:0x%016lx lr:0x%016lx %s",
				index, stack->frame[index].fp,
				stack->frame[index].lr, symbol);
		} else {
			shell_item_line("[%02u] fp:0x%016lx lr:0x%016lx",
				index, stack->frame[index].fp,
				stack->frame[index].lr);
		}
		shell_output_checkpoint();
	}
}

static void hwtdbg_print_vcpu(const struct hwtdbg_vcpu_snapshot *snapshot)
{
	shell_item_section("✔  DUMP vcpu %hu", snapshot->vcpu_id);
	shell_item_line("pcpu:%hu lifecycle:%8s thread:%s current:%s live:%s",
		snapshot->pcpu_id,
		vcpu_state_to_str(snapshot->vcpu_state),
		hwtdbg_thread_state_str(snapshot->thread_state),
		hwtdbg_yes_no(snapshot->current), hwtdbg_yes_no(snapshot->live));
	if (!snapshot->pcpu_valid) {
		shell_item_line("pcpu-owner:invalid-pcpu state:N/A vm:N/A vcpu:N/A resched:N");
	} else if (!snapshot->pcpu_owner_present) {
		shell_item_line("pcpu-owner:none state:N/A vm:N/A vcpu:N/A resched:%s",
			hwtdbg_yes_no(snapshot->pcpu_need_reschedule));
	} else if (snapshot->pcpu_owner_is_vcpu) {
		shell_item_line("pcpu-owner:%s state:%s vm:%hu vcpu:%hu resched:%s",
			snapshot->pcpu_owner_name,
			hwtdbg_thread_state_str(snapshot->pcpu_owner_state),
			snapshot->pcpu_owner_vm_id, snapshot->pcpu_owner_vcpu_id,
			hwtdbg_yes_no(snapshot->pcpu_need_reschedule));
	} else {
		shell_item_line("pcpu-owner:%s state:%s vm:N/A vcpu:N/A resched:%s",
			snapshot->pcpu_owner_name,
			hwtdbg_thread_state_str(snapshot->pcpu_owner_state),
			hwtdbg_yes_no(snapshot->pcpu_need_reschedule));
	}
	shell_item_line("sched:switches:%lu runtime.us:%lu wait.us:last:%lu max:%lu runnable-since:%lu",
		snapshot->latency.switches,
		ticks_to_us(snapshot->latency.runtime_ticks),
		ticks_to_us(snapshot->latency.last_wait_ticks),
		ticks_to_us(snapshot->latency.max_wait_ticks),
		snapshot->latency.runnable_since);
	shell_item_line("requests:pending:0x%016lx arch-irqs:0x%016lx mask:0x%016lx",
		snapshot->pending_req, snapshot->irqs_pending,
		snapshot->irqs_pending_mask);
	shell_item_section("minidump EL1/EL2 context");
	hwtdbg_print_gctx(&snapshot->gctx);
	shell_item_section("minidump guest regs");
	hwtdbg_print_regs(&snapshot->regs);
	shell_item_section("minidump guest stack");
	hwtdbg_print_stack(&snapshot->guest_stack, false);
	shell_item_section("minidump host stack");
	hwtdbg_print_stack(&snapshot->host_stack, true);
}

static bool hwtdbg_copy_latest(uint16_t vm_id,
	struct hwtdbg_event *event, bool *corrupt)
{
	const struct hwtdbg_event *selected = NULL;
	uint64_t rflags;
	uint32_t slot;

	if ((event == NULL) || (corrupt == NULL) || (vm_id >= HWTDBG_VM_NUM)) {
		return false;
	}
	*corrupt = false;
	spinlock_irqsave_obtain(&hwtdbg_lock, &rflags);
	for (slot = 0U; slot < HWTDBG_EVENT_SLOTS; slot++) {
		const struct hwtdbg_event *candidate = &hwtdbg_events[vm_id][slot];

		if (candidate->valid &&
			((selected == NULL) ||
			 (candidate->sequence > selected->sequence))) {
			selected = candidate;
		}
	}
	if (selected != NULL) {
		(void)memcpy_s(event, sizeof(*event), selected, sizeof(*selected));
	}
	spinlock_irqrestore_release(&hwtdbg_lock, rflags);

	if (selected == NULL) {
		return false;
	}
	if ((event->magic != HWTDBG_MAGIC) || (event->version != HWTDBG_VERSION) ||
		(event->checksum != hwtdbg_checksum(event))) {
		*corrupt = true;
	}

	return true;
}

static void hwtdbg_print_event(const struct hwtdbg_event *event)
{
	uint16_t vcpu_id;

	shell_item_begin("HWTDBG (vm%hu seq:%lu)", event->vm_id, event->sequence);
	shell_item_line("timeout:%s cause:%s threshold:%ums age:%lums detected:0x%016lx heartbeat:0x%016lx",
		hwtdbg_timeout_kind_str(event->timeout.kind),
		hwtdbg_cause_str(event->timeout.cause), event->timeout.timeout_ms,
		event->timeout.age_ms, event->timeout.detected_tsc,
		event->timeout.heartbeat_tsc);
	shell_item_line("wdt:timeouts:%lu token:0x%016lx restarts:%lu failed:%lu",
		event->timeout.timeout_count,
		event->timeout.last_token, event->timeout.restart_count,
		event->timeout.restart_fail_count);
	shell_item_line("capture:valid flags:0x%08x tsc:0x%016lx",
		event->capture_flags, event->captured_tsc);
	shell_item_line("live:req:0x%016lx captured:0x%016lx timeout:0x%016lx budget:%uus",
		event->live_requested_mask, event->live_captured_mask,
		event->live_timeout_mask, HWTDBG_LIVE_TIMEOUT_US);
	shell_item_line("recovery:%s enabled:%s attempt:%lu/%u wait:0x%016lx reset-ret:%d updated:0x%016lx",
		hwtdbg_recovery_str(event->recovery),
		hwtdbg_yes_no(event->timeout.restart_enabled),
		event->recovery_attempt, CONFIG_VM_WDT_RESTART_MAX,
		event->recovery_wait_vcpus, event->reset_ret,
		event->recovery_updated_tsc);
	shell_item_section("MINIDUMP");
	shell_item_line("format:%u scope:bounded-regs-context-stacks-sched-irq-virtio guest-memory:not-captured",
		event->version);
	shell_item_section("vm/vcpu");
	shell_item_line("vm:%s state:%s vcpus:%hu",
		event->vm_name, hwtdbg_vm_state_str(event->vm_state),
		event->vcpu_count);
	for (vcpu_id = 0U; vcpu_id < event->vcpu_count; vcpu_id++) {
		hwtdbg_print_vcpu(&event->vcpu[vcpu_id]);
		shell_output_checkpoint();
	}
	shell_item_section("irq/virtio");
	shell_item_line("irq:total:%lu delta:%lu", event->timeout.irq_total,
		event->timeout.irq_delta);
	shell_item_line("virtio:devices:%hu pending:%hu/%hu timeout:%lu completed:%lu notify:%lu unhealthy:%hu errors:%lu",
		event->virtio.device_count, event->virtio.pending_active,
		event->virtio.pending_limit, event->virtio.timeout_count,
		event->virtio.completed_count, event->virtio.notify_count,
		event->virtio.unhealthy_count, event->virtio.error_count);
	shell_item_end();
}

static const char *hwtdbg_fault_reason_str(enum hwtdbg_guest_fault_reason reason)
{
	switch (reason) {
	case HWTDBG_GUEST_FAULT_IABT:
		return "instruction-abort";
	case HWTDBG_GUEST_FAULT_SERROR:
		return "serror";
	default:
		return "unhandled-exit";
	}
}

static bool hwtdbg_copy_latest_fault(uint16_t vm_id,
	struct hwtdbg_fault_event *event, bool *corrupt)
{
	const struct hwtdbg_fault_event *selected = NULL;
	uint64_t rflags;
	uint32_t slot;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (event == NULL) || (corrupt == NULL)) {
		return false;
	}
	*corrupt = false;
	spinlock_irqsave_obtain(&hwtdbg_fault_locks[vm_id], &rflags);
	for (slot = 0U; slot < HWTDBG_FAULT_EVENT_SLOTS; slot++) {
		const struct hwtdbg_fault_event *candidate =
			&hwtdbg_fault_events[vm_id][slot];

		if (__atomic_load_n(&candidate->valid, __ATOMIC_ACQUIRE) &&
			((selected == NULL) || (candidate->sequence > selected->sequence))) {
			selected = candidate;
		}
	}
	if (selected != NULL) {
		(void)memcpy_s(event, sizeof(*event), selected, sizeof(*selected));
	}
	spinlock_irqrestore_release(&hwtdbg_fault_locks[vm_id], rflags);
	if (selected == NULL) {
		return false;
	}
	if ((event->magic != HWTDBG_FAULT_MAGIC) ||
		(event->version != HWTDBG_FAULT_VERSION) || (event->vm_id != vm_id) ||
		(event->checksum != hwtdbg_fault_checksum(event))) {
		*corrupt = true;
	}
	return true;
}

static void hwtdbg_erase_fault(uint16_t vm_id)
{
	uint64_t rflags;
	uint32_t slot;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return;
	}
	spinlock_irqsave_obtain(&hwtdbg_fault_locks[vm_id], &rflags);
	for (slot = 0U; slot < HWTDBG_FAULT_EVENT_SLOTS; slot++) {
		__atomic_store_n(&hwtdbg_fault_events[vm_id][slot].valid, false,
			__ATOMIC_RELEASE);
	}
	spinlock_irqrestore_release(&hwtdbg_fault_locks[vm_id], rflags);
}

static void hwtdbg_print_fault_event(const struct hwtdbg_fault_event *event)
{
	shell_item_begin("CRASH (vm%hu seq:%lu)", event->vm_id, event->sequence);
	shell_item_line("reason:%s exit-ret:%d captured:0x%016lx pcpu:%hu",
		hwtdbg_fault_reason_str(event->reason), event->exit_ret,
		event->captured_tsc, event->pcpu_id);
	shell_item_line("state:vm:%s vcpu%hu:%s thread:%s",
		hwtdbg_vm_state_str(event->vm_state), event->vcpu_id,
		vcpu_state_to_str(event->vcpu_state),
		hwtdbg_thread_state_str(event->thread_state));
	shell_item_line("exit:ec:0x%02lx esr:0x%016lx elr:0x%016lx far:0x%016lx hpfar:0x%016lx",
		ESR_EL2_EC(event->regs.esr), event->regs.esr, event->regs.elr,
		event->regs.far, event->regs.hpfar);
	if (event->wdt_valid) {
		shell_item_line("wdt:status:%u cause:%u timeout:%lu restart:%lu pending:%s",
			event->wdt.status, event->wdt.cause,
			event->wdt.timeout_count, event->wdt.restart_count,
			hwtdbg_yes_no(event->wdt.restart_pending));
	} else {
		shell_item_line("wdt:unavailable");
	}
	shell_item_section("GUEST CONTEXT");
	hwtdbg_print_gctx(&event->gctx);
	hwtdbg_print_regs(&event->regs);
	shell_item_end();
}

static bool hwtdbg_copy_ras_event(uint16_t pcpu_id, uint32_t slot,
	struct hwtdbg_ras_event *event)
{
	const struct hwtdbg_ras_event *source;

	if ((pcpu_id >= MAX_PCPU_NUM) || (slot >= HWTDBG_RAS_EVENT_SLOTS) ||
		(event == NULL)) {
		return false;
	}
	source = &hwtdbg_ras_events[pcpu_id][slot];
	if (!__atomic_load_n(&source->valid, __ATOMIC_ACQUIRE)) {
		return false;
	}
	(void)memcpy_s(event, sizeof(*event), source, sizeof(*source));
	cpu_read_memory_barrier();
	return __atomic_load_n(&source->valid, __ATOMIC_ACQUIRE) &&
		(event->magic == HWTDBG_RAS_MAGIC) &&
		(event->version == HWTDBG_VERSION) &&
		(event->checksum == hwtdbg_ras_checksum(event));
}

static void hwtdbg_print_ras_event(const struct hwtdbg_ras_event *event)
{
	uint16_t index;

	shell_item_begin("HWTDBG RAS (pcpu%hu seq:%lu)", event->pcpu_id,
		event->sequence);
	shell_item_line("ras:erridr:0x%016lx disr:0x%016lx flags:0x%08x records:%u valid:%hu",
		event->erridr, event->disr, event->ras_flags, event->record_count,
		event->valid_count);
	if (event->guest_context) {
		shell_item_line("guest:vm:%hu vcpu:%hu vmid:%hu vttbr:0x%016lx",
			event->vm_id, event->vcpu_id, event->hw_vmid, event->vttbr);
		shell_item_section("vm/vcpu");
		hwtdbg_print_vcpu(&event->vcpu);
	} else {
		shell_item_line("guest:unavailable vm:N/A vcpu:N/A vmid:N/A");
		shell_item_line("exception regs:");
		hwtdbg_print_regs(&event->regs);
	}
	shell_item_section("ras records");
	for (index = 0U; index < event->valid_count; index++) {
		const struct hwtdbg_ras_record *record = &event->record[index];

		if ((record->flags & ARM64_RAS_RECORD_ADDRESS_VALID) == 0U) {
			shell_item_line("err%hu:status:0x%016lx pa:N/A gpa:N/A misc0:0x%016lx",
				record->index, record->status, record->misc0);
		} else if ((record->flags & HWTDBG_RAS_RECORD_GPA_VALID) == 0U) {
			shell_item_line("err%hu:status:0x%016lx pa:0x%016lx gpa:N/A misc0:0x%016lx",
				record->index, record->status, record->pa, record->misc0);
		} else {
			shell_item_line("err%hu:status:0x%016lx pa:0x%016lx gpa:0x%016lx misc0:0x%016lx",
				record->index, record->status, record->pa, record->gpa,
				record->misc0);
		}
	}
	shell_item_end();
}

static bool hwtdbg_parse_vmid(const char *text, uint16_t limit, uint16_t *vm_id)
{
	uint32_t value = 0U;
	uint32_t index;

	if ((text == NULL) || (vm_id == NULL) || (text[0] == '\0')) {
		return false;
	}
	for (index = 0U; text[index] != '\0'; index++) {
		uint8_t ch = (uint8_t)text[index];

		if ((ch < (uint8_t)'0') || (ch > (uint8_t)'9') ||
			(value > ((0xffffU - (uint32_t)(ch - (uint8_t)'0')) / 10U))) {
			return false;
		}
		value = (value * 10U) + (uint32_t)(ch - (uint8_t)'0');
	}
	if (value >= limit) {
		return false;
	}
	*vm_id = (uint16_t)value;

	return true;
}

int32_t shell_hwtdbg(int32_t argc, char **argv)
{
	uint16_t vm_id;
	uint16_t pcpu_id;
	bool corrupt;

	if ((argc != 2) || !hwtdbg_parse_vmid(argv[1], HWTDBG_VM_NUM, &vm_id)) {
		shell_puts("usage: hwtdbg <vmid>\r\n");
		return -EINVAL;
	}
	if (!hwtdbg_copy_latest(vm_id, &hwtdbg_readback, &corrupt)) {
		char line[MAX_STR_SIZE];

		(void)snprintf(line, sizeof(line),
			"HWTDBG: VM%hu:no retained HWT event\r\n", vm_id);
		shell_puts(line);
		return 0;
	}
	if (corrupt) {
		char line[MAX_STR_SIZE];

		(void)snprintf(line, sizeof(line),
			"HWTDBG: vm%hu latest event is checksum-invalid\r\n", vm_id);
		shell_puts(line);
		return -EIO;
	}
	hwtdbg_print_event(&hwtdbg_readback);

	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		uint32_t slot;

		for (slot = 0U; slot < HWTDBG_RAS_EVENT_SLOTS; slot++) {
			if (hwtdbg_copy_ras_event(pcpu_id, slot,
				&hwtdbg_ras_readback) && hwtdbg_ras_readback.guest_context &&
				(hwtdbg_ras_readback.vm_id == vm_id)) {
				hwtdbg_print_ras_event(&hwtdbg_ras_readback);
			}
		}
	}
	return 0;
}

int32_t shell_crash(int32_t argc, char **argv)
{
	uint16_t vm_id;
	bool fault_corrupt;
	bool guest_corrupt;
	bool printed = false;
	uint32_t guest_count;
	uint32_t index;

	if (((argc != 2) && (argc != 3)) ||
		!hwtdbg_parse_vmid(argv[1], CONFIG_MAX_VM_NUM, &vm_id) ||
		((argc == 3) && (strcmp(argv[2], "erase") != 0))) {
		shell_puts("usage: crash <vmid> [erase]\r\n");
		return -EINVAL;
	}
	if (argc == 3) {
		vm_crash_erase(vm_id);
		hwtdbg_erase_fault(vm_id);
		shell_puts("CRASH: erased\r\n");
		return 0;
	}
	guest_count = vm_crash_copy_history(vm_id, vm_crash_readback,
		ARRAY_SIZE(vm_crash_readback), &guest_corrupt);
	for (index = 0U; index < guest_count; index++) {
		vm_crash_print(&vm_crash_readback[index], index, guest_count);
		printed = true;
	}
	if (guest_corrupt) {
		shell_puts("CRASH: checksum-invalid guest history entries skipped\r\n");
	}
	if (!hwtdbg_copy_latest_fault(vm_id, &hwtdbg_fault_readback, &fault_corrupt)) {
		char line[MAX_STR_SIZE];

		if (!printed) {
			(void)snprintf(line, sizeof(line),
				"CRASH: vm%hu has no retained guest fault event\r\n", vm_id);
			shell_puts(line);
		}
		return guest_corrupt ? -EIO : 0;
	}
	if (fault_corrupt) {
		shell_puts("CRASH: latest event is checksum-invalid\r\n");
		return -EIO;
	}
	hwtdbg_print_fault_event(&hwtdbg_fault_readback);
	return guest_corrupt ? -EIO : 0;
}
