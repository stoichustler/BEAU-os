/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <rtl.h>
#include <serial.h>
#include <sprintf.h>
#include <ticks.h>
#include "ddb_internal.h"

#define DDB_PRINT_SIZE	256U
#define DDB_ASCII_BS	'\b'
#define DDB_ASCII_DEL	0x7f

void ddb_puts(const char *text)
{
	if (text != NULL) {
		(void)serial_debug_puts(text,
			(uint32_t)strnlen_s(text, DDB_PRINT_SIZE * 4U));
	}
}

void ddb_printf(const char *fmt, ...)
{
	char buffer[DDB_PRINT_SIZE];
	va_list args;

	va_start(args, fmt);
	(void)vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	ddb_puts(buffer);
}

int32_t ddb_read_line(char *line, uint32_t size)
{
	uint64_t deadline;
	uint32_t length = 0U;

	if ((line == NULL) || (size < 2U)) {
		return -1;
	}
	line[0] = '\0';
	deadline = cpu_ticks() + ((uint64_t)CONFIG_DDB_IDLE_TIMEOUT_MS *
		TICKS_PER_MS);
	while (true) {
		char ch = serial_debug_getc();

		if (ch == -1) {
			if ((int64_t)(cpu_ticks() - deadline) >= 0L) {
				return -ETIMEDOUT;
			}
			cpu_relax();
			continue;
		}
		if (ch == '\n') {
			continue;
		}
		if (ch == '\r') {
			ddb_puts("\n");
			line[length] = '\0';
			return (int32_t)length;
		}
		if ((ch == DDB_ASCII_BS) || ((uint8_t)ch == DDB_ASCII_DEL)) {
			if (length > 0U) {
				length--;
				line[length] = '\0';
				ddb_puts("\b \b");
			}
			continue;
		}
		if ((ch >= 32) && (ch <= 126) && (length < (size - 1U))) {
			line[length++] = ch;
			line[length] = '\0';
			(void)serial_debug_puts(&ch, 1U);
		}
	}
}

uint32_t ddb_split_line(char *line, char **argv, uint32_t max_args)
{
	uint32_t argc = 0U;
	char *cursor = line;

	if ((line == NULL) || (argv == NULL) || (max_args == 0U)) {
		return 0U;
	}
	while (*cursor != '\0') {
		while ((*cursor == ' ') || (*cursor == '\t')) {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		if (argc >= max_args) {
			return max_args + 1U;
		}
		argv[argc++] = cursor;
		while ((*cursor != '\0') && (*cursor != ' ') && (*cursor != '\t')) {
			cursor++;
		}
		if (*cursor != '\0') {
			*cursor = '\0';
			cursor++;
		}
	}

	return argc;
}

bool ddb_parse_u64(const char *text, uint32_t base, uint64_t *value)
{
	uint64_t result = 0UL;
	uint32_t digits = 0U;
	const char *cursor = text;

	if ((text == NULL) || (value == NULL) ||
		((base != 10U) && (base != 16U))) {
		return false;
	}
	if ((base == 16U) && (cursor[0] == '0') &&
		((cursor[1] == 'x') || (cursor[1] == 'X'))) {
		cursor += 2;
	}
	while (*cursor != '\0') {
		uint32_t digit;

		if ((*cursor >= '0') && (*cursor <= '9')) {
			digit = (uint32_t)(*cursor - '0');
		} else if ((base == 16U) && (*cursor >= 'a') && (*cursor <= 'f')) {
			digit = (uint32_t)(*cursor - 'a') + 10U;
		} else if ((base == 16U) && (*cursor >= 'A') && (*cursor <= 'F')) {
			digit = (uint32_t)(*cursor - 'A') + 10U;
		} else {
			return false;
		}
		if ((digit >= base) ||
			(result > ((UINT64_MAX - (uint64_t)digit) / base))) {
			return false;
		}
		result = (result * base) + digit;
		digits++;
		cursor++;
	}
	if (digits == 0U) {
		return false;
	}
	*value = result;
	return true;
}
