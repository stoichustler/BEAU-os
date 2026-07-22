/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <memory.h>
#include <pgtable.h>
#include <spinlock.h>
#include <vm.h>
#include <vconfig.h>
#include <guest_memory.h>
#include <asm/mmu.h>
#include <asm/platform.h>
#include <debug/ramlog.h>

#define RAMLOG_MAGIC            0x524c4f47U
#define RAMLOG_VERSION          3U
#define RAMLOG_VALID             0x56414c49U
#define RAMLOG_HEADER_SIZE       0x1000U
#define RAMLOG_PSTORE_SIZE       0x00400000U
#define RAMLOG_PSTORE_DMESG_SIZE 0x00200000U
#define RAMLOG_PSTORE_CONSOLE_SIZE 0x00200000U
#define RAMLOG_PSTORE_SNAPSHOT_SIZE RAMLOG_PSTORE_SIZE
#define RAMLOG_PSTORE_HEADER_SIZE 12U
#define RAMLOG_PSTORE_COPY_SIZE  0x1000U
#define RAMLOG_PSTORE_SIGNATURE  0x43474244U

#define RAMLOG_SNAPSHOT_VALID    (1U << 0U)
#define RAMLOG_SNAPSHOT_CAPTURING (1U << 1U)

struct ramlog_slot_header {
	uint32_t offset;
	uint32_t size;
	uint32_t live_offset;
	uint32_t live_size;
	uint32_t snapshot_size;
	uint32_t snapshot_dmesg_size;
	uint32_t snapshot_console_size;
	uint32_t snapshot_flags;
	uint64_t prod;
	uint64_t cons;
	uint64_t stored_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t snapshot_generation;
	uint64_t snapshot_failures;
};

struct ramlog_pstore_buffer_header {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
};

enum ramlog_pstore_header_status {
	RAMLOG_PSTORE_HEADER_VALID,
	RAMLOG_PSTORE_HEADER_INVALID_ARGUMENT,
	RAMLOG_PSTORE_HEADER_COPY_FAILED,
	RAMLOG_PSTORE_HEADER_SIGNATURE_INVALID,
	RAMLOG_PSTORE_HEADER_SIZE_INVALID,
	RAMLOG_PSTORE_HEADER_START_INVALID,
};

struct ramlog_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t total_size;
	uint32_t checksum;
	uint32_t valid;
	uint64_t generation;
	struct ramlog_slot_header slots[RAMLOG_VM_SLOT_COUNT];
};

_Static_assert(sizeof(struct ramlog_header) <= RAMLOG_HEADER_SIZE,
	"ramlog header exceeds reserved header page");
_Static_assert(sizeof(struct ramlog_pstore_buffer_header) == RAMLOG_PSTORE_HEADER_SIZE,
	"ramoops header size changed");
_Static_assert((RAMLOG_PSTORE_DMESG_SIZE + RAMLOG_PSTORE_CONSOLE_SIZE) ==
	RAMLOG_PSTORE_SIZE, "ramoops zone layout changed");

static struct ramlog_header *ramlog_header;
static spinlock_t ramlog_lock;
static bool ramlog_ready;

/* [20260721] Persistent VM log publication
 *
 * payload bytes -> cache clean -> slot cursor -> checksum -> valid marker
 *
 * Key rule:
 *   - the reserved-memory header owns all slot metadata as one integrity unit;
 *   - one lock serializes header publication so a reset never accepts a mixed
 *     checksum from concurrent VM writers;
 *   - payload is visible before the valid header publishes its new cursor.
 */

static uint32_t ramlog_checksum(const struct ramlog_header *header)
{
	const uint8_t *bytes = (const uint8_t *)header;
	uint32_t checksum = 2166136261U;

	for (uint32_t index = 0U; index < sizeof(*header); index++) {
		bool checksum_byte = (index >= offsetof(struct ramlog_header, checksum)) &&
			(index < (offsetof(struct ramlog_header, checksum) +
			 sizeof(header->checksum)));
		bool valid_byte = (index >= offsetof(struct ramlog_header, valid)) &&
			(index < (offsetof(struct ramlog_header, valid) + sizeof(header->valid)));

		if (!checksum_byte && !valid_byte) {
			checksum ^= bytes[index];
			checksum *= 16777619U;
		}
	}

	return checksum;
}

static uint32_t ramlog_slot_size(uint16_t vmid)
{
	return vmid < beau_config.ramlog_rtos_vm_count ?
		beau_config.ramlog_rtos_size : beau_config.ramlog_linux_size;
}

static uint32_t ramlog_snapshot_size(uint16_t vmid)
{
	const struct acrn_vm_config *vm_config;

	if (vmid >= CONFIG_MAX_VM_NUM) {
		return 0U;
	}
	vm_config = get_vm_config(vmid);
	return (vm_config->arch.guest_pstore_size == RAMLOG_PSTORE_SIZE) ?
		RAMLOG_PSTORE_SNAPSHOT_SIZE : 0U;
}

static bool ramlog_slot_layout_valid(const struct ramlog_slot_header *slot,
	uint16_t vmid, uint32_t offset)
{
	uint32_t snapshot_size = ramlog_snapshot_size(vmid);

	return (slot->offset == offset) && (slot->size == ramlog_slot_size(vmid)) &&
		(slot->snapshot_size == snapshot_size) &&
		(slot->live_offset == snapshot_size) &&
		(slot->live_size == (slot->size - snapshot_size)) &&
		(slot->live_size != 0U);
}

static bool ramlog_layout_valid(const struct ramlog_header *header)
{
	uint32_t offset;

	if ((header == NULL) || (header->magic != RAMLOG_MAGIC) ||
		(header->version != RAMLOG_VERSION) ||
		(header->header_size != RAMLOG_HEADER_SIZE) ||
		(header->total_size != beau_config.ramlog_size) ||
		(header->valid != RAMLOG_VALID) ||
		(header->checksum != ramlog_checksum(header))) {
		return false;
	}

	offset = RAMLOG_HEADER_SIZE;
	for (uint16_t vmid = 0U; vmid < RAMLOG_VM_SLOT_COUNT; vmid++) {
		if (!ramlog_slot_layout_valid(&header->slots[vmid], vmid, offset)) {
			return false;
		}
		offset += header->slots[vmid].size;
	}

	return offset == beau_config.ramlog_size;
}

static void ramlog_flush(const volatile void *address, uint64_t size)
{
	if (arm64_mmu_is_enabled()) {
		flush_cache_range(address, size);
	}
}

static void ramlog_publish(void)
{
	ramlog_header->valid = 0U;
	ramlog_flush(ramlog_header, RAMLOG_HEADER_SIZE);
	ramlog_header->checksum = ramlog_checksum(ramlog_header);
	ramlog_header->valid = RAMLOG_VALID;
	ramlog_flush(ramlog_header, RAMLOG_HEADER_SIZE);
}

static void ramlog_initialize_header(void)
{
	uint32_t offset = RAMLOG_HEADER_SIZE;

	(void)memset(ramlog_header, 0U, RAMLOG_HEADER_SIZE);
	ramlog_header->magic = RAMLOG_MAGIC;
	ramlog_header->version = RAMLOG_VERSION;
	ramlog_header->header_size = RAMLOG_HEADER_SIZE;
	ramlog_header->total_size = (uint32_t)beau_config.ramlog_size;
	ramlog_header->generation = 1UL;
	for (uint16_t vmid = 0U; vmid < RAMLOG_VM_SLOT_COUNT; vmid++) {
		ramlog_header->slots[vmid].offset = offset;
		ramlog_header->slots[vmid].size = ramlog_slot_size(vmid);
		ramlog_header->slots[vmid].snapshot_size = ramlog_snapshot_size(vmid);
		ramlog_header->slots[vmid].live_offset =
			ramlog_header->slots[vmid].snapshot_size;
		ramlog_header->slots[vmid].live_size =
			ramlog_header->slots[vmid].size -
			ramlog_header->slots[vmid].snapshot_size;
		offset += ramlog_header->slots[vmid].size;
	}
	ramlog_publish();
}

void ramlog_init(void)
{
	if ((beau_config.ramlog_base == 0UL) ||
		(beau_config.ramlog_size != (RAMLOG_HEADER_SIZE +
		((uint64_t)beau_config.ramlog_rtos_vm_count * beau_config.ramlog_rtos_size) +
		((uint64_t)beau_config.ramlog_linux_vm_count * beau_config.ramlog_linux_size))) ||
		(beau_config.ramlog_rtos_vm_count + beau_config.ramlog_linux_vm_count !=
		 RAMLOG_VM_SLOT_COUNT)) {

		return;
	}

	ramlog_header = (struct ramlog_header *)hpa2hva_early(beau_config.ramlog_base);
	spinlock_init(&ramlog_lock);
	if (ramlog_layout_valid(ramlog_header)) {
		ramlog_header->generation++;
		ramlog_publish();
	} else {
		ramlog_initialize_header();
	}
	ramlog_ready = true;
}

bool ramlog_append_vm_console(uint16_t vmid, const char *buffer, uint32_t length)
{
	struct ramlog_slot_header *slot;
	char *data;
	uint64_t rflags;
	uint32_t first;
	uint32_t original_length = length;

	if (!ramlog_ready || (buffer == NULL) || (length == 0U) ||
		(vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return false;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	data = (char *)ramlog_header + slot->offset + slot->live_offset;
	if (length > slot->live_size) {
		buffer += length - slot->live_size;
		length = slot->live_size;
		slot->dropped_bytes += original_length - length;
		slot->overflow_events++;
	}
	first = slot->live_size - (uint32_t)(slot->prod % slot->live_size);
	if (first > length) {
		first = length;
	}
	(void)memcpy(data + (slot->prod % slot->live_size), buffer, first);
	if (first < length) {
		(void)memcpy(data, buffer + first, length - first);
	}
	ramlog_flush(data + (slot->prod % slot->live_size), first);
	if (first < length) {
		ramlog_flush(data, length - first);
	}
	slot->prod += length;
	if ((slot->prod - slot->cons) > slot->live_size) {
		slot->dropped_bytes += (slot->prod - slot->cons) - slot->live_size;
		slot->overflow_events++;
		slot->cons = slot->prod - slot->live_size;
	}
	slot->stored_bytes += original_length;
	ramlog_publish();
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return true;
}

bool ramlog_get_stats(uint16_t vmid, struct ramlog_stats *stats)
{
	const struct ramlog_slot_header *slot;
	uint64_t rflags;

	if (!ramlog_ready || (stats == NULL) || (vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return false;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	stats->vmid = vmid;
	stats->capacity = slot->live_size;
	stats->snapshot_capacity = slot->snapshot_size;
	stats->snapshot_dmesg_bytes = slot->snapshot_dmesg_size;
	stats->snapshot_console_bytes = slot->snapshot_console_size;
	stats->generation = ramlog_header->generation;
	stats->snapshot_generation = slot->snapshot_generation;
	stats->snapshot_failures = slot->snapshot_failures;
	stats->queued = slot->prod - slot->cons;
	stats->stored_bytes = slot->stored_bytes;
	stats->dropped_bytes = slot->dropped_bytes;
	stats->overflow_events = slot->overflow_events;
	stats->retained = true;
	stats->snapshot_valid = (slot->snapshot_flags & RAMLOG_SNAPSHOT_VALID) != 0U;
	stats->snapshot_capturing =
		(slot->snapshot_flags & RAMLOG_SNAPSHOT_CAPTURING) != 0U;
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return true;
}

bool ramlog_get_window(uint16_t vmid, struct ramlog_window *window)
{
	const struct ramlog_slot_header *slot;
	uint64_t rflags;

	if (!ramlog_ready || (window == NULL) || (vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return false;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	window->oldest = slot->cons;
	window->next = slot->prod;
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return true;
}

uint32_t ramlog_read_vm_console(uint16_t vmid, uint64_t *cursor, char *buffer,
	uint32_t length, uint64_t *skipped)
{
	const struct ramlog_slot_header *slot;
	const char *data;
	uint64_t rflags;
	uint32_t count = 0U;
	uint32_t first;
	uint64_t read_cursor;

	if (!ramlog_ready || (cursor == NULL) || (buffer == NULL) || (length == 0U) ||
		(vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return 0U;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	read_cursor = *cursor;
	if (read_cursor < slot->cons) {
		if (skipped != NULL) {
			*skipped += slot->cons - read_cursor;
		}
		read_cursor = slot->cons;
	} else if (read_cursor > slot->prod) {
		read_cursor = slot->prod;
	}
	if (read_cursor < slot->prod) {
		count = (uint32_t)(slot->prod - read_cursor);
		if (count > length) {
			count = length;
		}
		data = (const char *)ramlog_header + slot->offset + slot->live_offset;
		first = slot->live_size - (uint32_t)(read_cursor % slot->live_size);
		if (first > count) {
			first = count;
		}
		(void)memcpy(buffer, data + (read_cursor % slot->live_size), first);
		if (first < count) {
			(void)memcpy(buffer + first, data, count - first);
		}
		read_cursor += count;
	}
	*cursor = read_cursor;
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return count;
}

uint32_t ramlog_copy(uint16_t vmid, uint64_t offset, char *buffer, uint32_t length)
{
	const struct ramlog_slot_header *slot;
	const char *data;
	uint64_t rflags;
	uint64_t queued;
	uint32_t count = 0U;
	uint32_t first;
	uint64_t cursor;

	if (!ramlog_ready || (buffer == NULL) || (length == 0U) ||
		(vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return 0U;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	queued = slot->prod - slot->cons;
	if (offset < queued) {
		count = (uint32_t)(queued - offset);
		if (count > length) {
			count = length;
		}
		data = (const char *)ramlog_header + slot->offset + slot->live_offset;
		cursor = slot->cons + offset;
		first = slot->live_size - (uint32_t)(cursor % slot->live_size);
		if (first > count) {
			first = count;
		}
		(void)memcpy(buffer, data + (cursor % slot->live_size), first);
		if (first < count) {
			(void)memcpy(buffer + first, data, count - first);
		}
	}
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return count;
}

uint32_t ramlog_copy_snapshot(uint16_t vmid, uint64_t offset, char *buffer,
	uint32_t length)
{
	const struct ramlog_slot_header *slot;
	const char *data;
	uint64_t rflags;
	uint64_t size;
	uint32_t count = 0U;

	if (!ramlog_ready || (buffer == NULL) || (length == 0U) ||
		(vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return 0U;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	if ((slot->snapshot_flags & RAMLOG_SNAPSHOT_VALID) != 0U) {
		size = (uint64_t)slot->snapshot_dmesg_size + slot->snapshot_console_size;
		if (offset < size) {
			count = (uint32_t)(size - offset);
			if (count > length) {
				count = length;
			}
			data = (const char *)ramlog_header + slot->offset;
			(void)memcpy(buffer, data + offset, count);
		}
	}
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return count;
}

static enum ramlog_pstore_header_status ramlog_pstore_header_read(struct acrn_vm *vm,
	uint64_t gpa,
	uint32_t zone_size, uint32_t expected_signature,
	struct ramlog_pstore_buffer_header *header)
{
	uint32_t capacity;

	if ((header == NULL) || (zone_size <= RAMLOG_PSTORE_HEADER_SIZE)) {
		return RAMLOG_PSTORE_HEADER_INVALID_ARGUMENT;
	}
	if (copy_from_gpa(vm, header, gpa, sizeof(*header)) != 0) {
		return RAMLOG_PSTORE_HEADER_COPY_FAILED;
	}
	if (header->sig != expected_signature) {
		return RAMLOG_PSTORE_HEADER_SIGNATURE_INVALID;
	}
	capacity = zone_size - RAMLOG_PSTORE_HEADER_SIZE;
	if (header->size > capacity) {
		return RAMLOG_PSTORE_HEADER_SIZE_INVALID;
	}
	if (header->start > header->size) {
		return RAMLOG_PSTORE_HEADER_START_INVALID;
	}

	return RAMLOG_PSTORE_HEADER_VALID;
}

static const char *ramlog_pstore_header_status_name(
	enum ramlog_pstore_header_status status)
{
	switch (status) {
	case RAMLOG_PSTORE_HEADER_VALID:
		return "valid";
	case RAMLOG_PSTORE_HEADER_INVALID_ARGUMENT:
		return "argument";
	case RAMLOG_PSTORE_HEADER_COPY_FAILED:
		return "copy";
	case RAMLOG_PSTORE_HEADER_SIGNATURE_INVALID:
		return "signature";
	case RAMLOG_PSTORE_HEADER_SIZE_INVALID:
		return "size";
	case RAMLOG_PSTORE_HEADER_START_INVALID:
		return "start";
	default:
		return "unknown";
	}
}

static bool ramlog_pstore_copy(struct acrn_vm *vm, uint64_t zone_gpa,
	uint32_t zone_size, const struct ramlog_pstore_buffer_header *header,
	uint32_t offset, char *destination, uint32_t length)
{
	uint32_t capacity = zone_size - RAMLOG_PSTORE_HEADER_SIZE;
	uint32_t cursor;

	if ((header == NULL) || (destination == NULL) || (offset > header->size) ||
		(length > (header->size - offset))) {
		return false;
	}

	cursor = (header->start + offset) % capacity;
	while (length != 0U) {
		uint32_t chunk = capacity - cursor;

		if (chunk > length) {
			chunk = length;
		}
		if (chunk > RAMLOG_PSTORE_COPY_SIZE) {
			chunk = RAMLOG_PSTORE_COPY_SIZE;
		}
		if (copy_from_gpa(vm, destination,
			zone_gpa + RAMLOG_PSTORE_HEADER_SIZE + cursor, chunk) != 0) {
			return false;
		}
		ramlog_flush(destination, chunk);
		destination += chunk;
		length -= chunk;
		cursor = 0U;
	}

	return true;
}

static bool ramlog_pstore_dmesg_valid(struct acrn_vm *vm, uint64_t zone_gpa,
	const struct ramlog_pstore_buffer_header *header)
{
	char sample[32U];
	uint32_t sample_size = header->size;

	if (sample_size > sizeof(sample)) {
		sample_size = sizeof(sample);
	}
	if ((sample_size < 8U) || !ramlog_pstore_copy(vm, zone_gpa,
		RAMLOG_PSTORE_DMESG_SIZE, header, 0U, sample, sample_size) ||
		(memcmp(sample, "====", 4U) != 0)) {
		return false;
	}
	for (uint32_t index = 4U; (index + 2U) < sample_size; index++) {
		if ((sample[index] == '-') && (sample[index + 1U] == 'D') &&
			(sample[index + 2U] == '\n')) {
			return true;
		}
	}

	return false;
}

/* [20260722] Linux ramoops reset snapshot
 *
 * paused Linux VM -> validate persistent-RAM headers -> copy circular records
 *        |                                                    |
 *        |                                                    v
 *        |                                              cache-clean payload
 *        v                                                    |
 * reject malformed guest state <--- publish snapshot metadata <---+
 *
 * Key rule:
 *   - the paused guest owns the ramoops bytes until this reset boundary;
 *   - console and dmesg headers share the ramoops DBGC signature, while only
 *     dmesg requires a valid uncompressed record header;
 *   - a WDT reset may have no dmesg record, but a bounded valid console record
 *     may still become a retained ramlog snapshot;
 *   - live console bytes use a disjoint subregion, preventing the next boot
 *     from overwriting the crash record before shell inspection.
 */
bool ramlog_capture_vm_pstore(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config;
	struct ramlog_pstore_buffer_header dmesg_header = { 0U };
	struct ramlog_pstore_buffer_header console_header = { 0U };
	struct ramlog_slot_header *slot;
	char *snapshot;
	uint64_t pstore_base;
	uint64_t rflags;
	uint32_t dmesg_size = 0U;
	uint32_t console_size = 0U;
	uint32_t console_offset = 0U;
	enum ramlog_pstore_header_status dmesg_status;
	enum ramlog_pstore_header_status console_status;
	const char *dmesg_result;
	const char *console_result;
	bool dmesg_valid;
	bool console_valid;
	bool captured = false;

	if (!ramlog_ready || (vm == NULL) || (vm->vm_id >= RAMLOG_VM_SLOT_COUNT)) {
		return false;
	}
	arch_config = &get_vm_config(vm->vm_id)->arch;
	if ((arch_config->guest_pstore_size != RAMLOG_PSTORE_SIZE) ||
		(arch_config->guest_pstore_base == 0UL)) {
		return false;
	}
	pstore_base = arch_config->guest_pstore_base;
	dmesg_status = ramlog_pstore_header_read(vm, pstore_base,
		RAMLOG_PSTORE_DMESG_SIZE, RAMLOG_PSTORE_SIGNATURE, &dmesg_header);
	dmesg_valid = dmesg_status == RAMLOG_PSTORE_HEADER_VALID;
	if (dmesg_valid) {
		dmesg_valid = ramlog_pstore_dmesg_valid(vm, pstore_base, &dmesg_header);
	}
	dmesg_result = dmesg_valid ? "valid" :
		((dmesg_status == RAMLOG_PSTORE_HEADER_VALID) ? "payload" :
		 ramlog_pstore_header_status_name(dmesg_status));
	console_status = ramlog_pstore_header_read(vm,
		pstore_base + RAMLOG_PSTORE_DMESG_SIZE, RAMLOG_PSTORE_CONSOLE_SIZE,
		RAMLOG_PSTORE_SIGNATURE, &console_header);
	console_valid = (console_status == RAMLOG_PSTORE_HEADER_VALID) &&
		(console_header.size != 0U);
	console_result = console_valid ? "valid" :
		((console_status == RAMLOG_PSTORE_HEADER_VALID) ? "empty" :
		 ramlog_pstore_header_status_name(console_status));
	if (!dmesg_valid && !console_valid) {
		if ((dmesg_header.sig != 0U) || (console_header.sig != 0U) ||
			(dmesg_status == RAMLOG_PSTORE_HEADER_COPY_FAILED) ||
			(console_status == RAMLOG_PSTORE_HEADER_COPY_FAILED)) {
			LOG_WRN("BUG: vm%u pstore rejected dmesg:%s console:%s",
				vm->vm_id, dmesg_result, console_result);
		}
		spinlock_irqsave_obtain(&ramlog_lock, &rflags);
		ramlog_header->slots[vm->vm_id].snapshot_failures++;
		ramlog_publish();
		spinlock_irqrestore_release(&ramlog_lock, rflags);
		return false;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vm->vm_id];
	if (slot->snapshot_size != RAMLOG_PSTORE_SNAPSHOT_SIZE) {
		spinlock_irqrestore_release(&ramlog_lock, rflags);
		return false;
	}
	slot->snapshot_flags = RAMLOG_SNAPSHOT_CAPTURING;
	slot->snapshot_dmesg_size = 0U;
	slot->snapshot_console_size = 0U;
	ramlog_publish();
	snapshot = (char *)ramlog_header + slot->offset;
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	if (dmesg_valid) {
		dmesg_size = dmesg_header.size;
		captured = ramlog_pstore_copy(vm, pstore_base, RAMLOG_PSTORE_DMESG_SIZE,
			&dmesg_header, 0U, snapshot, dmesg_size);
	}
	if (captured && console_valid) {
		console_size = console_header.size;
		if (console_size > (RAMLOG_PSTORE_SNAPSHOT_SIZE - dmesg_size)) {
			console_size = RAMLOG_PSTORE_SNAPSHOT_SIZE - dmesg_size;
		}
		console_offset = console_header.size - console_size;
		captured = ramlog_pstore_copy(vm,
			pstore_base + RAMLOG_PSTORE_DMESG_SIZE,
			RAMLOG_PSTORE_CONSOLE_SIZE, &console_header, console_offset,
			snapshot + dmesg_size, console_size);
	} else if (!dmesg_valid && console_valid) {
		console_size = console_header.size;
		if (console_size > RAMLOG_PSTORE_SNAPSHOT_SIZE) {
			console_size = RAMLOG_PSTORE_SNAPSHOT_SIZE;
		}
		console_offset = console_header.size - console_size;
		captured = ramlog_pstore_copy(vm,
			pstore_base + RAMLOG_PSTORE_DMESG_SIZE,
			RAMLOG_PSTORE_CONSOLE_SIZE, &console_header, console_offset,
			snapshot, console_size);
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vm->vm_id];
	if (captured) {
		slot->snapshot_dmesg_size = dmesg_size;
		slot->snapshot_console_size = console_size;
		slot->snapshot_flags = RAMLOG_SNAPSHOT_VALID;
		slot->snapshot_generation++;
	} else {
		slot->snapshot_dmesg_size = 0U;
		slot->snapshot_console_size = 0U;
		slot->snapshot_flags = 0U;
		slot->snapshot_failures++;
	}
	ramlog_publish();
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return captured;
}
