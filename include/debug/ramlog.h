/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RAMLOG_H
#define RAMLOG_H

#include <types.h>

#define RAMLOG_VM_SLOT_COUNT 5U
#define RAMLOG_SNAPSHOT_BANK_COUNT 2U
#define RAMLOG_SNAPSHOT_BANK_INVALID 0xffU

struct acrn_vm;

enum ramlog_snapshot_state {
	RAMLOG_SNAPSHOT_EMPTY = 0U,
	RAMLOG_SNAPSHOT_CAPTURING,
	RAMLOG_SNAPSHOT_VALID,
	RAMLOG_SNAPSHOT_CORRUPT,
	RAMLOG_SNAPSHOT_CAPTURE_FAILED,
};

enum ramlog_snapshot_failure {
	RAMLOG_SNAPSHOT_FAILURE_NONE = 0U,
	RAMLOG_SNAPSHOT_FAILURE_CONFIG,
	RAMLOG_SNAPSHOT_FAILURE_SOURCE,
	RAMLOG_SNAPSHOT_FAILURE_COPY,
	RAMLOG_SNAPSHOT_FAILURE_CHECKSUM,
	RAMLOG_SNAPSHOT_FAILURE_INTERRUPTED,
	RAMLOG_SNAPSHOT_FAILURE_BUSY,
};

struct ramlog_snapshot_bank_stats {
	uint32_t dmesg_bytes;
	uint32_t console_bytes;
	uint32_t payload_bytes;
	uint32_t checksum;
	uint64_t generation;
	enum ramlog_snapshot_state state;
	enum ramlog_snapshot_failure failure;
	bool active;
};

struct ramlog_stats {
	uint16_t vmid;
	uint8_t snapshot_active_bank;
	uint8_t snapshot_capturing_bank;
	uint32_t capacity;
	uint32_t snapshot_capacity;
	uint32_t snapshot_dmesg_bytes;
	uint32_t snapshot_console_bytes;
	uint64_t generation;
	uint64_t snapshot_generation;
	uint64_t snapshot_failures;
	uint64_t queued;
	uint64_t stored_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	enum ramlog_snapshot_failure snapshot_last_failure;
	struct ramlog_snapshot_bank_stats snapshot_banks[RAMLOG_SNAPSHOT_BANK_COUNT];
	bool retained;
	bool snapshot_valid;
	bool snapshot_capturing;
};

const char *ramlog_snapshot_state_name(enum ramlog_snapshot_state state);
const char *ramlog_snapshot_failure_name(enum ramlog_snapshot_failure failure);

struct ramlog_window {
	uint64_t oldest;
	uint64_t next;
};

void ramlog_init(void);
bool ramlog_append_vm_console(uint16_t vmid, const char *buffer, uint32_t length);
bool ramlog_capture_vm_pstore(struct acrn_vm *vm);
bool ramlog_get_stats(uint16_t vmid, struct ramlog_stats *stats);
bool ramlog_get_window(uint16_t vmid, struct ramlog_window *window);
uint32_t ramlog_read_vm_console(uint16_t vmid, uint64_t *cursor, char *buffer,
	uint32_t length, uint64_t *skipped);
uint32_t ramlog_copy(uint16_t vmid, uint64_t offset, char *buffer,
	uint32_t length);
uint32_t ramlog_copy_snapshot(uint16_t vmid, uint64_t offset, char *buffer,
	uint32_t length);

#endif /* RAMLOG_H */
