/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <sprintf.h>
#include <util.h>
#include <version.h>
#include <banner.h>
#include <debug/symbol.h>

#include "shell_cmds.h"

int32_t shell_version(__unused int32_t argc, __unused char **argv)
{
	char temp_str[MAX_STR_SIZE];

	/* Fields identify the build version, source revision state, scenario/board,
	 * and the user/time that produced this image.
	 */
	snprintf(temp_str, MAX_STR_SIZE, "BEAU OS v%s %s-%s %s%s%s%s %s@%s build by %s %s\r\n",
		BEAU_OS_VERSION, HV_COMMIT_TIME, HV_COMMIT_DIRTY, HV_BUILD_TYPE,
		(sizeof(HV_COMMIT_TAGS) > 1) ? "(tag: " : "", HV_COMMIT_TAGS,
		(sizeof(HV_COMMIT_TAGS) > 1) ? ")" : "",
		HV_BUILD_SCENARIO, HV_BUILD_BOARD, HV_BUILD_USER, HV_BUILD_TIME);
	shell_puts(temp_str);

	return 0;
}

int32_t shell_clear(__unused int32_t argc, __unused char **argv)
{
	shell_puts("\e[2J\e[;H");
	return 0;
}

int32_t shell_symtab(int32_t argc, __unused char **argv)
{
	uint32_t i;

	if (argc != 1) {
		return -EINVAL;
	}

	shell_item_begin("symtab symbols:%u", dbg_symbol_count);
	if (dbg_symbol_count == 0U) {
		shell_item_line("symbol table is empty");
		shell_item_end();
		return 0;
	}

	/* index is generated-table order; offset is relative to hypervisor text;
	 * symbol is the retained linker/debug name.
	 */
	shell_item_line("%-5s  %-18s  %s", "index", "offset", "symbol");
	shell_item_line("─────  ──────────────────  ────────────────────────────────");
	for (i = 0U; i < dbg_symbol_count; i++) {
		char index[16U];
		const char *name = dbg_symbol_table[i].name;
		uint64_t offset = dbg_symbol_table[i].addr;

		/*
		 * The generated table stores absolute text addresses. The shell
		 * command presents offsets relative to dbg_symbol_text_start so
		 * different load addresses can be compared directly.
		 */
		if ((dbg_symbol_text_start != 0UL) && (offset >= dbg_symbol_text_start)) {
				offset -= dbg_symbol_text_start;
		}

		(void)snprintf(index, sizeof(index), "%04u", i + 1U);
		shell_item_line("%-5s  0x%016lx  %s", index,
			offset, (name == NULL) ? "<null>" : name);
		shell_output_checkpoint();
	}
	shell_item_end();

	return 0;
}

int32_t shell_loglevel(int32_t argc, char **argv)
{
	char str[MAX_STR_SIZE] = {0};

	switch (argc) {
	case 4:
		npk_loglevel = (uint16_t)strtol_deci(argv[3]);
		/* falls through */
	case 3:
		mem_loglevel = (uint16_t)strtol_deci(argv[2]);
		/* falls through */
	case 2:
		console_loglevel = (uint16_t)strtol_deci(argv[1]);
		break;
	case 1:
		/* The fields are independent console, memory, and NPK log thresholds. */
		snprintf(str, MAX_STR_SIZE, "console_loglevel: %u, "
			"mem_loglevel: %u, npk_loglevel: %u\r\n",
			console_loglevel, mem_loglevel, npk_loglevel);
		shell_puts(str);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
