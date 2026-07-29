/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <logmsg.h>
#include <asm/esr.h>

#include "shell_cmds.h"

#define SHELL_GEN_BRK_ASM_IMM	"0x0d01"
#define SHELL_GEN_HEX_DIGITS_MAX	16U

/* [20260729] Controlled shell fault generation
 *
 *   shell input
 *       |
 *       +--> gen esr <hex> -> strict parse -> ESR diagnostic only
 *       |
 *       +--> gen brk       -> non-DDB BRK -> existing host dump/reset path
 *       |
 *       `--> gen panic     -> existing panic/coredump/DDB path
 *
 * Key rule:
 *   - only explicit shell input can enter a destructive path;
 *   - BRK must not use either reserved DDB immediate;
 *   - malformed input never changes EL2, VM, or device state.
 */
static bool shell_gen_hex_nibble(char ch, uint8_t *nibble)
{
	if (nibble == NULL) {
		return false;
	}
	if ((ch >= '0') && (ch <= '9')) {
		*nibble = (uint8_t)(ch - '0');
	} else if ((ch >= 'a') && (ch <= 'f')) {
		*nibble = (uint8_t)(ch - 'a' + 10U);
	} else if ((ch >= 'A') && (ch <= 'F')) {
		*nibble = (uint8_t)(ch - 'A' + 10U);
	} else {
		return false;
	}

	return true;
}

static bool shell_gen_parse_esr(const char *text, uint64_t *esr)
{
	const char *cursor = text;
	uint64_t value = 0UL;
	uint8_t digit;
	uint32_t count = 0U;

	if ((text == NULL) || (esr == NULL)) {
		return false;
	}
	if ((cursor[0] == '0') && ((cursor[1] == 'x') || (cursor[1] == 'X'))) {
		cursor += 2;
	}
	while (*cursor != '\0') {
		if ((count >= SHELL_GEN_HEX_DIGITS_MAX) ||
			!shell_gen_hex_nibble(*cursor, &digit) ||
			(value > ((UINT64_MAX - (uint64_t)digit) >> 4U))) {
			return false;
		}
		value = (value << 4U) | (uint64_t)digit;
		count++;
		cursor++;
	}
	if (count == 0U) {
		return false;
	}

	*esr = value;
	return true;
}

static void shell_gen_usage(void)
{
	shell_puts("usage: gen <panic|brk|esr <hex>>\r\n");
}

int32_t shell_gen(int32_t argc, char **argv)
{
	uint64_t esr;

	if ((argc == 2) && (strcmp(argv[1], "panic") == 0)) {
		panic("synthetic shell fault injection");
	}
	if ((argc == 2) && (strcmp(argv[1], "brk") == 0)) {
		__asm__ volatile("brk #" SHELL_GEN_BRK_ASM_IMM ::: "memory");
		return 0;
	}
	if ((argc == 3) && (strcmp(argv[1], "esr") == 0) &&
		shell_gen_parse_esr(argv[2], &esr)) {
		arm64_esr_log(LOG_INFO, "gen.esr", esr);
		return 0;
	}

	shell_gen_usage();
	return -EINVAL;
}
