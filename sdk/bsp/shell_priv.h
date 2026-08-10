/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SHELL_PRIV_H
#define SHELL_PRIV_H

#ifndef CONFIG_STATIC_ARM64_PLATFORM
#include <board_info.h>
#endif
#include <spinlock.h>
#include <asm/page.h>

#define SHELL_CMD_MAX_LEN		100U
#define SHELL_STRING_MAX_LEN		(PAGE_SIZE << 2U)

#define TEMP_STR_SIZE		128U
#define MAX_STR_SIZE		256U
#define SHELL_LOG_BUF_SIZE		(PAGE_SIZE * MAX_PCPU_NUM / 2U)

extern uint16_t console_vmid;
extern char shell_log_buf[SHELL_LOG_BUF_SIZE];

/* Commands execute synchronously in the shell thread. */
typedef int32_t (*shell_cmd_fn_t)(int32_t argc, char **argv);

#define SHELL_CMD_FLAG_SENSITIVE_ARGS	(1U << 0U)
#define SHELL_COMPLETION_FLAG_VALUE	(1U << 0U)
#define SHELL_COMPLETION_FLAG_REPEAT	(1U << 1U)

struct shell_completion_set;

struct shell_completion {
	const char *str;
	const struct shell_completion_set *children;
	uint32_t flags;
};

struct shell_completion_set {
	const struct shell_completion *entries;
	uint32_t count;
};

struct shell_cmd {
	char *str;
	char *cmd_param;
	char *help_str;
	shell_cmd_fn_t fcn;
	uint32_t flags;
	const struct shell_completion_set *completion;
};

#define MAX_BUFFERED_CMDS 8

/*
 * The shell thread exclusively owns input history, cursor state, and sensitive
 * input tracking. Command arrays are immutable after shell_init() publishes the
 * registry-owned tables to this control block.
 */
struct shell {
	char buffered_line[MAX_BUFFERED_CMDS][SHELL_CMD_MAX_LEN + 1U];
	uint32_t input_line_len;
	int32_t input_line_active;
	uint32_t sensitive_mask_start;
	bool input_sensitive;

	int32_t to_select_index;
	uint32_t cursor_offset;

	struct shell_cmd *cmds;
	uint32_t cmd_count;

	struct shell_cmd *arch_cmds;
	uint32_t arch_cmd_count;
};

void shell_puts(const char *string_ptr);
void shell_item_begin(const char *fmt, ...);
void shell_item_section(const char *fmt, ...);
void shell_item_line(const char *fmt, ...);
void shell_item_end(void);
void shell_output_checkpoint(void);
uint16_t sanitize_vmid(uint16_t vmid);

#endif /* SHELL_PRIV_H */
