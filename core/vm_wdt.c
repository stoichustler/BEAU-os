/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
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

struct vm_wdt_entry {
	uint64_t start_tsc;
	uint64_t last_kick_tsc;
	uint64_t kick_count;
	uint64_t timeout_count;
	uint64_t last_token;
	enum vm_wdt_status reported_status;
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

static bool vm_wdt_is_monitored(uint16_t vm_id)
{
	uint16_t max_vm_id = (CONFIG_VM_WDT_MONITOR_VM_NUM < CONFIG_MAX_VM_NUM) ?
		CONFIG_VM_WDT_MONITOR_VM_NUM : CONFIG_MAX_VM_NUM;

	return vm_id < max_vm_id;
}

static bool vm_wdt_should_report(uint16_t vm_id, enum vm_wdt_status status, bool force)
{
	bool report = false;
	uint64_t rflags;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return false;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	if (force || (vm_wdt_entries[vm_id].reported_status != status)) {
		report = true;
	}
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	return report;
}

static void vm_wdt_mark_reported(uint16_t vm_id, enum vm_wdt_status status)
{
	uint64_t rflags;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return;
	}

	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	vm_wdt_entries[vm_id].reported_status = status;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
}

static void vm_wdt_print_one(uint16_t vm_id, const struct vm_wdt_snapshot *snapshot,
	const char *event)
{
	uint64_t timestamp;
	const char *color;
	char timestamp_str[LOG_TIMESTAMP_MAX_SIZE];
	char line[144];

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
	(void)snprintf(line, sizeof(line),
		"%s[κ][%s] event:%7s vm%hu:%7s status:%7s kick:%8lu" SHELL_COLOR_RESET "\r\n",
		color, timestamp_str, event, vm_id, vm_wdt_name(vm_id),
		vm_wdt_status_str(snapshot->status), snapshot->kick_count);
	if (shell_async_puts(line)) {
		vm_wdt_mark_reported(vm_id, snapshot->status);
	}
}

static void vm_wdt_print_hcall(uint16_t vm_id)
{
	struct vm_wdt_snapshot snapshot;

	if ((vm_wdt_get_snapshot(vm_id, &snapshot) == 0) &&
		vm_wdt_should_report(vm_id, snapshot.status, true)) {
		vm_wdt_print_one(vm_id, &snapshot, "hcall");
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
			vm_wdt_should_report(vm_id, snapshot.status, false)) {
			vm_wdt_print_one(vm_id, &snapshot, "timeout");
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
	vm_wdt_entries[vm->vm_id].timeout_active = false;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);
}

void vm_wdt_kick(const struct acrn_vm *vm, uint64_t token)
{
	uint64_t rflags;
	uint64_t now;
	uint64_t age_ticks;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	now = cpu_ticks();
	spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
	age_ticks = vm_wdt_heartbeat_age_ticks(now, &vm_wdt_entries[vm->vm_id]);
	vm_wdt_record_timeout_locked(&vm_wdt_entries[vm->vm_id],
		vm_wdt_is_timeout_age(age_ticks));
	vm_wdt_entries[vm->vm_id].last_kick_tsc = now;
	vm_wdt_entries[vm->vm_id].kick_count++;
	vm_wdt_entries[vm->vm_id].timeout_active = false;
	vm_wdt_entries[vm->vm_id].last_token = token;
	spinlock_irqrestore_release(&vm_wdt_lock, rflags);

	vm_wdt_print_hcall(vm->vm_id);
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
	snapshot->age_ms = 0UL;
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
	if (vm_wdt_is_timeout_age(age_ticks)) {
		spinlock_irqsave_obtain(&vm_wdt_lock, &rflags);
		age_ticks = vm_wdt_heartbeat_age_ticks(now, &vm_wdt_entries[vm_id]);
		vm_wdt_record_timeout_locked(&vm_wdt_entries[vm_id],
			vm_wdt_is_timeout_age(age_ticks));
		entry = vm_wdt_entries[vm_id];
		spinlock_irqrestore_release(&vm_wdt_lock, rflags);
	}
	snapshot->status = !vm_wdt_is_timeout_age(age_ticks) ?
		((entry.kick_count == 0UL) ? VM_WDT_STATUS_UNKNOWN : VM_WDT_STATUS_ALIVE) :
		VM_WDT_STATUS_STUCK;

	snapshot->age_ms = ticks_to_ms(age_ticks);
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
