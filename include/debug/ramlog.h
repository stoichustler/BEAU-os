/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RAMLOG_H
#define RAMLOG_H

#include <types.h>

#define RAMLOG_VM_SLOT_COUNT 5U

struct acrn_vm;

struct ramlog_stats {
	uint16_t vmid;
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
	bool retained;
	bool snapshot_valid;
	bool snapshot_capturing;
};

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
