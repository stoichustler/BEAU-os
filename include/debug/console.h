/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <bsp/vuart.h>


struct acrn_vcpu;

struct console_vm_ring_stats {
	uint16_t vmid;
	uint32_t size;
	uint32_t capacity;
	uint32_t drain_budget;
	uint32_t tx_high_water;
	uint32_t tx_low_water;
	uint32_t queued;
	uint32_t high_water;
	uint64_t input_bytes;
	uint64_t stored_bytes;
	uint64_t drained_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t last_overflow_tsc;
	bool pending;
	bool draining;
	bool vuart_bound;
	bool tx_blocked;
};

struct console_vm_exception_stats {
	uint16_t vmid;
	uint32_t size;
	uint32_t capacity;
	uint32_t queued;
	uint32_t high_water;
	uint64_t input_bytes;
	uint64_t stored_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t last_overflow_tsc;
	bool pending;
};

struct console_vm_input_stats {
	uint16_t selected_vmid;
	uint16_t input_vmid;
	uint32_t queued;
	uint32_t capacity;
	uint32_t guest_budget;
	bool last_enter;
	bool has_non_enter;
};

/** Initializes the console module.
 *
 */
void console_init(void);

/** Writes a given number of characters to the console.
 *
 *  @param s A pointer to character array to write.
 *  @param len The number of characters to write.
 *
 *  @return The number of characters written or -1 if an error occurred
 *          and no character was written.
 */
size_t console_write(const char *s, size_t len);

/** Writes a single character to the console.
 *
 *  @param ch The character to write.
 *
 *  @preturn The number of characters written or -1 if an error
 *           occurred before any character was written.
 */
void console_putc(const char *ch);
char console_getc(void);

void console_setup_timer(void);
void console_vmexit_callback(struct acrn_vcpu *vcpu);
bool console_is_hv(void);
bool console_is_vm_active(uint16_t vmid);
bool console_vm_kick(void);
bool console_vm_tx_put(uint16_t vmid, char ch);
bool console_vm_tx_can_accept(uint16_t vmid);
void console_vm_tx_space_changed(uint16_t vmid);
bool console_vm_rx_refill(struct acrn_vuart *vu);
void console_vm_ring_drain(uint16_t vmid);
bool console_vm_vuart_bind(uint16_t vmid);
void console_vm_vuart_unbind(uint16_t vmid);
bool console_vm_ring_get_stats(uint16_t vmid, struct console_vm_ring_stats *stats);
uint32_t console_vm_ring_copy(uint16_t vmid, uint32_t offset, char *buf, uint32_t len);
bool console_vm_input_get_stats(struct console_vm_input_stats *stats);
void console_vm_exception_log(uint16_t vmid, const char *buf, size_t len);
uint32_t console_vm_exception_count(void);
bool console_vm_exception_get_stats(uint16_t vmid, struct console_vm_exception_stats *stats);
uint32_t console_vm_exception_copy(uint16_t vmid, uint32_t offset, char *buf, uint32_t len);
void console_vm_exception_clear(uint16_t vmid);
void console_vm_exception_clear_all(void);

void suspend_console(void);
void resume_console(void);
struct acrn_vuart *vm_console_vuart(struct acrn_vm *vm);

bool console_need_log(uint32_t severity);
void console_log(uint32_t severity, char *buffer);

#endif /* CONSOLE_H */
