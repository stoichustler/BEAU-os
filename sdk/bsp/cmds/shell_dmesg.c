/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <logmsg.h>
#include <sprintf.h>
#include <util.h>

#include "shell_cmds.h"

static bool shell_dmesg_parse_count(const char *text, uint32_t *count)
{
	uint32_t value = 0U;

	if ((text == NULL) || (count == NULL) || (text[0] == '\0')) {
		return false;
	}
	for (uint32_t index = 0U; text[index] != '\0'; index++) {
		uint8_t ch = (uint8_t)text[index];

		if ((ch < (uint8_t)'0') || (ch > (uint8_t)'9') ||
			(value > ((0xffffffffU - (uint32_t)(ch - (uint8_t)'0')) / 10U))) {
			return false;
		}
		value = (value * 10U) + (uint32_t)(ch - (uint8_t)'0');
	}
	if (value == 0U) {
		return false;
	}
	*count = value;

	return true;
}

static void shell_dmesg_print_record(const struct host_dmesg_record *record)
{
	char line[LOG_MESSAGE_MAX_SIZE + 3U];
	uint16_t length = record->length;

	while ((length > 0U) && ((record->message[length - 1U] == '\n') ||
		(record->message[length - 1U] == '\r'))) {
		length--;
	}
	(void)memcpy(line, record->message, length);
	line[length] = '\r';
	line[length + 1U] = '\n';
	line[length + 2U] = '\0';
	shell_puts(line);
	shell_output_checkpoint();
}

int32_t shell_dmesg(int32_t argc, char **argv)
{
	struct host_dmesg_stats before;
	struct host_dmesg_stats after;
	struct host_dmesg_record record;
	uint64_t cursor;
	uint64_t limit;
	uint64_t skipped;
	uint32_t count;

	if ((argc > 2) || !host_dmesg_get_stats(&before)) {
		shell_puts("usage: dmesg [count]\r\n");
		return -EINVAL;
	}
	count = before.queued;
	if ((argc == 2) && !shell_dmesg_parse_count(argv[1], &count)) {
		shell_puts("usage: dmesg [count]\r\n");
		return -EINVAL;
	}
	if (count > before.queued) {
		count = before.queued;
	}
	cursor = before.next - count;
	limit = before.next;

	shell_item_begin("DMESG");
	shell_item_line("records:%u/%u stored:%lu overwritten:%lu", before.queued,
		before.capacity, before.stored, before.overwritten);
	shell_item_end();
	while (cursor < limit) {
		if (!host_dmesg_read(&cursor, &record, &skipped)) {
			break;
		}
		if (skipped != 0UL) {
			shell_puts("dmesg records overwritten while dumping; continuing from newest data\r\n");
		}
		shell_dmesg_print_record(&record);
	}
	if (host_dmesg_get_stats(&after) && (after.oldest > before.oldest)) {
		shell_puts("dmesg changed during dump; retry for a complete newer view\r\n");
	}

	shell_item_begin("THE END");
	shell_item_end();

	return 0;
}
