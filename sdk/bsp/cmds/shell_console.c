/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <console.h>
#include <debug/ramlog.h>
#include <debug/shell.h>
#include <errno.h>
#include <sprintf.h>
#include <util.h>
#include <vm.h>
#include <vconfig.h>
#include <bsp/vuart.h>

#include "shell_cmds.h"

#define VM_CONSOLE_PROMPT_KEY	'\r'

int32_t shell_to_vm_console(int32_t argc, char **argv)
{
	char temp_str[TEMP_STR_SIZE];
	uint16_t vm_id = 0U;

	struct acrn_vm *vm;
	struct acrn_vuart *vu;

	if (argc == 2) {
		vm_id = sanitize_vmid((uint16_t)strtol_deci(argv[1]));
	}

	vm = get_vm_from_vmid(vm_id);
	if (is_poweroff_vm(vm)) {
		shell_puts("vm is not valid \n");
		return -EINVAL;
	}
	vu = vm_console_vuart(vm);
	if (!vu->active) {
		shell_puts("vuart console is not active \n");
		return 0;
	}

	/* [20260716] VM console attach transaction:
	 *
	 *   validate -> bind -> publish owner -> non-blocking prompt key
	 *
	 * Binding must succeed before BEAU input is disabled. The prompt key mirrors
	 * a physical serial attach: it reveals a quiet guest prompt immediately, but
	 * a full guest RX FIFO must never stall or fail the switch command.
	 */
	if (!console_vm_vuart_bind(vm_id)) {
		return -ENODEV;
	}
	if ((console_vmid != ACRN_INVALID_VMID) && (console_vmid != vm_id)) {
		console_vm_vuart_unbind(console_vmid);
	}
	snprintf(temp_str, TEMP_STR_SIZE,
		"\r\n%s───────────── [switch to VM-%d console] ─────────────%s\r\n",
		SHELL_COLOR_YELLOW, vm_id, SHELL_COLOR_RESET);
	shell_puts(temp_str);
	shell_set_input_active(false);
	console_vmid = vm_id;
	if (vuart_try_putchar(vu, VM_CONSOLE_PROMPT_KEY)) {
		vuart_notify_rx(vu);
	}

	return 0;
}

static bool shell_parse_ramlog_vmid(const char *text, uint16_t *vmid)
{
	uint32_t value = 0U;

	if ((text == NULL) || (vmid == NULL) || (text[0] == '\0')) {
		return false;
	}
	for (uint32_t index = 0U; text[index] != '\0'; index++) {
		uint8_t ch = (uint8_t)text[index];

		if ((ch < (uint8_t)'0') || (ch > (uint8_t)'9') ||
			(value > ((0xffffU - (uint32_t)(ch - (uint8_t)'0')) / 10U))) {
			return false;
		}
		value = (value * 10U) + (uint32_t)(ch - (uint8_t)'0');
	}
	*vmid = (uint16_t)value;

	return true;
}

#define SHELL_RAMLOG_CPR_QUERY_LEN	4U
#define SHELL_RAMLOG_DUMP_BUF_SIZE	260U

struct shell_ramlog_dump_state {
	uint8_t query_len;
	char query[SHELL_RAMLOG_CPR_QUERY_LEN];
	bool line_start;
};

static const char shell_ramlog_cpr_query[] = "\033[6n";

static void shell_ramlog_dump_write_byte(struct shell_ramlog_dump_state *state,
	char *output, uint32_t *output_len, char ch)
{
	if (*output_len == SHELL_RAMLOG_DUMP_BUF_SIZE) {
		(void)console_write(output, *output_len);
		*output_len = 0U;
	}
	output[*output_len] = ch;
	(*output_len)++;
	state->line_start = (ch == '\n');
}

/* [20260721] RAMLOG terminal-query display filter
 *
 * retained guest bytes -> exact CPR query suppression -> physical terminal
 *
 * Key rule:
 *   - the retained record is never rewritten for presentation;
 *   - only ESC[6n is withheld so a serial terminal cannot inject a cursor
 *     reply into an unrelated console session;
 *   - partial escape sequences are emitted at end-of-dump.
 */
static void shell_ramlog_dump_write(struct shell_ramlog_dump_state *state,
	const char *input, uint32_t length, bool finish)
{
	char output[SHELL_RAMLOG_DUMP_BUF_SIZE];
	uint32_t output_len = 0U;

	if ((state == NULL) || (input == NULL)) {
		return;
	}

	for (uint32_t index = 0U; index < length; index++) {
		char ch = input[index];

		if ((state->query_len != 0U) || (ch == shell_ramlog_cpr_query[0U])) {
			if (state->query_len < SHELL_RAMLOG_CPR_QUERY_LEN) {
				state->query[state->query_len] = ch;
				state->query_len++;
			}
			if (memcmp(state->query, shell_ramlog_cpr_query, state->query_len) == 0) {
				if (state->query_len == SHELL_RAMLOG_CPR_QUERY_LEN) {
					state->query_len = 0U;
				}
				continue;
			}
			for (uint32_t query_index = 0U; query_index < state->query_len; query_index++) {
				shell_ramlog_dump_write_byte(state, output, &output_len,
					state->query[query_index]);
			}
			state->query_len = 0U;
			continue;
		}
		shell_ramlog_dump_write_byte(state, output, &output_len, ch);
	}

	if (finish) {
		for (uint32_t index = 0U; index < state->query_len; index++) {
			shell_ramlog_dump_write_byte(state, output, &output_len, state->query[index]);
		}
		state->query_len = 0U;
	}
	if (output_len != 0U) {
		(void)console_write(output, output_len);
	}
}

int32_t shell_ramlog(int32_t argc, char **argv)
{
	struct ramlog_stats before;
	struct ramlog_stats after;
	struct shell_ramlog_dump_state dump_state = {
		.query_len = 0U,
		.line_start = true,
	};
	char buffer[256U];
	uint16_t vmid;
	uint64_t offset = 0UL;
	uint32_t copied;

	if ((argc != 2) || !shell_parse_ramlog_vmid(argv[1], &vmid) ||
		!ramlog_get_stats(vmid, &before)) {
		shell_puts("usage: ramlog <vmid>\r\n");
		return -EINVAL;
	}

	shell_item_begin("RAMLOG VM%hu", vmid);
	/* generation changes when retention resets; bytes is queued/capacity; stored
	 * is accepted lifetime data; dropped/overflow report loss. pstore fields
	 * describe the independent prior-boot snapshot.
	 */
	shell_item_line("generation:%lu retained:%s bytes:%lu/%u stored:%lu dropped:%lu overflow:%lu",
		before.generation, before.retained ? "yes" : "no", before.queued,
		before.capacity, before.stored_bytes, before.dropped_bytes,
		before.overflow_events);
	if (before.snapshot_capacity != 0U) {
		shell_item_line("pstore:active:%s bank:%u capture:%c bank:%u generation:%lu dmesg:%u console:%u/%u failures:%lu last:%s",
			before.snapshot_valid ? "valid" : "none", before.snapshot_active_bank,
			before.snapshot_capturing ? 'Y' : 'N',
			before.snapshot_capturing_bank, before.snapshot_generation,
			before.snapshot_dmesg_bytes, before.snapshot_console_bytes,
			before.snapshot_capacity, before.snapshot_failures,
			ramlog_snapshot_failure_name(before.snapshot_last_failure));
		for (uint8_t bank = 0U; bank < RAMLOG_SNAPSHOT_BANK_COUNT; bank++) {
			const struct ramlog_snapshot_bank_stats *bank_stats =
				&before.snapshot_banks[bank];

			shell_item_line("pstore:bank%u state:%-5s active:%c generation:%1lu bytes:%5u dmesg:%1u console:%5u checksum:%08x failure:%-4s",
				bank, ramlog_snapshot_state_name(bank_stats->state),
				bank_stats->active ? 'Y' : 'N', bank_stats->generation,
				bank_stats->payload_bytes, bank_stats->dmesg_bytes,
				bank_stats->console_bytes, bank_stats->checksum,
				ramlog_snapshot_failure_name(bank_stats->failure));
		}
	}
	shell_item_end();

	if (before.snapshot_valid && (before.snapshot_dmesg_bytes != 0U)) {
		shell_item_begin("PREV RAMOOPS DMESG");
		shell_item_line("bytes:%u", before.snapshot_dmesg_bytes);
		shell_item_end();
		while (offset < before.snapshot_dmesg_bytes) {
			copied = ramlog_copy_snapshot(vmid, offset, buffer, sizeof(buffer));
			if (copied == 0U) {
				break;
			}
			shell_ramlog_dump_write(&dump_state, buffer, copied, false);
			offset += copied;
		}
		shell_ramlog_dump_write(&dump_state, "", 0U, true);
		if (!dump_state.line_start) {
			shell_puts("\r\n");
			dump_state.line_start = true;
		}
	}
	if (before.snapshot_valid && (before.snapshot_console_bytes != 0U)) {
		shell_item_begin("PREV RAMOOPS CONSOLE");
		shell_item_line("bytes:%u", before.snapshot_console_bytes);
		shell_item_end();
		while (offset < ((uint64_t)before.snapshot_dmesg_bytes +
			before.snapshot_console_bytes)) {
			copied = ramlog_copy_snapshot(vmid, offset, buffer, sizeof(buffer));
			if (copied == 0U) {
				break;
			}
			shell_ramlog_dump_write(&dump_state, buffer, copied, false);
			offset += copied;
		}
		shell_ramlog_dump_write(&dump_state, "", 0U, true);
		if (!dump_state.line_start) {
			shell_puts("\r\n");
			dump_state.line_start = true;
		}
	}

	offset = 0UL;
	shell_item_begin("CURR LIVE CONSOLE");
	shell_item_line("bytes:%lu/%u stored:%lu dropped:%lu overflow:%lu",
		before.queued, before.capacity, before.stored_bytes, before.dropped_bytes,
		before.overflow_events);
	shell_item_end();
	while (offset < before.queued) {
		copied = ramlog_copy(vmid, offset, buffer, sizeof(buffer));
		if (copied == 0U) {
			break;
		}
		shell_ramlog_dump_write(&dump_state, buffer, copied, false);
		offset += copied;
	}
	shell_ramlog_dump_write(&dump_state, "", 0U, true);
	if (ramlog_get_stats(vmid, &after) &&
		((after.dropped_bytes != before.dropped_bytes) ||
		(after.queued < before.queued))) {
		if (!dump_state.line_start) {
			shell_puts("\r\n");
		}
		shell_puts("ramlog changed during dump; retry for a newer snapshot\r\n");
		dump_state.line_start = true;
	}
	if (!dump_state.line_start) {
		shell_puts("\r\n");
	}
	shell_item_begin("RAMLOG END");
	shell_item_end();

	return 0;
}
