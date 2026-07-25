/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <console.h>
#include <errno.h>
#include <irq.h>
#include <logmsg.h>
#include <per_cpu.h>
#include <rtl.h>
#include <schedule.h>
#include <spinlock.h>
#include <ticks.h>
#include <timer.h>
#include <vm.h>
#include <vcpu.h>
#include <vconfig.h>
#include <vm_wdt.h>
#include <virtio_proxy.h>
#include <hwtdbg.h>

/* [20260712] VM watchdog diagnosis model
 *
 * The watchdog is a BEAU OS service that correlates several liveness signals
 * instead of treating a missing guest heartbeat as the only failure mode.
 * Guest HVC kicks prove that the guest OS timer path made forward progress;
 * scheduler, IRQ, console, and virtio-proxy samples explain why progress may
 * have stopped.
 *
 *   guest OS timer callback
 *          |
 *          v
 *   HC_VM_WDT_KICK
 *          |
 *          v
 *   vm_wdt_entry.last_kick_tsc
 *          |
 *          v
 *   periodic BEAU watchdog thread
 *      |
 *      +-- heartbeat age     -> timeout
 *      +-- runnable vCPU age -> vCPU stall
 *      +-- IRQ sample delta  -> IRQ storm
 *      +-- console backlog   -> console stuck
 *      +-- proxy pending age -> virtio stuck
 *
 * Key rule:
 *   - guest OS code owns only the periodic kick source;
 *   - core/vm_wdt.c owns correlation, reporting, and recovery decisions;
 *   - device modules own evidence counters, so watchdog output remains a
 *     diagnosis attached to a controlled VM recovery action.
 *
 * Recovery rule:
 *   - only VMs in CONFIG_VM_WDT_RESTART_VM_MASK may be restarted;
 *   - a missing first kick is a timeout after CONFIG_VM_WDT_TIMEOUT_MS, so a
 *     guest which hangs during boot is recovered as well;
 *   - recovery first requests all vCPUs to block, then polls their scheduler
 *     acknowledgement from a CPU0-local one-shot timer; the WDT thread never
 *     waits synchronously for a remote pCPU;
 *   - once quiesced, the WDT thread publishes an owned reset request to the
 *     VM BSP pCPU; its idle thread reloads the boot payload and reports completion,
 *     so one slow image copy cannot delay recovery of another VM;
 *   - CONFIG_VM_WDT_RESTART_MAX bounds recovery attempts for one VM instance.
 */

#define VM_WDT_IRQ_STORM_PER_SEC	10000UL
#define VM_WDT_QUEUE_STUCK_PERIODS	2U
#define VM_WDT_STUCK_PERIOD_MAX		255U
#define VM_WDT_PENDING_TIMEOUT_NUM	4U
#define VM_WDT_RECOVERY_NOTICE_NUM	4U

struct vm_wdt_pending_timeout {
	struct hwtdbg_timeout_context context;
};

struct vm_wdt_recovery_notice {
	uint64_t sequence;
	uint64_t wait_vcpus;
	int32_t reset_ret;
	enum hwtdbg_recovery_result result;
};

struct vm_wdt_daemon_event {
	uint64_t kick_tsc;
	uint64_t gap_ticks;
	uint64_t token;
};

struct vm_wdt_entry {
	uint64_t start_tsc;
	uint64_t last_kick_tsc;
	uint64_t timeout_count;
	uint64_t last_token;
	struct vm_wdt_daemon_event daemon_event;
	uint64_t daemon_merged;
	uint64_t daemon_dropped;
	uint64_t last_irq_total;
	uint64_t last_irq_delta;
	uint64_t last_irq_sample_tsc;
	uint64_t last_console_drained;
	uint32_t last_console_queued;
	uint8_t console_stuck_periods;
	uint64_t last_virtio_timeout_count;
	uint16_t last_virtio_pending_active;
	uint8_t virtio_stuck_periods;
	uint64_t restart_count;
	uint64_t restart_fail_count;
	enum vm_wdt_recovery_state recovery_state;
	enum vm_wdt_cause recovery_cause;
	uint64_t recovery_start_tsc;
	uint64_t recovery_wait_vcpus;
	uint64_t recovery_quiesce_generation;
	int32_t reset_ret;
	uint64_t candidate_event_sequence;
	uint64_t recovery_event_sequence;
	struct vm_wdt_pending_timeout pending[VM_WDT_PENDING_TIMEOUT_NUM];
	uint8_t pending_head;
	uint8_t pending_count;
	struct vm_wdt_recovery_notice notice[VM_WDT_RECOVERY_NOTICE_NUM];
	uint8_t notice_head;
	uint8_t notice_count;
	uint64_t remaining_ticks;
	uint64_t suspend_epoch;
	uint64_t suspend_ticks;
	bool timeout_active;
	bool restart_pending;
	bool reset_complete;
	bool pm_suspended;
	bool heartbeat_started;
	bool daemon_pending;
};

static struct vm_wdt_entry vm_wdt_entries[CONFIG_MAX_VM_NUM];
static spinlock_t vm_wdt_lock = { .head = 0U, .tail = 0U, };
static struct thread_object vm_wdt_thread;
static uint8_t vm_wdt_stack[CONFIG_STACK_SIZE] __aligned(16);
static struct hv_timer vm_wdt_timer;
static struct hv_timer vm_wdt_recovery_timer;
static bool vm_wdt_started;
static uint64_t vm_wdt_suspend_epoch;
static uint64_t vm_wdt_suspend_ticks;
static uint64_t vm_wdt_next_quiesce_generation;
static bool vm_wdt_pm_suspended;

static bool vm_wdt_restart_enabled(uint16_t vm_id);

static bool vm_wdt_config_present(const struct acrn_vm_config *vm_config)
{
	return (vm_config->name[0] != '\0') || (vm_config->cpu_affinity != 0UL) ||
		((vm_config->guest_flags & GUEST_FLAG_STATIC_VM) != 0UL);
}

static uint64_t vm_wdt_elapsed_ticks(uint64_t now, uint64_t since)
{
	return (now >= since) ? (now - since) : 0UL;
}

static uint64_t vm_wdt_heartbeat_age_ticks(uint64_t now, const struct vm_wdt_entry *entry)
{
	uint64_t since = !entry->heartbeat_started ? entry->start_tsc : entry->last_kick_tsc;

	return vm_wdt_elapsed_ticks(now, since);
}

static bool vm_wdt_heartbeat_started(const struct vm_wdt_entry *entry)
{
	return (entry != NULL) && entry->heartbeat_started;
}

static bool vm_wdt_is_pm_suspended(void)
{
	uint64_t rflags;
	bool suspended;

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	suspended = vm_wdt_pm_suspended;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return suspended;
}

static bool vm_wdt_vm_is_pm_suspended(uint16_t vm_id)
{
	uint64_t rflags;
	bool suspended = false;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	suspended = vm_wdt_entries[vm_id].pm_suspended;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return suspended;
}

static bool vm_wdt_is_timeout_age(uint64_t age_ticks)
{
	return age_ticks > ((uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS);
}

static void vm_wdt_queue_timeout_locked(uint16_t vm_id,
	struct vm_wdt_entry *entry, uint64_t now)
{
	struct vm_wdt_pending_timeout *pending;
	uint8_t slot;

	if (entry->pending_count == VM_WDT_PENDING_TIMEOUT_NUM) {
		entry->pending_head = (uint8_t)((entry->pending_head + 1U) %
			VM_WDT_PENDING_TIMEOUT_NUM);
		entry->pending_count--;
	}
	slot = (uint8_t)((entry->pending_head + entry->pending_count) %
		VM_WDT_PENDING_TIMEOUT_NUM);
	pending = &entry->pending[slot];
	(void)memset(pending, 0U, sizeof(*pending));
	pending->context.kind = !entry->heartbeat_started ?
		HWTDBG_TIMEOUT_FIRST_KICK : HWTDBG_TIMEOUT_RUNTIME;
	pending->context.cause = VM_WDT_CAUSE_TIMEOUT;
	pending->context.detected_tsc = now;
	pending->context.heartbeat_tsc = !entry->heartbeat_started ?
		entry->start_tsc : entry->last_kick_tsc;
	pending->context.age_ms = ticks_to_ms(vm_wdt_heartbeat_age_ticks(now,
		entry));
	pending->context.timeout_count = entry->timeout_count;
	pending->context.restart_count = entry->restart_count;
	pending->context.restart_fail_count = entry->restart_fail_count;
	pending->context.last_token = entry->last_token;
	pending->context.irq_total = entry->last_irq_total;
	pending->context.irq_delta = entry->last_irq_delta;
	pending->context.timeout_ms = CONFIG_VM_WDT_TIMEOUT_MS;
	pending->context.restart_enabled = vm_wdt_restart_enabled(vm_id);
	entry->pending_count++;
}

static bool vm_wdt_record_timeout_locked(uint16_t vm_id,
	struct vm_wdt_entry *entry, bool timed_out, uint64_t now)
{
	bool transitioned = false;

	if (timed_out) {
		if (!entry->timeout_active) {
			entry->timeout_count++;
			entry->timeout_active = true;
			vm_wdt_queue_timeout_locked(vm_id, entry, now);
			transitioned = true;
		}
	} else {
		entry->timeout_active = false;
	}

	return transitioned;
}

/* [20260724] WDT daemon event ownership
 *
 * HVC heartbeat -> protected display slot -> vm-wdt thread -> daemon_log
 *                         |
 *                         +--> replace prior display-only event and count merge
 *
 * Key rule:
 *   - the WDT lock publishes heartbeat state before its display copy;
 *   - the one-entry slot is lossy only for console presentation;
 *   - timeout, capture, and recovery state never consume this slot.
 */
static void vm_wdt_queue_daemon_event_locked(struct vm_wdt_entry *entry,
	uint64_t kick_tsc, uint64_t gap_ticks)
{
	if (entry->daemon_pending) {
		entry->daemon_merged++;
	}
	entry->daemon_event.kick_tsc = kick_tsc;
	entry->daemon_event.gap_ticks = gap_ticks;
	entry->daemon_event.token = entry->last_token;
	entry->daemon_pending = true;
}

static bool vm_wdt_claim_daemon_event(uint16_t vm_id,
	struct vm_wdt_daemon_event *event)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool claimed = false;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (event == NULL)) {
		return false;
	}
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->daemon_pending) {
		*event = entry->daemon_event;
		entry->daemon_pending = false;
		claimed = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return claimed;
}

static void vm_wdt_record_daemon_drop(uint16_t vm_id)
{
	uint64_t rflags;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return;
	}
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	vm_wdt_entries[vm_id].daemon_dropped++;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
}

static void vm_wdt_queue_recovery_notice_locked(struct vm_wdt_entry *entry,
	uint64_t sequence, enum hwtdbg_recovery_result result,
	uint64_t wait_vcpus, int32_t reset_ret)
{
	struct vm_wdt_recovery_notice *notice;
	uint8_t slot;

	if (sequence == 0UL) {
		return;
	}
	if (entry->notice_count == VM_WDT_RECOVERY_NOTICE_NUM) {
		entry->notice_head = (uint8_t)((entry->notice_head + 1U) %
			VM_WDT_RECOVERY_NOTICE_NUM);
		entry->notice_count--;
	}
	slot = (uint8_t)((entry->notice_head + entry->notice_count) %
		VM_WDT_RECOVERY_NOTICE_NUM);
	notice = &entry->notice[slot];
	notice->sequence = sequence;
	notice->result = result;
	notice->wait_vcpus = wait_vcpus;
	notice->reset_ret = reset_ret;
	entry->notice_count++;
}

static bool vm_wdt_claim_recovery_notice(uint16_t vm_id,
	struct vm_wdt_recovery_notice *notice)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool claimed = false;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (notice == NULL)) {
		return false;
	}
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->notice_count != 0U) {
		*notice = entry->notice[entry->notice_head];
		entry->notice_head = (uint8_t)((entry->notice_head + 1U) %
			VM_WDT_RECOVERY_NOTICE_NUM);
		entry->notice_count--;
		claimed = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return claimed;
}

static void vm_wdt_flush_recovery_notices(uint16_t vm_id)
{
	uint32_t index;

	for (index = 0U; index < VM_WDT_RECOVERY_NOTICE_NUM; index++) {
		struct vm_wdt_recovery_notice notice;

		if (!vm_wdt_claim_recovery_notice(vm_id, &notice)) {
			break;
		}
		hwtdbg_update_recovery(vm_id, notice.sequence, notice.result,
			0UL, notice.wait_vcpus, notice.reset_ret);
	}
}

static const char *vm_wdt_cause_str(enum vm_wdt_cause cause)
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

static const char *vm_wdt_name(uint16_t vm_id)
{
	const struct acrn_vm_config *vm_config = get_vm_config(vm_id);
	const struct acrn_vm *vm = get_vm_from_vmid(vm_id);
	const char *name = "";

	if ((vm != NULL) && (vm->name[0] != '\0')) {
		name = vm->name;
	} else if (vm_config->name[0] != '\0') {
		name = vm_config->name;
	}

	return name;
}

static bool vm_wdt_detect_vcpu_stall(const struct acrn_vm *vm)
{
	uint64_t threshold_ticks = (uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS;
	uint16_t vcpu_id;

	if (vm == NULL) {
		return false;
	}

	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		const struct acrn_vcpu *vcpu = vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);
		struct sched_latency_stats latency = { 0U };

		if ((vcpu == NULL) || (vcpu_get_state(vcpu) == VCPU_OFFLINE)) {
			continue;
		}
		sched_get_latency(&vcpu->thread_obj, &latency);
		if ((vcpu->thread_obj.status == THREAD_STS_RUNNABLE) &&
			(latency.runnable_since != 0UL) &&
			((cpu_ticks() - latency.runnable_since) > threshold_ticks)) {
			return true;
		}
	}

	return false;
}

static bool vm_wdt_is_monitored(uint16_t vm_id)
{
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;

	return vm_id < max_vm_id;
}

static bool vm_wdt_restart_enabled(uint16_t vm_id)
{
	return (vm_id < 64U) && vm_wdt_is_monitored(vm_id) &&
		((CONFIG_VM_WDT_RESTART_VM_MASK & (1UL << vm_id)) != 0UL);
}

static bool vm_wdt_claim_pending_timeout(uint16_t vm_id,
	const struct vm_wdt_snapshot *snapshot,
	struct hwtdbg_timeout_context *context)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool claimed = false;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (context == NULL)) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->pending_count != 0U) {
		*context = entry->pending[entry->pending_head].context;
		entry->pending_head = (uint8_t)((entry->pending_head + 1U) %
			VM_WDT_PENDING_TIMEOUT_NUM);
		entry->pending_count--;
		if ((snapshot != NULL) &&
			(snapshot->status == VM_WDT_STATUS_STUCK) &&
			(snapshot->timeout_count == context->timeout_count)) {
			context->cause = snapshot->cause;
			context->irq_total = snapshot->irq_total;
			context->irq_delta = snapshot->irq_delta;
		}
		claimed = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return claimed;
}

static bool vm_wdt_bind_captured_event(uint16_t vm_id,
	const struct hwtdbg_timeout_context *context, uint64_t sequence)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool current = false;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (context == NULL) ||
		(sequence == 0UL)) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->timeout_active &&
		(entry->timeout_count == context->timeout_count)) {
		entry->candidate_event_sequence = sequence;
		current = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return current;
}

static void vm_wdt_capture_pending_timeouts(uint16_t vm_id,
	const struct vm_wdt_snapshot *snapshot)
{
	uint32_t index;

	for (index = 0U; index < VM_WDT_PENDING_TIMEOUT_NUM; index++) {
		struct hwtdbg_timeout_context context;
		uint64_t sequence;

		if (!vm_wdt_claim_pending_timeout(vm_id, snapshot, &context)) {
			break;
		}
		sequence = hwtdbg_capture_timeout(vm_id, &context);
		if ((sequence != 0UL) &&
			!vm_wdt_bind_captured_event(vm_id, &context, sequence)) {
			hwtdbg_update_recovery(vm_id, sequence,
				HWTDBG_RECOVERY_LATE_KICK, 0UL, 0UL, 0);
		}
	}
}

static uint64_t vm_wdt_irq_total(void)
{
	uint64_t total = 0UL;
	uint16_t pcpu_id;
	uint32_t irq;

	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		for (irq = 0U; irq < NR_IRQS; irq++) {
			total += per_cpu(irq_count, pcpu_id)[irq];
		}
	}

	return total;
}

static bool vm_wdt_detect_irq_storm_locked(struct vm_wdt_entry *entry, uint64_t now)
{
	uint64_t total = vm_wdt_irq_total();
	uint64_t delta = 0UL;
	uint64_t period_ticks;
	uint64_t limit;
	bool storm = false;

	if ((entry->last_irq_sample_tsc != 0UL) && (now > entry->last_irq_sample_tsc)) {
		delta = total - entry->last_irq_total;
		period_ticks = now - entry->last_irq_sample_tsc;
		limit = (VM_WDT_IRQ_STORM_PER_SEC * period_ticks) /
			(TICKS_PER_MS * 1000UL);
		storm = delta > limit;
	}

	entry->last_irq_total = total;
	entry->last_irq_delta = (entry->last_irq_sample_tsc == 0UL) ? 0UL : delta;
	entry->last_irq_sample_tsc = now;

	return storm;
}

static bool vm_wdt_detect_console_stuck_locked(uint16_t vm_id, struct vm_wdt_entry *entry)
{
	struct console_vm_ring_stats stats = { 0U };
	bool valid;
	bool stuck = false;

	valid = console_vm_ring_get_stats(vm_id, &stats);
	if (!valid || !stats.vuart_bound || !stats.pending || (stats.queued == 0U)) {
		entry->console_stuck_periods = 0U;
	} else if ((stats.queued >= entry->last_console_queued) &&
		(stats.drained_bytes == entry->last_console_drained)) {
		if (entry->console_stuck_periods < VM_WDT_STUCK_PERIOD_MAX) {
			entry->console_stuck_periods++;
		}
		stuck = entry->console_stuck_periods >= VM_WDT_QUEUE_STUCK_PERIODS;
	} else {
		entry->console_stuck_periods = 0U;
	}

	if (valid && (stats.vmid == vm_id)) {
		entry->last_console_queued = stats.queued;
		entry->last_console_drained = stats.drained_bytes;
	}

	return stuck;
}

static bool vm_wdt_detect_virtio_stuck_locked(uint16_t vm_id, struct vm_wdt_entry *entry)
{
	uint16_t count = virtio_proxy_device_count(vm_id);
	uint16_t index;
	uint64_t timeout_count = 0UL;
	uint16_t pending_active = 0U;
	uint16_t pending_limit = 0U;
	bool stuck = false;

	for (index = 0U; index < count; index++) {
		struct virtio_proxy_stats stats;

		if (virtio_proxy_get_stats(vm_id, index, &stats)) {
			timeout_count += stats.timeout_count;
			pending_active += stats.pending_active;
			pending_limit += stats.pending_limit;
		}
	}

	if ((timeout_count > entry->last_virtio_timeout_count) ||
		((pending_limit != 0U) && (pending_active >= pending_limit) &&
		(pending_active >= entry->last_virtio_pending_active))) {
		if (entry->virtio_stuck_periods < VM_WDT_STUCK_PERIOD_MAX) {
			entry->virtio_stuck_periods++;
		}
		stuck = entry->virtio_stuck_periods >= VM_WDT_QUEUE_STUCK_PERIODS;
	} else {
		entry->virtio_stuck_periods = 0U;
	}

	entry->last_virtio_timeout_count = timeout_count;
	entry->last_virtio_pending_active = pending_active;

	return stuck;
}

static enum vm_wdt_cause vm_wdt_classify_locked(uint16_t vm_id,
	const struct acrn_vm *vm, struct vm_wdt_entry *entry, uint64_t now)
{
	enum vm_wdt_cause cause = VM_WDT_CAUSE_NONE;

	if (vm_wdt_detect_irq_storm_locked(entry, now)) {
		cause = VM_WDT_CAUSE_IRQ_STORM;
	} else if (vm_wdt_detect_vcpu_stall(vm)) {
		cause = VM_WDT_CAUSE_VCPU_STALL;
	} else if (vm_wdt_detect_virtio_stuck_locked(vm_id, entry)) {
		cause = VM_WDT_CAUSE_VIRTIO_STUCK;
	} else if (vm_wdt_detect_console_stuck_locked(vm_id, entry)) {
		cause = VM_WDT_CAUSE_CONSOLE_STUCK;
	}

	return cause;
}

static bool vm_wdt_recovery_timed_out(uint64_t started_tsc, uint32_t timeout_ms)
{
	return vm_wdt_elapsed_ticks(cpu_ticks(), started_tsc) >
		((uint64_t)timeout_ms * TICKS_PER_MS);
}

static bool vm_wdt_recovery_pending(uint16_t vm_id)
{
	uint64_t rflags;
	bool pending = false;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	pending = vm_wdt_entries[vm_id].restart_pending;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return pending;
}

static bool vm_wdt_complete_recovery(uint16_t vm_id, bool success,
	enum hwtdbg_recovery_result result, uint64_t wait_vcpus,
	int32_t reset_ret)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	uint64_t sequence = 0UL;
	bool completed = false;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending) {
		sequence = entry->recovery_event_sequence;
		entry->restart_pending = false;
		entry->recovery_state = VM_WDT_RECOVERY_IDLE;
		entry->recovery_cause = VM_WDT_CAUSE_NONE;
		entry->recovery_start_tsc = 0UL;
		entry->recovery_wait_vcpus = 0UL;
		entry->recovery_quiesce_generation = 0UL;
		entry->reset_ret = 0;
		entry->recovery_event_sequence = 0UL;
		entry->reset_complete = false;
		entry->restart_count++;
		if (!success) {
			entry->restart_fail_count++;
		}
		completed = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	if (completed && (sequence != 0UL)) {
		hwtdbg_update_recovery(vm_id, sequence, result, 0UL,
			wait_vcpus, reset_ret);
	}

	return completed;
}

static bool vm_wdt_claim_restart(uint16_t vm_id,
	const struct vm_wdt_snapshot *snapshot)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	uint64_t age_ticks;
	uint64_t sequence = 0UL;
	bool claimed = false;

	if ((snapshot == NULL) || (vm_id >= CONFIG_MAX_VM_NUM) ||
		!vm_wdt_restart_enabled(vm_id)) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	age_ticks = vm_wdt_heartbeat_age_ticks(cpu_ticks(), entry);
	if (!entry->restart_pending &&
		(entry->restart_count < CONFIG_VM_WDT_RESTART_MAX) &&
		vm_wdt_is_timeout_age(age_ticks)) {
		entry->restart_pending = true;
		entry->recovery_state = VM_WDT_RECOVERY_QUIESCING;
		entry->recovery_cause = snapshot->cause;
		entry->recovery_start_tsc = cpu_ticks();
		entry->recovery_wait_vcpus = 0UL;
		vm_wdt_next_quiesce_generation++;
		if (vm_wdt_next_quiesce_generation == 0UL) {
			vm_wdt_next_quiesce_generation++;
		}
		entry->recovery_quiesce_generation =
			vm_wdt_next_quiesce_generation;
		entry->reset_ret = 0;
		entry->recovery_event_sequence = entry->candidate_event_sequence;
		entry->candidate_event_sequence = 0UL;
		entry->reset_complete = false;
		sequence = entry->recovery_event_sequence;
		claimed = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	if (claimed && (sequence != 0UL)) {
		hwtdbg_update_recovery(vm_id, sequence, HWTDBG_RECOVERY_QUIESCING,
			snapshot->restart_count + 1UL, 0UL, 0);
	}

	return claimed;
}

static void vm_wdt_mark_restart_exhausted(uint16_t vm_id,
	const struct vm_wdt_snapshot *snapshot)
{
	uint64_t sequence = 0UL;
	uint64_t rflags;

	if ((snapshot == NULL) || !snapshot->restart_enabled ||
		(snapshot->restart_count < CONFIG_VM_WDT_RESTART_MAX)) {
		return;
	}
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	sequence = vm_wdt_entries[vm_id].candidate_event_sequence;
	vm_wdt_entries[vm_id].candidate_event_sequence = 0UL;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	if (sequence != 0UL) {
		hwtdbg_update_recovery(vm_id, sequence, HWTDBG_RECOVERY_EXHAUSTED,
			0UL, 0UL, 0);
	}
}

static bool vm_wdt_update_quiesce(uint16_t vm_id, uint64_t wait_vcpus)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	uint64_t sequence = 0UL;
	bool timed_out = false;

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_QUIESCING)) {
		if (entry->recovery_wait_vcpus != wait_vcpus) {
			entry->recovery_wait_vcpus = wait_vcpus;
			sequence = entry->recovery_event_sequence;
		}
		timed_out = (wait_vcpus != 0UL) &&
			vm_wdt_recovery_timed_out(entry->recovery_start_tsc,
				CONFIG_VM_WDT_RESTART_QUIESCE_TIMEOUT_MS);
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	if (sequence != 0UL) {
		hwtdbg_update_recovery(vm_id, sequence, HWTDBG_RECOVERY_QUIESCING,
			0UL, wait_vcpus, 0);
	}

	return timed_out;
}

static bool vm_wdt_begin_reset(uint16_t vm_id)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	uint64_t sequence = 0UL;
	bool begun = false;

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_QUIESCING)) {
		entry->recovery_state = VM_WDT_RECOVERY_RESETTING;
		entry->recovery_start_tsc = cpu_ticks();
		entry->recovery_wait_vcpus = 0UL;
		entry->reset_ret = 0;
		entry->reset_complete = false;
		sequence = entry->recovery_event_sequence;
		begun = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	if (begun && (sequence != 0UL)) {
		hwtdbg_update_recovery(vm_id, sequence, HWTDBG_RECOVERY_RESETTING,
			0UL, 0UL, 0);
	}

	return begun;
}

void vm_wdt_restart_complete(uint16_t vm_id, int32_t reset_ret)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool accepted = false;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_RESETTING) &&
		!entry->reset_complete) {
		entry->reset_ret = reset_ret;
		entry->reset_complete = true;
		accepted = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	if (accepted && vm_wdt_started) {
		wake_thread(&vm_wdt_thread);
	}
}

static bool vm_wdt_claim_reset_completion(uint16_t vm_id, int32_t *reset_ret)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool complete = false;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (reset_ret == NULL)) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_RESETTING) &&
		entry->reset_complete) {
		*reset_ret = entry->reset_ret;
		complete = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return complete;
}

static bool vm_wdt_start_verification(uint16_t vm_id)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	uint64_t sequence = 0UL;
	bool verified = false;

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_RESETTING)) {
		sequence = entry->recovery_event_sequence;
		entry->reset_complete = false;
		entry->reset_ret = 0;
		if (entry->heartbeat_started) {
			entry->restart_pending = false;
			entry->recovery_state = VM_WDT_RECOVERY_IDLE;
			entry->recovery_cause = VM_WDT_CAUSE_NONE;
			entry->recovery_start_tsc = 0UL;
			entry->recovery_wait_vcpus = 0UL;
			entry->recovery_quiesce_generation = 0UL;
			entry->recovery_event_sequence = 0UL;
			entry->restart_count++;
			verified = true;
		} else {
			entry->recovery_state = VM_WDT_RECOVERY_VERIFYING;
			entry->recovery_start_tsc = cpu_ticks();
			entry->recovery_wait_vcpus = 0UL;
		}
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	if (sequence != 0UL) {
		hwtdbg_update_recovery(vm_id, sequence,
			verified ? HWTDBG_RECOVERY_VERIFIED : HWTDBG_RECOVERY_LAUNCHED,
			0UL, 0UL, 0);
	}

	return verified;
}

static bool vm_wdt_verify_timed_out(uint16_t vm_id)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	bool timed_out = false;

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_VERIFYING)) {
		timed_out = vm_wdt_recovery_timed_out(entry->recovery_start_tsc,
			CONFIG_VM_WDT_TIMEOUT_MS);
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return timed_out;
}

static bool vm_wdt_process_recovery(uint16_t vm_id)
{
	struct acrn_vm *vm;
	enum vm_wdt_recovery_state state;
	uint64_t rflags;
	uint64_t wait_vcpus;
	uint64_t quiesce_generation;
	int32_t ret;

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	state = vm_wdt_entries[vm_id].recovery_state;
	quiesce_generation = vm_wdt_entries[vm_id].recovery_quiesce_generation;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	if (state == VM_WDT_RECOVERY_QUIESCING) {
		vm = get_vm_from_vmid(vm_id);
		if ((vm == NULL) || (vm->state != VM_RUNNING) || is_service_vm(vm)) {
			LOG_ERR("HWT:    VM%u restart failed cause:invalid-state", vm_id);
			(void)vm_wdt_complete_recovery(vm_id, false,
				HWTDBG_RECOVERY_INVALID_STATE, 0UL, -EINVAL);
			return false;
		}

		/*
		 * [20260719] CASE-00: FIXED.
		 *
		 * FIXME(core/scheduler, safety): A remote vCPU may reach VCPU_PAUSED
		 * while its scheduler thread has not acknowledged the reschedule by
		 * entering THREAD_STS_BLOCKED. Its vCPU-ID bit then remains in
		 * wait_vcpus, so recovery must fail closed before cold reset rather
		 * than reload a VM with a still-running vCPU.
		 *
		 * METHOD(core/scheduler, safety): Generation-tagged quiesce ACK
		 *
		 * WDT poll(generation)        target pCPU scheduler / idle
		 *        |                               |
		 *        +-- pause + publish VM bit ---->+-- reject current-only clear
		 *        +-- repeat reschedule SGI       +-- force idle and switch stacks
		 *        |                               +-- verify BLOCKED; gate stale wake
		 *        |<--------- release ACK --------+
		 *        +-- acquire matching ACK -> cold reset
		 *
		 * Key rule:
		 *   - vm_wdt_entry owns the non-zero recovery generation;
		 *   - the target idle context publishes ACK only after the outgoing vCPU
		 *     host stack has been switched out;
		 *   - the generation gate rejects wake until the reset instance is launched;
		 *   - a stale generation cannot authorize reset, and a missing ACK keeps
		 *     recovery fail closed with the unacknowledged vCPU mask.
		 */
		get_vm_lock(vm);
		wait_vcpus = pause_vm_async(vm, quiesce_generation);
		put_vm_lock(vm);
		if (wait_vcpus == 0UL) {
			if (!vm_wdt_begin_reset(vm_id)) {
				return vm_wdt_recovery_pending(vm_id);
			}
			LOG_INF("HWT:    VM%u quiesced; cold restart queued", vm_id);
			ret = request_vm_wdt_restart(vm);
			if (ret != 0) {
				LOG_ERR("HWT:    VM%u restart failed cause:reset-queue ret=%d",
					vm_id, ret);
				(void)vm_wdt_complete_recovery(vm_id, false,
					HWTDBG_RECOVERY_RESET_FAILED, 0UL, ret);
				return false;
			}
		} else if (vm_wdt_update_quiesce(vm_id, wait_vcpus)) {
			LOG_ERR("HWT:    VM%u restart failed cause:quiesce-timeout wait:0x%lx",
				vm_id, wait_vcpus);
			(void)vm_wdt_complete_recovery(vm_id, false,
				HWTDBG_RECOVERY_QUIESCE_TIMEOUT, wait_vcpus, 0);
			return false;
		}
	} else if (state == VM_WDT_RECOVERY_RESETTING) {
		if (vm_wdt_claim_reset_completion(vm_id, &ret)) {
			if (ret != 0) {
				LOG_ERR("HWT:    VM%u restart failed cause:reset ret=%d", vm_id, ret);
				(void)vm_wdt_complete_recovery(vm_id, false,
					HWTDBG_RECOVERY_RESET_FAILED, 0UL, ret);
				return false;
			}
			if (vm_wdt_start_verification(vm_id)) {
				LOG_INF("HWT:    VM%u restart verified", vm_id);
			} else {
				LOG_INF("HWT:    VM%u restart launched", vm_id);
			}
		}
	} else if (state == VM_WDT_RECOVERY_VERIFYING) {
		if (vm_wdt_verify_timed_out(vm_id)) {
			LOG_ERR("HWT:    VM%u restart failed cause:verify-timeout", vm_id);
			(void)vm_wdt_complete_recovery(vm_id, false,
				HWTDBG_RECOVERY_VERIFY_TIMEOUT, 0UL, 0);
			return false;
		}
	}

	return vm_wdt_recovery_pending(vm_id);
}

static void vm_wdt_schedule_recovery_poll(void)
{
	uint64_t poll_ticks = (uint64_t)CONFIG_VM_WDT_RESTART_POLL_MS * TICKS_PER_MS;

	if (!timer_is_started(&vm_wdt_recovery_timer)) {
		update_timer(&vm_wdt_recovery_timer, cpu_ticks() + poll_ticks, 0UL);
		if (add_timer(&vm_wdt_recovery_timer) != 0) {
			LOG_ERR("HWT:    cannot schedule recovery poll");
		}
	}
}

int32_t vm_wdt_pm_suspend(uint64_t epoch)
{
	const uint64_t timeout_ticks =
		(uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS;
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;
	uint64_t now;
	uint64_t rflags;
	uint16_t vm_id;
	int32_t status = 0;

	if (epoch == 0UL) {
		return -EINVAL;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	if (vm_wdt_pm_suspended) {
		status = (vm_wdt_suspend_epoch == epoch) ? 0 : -EBUSY;
	} else {
		vm_wdt_suspend_epoch = epoch;
		vm_wdt_suspend_ticks = now;
		vm_wdt_pm_suspended = true;
		for (vm_id = 0U; vm_id < max_vm_id; vm_id++) {
			struct vm_wdt_entry *entry = &vm_wdt_entries[vm_id];
			uint64_t age_ticks = vm_wdt_heartbeat_age_ticks(now, entry);

			entry->remaining_ticks = (age_ticks < timeout_ticks) ?
				(timeout_ticks - age_ticks) : 0UL;
			entry->suspend_epoch = epoch;
			entry->suspend_ticks = now;
			entry->pm_suspended = true;
		}
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	if ((status == 0) && vm_wdt_started) {
		if (timer_is_started(&vm_wdt_timer)) {
			del_timer(&vm_wdt_timer);
		}
		if (timer_is_started(&vm_wdt_recovery_timer)) {
			del_timer(&vm_wdt_recovery_timer);
		}
	}

	return status;
}

int32_t vm_wdt_pm_resume(uint64_t epoch)
{
	const uint64_t timeout_ticks =
		(uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS;
	const uint64_t period_ticks =
		(uint64_t)CONFIG_VM_WDT_PRINT_PERIOD_MS * TICKS_PER_MS;
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;
	uint64_t now;
	uint64_t sleep_ticks;
	uint64_t rflags;
	uint16_t vm_id;
	bool recovery_pending = false;
	int32_t status = 0;

	if (epoch == 0UL) {
		return -EINVAL;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	if (!vm_wdt_pm_suspended) {
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
		return 0;
	}
	if (vm_wdt_suspend_epoch != epoch) {
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
		return -EINVAL;
	}
	for (vm_id = 0U; vm_id < max_vm_id; vm_id++) {
		const struct vm_wdt_entry *entry = &vm_wdt_entries[vm_id];

		if (!entry->pm_suspended || (entry->suspend_epoch != epoch)) {
			spinlock_irqrestore_release(&vm_wdt_lock, rflags);
			return -EFAULT;
		}
	}

	sleep_ticks = vm_wdt_elapsed_ticks(now, vm_wdt_suspend_ticks);
	for (vm_id = 0U; vm_id < max_vm_id; vm_id++) {
		struct vm_wdt_entry *entry = &vm_wdt_entries[vm_id];
		uint64_t age_ticks = (entry->remaining_ticks == 0UL) ?
			(timeout_ticks + 1UL) : (timeout_ticks - entry->remaining_ticks);
		uint64_t heartbeat_base = (now > age_ticks) ? (now - age_ticks) : 0UL;

		if (!entry->heartbeat_started) {
			entry->start_tsc = heartbeat_base;
		} else {
			entry->last_kick_tsc = heartbeat_base;
		}
		entry->last_irq_total = vm_wdt_irq_total();
		entry->last_irq_delta = 0UL;
		entry->last_irq_sample_tsc = now;
		if (entry->restart_pending && (entry->recovery_start_tsc != 0UL)) {
			entry->recovery_start_tsc += sleep_ticks;
			recovery_pending = true;
		}
		entry->remaining_ticks = 0UL;
		entry->suspend_epoch = 0UL;
		entry->suspend_ticks = 0UL;
		entry->pm_suspended = false;
	}
	vm_wdt_suspend_epoch = 0UL;
	vm_wdt_suspend_ticks = 0UL;
	vm_wdt_pm_suspended = false;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	if (vm_wdt_started) {
		update_timer(&vm_wdt_timer, now + period_ticks, period_ticks);
		if (add_timer(&vm_wdt_timer) != 0) {
			LOG_ERR("HWT:    cannot resume periodic timer");
			status = -EIO;
		}
		if (recovery_pending) {
			vm_wdt_schedule_recovery_poll();
		}
	}

	return status;
}

static void vm_wdt_resume_entry_locked(struct vm_wdt_entry *entry, uint64_t now)
{
	const uint64_t timeout_ticks =
		(uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS;
	uint64_t age_ticks = (entry->remaining_ticks == 0UL) ?
		(timeout_ticks + 1UL) : (timeout_ticks - entry->remaining_ticks);
	uint64_t heartbeat_base = (now > age_ticks) ? (now - age_ticks) : 0UL;
	uint64_t sleep_ticks = vm_wdt_elapsed_ticks(now, entry->suspend_ticks);

	if (!entry->heartbeat_started) {
		entry->start_tsc = heartbeat_base;
	} else {
		entry->last_kick_tsc = heartbeat_base;
	}
	entry->last_irq_total = vm_wdt_irq_total();
	entry->last_irq_delta = 0UL;
	entry->last_irq_sample_tsc = now;
	if (entry->restart_pending && (entry->recovery_start_tsc != 0UL)) {
		entry->recovery_start_tsc += sleep_ticks;
	}
	entry->remaining_ticks = 0UL;
	entry->suspend_epoch = 0UL;
	entry->suspend_ticks = 0UL;
	entry->pm_suspended = false;
}

int32_t vm_wdt_pm_suspend_vm(uint16_t vm_id, uint64_t epoch)
{
	const uint64_t timeout_ticks =
		(uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS;
	struct vm_wdt_entry *entry;
	uint64_t now;
	uint64_t age_ticks;
	uint64_t rflags;
	int32_t status = 0;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (epoch == 0UL)) {
		return -EINVAL;
	}
	if (!vm_wdt_is_monitored(vm_id)) {
		return 0;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (vm_wdt_pm_suspended) {
		status = -EBUSY;
	} else if (entry->pm_suspended) {
		status = (entry->suspend_epoch == epoch) ? 0 : -EBUSY;
	} else if (entry->restart_pending) {
		status = -EBUSY;
	} else {
		/*
		 * VM STR is transparent to guest watchdog drivers. Freeze only
		 * the target VM's timeout basis; the global watchdog timer keeps
		 * scanning other VMs during the BEAU-owned suspend window.
		 */
		age_ticks = vm_wdt_heartbeat_age_ticks(now, entry);
		entry->remaining_ticks = (age_ticks < timeout_ticks) ?
			(timeout_ticks - age_ticks) : 0UL;
		entry->suspend_epoch = epoch;
		entry->suspend_ticks = now;
		entry->pm_suspended = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return status;
}

int32_t vm_wdt_pm_resume_vm(uint16_t vm_id, uint64_t epoch)
{
	struct vm_wdt_entry *entry;
	uint64_t now;
	uint64_t rflags;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (epoch == 0UL)) {
		return -EINVAL;
	}
	if (!vm_wdt_is_monitored(vm_id)) {
		return 0;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm_id];
	if (!entry->pm_suspended) {
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
		return 0;
	}
	if (entry->suspend_epoch != epoch) {
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
		return -EINVAL;
	}
	vm_wdt_resume_entry_locked(entry, now);
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return 0;
}

static bool vm_wdt_print_daemon_event(uint16_t vm_id,
	const struct vm_wdt_daemon_event *event)
{
	uint64_t gap_sec;
	uint64_t gap_msec;

	if ((event == NULL) || !vm_wdt_is_monitored(vm_id)) {
		return false;
	}
	gap_sec = ticks_to_ms(event->gap_ticks) / 1000UL;
	gap_msec = ticks_to_ms(event->gap_ticks) % 1000UL;

	return daemon_log(LOG_INFO,
		"HWT: vm%hu:%9s gap:%02lu.%03lus token:0x%016lx",
		vm_id, vm_wdt_name(vm_id), gap_sec, gap_msec, event->token);
}

static void vm_wdt_drain_daemon_events(void)
{
	uint16_t vm_id;
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;

	for (vm_id = 0U; vm_id < max_vm_id; vm_id++) {
		struct vm_wdt_daemon_event event;

		if (vm_wdt_claim_daemon_event(vm_id, &event) &&
			!vm_wdt_print_daemon_event(vm_id, &event)) {
			vm_wdt_record_daemon_drop(vm_id);
		}
	}
}

static void vm_wdt_check_timeouts(void)
{
	struct vm_wdt_snapshot snapshot;
	bool recovery_pending = false;
	uint16_t vm_id;
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;

	for (vm_id = 0U; vm_id < max_vm_id; vm_id++) {
		if (vm_wdt_vm_is_pm_suspended(vm_id)) {
			continue;
		}
		vm_wdt_flush_recovery_notices(vm_id);
		if (vm_wdt_get_snapshot(vm_id, &snapshot) != 0) {
			continue;
		}
		vm_wdt_capture_pending_timeouts(vm_id, &snapshot);
		if (vm_wdt_recovery_pending(vm_id)) {
			recovery_pending |= vm_wdt_process_recovery(vm_id);
			continue;
		}
		if (snapshot.status == VM_WDT_STATUS_STUCK) {
			if (vm_wdt_claim_restart(vm_id, &snapshot)) {
				LOG_INF("HWT:    VM%u restart cause:%s age:%lums attempt:%lu/%u",
					vm_id, vm_wdt_cause_str(snapshot.cause), snapshot.last_ms,
					snapshot.restart_count + 1UL, CONFIG_VM_WDT_RESTART_MAX);
				recovery_pending |= vm_wdt_process_recovery(vm_id);
			} else {
				vm_wdt_mark_restart_exhausted(vm_id, &snapshot);
			}
		}
	}
	if (recovery_pending) {
		vm_wdt_schedule_recovery_poll();
	}
}

static void vm_wdt_timer_callback(__unused void *data)
{
	if (!vm_wdt_is_pm_suspended()) {
		wake_thread(&vm_wdt_thread);
	}
}

static void vm_wdt_thread_main(__unused struct thread_object *obj)
{
	while (true) {
		sleep_thread(&vm_wdt_thread);
		schedule();
		if (!vm_wdt_is_pm_suspended()) {
			vm_wdt_drain_daemon_events();
			vm_wdt_check_timeouts();
		}
	}
}

void vm_wdt_start(void)
{
	struct sched_params vm_wdt_params = {0U};
	uint64_t period_ticks = (uint64_t)CONFIG_VM_WDT_PRINT_PERIOD_MS * TICKS_PER_MS;

	if (vm_wdt_started) {
		return;
	}

	(void)strncpy_s(vm_wdt_thread.name, sizeof(vm_wdt_thread.name), "vm-wdt",
		sizeof(vm_wdt_thread.name));
	vm_wdt_thread.pcpu_id = BSP_CPU_ID;
	vm_wdt_thread.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	vm_wdt_thread.thread_entry = vm_wdt_thread_main;
	vm_wdt_thread.switch_out = NULL;
	vm_wdt_thread.switch_in = NULL;
	vm_wdt_thread.host_sp = arch_setup_thread_stack(&vm_wdt_thread, vm_wdt_stack,
		CONFIG_STACK_SIZE);
	initialize_timer(&vm_wdt_timer, vm_wdt_timer_callback, NULL,
		cpu_ticks() + period_ticks, period_ticks);
	initialize_timer(&vm_wdt_recovery_timer, vm_wdt_timer_callback, NULL, 0UL, 0UL);

	vm_wdt_params.prio = PRIO_LOW;
	init_thread_data(&vm_wdt_thread, &vm_wdt_params);
	if (add_timer(&vm_wdt_timer) != 0) {
		LOG_ERR("HWT:    cannot start periodic timer");
		return;
	}
	wake_thread(&vm_wdt_thread);
	vm_wdt_started = true;
}

void vm_wdt_reset(const struct acrn_vm *vm)
{
	uint64_t rflags;
	uint64_t restart_count;
	uint64_t restart_fail_count;
	uint64_t recovery_start_tsc;
	uint64_t recovery_wait_vcpus;
	uint64_t recovery_quiesce_generation;
	uint64_t recovery_event_sequence;
	int32_t reset_ret;
	enum vm_wdt_recovery_state recovery_state;
	enum vm_wdt_cause recovery_cause;
	bool clear_history;
	bool restart_pending;
	bool reset_complete;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	/* A destroyed VMID may be reused by a different guest instance. */
	clear_history = is_poweroff_vm(vm);
	restart_count = clear_history ? 0UL : vm_wdt_entries[vm->vm_id].restart_count;
	restart_fail_count = clear_history ? 0UL :
		vm_wdt_entries[vm->vm_id].restart_fail_count;
	restart_pending = !clear_history && vm_wdt_entries[vm->vm_id].restart_pending;
	recovery_state = restart_pending ? vm_wdt_entries[vm->vm_id].recovery_state :
		VM_WDT_RECOVERY_IDLE;
	recovery_cause = restart_pending ? vm_wdt_entries[vm->vm_id].recovery_cause :
		VM_WDT_CAUSE_NONE;
	recovery_start_tsc = restart_pending ? vm_wdt_entries[vm->vm_id].recovery_start_tsc : 0UL;
	recovery_wait_vcpus = restart_pending ? vm_wdt_entries[vm->vm_id].recovery_wait_vcpus : 0UL;
	recovery_quiesce_generation = restart_pending ?
		vm_wdt_entries[vm->vm_id].recovery_quiesce_generation : 0UL;
	recovery_event_sequence = restart_pending ?
		vm_wdt_entries[vm->vm_id].recovery_event_sequence : 0UL;
	reset_ret = restart_pending ? vm_wdt_entries[vm->vm_id].reset_ret : 0;
	reset_complete = restart_pending && vm_wdt_entries[vm->vm_id].reset_complete;
	vm_wdt_entries[vm->vm_id].start_tsc = cpu_ticks();
	vm_wdt_entries[vm->vm_id].last_kick_tsc = 0UL;
	vm_wdt_entries[vm->vm_id].timeout_count = 0UL;
	vm_wdt_entries[vm->vm_id].last_token = 0UL;
	vm_wdt_entries[vm->vm_id].last_irq_total = vm_wdt_irq_total();
	vm_wdt_entries[vm->vm_id].last_irq_delta = 0UL;
	vm_wdt_entries[vm->vm_id].last_irq_sample_tsc = vm_wdt_entries[vm->vm_id].start_tsc;
	vm_wdt_entries[vm->vm_id].last_console_drained = 0UL;
	vm_wdt_entries[vm->vm_id].last_console_queued = 0U;
	vm_wdt_entries[vm->vm_id].console_stuck_periods = 0U;
	vm_wdt_entries[vm->vm_id].last_virtio_timeout_count = 0UL;
	vm_wdt_entries[vm->vm_id].last_virtio_pending_active = 0U;
	vm_wdt_entries[vm->vm_id].virtio_stuck_periods = 0U;
	vm_wdt_entries[vm->vm_id].restart_count = restart_count;
	vm_wdt_entries[vm->vm_id].restart_fail_count = restart_fail_count;
	vm_wdt_entries[vm->vm_id].recovery_state = recovery_state;
	vm_wdt_entries[vm->vm_id].recovery_cause = recovery_cause;
	vm_wdt_entries[vm->vm_id].recovery_start_tsc = recovery_start_tsc;
	vm_wdt_entries[vm->vm_id].recovery_wait_vcpus = recovery_wait_vcpus;
	vm_wdt_entries[vm->vm_id].recovery_quiesce_generation =
		recovery_quiesce_generation;
	vm_wdt_entries[vm->vm_id].recovery_event_sequence = recovery_event_sequence;
	vm_wdt_entries[vm->vm_id].reset_ret = reset_ret;
	vm_wdt_entries[vm->vm_id].reset_complete = reset_complete;
	if (clear_history) {
		vm_wdt_entries[vm->vm_id].candidate_event_sequence = 0UL;
		vm_wdt_entries[vm->vm_id].pending_head = 0U;
		vm_wdt_entries[vm->vm_id].pending_count = 0U;
		(void)memset(vm_wdt_entries[vm->vm_id].pending, 0U,
			sizeof(vm_wdt_entries[vm->vm_id].pending));
		vm_wdt_entries[vm->vm_id].notice_head = 0U;
		vm_wdt_entries[vm->vm_id].notice_count = 0U;
		(void)memset(vm_wdt_entries[vm->vm_id].notice, 0U,
			sizeof(vm_wdt_entries[vm->vm_id].notice));
	}
	vm_wdt_entries[vm->vm_id].remaining_ticks = 0UL;
	vm_wdt_entries[vm->vm_id].suspend_epoch = 0UL;
	vm_wdt_entries[vm->vm_id].suspend_ticks = 0UL;
	(void)memset(&vm_wdt_entries[vm->vm_id].daemon_event, 0U,
		sizeof(vm_wdt_entries[vm->vm_id].daemon_event));
	vm_wdt_entries[vm->vm_id].daemon_merged = 0UL;
	vm_wdt_entries[vm->vm_id].daemon_dropped = 0UL;
	vm_wdt_entries[vm->vm_id].heartbeat_started = false;
	vm_wdt_entries[vm->vm_id].daemon_pending = false;
	vm_wdt_entries[vm->vm_id].timeout_active = false;
	vm_wdt_entries[vm->vm_id].restart_pending = restart_pending;
	vm_wdt_entries[vm->vm_id].pm_suspended = false;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
}

void vm_wdt_kick(const struct acrn_vm *vm, uint64_t token)
{
	struct vm_wdt_entry *entry;
	uint64_t rflags;
	uint64_t now;
	uint64_t gap_ticks = 0UL;
	uint64_t late_sequence = 0UL;
	uint64_t verified_sequence = 0UL;
	bool transitioned = false;
	bool verified = false;
	bool daemon_event = false;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = &vm_wdt_entries[vm->vm_id];
	if (vm_wdt_heartbeat_started(entry) || vm_wdt_is_monitored(vm->vm_id)) {
		gap_ticks = vm_wdt_heartbeat_age_ticks(now, entry);
		transitioned = vm_wdt_record_timeout_locked(vm->vm_id, entry,
			vm_wdt_is_timeout_age(gap_ticks), now);
	}
	late_sequence = entry->candidate_event_sequence;
	entry->candidate_event_sequence = 0UL;
	entry->last_kick_tsc = now;
	entry->heartbeat_started = true;
	entry->timeout_active = false;
	entry->last_token = token;
	if (vm_wdt_is_monitored(vm->vm_id)) {
		vm_wdt_queue_daemon_event_locked(entry, now, gap_ticks);
		daemon_event = true;
	}
	if (entry->restart_pending &&
		(entry->recovery_state == VM_WDT_RECOVERY_VERIFYING)) {
		verified_sequence = entry->recovery_event_sequence;
		entry->restart_pending = false;
		entry->recovery_state = VM_WDT_RECOVERY_IDLE;
		entry->recovery_cause = VM_WDT_CAUSE_NONE;
		entry->recovery_start_tsc = 0UL;
		entry->recovery_wait_vcpus = 0UL;
		entry->recovery_quiesce_generation = 0UL;
		entry->reset_ret = 0;
		entry->recovery_event_sequence = 0UL;
		entry->reset_complete = false;
		entry->restart_count++;
		verified = true;
	}
	vm_wdt_queue_recovery_notice_locked(entry, late_sequence,
		HWTDBG_RECOVERY_LATE_KICK, 0UL, 0);
	vm_wdt_queue_recovery_notice_locked(entry, verified_sequence,
		HWTDBG_RECOVERY_VERIFIED, 0UL, 0);
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	if ((daemon_event || transitioned || (late_sequence != 0UL) ||
		(verified_sequence != 0UL)) && vm_wdt_started) {
		wake_thread(&vm_wdt_thread);
	}
	if (verified) {
		LOG_INF("HWT:    VM%u restart verified", vm->vm_id);
	}
}

static void vm_wdt_fill_snapshot_metadata(uint16_t vm_id,
	struct vm_wdt_snapshot *snapshot, const struct vm_wdt_entry *entry,
	uint64_t observed_tsc)
{
	snapshot->observed_tsc = observed_tsc;
	snapshot->start_tsc = entry->start_tsc;
	snapshot->last_kick_tsc = entry->last_kick_tsc;
	snapshot->irq_total = entry->last_irq_total;
	snapshot->irq_delta = entry->last_irq_delta;
	snapshot->daemon_merged = entry->daemon_merged;
	snapshot->daemon_dropped = entry->daemon_dropped;
	snapshot->timeout_ms = CONFIG_VM_WDT_TIMEOUT_MS;
	snapshot->heartbeat_started = vm_wdt_heartbeat_started(entry);
	snapshot->timeout_active = entry->timeout_active;
	snapshot->restart_enabled = vm_wdt_restart_enabled(vm_id);
	snapshot->daemon_pending = entry->daemon_pending;
}

int32_t vm_wdt_get_snapshot(uint16_t vm_id, struct vm_wdt_snapshot *snapshot)
{
	const struct acrn_vm_config *vm_config;
	const struct acrn_vm *vm;
	struct vm_wdt_entry entry;
	uint64_t now;
	uint64_t age_ticks;
	uint64_t rflags;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (snapshot == NULL)) {
		return -EINVAL;
	}

	snapshot->status = VM_WDT_STATUS_UNUSED;
	snapshot->cause = VM_WDT_CAUSE_NONE;
	snapshot->recovery_state = VM_WDT_RECOVERY_IDLE;
	snapshot->last_ms = 0UL;
	snapshot->timeout_count = 0UL;
	snapshot->restart_count = 0UL;
	snapshot->restart_fail_count = 0UL;
	snapshot->last_token = 0UL;
	snapshot->recovery_wait_vcpus = 0UL;
	snapshot->observed_tsc = 0UL;
	snapshot->start_tsc = 0UL;
	snapshot->last_kick_tsc = 0UL;
	snapshot->irq_total = 0UL;
	snapshot->irq_delta = 0UL;
	snapshot->daemon_merged = 0UL;
	snapshot->daemon_dropped = 0UL;
	snapshot->timeout_ms = CONFIG_VM_WDT_TIMEOUT_MS;
	snapshot->restart_pending = false;
	snapshot->heartbeat_started = false;
	snapshot->timeout_active = false;
	snapshot->restart_enabled = false;
	snapshot->daemon_pending = false;

	vm_config = get_vm_config(vm_id);
	vm = get_vm_from_vmid(vm_id);
	if (!vm_wdt_config_present(vm_config)) {
		return 0;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = vm_wdt_entries[vm_id];
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	now = cpu_ticks();
	vm_wdt_fill_snapshot_metadata(vm_id, snapshot, &entry, now);
	snapshot->recovery_state = entry.recovery_state;
	snapshot->recovery_wait_vcpus = entry.recovery_wait_vcpus;
	snapshot->restart_pending = entry.restart_pending;

	if ((vm == NULL) || (vm->state != VM_RUNNING)) {
		snapshot->status = VM_WDT_STATUS_OFFLINE;
		return 0;
	}

	if (entry.pm_suspended) {
		const uint64_t timeout_ticks =
			(uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS;

		age_ticks = (entry.remaining_ticks == 0UL) ?
			(timeout_ticks + 1UL) : (timeout_ticks - entry.remaining_ticks);
		snapshot->status = vm_wdt_heartbeat_started(&entry) ?
			VM_WDT_STATUS_ALIVE : VM_WDT_STATUS_UNKNOWN;
		snapshot->cause = vm_wdt_heartbeat_started(&entry) ?
			VM_WDT_CAUSE_HEARTBEAT : VM_WDT_CAUSE_NONE;
		snapshot->last_ms = ticks_to_ms(age_ticks);
		snapshot->timeout_count = entry.timeout_count;
		snapshot->restart_count = entry.restart_count;
		snapshot->restart_fail_count = entry.restart_fail_count;
		snapshot->last_token = entry.last_token;
		snapshot->recovery_state = entry.recovery_state;
		snapshot->recovery_wait_vcpus = entry.recovery_wait_vcpus;
		snapshot->restart_pending = entry.restart_pending;
		return 0;
	}
	/*
	 * A hypercall kick is the explicit guest heartbeat. Normal VM-exits can
	 * continue while the guest watchdog worker is stuck, so they must not
	 * refresh the timeout basis.
	 */
	age_ticks = vm_wdt_heartbeat_age_ticks(now, &entry);
	if (!vm_wdt_heartbeat_started(&entry) && vm_wdt_is_monitored(vm_id) &&
		vm_wdt_is_timeout_age(age_ticks)) {
		spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
		age_ticks = vm_wdt_heartbeat_age_ticks(now, &vm_wdt_entries[vm_id]);
		if (!vm_wdt_heartbeat_started(&vm_wdt_entries[vm_id]) &&
			vm_wdt_is_timeout_age(age_ticks)) {
			(void)vm_wdt_record_timeout_locked(vm_id,
				&vm_wdt_entries[vm_id], true, now);
		}
		entry = vm_wdt_entries[vm_id];
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	}
	if (!vm_wdt_heartbeat_started(&entry)) {
		snapshot->status = vm_wdt_is_monitored(vm_id) &&
			vm_wdt_is_timeout_age(age_ticks) ? VM_WDT_STATUS_STUCK :
			VM_WDT_STATUS_UNKNOWN;
		snapshot->cause = snapshot->status == VM_WDT_STATUS_STUCK ?
			VM_WDT_CAUSE_TIMEOUT : VM_WDT_CAUSE_NONE;
		snapshot->last_ms = ticks_to_ms(age_ticks);
		snapshot->timeout_count = entry.timeout_count;
		snapshot->restart_count = entry.restart_count;
		snapshot->restart_fail_count = entry.restart_fail_count;
		snapshot->last_token = entry.last_token;
		snapshot->recovery_state = entry.recovery_state;
		snapshot->recovery_wait_vcpus = entry.recovery_wait_vcpus;
		snapshot->restart_pending = entry.restart_pending;
		vm_wdt_fill_snapshot_metadata(vm_id, snapshot, &entry, now);
		return 0;
	}

	if (vm_wdt_is_timeout_age(age_ticks)) {
		spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
		age_ticks = vm_wdt_heartbeat_age_ticks(now, &vm_wdt_entries[vm_id]);
		if (vm_wdt_heartbeat_started(&vm_wdt_entries[vm_id])) {
			(void)vm_wdt_record_timeout_locked(vm_id,
				&vm_wdt_entries[vm_id], vm_wdt_is_timeout_age(age_ticks),
				now);
		}
		if (vm_wdt_heartbeat_started(&vm_wdt_entries[vm_id]) &&
			vm_wdt_is_timeout_age(age_ticks)) {
			snapshot->cause = vm_wdt_classify_locked(vm_id, vm,
				&vm_wdt_entries[vm_id], now);
		}
		entry = vm_wdt_entries[vm_id];
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	}
	if (!vm_wdt_heartbeat_started(&entry)) {
		snapshot->status = VM_WDT_STATUS_UNKNOWN;
	} else {
		snapshot->status = !vm_wdt_is_timeout_age(age_ticks) ?
			VM_WDT_STATUS_ALIVE : VM_WDT_STATUS_STUCK;
	}
	if (snapshot->status == VM_WDT_STATUS_ALIVE) {
		snapshot->cause = VM_WDT_CAUSE_HEARTBEAT;
	} else if (snapshot->status == VM_WDT_STATUS_STUCK) {
		if (snapshot->cause == VM_WDT_CAUSE_NONE) {
			snapshot->cause = VM_WDT_CAUSE_TIMEOUT;
		}
	}

	snapshot->last_ms = ticks_to_ms(age_ticks);
	snapshot->timeout_count = entry.timeout_count;
	snapshot->restart_count = entry.restart_count;
	snapshot->restart_fail_count = entry.restart_fail_count;
	snapshot->last_token = entry.last_token;
	snapshot->recovery_state = entry.recovery_state;
	snapshot->recovery_wait_vcpus = entry.recovery_wait_vcpus;
	snapshot->restart_pending = entry.restart_pending;
	vm_wdt_fill_snapshot_metadata(vm_id, snapshot, &entry, now);

	return 0;
}

int32_t hcall_vm_wdt_kick(struct acrn_vcpu *vcpu, __unused struct acrn_vm *target_vm,
	uint64_t param1, __unused uint64_t param2)
{
	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return -EINVAL;
	}

	vm_wdt_kick(vcpu->vm, param1);

	return 0;
}
