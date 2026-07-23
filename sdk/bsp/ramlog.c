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
#define RAMLOG_VERSION          4U
#define RAMLOG_VALID             0x56414c49U
#define RAMLOG_HEADER_SIZE       0x1000U
#define RAMLOG_HEADER_COPY_COUNT 2U
#define RAMLOG_HEADER_COPY_SIZE  (RAMLOG_HEADER_SIZE / RAMLOG_HEADER_COPY_COUNT)
#define RAMLOG_PSTORE_SIZE       0x00400000U
#define RAMLOG_PSTORE_DMESG_SIZE 0x00200000U
#define RAMLOG_PSTORE_CONSOLE_SIZE 0x00200000U
#define RAMLOG_PSTORE_SNAPSHOT_SIZE (RAMLOG_SNAPSHOT_BANK_COUNT * RAMLOG_PSTORE_SIZE)
#define RAMLOG_PSTORE_HEADER_SIZE 12U
#define RAMLOG_PSTORE_COPY_SIZE  0x1000U
#define RAMLOG_PSTORE_SIGNATURE  0x43474244U
#define RAMLOG_SNAPSHOT_MAGIC    0x4245415550535445UL

struct ramlog_snapshot_descriptor {
	uint64_t magic;
	uint64_t length;
	uint64_t nmagic;
	uint64_t nlength;
	uint64_t generation;
	uint32_t dmesg_size;
	uint32_t console_size;
	uint32_t checksum;
	uint16_t state;
	uint16_t failure;
};

struct ramlog_slot_header {
	uint32_t offset;
	uint32_t size;
	uint32_t live_offset;
	uint32_t live_size;
	uint32_t snapshot_size;
	uint32_t snapshot_bank_size;
	uint64_t prod;
	uint64_t cons;
	uint64_t stored_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t snapshot_failures;
	uint8_t snapshot_active_bank;
	uint8_t snapshot_capturing_bank;
	uint16_t snapshot_last_failure;
	struct ramlog_snapshot_descriptor snapshots[RAMLOG_SNAPSHOT_BANK_COUNT];
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
	uint64_t sequence;
	struct ramlog_slot_header slots[RAMLOG_VM_SLOT_COUNT];
};

_Static_assert(sizeof(struct ramlog_header) <= RAMLOG_HEADER_COPY_SIZE,
	"ramlog header exceeds redundant header copy");
_Static_assert(sizeof(struct ramlog_pstore_buffer_header) == RAMLOG_PSTORE_HEADER_SIZE,
	"ramoops header size changed");
_Static_assert((RAMLOG_PSTORE_DMESG_SIZE + RAMLOG_PSTORE_CONSOLE_SIZE) ==
	RAMLOG_PSTORE_SIZE, "ramoops zone layout changed");
_Static_assert(sizeof(struct ramlog_snapshot_descriptor) == 56U,
	"ramlog snapshot descriptor changed");

static struct ramlog_header ramlog_working_header;
static struct ramlog_header *ramlog_header = &ramlog_working_header;
static char *ramlog_base;
static uint8_t ramlog_active_header_copy;
static spinlock_t ramlog_lock;
static bool ramlog_ready;

/* [20260723] Persistent VM log publication
 *
 * working metadata -> inactive header copy -> cache clean -> valid marker
 *
 * Key rule:
 *   - the BSS working header owns mutable metadata; retained memory holds only
 *     committed copies, so a torn publication leaves the prior copy intact;
 *   - one lock serializes working-metadata changes and selects exactly one
 *     inactive header target for each publication;
 *   - payload cache clean and descriptor commit happen before the header valid
 *     marker makes a new cursor or active snapshot bank visible after reset.
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
		(slot->snapshot_bank_size ==
			(snapshot_size == 0U ? 0U : RAMLOG_PSTORE_SIZE)) &&
		(slot->live_offset == snapshot_size) &&
		(slot->live_size == (slot->size - snapshot_size)) &&
		(slot->live_size != 0U) &&
		((slot->snapshot_active_bank < RAMLOG_SNAPSHOT_BANK_COUNT) ||
			(slot->snapshot_active_bank == RAMLOG_SNAPSHOT_BANK_INVALID)) &&
		((slot->snapshot_capturing_bank < RAMLOG_SNAPSHOT_BANK_COUNT) ||
			(slot->snapshot_capturing_bank == RAMLOG_SNAPSHOT_BANK_INVALID));
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

static void ramlog_clean(const volatile void *address, uint64_t size)
{
	if (arm64_mmu_is_enabled()) {
		clean_cache_range(address, size);
	}
}

static uint32_t ramlog_payload_checksum(const void *address, uint64_t size)
{
	const uint8_t *bytes = address;
	uint32_t checksum = 2166136261U;
	uint64_t index;

	for (index = 0UL; index < size; index++) {
		checksum ^= bytes[index];
		checksum *= 16777619U;
	}

	return checksum;
}

static bool ramlog_snapshot_bank_valid(uint8_t bank)
{
	return bank < RAMLOG_SNAPSHOT_BANK_COUNT;
}

static char *ramlog_snapshot_bank_data(const struct ramlog_slot_header *slot,
	uint8_t bank)
{
	return ramlog_base + slot->offset +
		((uint64_t)bank * slot->snapshot_bank_size);
}

static bool ramlog_snapshot_descriptor_valid(
	const struct ramlog_snapshot_descriptor *snapshot, uint32_t bank_size)
{
	uint64_t payload_size;

	if ((snapshot == NULL) ||
		(snapshot->state != RAMLOG_SNAPSHOT_VALID) ||
		(snapshot->magic != RAMLOG_SNAPSHOT_MAGIC) ||
		(snapshot->nmagic != ~RAMLOG_SNAPSHOT_MAGIC) ||
		(snapshot->length > bank_size) ||
		(snapshot->nlength != ~snapshot->length)) {
		return false;
	}

	payload_size = (uint64_t)snapshot->dmesg_size + snapshot->console_size;
	return payload_size == snapshot->length;
}

static bool ramlog_snapshot_payload_valid(const struct ramlog_slot_header *slot,
	uint8_t bank)
{
	const struct ramlog_snapshot_descriptor *snapshot;

	if (!ramlog_snapshot_bank_valid(bank)) {
		return false;
	}

	snapshot = &slot->snapshots[bank];
	return ramlog_snapshot_descriptor_valid(snapshot, slot->snapshot_bank_size) &&
		(snapshot->checksum == ramlog_payload_checksum(
			ramlog_snapshot_bank_data(slot, bank), snapshot->length));
}

static void ramlog_snapshot_clear(struct ramlog_snapshot_descriptor *snapshot,
	enum ramlog_snapshot_state state, enum ramlog_snapshot_failure failure)
{
	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->state = (uint16_t)state;
	snapshot->failure = (uint16_t)failure;
}

static void ramlog_snapshot_commit(struct ramlog_snapshot_descriptor *snapshot,
	uint64_t generation, uint32_t dmesg_size, uint32_t console_size,
	uint32_t checksum)
{
	uint64_t length = (uint64_t)dmesg_size + console_size;

	ramlog_snapshot_clear(snapshot, RAMLOG_SNAPSHOT_VALID,
		RAMLOG_SNAPSHOT_FAILURE_NONE);
	snapshot->length = length;
	snapshot->nlength = ~length;
	snapshot->generation = generation;
	snapshot->dmesg_size = dmesg_size;
	snapshot->console_size = console_size;
	snapshot->checksum = checksum;
	cpu_write_memory_barrier();
	snapshot->magic = RAMLOG_SNAPSHOT_MAGIC;
	snapshot->nmagic = ~RAMLOG_SNAPSHOT_MAGIC;
}

static bool ramlog_snapshot_recover_slot(struct ramlog_slot_header *slot)
{
	uint8_t active_bank = RAMLOG_SNAPSHOT_BANK_INVALID;
	uint8_t bank;
	bool changed = false;

	for (bank = 0U; bank < RAMLOG_SNAPSHOT_BANK_COUNT; bank++) {
		struct ramlog_snapshot_descriptor *snapshot = &slot->snapshots[bank];

		if (snapshot->state == RAMLOG_SNAPSHOT_CAPTURING) {
			ramlog_snapshot_clear(snapshot, RAMLOG_SNAPSHOT_CAPTURE_FAILED,
				RAMLOG_SNAPSHOT_FAILURE_INTERRUPTED);
			slot->snapshot_failures++;
			slot->snapshot_last_failure = RAMLOG_SNAPSHOT_FAILURE_INTERRUPTED;
			changed = true;
		} else if ((snapshot->state == RAMLOG_SNAPSHOT_VALID) &&
			!ramlog_snapshot_payload_valid(slot, bank)) {
			ramlog_snapshot_clear(snapshot, RAMLOG_SNAPSHOT_CORRUPT,
				RAMLOG_SNAPSHOT_FAILURE_CHECKSUM);
			slot->snapshot_last_failure = RAMLOG_SNAPSHOT_FAILURE_CHECKSUM;
			changed = true;
		} else if (snapshot->state > RAMLOG_SNAPSHOT_CAPTURE_FAILED) {
			ramlog_snapshot_clear(snapshot, RAMLOG_SNAPSHOT_CORRUPT,
				RAMLOG_SNAPSHOT_FAILURE_CHECKSUM);
			slot->snapshot_last_failure = RAMLOG_SNAPSHOT_FAILURE_CHECKSUM;
			changed = true;
		}

		if (ramlog_snapshot_payload_valid(slot, bank) &&
			((active_bank == RAMLOG_SNAPSHOT_BANK_INVALID) ||
			(snapshot->generation > slot->snapshots[active_bank].generation))) {
			active_bank = bank;
		}
	}

	if (slot->snapshot_active_bank != active_bank) {
		slot->snapshot_active_bank = active_bank;
		changed = true;
	}
	if (slot->snapshot_capturing_bank != RAMLOG_SNAPSHOT_BANK_INVALID) {
		slot->snapshot_capturing_bank = RAMLOG_SNAPSHOT_BANK_INVALID;
		changed = true;
	}

	return changed;
}

const char *ramlog_snapshot_state_name(enum ramlog_snapshot_state state)
{
	switch (state) {
	case RAMLOG_SNAPSHOT_EMPTY:
		return "empty";
	case RAMLOG_SNAPSHOT_CAPTURING:
		return "capturing";
	case RAMLOG_SNAPSHOT_VALID:
		return "valid";
	case RAMLOG_SNAPSHOT_CORRUPT:
		return "corrupt";
	case RAMLOG_SNAPSHOT_CAPTURE_FAILED:
		return "capture-failed";
	default:
		return "unknown";
	}
}

const char *ramlog_snapshot_failure_name(enum ramlog_snapshot_failure failure)
{
	switch (failure) {
	case RAMLOG_SNAPSHOT_FAILURE_NONE:
		return "none";
	case RAMLOG_SNAPSHOT_FAILURE_CONFIG:
		return "config";
	case RAMLOG_SNAPSHOT_FAILURE_SOURCE:
		return "source";
	case RAMLOG_SNAPSHOT_FAILURE_COPY:
		return "copy";
	case RAMLOG_SNAPSHOT_FAILURE_CHECKSUM:
		return "checksum";
	case RAMLOG_SNAPSHOT_FAILURE_INTERRUPTED:
		return "interrupted";
	case RAMLOG_SNAPSHOT_FAILURE_BUSY:
		return "busy";
	default:
		return "unknown";
	}
}

static void ramlog_publish(void)
{
	struct ramlog_header *target;
	uint8_t target_copy = ramlog_active_header_copy ^ 1U;

	ramlog_header->sequence++;
	ramlog_header->checksum = ramlog_checksum(ramlog_header);
	ramlog_header->valid = RAMLOG_VALID;
	target = (struct ramlog_header *)(ramlog_base +
		((uint64_t)target_copy * RAMLOG_HEADER_COPY_SIZE));
	(void)memset(target, 0U, RAMLOG_HEADER_COPY_SIZE);
	(void)memcpy(target, ramlog_header, sizeof(*target));
	target->valid = 0U;
	ramlog_clean(target, RAMLOG_HEADER_COPY_SIZE);
	target->valid = RAMLOG_VALID;
	ramlog_clean(target, sizeof(*target));
	ramlog_active_header_copy = target_copy;
}

static void ramlog_initialize_header(void)
{
	uint32_t offset = RAMLOG_HEADER_SIZE;

	(void)memset(ramlog_header, 0U, sizeof(*ramlog_header));
	ramlog_header->magic = RAMLOG_MAGIC;
	ramlog_header->version = RAMLOG_VERSION;
	ramlog_header->header_size = RAMLOG_HEADER_SIZE;
	ramlog_header->total_size = (uint32_t)beau_config.ramlog_size;
	ramlog_header->generation = 1UL;
	for (uint16_t vmid = 0U; vmid < RAMLOG_VM_SLOT_COUNT; vmid++) {
		uint8_t bank;

		ramlog_header->slots[vmid].offset = offset;
		ramlog_header->slots[vmid].size = ramlog_slot_size(vmid);
		ramlog_header->slots[vmid].snapshot_size = ramlog_snapshot_size(vmid);
		ramlog_header->slots[vmid].snapshot_bank_size =
			ramlog_header->slots[vmid].snapshot_size == 0U ? 0U :
			RAMLOG_PSTORE_SIZE;
		ramlog_header->slots[vmid].live_offset =
			ramlog_header->slots[vmid].snapshot_size;
		ramlog_header->slots[vmid].live_size =
			ramlog_header->slots[vmid].size -
			 ramlog_header->slots[vmid].snapshot_size;
		ramlog_header->slots[vmid].snapshot_active_bank = RAMLOG_SNAPSHOT_BANK_INVALID;
		ramlog_header->slots[vmid].snapshot_capturing_bank =
			RAMLOG_SNAPSHOT_BANK_INVALID;
		for (bank = 0U; bank < RAMLOG_SNAPSHOT_BANK_COUNT; bank++) {
			ramlog_snapshot_clear(&ramlog_header->slots[vmid].snapshots[bank],
				RAMLOG_SNAPSHOT_EMPTY, RAMLOG_SNAPSHOT_FAILURE_NONE);
		}
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

	ramlog_base = hpa2hva_early(beau_config.ramlog_base);
	if (ramlog_base == NULL) {
		return;
	}
	ramlog_active_header_copy = 0U;
	spinlock_init(&ramlog_lock);
	{
		const struct ramlog_header *header0 = (const struct ramlog_header *)ramlog_base;
		const struct ramlog_header *header1 = (const struct ramlog_header *)
			(ramlog_base + RAMLOG_HEADER_COPY_SIZE);
		const struct ramlog_header *selected = NULL;

		if (ramlog_layout_valid(header0)) {
			selected = header0;
		}
		if (ramlog_layout_valid(header1) &&
			((selected == NULL) || (header1->sequence > selected->sequence))) {
			selected = header1;
			ramlog_active_header_copy = 1U;
		}
		if (selected != NULL) {
			(void)memcpy(ramlog_header, selected, sizeof(*ramlog_header));
		}
	}
	if (ramlog_layout_valid(ramlog_header)) {
		for (uint16_t vmid = 0U; vmid < RAMLOG_VM_SLOT_COUNT; vmid++) {
			(void)ramlog_snapshot_recover_slot(&ramlog_header->slots[vmid]);
		}
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
	data = ramlog_base + slot->offset + slot->live_offset;
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
	ramlog_clean(data + (slot->prod % slot->live_size), first);
	if (first < length) {
		ramlog_clean(data, length - first);
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
	const struct ramlog_snapshot_descriptor *active = NULL;
	uint64_t rflags;
	uint8_t bank;

	if (!ramlog_ready || (stats == NULL) || (vmid >= RAMLOG_VM_SLOT_COUNT)) {
		return false;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	stats->vmid = vmid;
	stats->snapshot_active_bank = slot->snapshot_active_bank;
	stats->snapshot_capturing_bank = slot->snapshot_capturing_bank;
	stats->capacity = slot->live_size;
	stats->snapshot_capacity = slot->snapshot_size;
	stats->generation = ramlog_header->generation;
	stats->snapshot_failures = slot->snapshot_failures;
	stats->queued = slot->prod - slot->cons;
	stats->stored_bytes = slot->stored_bytes;
	stats->dropped_bytes = slot->dropped_bytes;
	stats->overflow_events = slot->overflow_events;
	stats->snapshot_last_failure =
		(enum ramlog_snapshot_failure)slot->snapshot_last_failure;
	stats->retained = true;
	stats->snapshot_valid = false;
	stats->snapshot_capturing = ramlog_snapshot_bank_valid(
		slot->snapshot_capturing_bank);
	stats->snapshot_dmesg_bytes = 0U;
	stats->snapshot_console_bytes = 0U;
	stats->snapshot_generation = 0UL;
	for (bank = 0U; bank < RAMLOG_SNAPSHOT_BANK_COUNT; bank++) {
		const struct ramlog_snapshot_descriptor *snapshot = &slot->snapshots[bank];
		struct ramlog_snapshot_bank_stats *bank_stats = &stats->snapshot_banks[bank];

		bank_stats->dmesg_bytes = snapshot->dmesg_size;
		bank_stats->console_bytes = snapshot->console_size;
		bank_stats->payload_bytes = (uint32_t)snapshot->length;
		bank_stats->checksum = snapshot->checksum;
		bank_stats->generation = snapshot->generation;
		bank_stats->state = (enum ramlog_snapshot_state)snapshot->state;
		bank_stats->failure = (enum ramlog_snapshot_failure)snapshot->failure;
		bank_stats->active = slot->snapshot_active_bank == bank;
	}
	if (ramlog_snapshot_bank_valid(slot->snapshot_active_bank)) {
		active = &slot->snapshots[slot->snapshot_active_bank];
		stats->snapshot_valid = ramlog_snapshot_descriptor_valid(active,
			slot->snapshot_bank_size);
		if (stats->snapshot_valid) {
			stats->snapshot_dmesg_bytes = active->dmesg_size;
			stats->snapshot_console_bytes = active->console_size;
			stats->snapshot_generation = active->generation;
		}
	}
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
		data = ramlog_base + slot->offset + slot->live_offset;
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
		data = ramlog_base + slot->offset + slot->live_offset;
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
	if (ramlog_snapshot_bank_valid(slot->snapshot_active_bank) &&
		ramlog_snapshot_descriptor_valid(
			&slot->snapshots[slot->snapshot_active_bank],
			slot->snapshot_bank_size)) {
		const struct ramlog_snapshot_descriptor *snapshot =
			&slot->snapshots[slot->snapshot_active_bank];

		size = snapshot->length;
		if (offset < size) {
			count = (uint32_t)(size - offset);
			if (count > length) {
				count = length;
			}
			data = ramlog_snapshot_bank_data(slot, slot->snapshot_active_bank);
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
		ramlog_clean(destination, chunk);
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

static void ramlog_snapshot_record_failure(uint16_t vmid,
	enum ramlog_snapshot_failure failure)
{
	uint64_t rflags;
	struct ramlog_slot_header *slot;

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vmid];
	slot->snapshot_failures++;
	slot->snapshot_last_failure = (uint16_t)failure;
	ramlog_publish();
	spinlock_irqrestore_release(&ramlog_lock, rflags);
}

/* [20260723] Zircon-inspired retained snapshot transaction
 *
 * active valid bank -> reserve inactive bank -> copy guest record outside lock
 *                                                   |
 *                                                   +--> failure: discard inactive
 *                                                   |
 *                                                   v
 *                              flush payload -> checksum -> commit descriptor
 *                                                   |
 *                                                   v
 *                                           switch active bank
 *
 * Key rule:
 *   - the active bank is immutable while a reset capture is in progress;
 *   - the slot metadata lock owns bank reservation and active-bank publication,
 *     but never covers bounded guest GPA copies;
 *   - a descriptor publishes magic and its complement only after its payload
 *     has been cache-cleaned and checksummed, preventing partial records from
 *     becoming visible after reset or power loss.
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
	uint64_t generation;
	uint32_t checksum = 0U;
	uint32_t dmesg_size = 0U;
	uint32_t console_size = 0U;
	uint32_t console_offset = 0U;
	uint8_t bank;
	enum ramlog_pstore_header_status dmesg_status;
	enum ramlog_pstore_header_status console_status;
	enum ramlog_snapshot_failure failure = RAMLOG_SNAPSHOT_FAILURE_NONE;
	bool dmesg_valid;
	bool console_valid;
	bool captured = false;

	if (!ramlog_ready || (vm == NULL) || (vm->vm_id >= RAMLOG_VM_SLOT_COUNT)) {
		return false;
	}
	arch_config = &get_vm_config(vm->vm_id)->arch;
	if ((arch_config->guest_pstore_size == 0UL) &&
		(arch_config->guest_pstore_base == 0UL)) {
		return false;
	}
	if ((arch_config->guest_pstore_size != RAMLOG_PSTORE_SIZE) ||
		(arch_config->guest_pstore_base == 0UL)) {
		ramlog_snapshot_record_failure(vm->vm_id, RAMLOG_SNAPSHOT_FAILURE_CONFIG);
		return false;
	}

	pstore_base = arch_config->guest_pstore_base;
	dmesg_status = ramlog_pstore_header_read(vm, pstore_base,
		RAMLOG_PSTORE_DMESG_SIZE, RAMLOG_PSTORE_SIGNATURE, &dmesg_header);
	dmesg_valid = dmesg_status == RAMLOG_PSTORE_HEADER_VALID;
	if (dmesg_valid) {
		dmesg_valid = ramlog_pstore_dmesg_valid(vm, pstore_base, &dmesg_header);
	}
	console_status = ramlog_pstore_header_read(vm,
		pstore_base + RAMLOG_PSTORE_DMESG_SIZE, RAMLOG_PSTORE_CONSOLE_SIZE,
		RAMLOG_PSTORE_SIGNATURE, &console_header);
	console_valid = (console_status == RAMLOG_PSTORE_HEADER_VALID) &&
		(console_header.size != 0U);
	if (!dmesg_valid && !console_valid) {
		if ((dmesg_header.sig != 0U) || (console_header.sig != 0U) ||
			(dmesg_status == RAMLOG_PSTORE_HEADER_COPY_FAILED) ||
			(console_status == RAMLOG_PSTORE_HEADER_COPY_FAILED)) {
			LOG_WRN("PSTORE: vm%u source rejected dmesg:%s console:%s",
				vm->vm_id,
				dmesg_status == RAMLOG_PSTORE_HEADER_VALID ? "payload" :
				ramlog_pstore_header_status_name(dmesg_status),
				console_status == RAMLOG_PSTORE_HEADER_VALID ? "empty" :
				ramlog_pstore_header_status_name(console_status));
		}
		ramlog_snapshot_record_failure(vm->vm_id, RAMLOG_SNAPSHOT_FAILURE_SOURCE);
		return false;
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vm->vm_id];
	if (slot->snapshot_size != RAMLOG_PSTORE_SNAPSHOT_SIZE) {
		failure = RAMLOG_SNAPSHOT_FAILURE_CONFIG;
	} else if (ramlog_snapshot_bank_valid(slot->snapshot_capturing_bank)) {
		failure = RAMLOG_SNAPSHOT_FAILURE_BUSY;
	} else {
		bank = ramlog_snapshot_bank_valid(slot->snapshot_active_bank) ?
			(slot->snapshot_active_bank ^ 1U) : 0U;
		ramlog_snapshot_clear(&slot->snapshots[bank], RAMLOG_SNAPSHOT_CAPTURING,
			RAMLOG_SNAPSHOT_FAILURE_NONE);
		slot->snapshot_capturing_bank = bank;
		ramlog_publish();
		snapshot = ramlog_snapshot_bank_data(slot, bank);
	}
	spinlock_irqrestore_release(&ramlog_lock, rflags);
	if (failure != RAMLOG_SNAPSHOT_FAILURE_NONE) {
		ramlog_snapshot_record_failure(vm->vm_id, failure);
		return false;
	}

	if (dmesg_valid) {
		dmesg_size = dmesg_header.size;
		captured = ramlog_pstore_copy(vm, pstore_base, RAMLOG_PSTORE_DMESG_SIZE,
			&dmesg_header, 0U, snapshot, dmesg_size);
	}
	if (captured && console_valid) {
		console_size = console_header.size;
		console_offset = console_header.size - console_size;
		captured = ramlog_pstore_copy(vm,
			pstore_base + RAMLOG_PSTORE_DMESG_SIZE,
			RAMLOG_PSTORE_CONSOLE_SIZE, &console_header, console_offset,
			snapshot + dmesg_size, console_size);
	} else if (!dmesg_valid && console_valid) {
		console_size = console_header.size;
		console_offset = console_header.size - console_size;
		captured = ramlog_pstore_copy(vm,
			pstore_base + RAMLOG_PSTORE_DMESG_SIZE,
			RAMLOG_PSTORE_CONSOLE_SIZE, &console_header, console_offset,
			 snapshot, console_size);
	}
	if (captured) {
		ramlog_clean(snapshot, (uint64_t)dmesg_size + console_size);
		checksum = ramlog_payload_checksum(snapshot,
			(uint64_t)dmesg_size + console_size);
	}

	spinlock_irqsave_obtain(&ramlog_lock, &rflags);
	slot = &ramlog_header->slots[vm->vm_id];
	if (slot->snapshot_capturing_bank != bank) {
		captured = false;
		failure = RAMLOG_SNAPSHOT_FAILURE_BUSY;
	} else if (captured) {
		uint8_t other = bank ^ 1U;

		generation = slot->snapshots[other].generation;
		if (generation < slot->snapshots[bank].generation) {
			generation = slot->snapshots[bank].generation;
		}
		generation++;
		ramlog_snapshot_commit(&slot->snapshots[bank], generation, dmesg_size,
			console_size, checksum);
		slot->snapshot_active_bank = bank;
		slot->snapshot_capturing_bank = RAMLOG_SNAPSHOT_BANK_INVALID;
		slot->snapshot_last_failure = RAMLOG_SNAPSHOT_FAILURE_NONE;
	} else {
		failure = RAMLOG_SNAPSHOT_FAILURE_COPY;
	}
	if (!captured) {
		ramlog_snapshot_clear(&slot->snapshots[bank], RAMLOG_SNAPSHOT_CAPTURE_FAILED,
			failure);
		slot->snapshot_capturing_bank = RAMLOG_SNAPSHOT_BANK_INVALID;
		slot->snapshot_failures++;
		slot->snapshot_last_failure = (uint16_t)failure;
	}
	ramlog_publish();
	spinlock_irqrestore_release(&ramlog_lock, rflags);

	return captured;
}
