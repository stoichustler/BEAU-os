/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cpu.h>
#include <errno.h>
#include <guest_memory.h>
#include <logmsg.h>
#include <rtl.h>
#include <spinlock.h>
#include <ticks.h>
#include <vm.h>
#include <vm_crash.h>

#include "../shell_priv.h"

/* [20260726] Guest crash report ownership
 *
 * Linux panic/oops -> bounded HVC report -> validate/copy -> checksum -> LOG_ERR
 *                                                        |
 *                                                        v
 *                                          per-VM bounded crash history ring
 *
 * Key rule:
 *   - the guest supplies diagnostic data but never a VM identity or host pointer;
 *   - Host copies and sanitizes a fixed-size report before it becomes observable;
 *   - records are published only after checksumming, then remain immutable until
 *     their oldest history slot is reused;
 *   - exact HVC retries do not consume another slot, while distinct crash events
 *     remain available across guest restarts until the bounded ring is full.
 */
static struct vm_crash_record
	vm_crash_events[CONFIG_MAX_VM_NUM][BEAU_VM_CRASH_HISTORY_SLOTS];
static uint8_t vm_crash_next_slot[CONFIG_MAX_VM_NUM];
static uint64_t vm_crash_next_sequence[CONFIG_MAX_VM_NUM];
static spinlock_t vm_crash_locks[CONFIG_MAX_VM_NUM];

static uint32_t vm_crash_checksum(const struct vm_crash_record *record)
{
	const uint8_t *bytes = (const uint8_t *)record;
	uint32_t checksum = 2166136261U;
	uint32_t index;

	for (index = sizeof(record->checksum);
		index < offsetof(struct vm_crash_record, valid); index++) {
		checksum ^= bytes[index];
		checksum *= 16777619U;
	}
	for (index = offsetof(struct vm_crash_record, valid) + sizeof(record->valid);
		index < sizeof(*record); index++) {
		checksum ^= bytes[index];
		checksum *= 16777619U;
	}

	return checksum;
}

static bool vm_crash_kind_valid(uint32_t kind)
{
	return (kind == BEAU_VM_CRASH_PANIC) || (kind == BEAU_VM_CRASH_OOPS);
}

static bool vm_crash_version_valid(const struct beau_vm_crash_report *report)
{
	return (report->version == BEAU_VM_CRASH_ABI_VERSION) &&
		(report->size == sizeof(*report));
}

static bool vm_crash_text_terminated(const char *text, uint32_t size)
{
	return (text != NULL) && (strnlen_s(text, size) < size);
}

static bool vm_crash_message_terminated(const struct beau_vm_crash_report *report)
{
	return (report != NULL) && vm_crash_text_terminated(report->message,
		sizeof(report->message));
}

static const char *vm_crash_kind_str(uint32_t kind)
{
	return (kind == BEAU_VM_CRASH_PANIC) ? "panic" : "oops";
}

static bool vm_crash_report_valid(const struct beau_vm_crash_report *report)
{
	return (report != NULL) && (report->magic == BEAU_VM_CRASH_MAGIC) &&
		vm_crash_version_valid(report) && vm_crash_kind_valid(report->kind) &&
		(report->cpu_id < MAX_VCPUS_PER_VM) &&
		vm_crash_message_terminated(report) &&
		(report->stack_count <= BEAU_VM_CRASH_STACK_MAX) &&
		vm_crash_text_terminated(report->comm, sizeof(report->comm)) &&
		((report->flags & ~(BEAU_VM_CRASH_F_REGS_VALID |
			BEAU_VM_CRASH_F_STACK_VALID)) == 0U);
}

static void vm_crash_sanitize_text(char *text, uint32_t size)
{
	uint32_t index;

	for (index = 0U; index < size; index++) {
		uint8_t ch = (uint8_t)text[index];

		if (ch == '\0') {
			break;
		}
		if ((ch < (uint8_t)' ') || (ch > (uint8_t)'~')) {
			text[index] = '?';
		}
	}
}

static bool vm_crash_is_duplicate_locked(uint16_t vm_id, uint64_t generation,
	const struct beau_vm_crash_report *report)
{
	uint32_t slot;

	for (slot = 0U; slot < BEAU_VM_CRASH_HISTORY_SLOTS; slot++) {
		const struct vm_crash_record *record = &vm_crash_events[vm_id][slot];

		if (__atomic_load_n(&record->valid, __ATOMIC_ACQUIRE) &&
			(record->vm_id == vm_id) &&
			(record->lifecycle_generation == generation) &&
			(record->report.kind == report->kind) &&
			(record->report.sequence == report->sequence)) {
			return true;
		}
	}

	return false;
}

int32_t hcall_vm_crash_report(struct acrn_vcpu *vcpu,
	__unused struct acrn_vm *target_vm, uint64_t param1, uint64_t param2)
{
	struct {
		uint32_t magic;
		uint16_t version;
		uint16_t size;
	} header;
	struct beau_vm_crash_report report;
	struct vm_crash_record *record;
	struct acrn_vm *vm;
	uint64_t flags;
	uint64_t generation;
	uint16_t vm_id;
	uint8_t slot;
	int32_t ret;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (param1 == 0UL)) {
		return -EINVAL;
	}
	vm = vcpu->vm;
	vm_id = vm->vm_id;
	if ((vm_id >= CONFIG_MAX_VM_NUM) || is_poweroff_vm(vm)) {
		return -EINVAL;
	}
	ret = copy_from_gpa(vm, &header, param1, sizeof(header));
	if (ret != 0) {
		LOG_ERR("KE:     VM%u reject gpa:0x%016lx header:%d", vm_id, param1, ret);
		return -EINVAL;
	}
	if ((header.magic != BEAU_VM_CRASH_MAGIC) ||
		(header.version != BEAU_VM_CRASH_ABI_VERSION) ||
		(header.size != sizeof(report))) {
		LOG_ERR("KE:     VM%u reject ABI magic:0x%08x ver:%u size:%u",
			vm_id, header.magic, header.version, header.size);
		return -EINVAL;
	}
	(void)memset(&report, 0U, sizeof(report));
	ret = copy_from_gpa(vm, &report, param1, header.size);
	if (ret != 0) {
		LOG_ERR("KE:     VM%u reject gpa:0x%016lx size:%u copy:%d",
			vm_id, param1, header.size, ret);
		return -EINVAL;
	}
	if (!vm_crash_report_valid(&report)) {
		LOG_ERR("KE:     VM%u reject ABI ver:%u size:%u kind:%u cpu:%u stack:%u flags:0x%x",
			vm_id, report.version, report.size, report.kind,
			report.cpu_id, report.stack_count, report.flags);
		return -EINVAL;
	}
	if (param2 != report.sequence) {
		LOG_ERR("KE:     VM%u reject sequence hvc:0x%016lx report:0x%016lx",
			vm_id, param2, report.sequence);
		return -EINVAL;
	}
	vm_crash_sanitize_text(report.message, sizeof(report.message));
	vm_crash_sanitize_text(report.comm, sizeof(report.comm));
	generation = vm->lifecycle.generation;

	spinlock_irqsave_obtain(&vm_crash_locks[vm_id], &flags);
	if (vm_crash_is_duplicate_locked(vm_id, generation, &report)) {
		spinlock_irqrestore_release(&vm_crash_locks[vm_id], flags);
		return 0;
	}
	slot = vm_crash_next_slot[vm_id];
	vm_crash_next_slot[vm_id] = (uint8_t)((slot + 1U) % BEAU_VM_CRASH_HISTORY_SLOTS);
	record = &vm_crash_events[vm_id][slot];
	__atomic_store_n(&record->valid, false, __ATOMIC_RELEASE);
	cpu_write_memory_barrier();
	(void)memset(record, 0U, sizeof(*record));
	record->vm_id = vm_id;
	record->lifecycle_generation = generation;
	record->captured_tsc = cpu_ticks();
	record->capture_sequence = __atomic_add_fetch(&vm_crash_next_sequence[vm_id], 1UL,
		__ATOMIC_RELAXED);
	if (record->capture_sequence == 0UL) {
		record->capture_sequence = __atomic_add_fetch(&vm_crash_next_sequence[vm_id], 1UL,
			__ATOMIC_RELAXED);
	}
	(void)memcpy_s(&record->report, sizeof(record->report), &report, sizeof(report));
	record->checksum = vm_crash_checksum(record);
	cpu_write_memory_barrier();
	__atomic_store_n(&record->valid, true, __ATOMIC_RELEASE);
	spinlock_irqrestore_release(&vm_crash_locks[vm_id], flags);

	LOG_ERR("KE:     VM%u [%5s] sequence:%lu (use 'crash %u' for more)", vm_id,
		vm_crash_kind_str(report.kind), report.sequence, vm_id);
	return 0;
}

static bool vm_crash_record_valid(const struct vm_crash_record *record, uint16_t vm_id)
{
	return (record->vm_id == vm_id) && (record->capture_sequence != 0UL) &&
		vm_crash_report_valid(&record->report) &&
		(record->checksum == vm_crash_checksum(record));
}

uint32_t vm_crash_copy_history(uint16_t vm_id, struct vm_crash_record *records,
	uint32_t records_capacity, bool *corrupt)
{
	uint64_t flags;
	uint32_t count = 0U;
	uint32_t slot;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (records == NULL) || (records_capacity == 0U) ||
		(corrupt == NULL)) {
		return 0U;
	}
	*corrupt = false;
	spinlock_irqsave_obtain(&vm_crash_locks[vm_id], &flags);
	for (slot = 0U; slot < BEAU_VM_CRASH_HISTORY_SLOTS; slot++) {
		const struct vm_crash_record *candidate = &vm_crash_events[vm_id][slot];

		if (__atomic_load_n(&candidate->valid, __ATOMIC_ACQUIRE)) {
			if ((count < records_capacity) && vm_crash_record_valid(candidate, vm_id)) {
				(void)memcpy_s(&records[count], sizeof(records[count]), candidate,
					sizeof(*candidate));
				count++;
			} else {
				*corrupt = true;
			}
		}
	}
	spinlock_irqrestore_release(&vm_crash_locks[vm_id], flags);
	for (slot = 1U; slot < count; slot++) {
		struct vm_crash_record current = records[slot];
		uint32_t index = slot;

		while ((index > 0U) &&
			(records[index - 1U].capture_sequence > current.capture_sequence)) {
			records[index] = records[index - 1U];
			index--;
		}
		records[index] = current;
	}

	return count;
}

void vm_crash_erase(uint16_t vm_id)
{
	uint64_t flags;
	uint32_t slot;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return;
	}
	spinlock_irqsave_obtain(&vm_crash_locks[vm_id], &flags);
	for (slot = 0U; slot < BEAU_VM_CRASH_HISTORY_SLOTS; slot++) {
		__atomic_store_n(&vm_crash_events[vm_id][slot].valid, false, __ATOMIC_RELEASE);
	}
	spinlock_irqrestore_release(&vm_crash_locks[vm_id], flags);
}

void vm_crash_print(const struct vm_crash_record *record, uint32_t history_index,
	uint32_t history_count)
{
	const struct beau_vm_crash_report *report = &record->report;

	shell_item_begin("CRASH (vm%hu guest-%s history:%u/%u)", record->vm_id,
		vm_crash_kind_str(report->kind), history_index + 1U, history_count);
	shell_item_line("  GEN:%lu host-sequence:%lu guest-sequence:%lu captured:0x%016lx cpu:%u",
		record->lifecycle_generation, record->capture_sequence, report->sequence,
		record->captured_tsc, report->cpu_id);
	shell_item_line("   PC:0x%016lx FAR:0x%016lx ERR:0x%016lx",
		report->pc, report->fault_address, report->error_code);

	shell_item_line(" task:pid:%u tgid:%u comm:%s", report->pid,
		report->tgid, report->comm);
	shell_item_line("stack:count:%u", report->stack_count);
	for (uint32_t index = 0U; index < report->stack_count; index++) {
		shell_item_line("stack[%02u]:0x%016lx", index, report->stack[index]);
	}

	shell_item_line("  MSG:%s", report->message);
	shell_item_end();
}
