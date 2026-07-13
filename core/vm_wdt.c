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
#include <shell.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <timer.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <vm_wdt.h>
#include <virtio_proxy.h>

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
 *   - core/vm_wdt.c owns correlation, state transitions, and reporting;
 *   - device modules own evidence counters, so watchdog output remains a
 *     diagnosis instead of a destructive recovery action.
 */

#define VM_WDT_IRQ_STORM_PER_SEC	10000UL
#define VM_WDT_QUEUE_STUCK_PERIODS	2U
#define VM_WDT_STUCK_PERIOD_MAX		255U

struct vm_wdt_entry {
	uint64_t start_tsc;
	uint64_t last_kick_tsc;
	uint64_t kick_count;
	uint64_t timeout_count;
	uint64_t last_token;
	enum vm_wdt_status reported_status;
	enum vm_wdt_reason reported_reason;
	uint64_t last_irq_total;
	uint64_t last_irq_sample_tsc;
	uint64_t last_console_drained;
	uint32_t last_console_queued;
	uint8_t console_stuck_periods;
	uint64_t last_virtio_timeout_count;
	uint16_t last_virtio_pending_active;
	uint8_t virtio_stuck_periods;
	bool timeout_active;
};

static struct vm_wdt_entry vm_wdt_entries[CONFIG_MAX_VM_NUM];
static spinlock_t vm_wdt_lock = { .head = 0U, .tail = 0U, };
static struct thread_object vm_wdt_thread;
static uint8_t vm_wdt_stack[CONFIG_STACK_SIZE] __aligned(16);
static struct hv_timer vm_wdt_timer;
static bool vm_wdt_started;

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
	uint64_t since = (entry->kick_count == 0UL) ? entry->start_tsc : entry->last_kick_tsc;

	return vm_wdt_elapsed_ticks(now, since);
}

static bool vm_wdt_heartbeat_started(const struct vm_wdt_entry *entry)
{
	return (entry != NULL) && (entry->kick_count != 0UL);
}

static bool vm_wdt_is_timeout_age(uint64_t age_ticks)
{
	return age_ticks > ((uint64_t)CONFIG_VM_WDT_TIMEOUT_MS * TICKS_PER_MS);
}

static void vm_wdt_record_timeout_locked(struct vm_wdt_entry *entry, bool timed_out)
{
	if (timed_out) {
		if (!entry->timeout_active) {
			entry->timeout_count++;
			entry->timeout_active = true;
		}
	} else {
		entry->timeout_active = false;
	}
}

static const char *vm_wdt_status_str(enum vm_wdt_status status)
{
	const char *str;

	switch (status) {
	case VM_WDT_STATUS_OFFLINE:
		str = "offline";
		break;
	case VM_WDT_STATUS_UNKNOWN:
		str = "none";
		break;
	case VM_WDT_STATUS_ALIVE:
		str = "alive";
		break;
	case VM_WDT_STATUS_STUCK:
		str = "stuck";
		break;
	default:
		str = "unused";
		break;
	}

	return str;
}

static const char *vm_wdt_status_color(enum vm_wdt_status status)
{
	const char *color;

	switch (status) {
	case VM_WDT_STATUS_ALIVE:
		color = SHELL_COLOR_GREEN;
		break;
	case VM_WDT_STATUS_STUCK:
		color = SHELL_COLOR_RED;
		break;
	case VM_WDT_STATUS_UNKNOWN:
		color = SHELL_COLOR_YELLOW;
		break;
	case VM_WDT_STATUS_OFFLINE:
		color = SHELL_COLOR_GREY;
		break;
	default:
		color = "";
		break;
	}

	return color;
}

static const char *vm_wdt_reason_str(enum vm_wdt_reason reason)
{
	const char *str;

	switch (reason) {
	case VM_WDT_REASON_HEARTBEAT:
		str = "Heartbeat";
		break;
	case VM_WDT_REASON_VCPU_STALL:
		str = "vCPUstall";
		break;
	case VM_WDT_REASON_IRQ_STORM:
		str = "IRQstorm";
		break;
	case VM_WDT_REASON_CONSOLE_STUCK:
		str = "consoleStuck";
		break;
	case VM_WDT_REASON_VIRTIO_STUCK:
		str = "virtioStuck";
		break;
	case VM_WDT_REASON_NONE:
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

		if ((vcpu == NULL) || (vcpu->state == VCPU_OFFLINE)) {
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
	uint64_t delta;
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

static enum vm_wdt_reason vm_wdt_classify_locked(uint16_t vm_id,
	const struct acrn_vm *vm, struct vm_wdt_entry *entry, uint64_t now)
{
	enum vm_wdt_reason reason = VM_WDT_REASON_HEARTBEAT;

	if (vm_wdt_detect_irq_storm_locked(entry, now)) {
		reason = VM_WDT_REASON_IRQ_STORM;
	} else if (vm_wdt_detect_vcpu_stall(vm)) {
		reason = VM_WDT_REASON_VCPU_STALL;
	} else if (vm_wdt_detect_virtio_stuck_locked(vm_id, entry)) {
		reason = VM_WDT_REASON_VIRTIO_STUCK;
	} else if (vm_wdt_detect_console_stuck_locked(vm_id, entry)) {
		reason = VM_WDT_REASON_CONSOLE_STUCK;
	}

	return reason;
}

static bool vm_wdt_should_report(uint16_t vm_id, const struct vm_wdt_snapshot *snapshot,
	bool force)
{
	bool report = false;
	uint64_t rflags;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (snapshot == NULL)) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	if (force || (vm_wdt_entries[vm_id].reported_status != snapshot->status) ||
		(vm_wdt_entries[vm_id].reported_reason != snapshot->reason)) {
		report = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return report;
}

static void vm_wdt_mark_reported(uint16_t vm_id, const struct vm_wdt_snapshot *snapshot)
{
	uint64_t rflags;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (snapshot == NULL)) {
		return;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	vm_wdt_entries[vm_id].reported_status = snapshot->status;
	vm_wdt_entries[vm_id].reported_reason = snapshot->reason;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
}

static void vm_wdt_print_one(uint16_t vm_id, const struct vm_wdt_snapshot *snapshot)
{
	uint64_t timestamp;
	const char *color;
	char timestamp_str[LOG_TIMESTAMP_MAX_SIZE];
	char line[144];
	uint64_t last_sec;
	uint64_t last_msec;

	if ((snapshot == NULL) || !vm_wdt_is_monitored(vm_id) || !shell_is_open() ||
		(snapshot->status == VM_WDT_STATUS_UNUSED)) {
		return;
	}

	/*
	 * This service is intentionally not a shell command. Guest OSes prove
	 * liveness by periodically issuing HC_VM_WDT_KICK; the BEAU shell is only
	 * used as a visible console gate so the background WDT line does not pollute
	 * boot logs or a selected guest console.
	 */
	timestamp = ticks_to_us(cpu_ticks());
	format_log_timestamp(timestamp_str, sizeof(timestamp_str), timestamp);
	color = vm_wdt_status_color(snapshot->status);
	last_sec = snapshot->last_ms / 1000UL;
	last_msec = snapshot->last_ms % 1000UL;
	if (snapshot->status == VM_WDT_STATUS_STUCK) {
		(void)snprintf(line, sizeof(line),
			"%s[κ][%s] WDT: vm%hu:%9s status:%7s (%02lu.%03lus) kick:%8lu reason:%s" SHELL_COLOR_RESET "\r\n",
			color, timestamp_str, vm_id, vm_wdt_name(vm_id),
			vm_wdt_status_str(snapshot->status), last_sec, last_msec,
			snapshot->kick_count, vm_wdt_reason_str(snapshot->reason));
	} else {
		(void)snprintf(line, sizeof(line),
			"%s[κ][%s] WDT: vm%hu:%9s status:%7s (%02lu.%03lus) kick:%8lu" SHELL_COLOR_RESET "\r\n",
			color, timestamp_str, vm_id, vm_wdt_name(vm_id),
			vm_wdt_status_str(snapshot->status), last_sec, last_msec,
			snapshot->kick_count);
	}
	if (shell_async_puts(line)) {
		vm_wdt_mark_reported(vm_id, snapshot);
	}
}

static void vm_wdt_print_hcall(uint16_t vm_id, uint64_t last_ms)
{
	struct vm_wdt_snapshot snapshot;

	if (vm_wdt_get_snapshot(vm_id, &snapshot) != 0) {
		return;
	}
	snapshot.last_ms = last_ms;

	if ((snapshot.status != VM_WDT_STATUS_UNUSED) &&
		vm_wdt_should_report(vm_id, &snapshot, true)) {
		vm_wdt_print_one(vm_id, &snapshot);
	}
}

static void vm_wdt_check_timeouts(void)
{
	struct vm_wdt_snapshot snapshot;
	uint16_t vm_id;
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;

	for (vm_id = 0U; vm_id < max_vm_id; vm_id++) {
		if ((vm_wdt_get_snapshot(vm_id, &snapshot) == 0) &&
			(snapshot.status == VM_WDT_STATUS_STUCK) &&
			vm_wdt_should_report(vm_id, &snapshot, false)) {
			vm_wdt_print_one(vm_id, &snapshot);
		}
	}
}

static void vm_wdt_timer_callback(__unused void *data)
{
	wake_thread(&vm_wdt_thread);
}

static void vm_wdt_sleep_period(void)
{
	uint64_t period_ticks = (uint64_t)CONFIG_VM_WDT_PRINT_PERIOD_MS * TICKS_PER_MS;

	update_timer(&vm_wdt_timer, cpu_ticks() + period_ticks, 0UL);
	if (add_timer(&vm_wdt_timer) == 0) {
		sleep_thread(&vm_wdt_thread);
	} else {
		yield_current();
	}
	schedule();
}

static void vm_wdt_thread_main(__unused struct thread_object *obj)
{
	while (true) {
		vm_wdt_sleep_period();
		vm_wdt_check_timeouts();
	}
}

void vm_wdt_start(void)
{
	struct sched_params vm_wdt_params = {0U};

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
	initialize_timer(&vm_wdt_timer, vm_wdt_timer_callback, NULL, 0UL, 0UL);

	vm_wdt_params.prio = PRIO_LOW;
	init_thread_data(&vm_wdt_thread, &vm_wdt_params);
	wake_thread(&vm_wdt_thread);
	vm_wdt_started = true;
}

void vm_wdt_reset(const struct acrn_vm *vm)
{
	uint64_t rflags;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	vm_wdt_entries[vm->vm_id].start_tsc = cpu_ticks();
	vm_wdt_entries[vm->vm_id].last_kick_tsc = 0UL;
	vm_wdt_entries[vm->vm_id].kick_count = 0UL;
	vm_wdt_entries[vm->vm_id].timeout_count = 0UL;
	vm_wdt_entries[vm->vm_id].last_token = 0UL;
	vm_wdt_entries[vm->vm_id].reported_status = VM_WDT_STATUS_UNUSED;
	vm_wdt_entries[vm->vm_id].reported_reason = VM_WDT_REASON_NONE;
	vm_wdt_entries[vm->vm_id].last_irq_total = vm_wdt_irq_total();
	vm_wdt_entries[vm->vm_id].last_irq_sample_tsc = vm_wdt_entries[vm->vm_id].start_tsc;
	vm_wdt_entries[vm->vm_id].last_console_drained = 0UL;
	vm_wdt_entries[vm->vm_id].last_console_queued = 0U;
	vm_wdt_entries[vm->vm_id].console_stuck_periods = 0U;
	vm_wdt_entries[vm->vm_id].last_virtio_timeout_count = 0UL;
	vm_wdt_entries[vm->vm_id].last_virtio_pending_active = 0U;
	vm_wdt_entries[vm->vm_id].virtio_stuck_periods = 0U;
	vm_wdt_entries[vm->vm_id].timeout_active = false;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
}

void vm_wdt_kick(const struct acrn_vm *vm, uint64_t token)
{
	uint64_t rflags;
	uint64_t now;
	uint64_t age_ticks = 0UL;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	if (vm_wdt_heartbeat_started(&vm_wdt_entries[vm->vm_id])) {
		age_ticks = vm_wdt_heartbeat_age_ticks(now, &vm_wdt_entries[vm->vm_id]);
		vm_wdt_record_timeout_locked(&vm_wdt_entries[vm->vm_id],
			vm_wdt_is_timeout_age(age_ticks));
	}
	vm_wdt_entries[vm->vm_id].last_kick_tsc = now;
	vm_wdt_entries[vm->vm_id].kick_count++;
	vm_wdt_entries[vm->vm_id].timeout_active = false;
	vm_wdt_entries[vm->vm_id].last_token = token;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	vm_wdt_print_hcall(vm->vm_id, ticks_to_ms(age_ticks));
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
	snapshot->reason = VM_WDT_REASON_NONE;
	snapshot->last_ms = 0UL;
	snapshot->kick_count = 0UL;
	snapshot->timeout_count = 0UL;
	snapshot->last_token = 0UL;

	vm_config = get_vm_config(vm_id);
	vm = get_vm_from_vmid(vm_id);
	if (!vm_wdt_config_present(vm_config)) {
		return 0;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	entry = vm_wdt_entries[vm_id];
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	if ((vm == NULL) || (vm->state != VM_RUNNING)) {
		snapshot->status = VM_WDT_STATUS_OFFLINE;
		return 0;
	}

	now = cpu_ticks();
	/*
	 * A hypercall kick is the explicit guest heartbeat. Normal VM-exits can
	 * continue while the guest watchdog worker is stuck, so they must not
	 * refresh the timeout basis.
	 */
	age_ticks = vm_wdt_heartbeat_age_ticks(now, &entry);
	if (!vm_wdt_heartbeat_started(&entry)) {
		snapshot->status = VM_WDT_STATUS_UNKNOWN;
		snapshot->last_ms = ticks_to_ms(age_ticks);
		snapshot->kick_count = entry.kick_count;
		snapshot->timeout_count = entry.timeout_count;
		snapshot->last_token = entry.last_token;
		return 0;
	}

	if (vm_wdt_is_timeout_age(age_ticks)) {
		spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
		age_ticks = vm_wdt_heartbeat_age_ticks(now, &vm_wdt_entries[vm_id]);
		if (vm_wdt_heartbeat_started(&vm_wdt_entries[vm_id])) {
			vm_wdt_record_timeout_locked(&vm_wdt_entries[vm_id],
				vm_wdt_is_timeout_age(age_ticks));
		}
		if (vm_wdt_heartbeat_started(&vm_wdt_entries[vm_id]) &&
			vm_wdt_is_timeout_age(age_ticks)) {
			snapshot->reason = vm_wdt_classify_locked(vm_id, vm,
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
		snapshot->reason = VM_WDT_REASON_HEARTBEAT;
	}

	snapshot->last_ms = ticks_to_ms(age_ticks);
	snapshot->kick_count = entry.kick_count;
	snapshot->timeout_count = entry.timeout_count;
	snapshot->last_token = entry.last_token;

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
