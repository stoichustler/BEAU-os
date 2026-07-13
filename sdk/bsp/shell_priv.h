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

/* Shell Command Function */
typedef int32_t (*shell_cmd_fn_t)(int32_t argc, char **argv);

/* Shell Command */
struct shell_cmd {
	char *str;		/* Command string */
	char *cmd_param;	/* Command parameter string */
	char *help_str;		/* Help text associated with the command */
	shell_cmd_fn_t fcn;	/* Command call-back function */

};

#define MAX_BUFFERED_CMDS 8

/* Shell Control Block */
struct shell {
	/* a ring buffer to buffer former commands and use one as current active input */
	char buffered_line[MAX_BUFFERED_CMDS][SHELL_CMD_MAX_LEN + 1U];
	uint32_t input_line_len;	/* Length of current input line */
	int32_t input_line_active;	/* Active input line index */

	int32_t to_select_index; /* used for up/down key to select former cmds */
	uint32_t cursor_offset; /* cursor offset position from left input line */

	struct shell_cmd *cmds;	/* cmds supported */
	uint32_t cmd_count;	/* Count of cmds supported */

	struct shell_cmd *arch_cmds;	/* arch cmds supported */
	uint32_t arch_cmd_count;	/* arch Count of cmds supported */
};

/* Shell Command list with parameters and help description */
#define SHELL_CMD_VERSION		"version"
#define SHELL_CMD_VERSION_PARAM		NULL
#define SHELL_CMD_VERSION_HELP		"display the hv version information"

#define SHELL_CMD_CLEAR		"clear"
#define SHELL_CMD_CLEAR_PARAM		NULL
#define SHELL_CMD_CLEAR_HELP		"clear the BEAU console screen"

#define SHELL_CMD_SYMTAB		"symtab"
#define SHELL_CMD_SYMTAB_PARAM		NULL
#define SHELL_CMD_SYMTAB_HELP		"list debug symbol offsets and names"

#define SHELL_CMD_LOG_LVL		"loglevel"
#define SHELL_CMD_LOG_LVL_PARAM		"[<console_loglevel> [<mem_loglevel> [npk_loglevel]]]"
#define SHELL_CMD_LOG_LVL_HELP		"loglevel {0-6}"

#define SHELL_CMD_DUMP_HOST_MEM		"memscan"
#define SHELL_CMD_DUMP_HOST_MEM_PARAM	"<addr, length>"
#define SHELL_CMD_DUMP_HOST_MEM_HELP	"dump host memory: address(hex), size(dec in bytes)"

#define SHELL_CMD_VCPU_LIST		"vcpus"
#define SHELL_CMD_VCPU_LIST_PARAM	NULL
#define SHELL_CMD_VCPU_LIST_HELP	"list all vcpus in all vms"

#define SHELL_CMD_THREAD_LIST		"ps"
#define SHELL_CMD_THREAD_LIST_PARAM	NULL
#define SHELL_CMD_THREAD_LIST_HELP	"list scheduler threads and state"

#define SHELL_CMD_SCHED		"schedstat"
#define SHELL_CMD_SCHED_PARAM		NULL
#define SHELL_CMD_SCHED_HELP		"list per-pcpu scheduler statistics"

#define SHELL_CMD_IRQ_STATS		"irqstat"
#define SHELL_CMD_IRQ_STATS_PARAM	NULL
#define SHELL_CMD_IRQ_STATS_HELP	"list host IRQ counts and ARM64 guest vIRQ latency"

#define SHELL_CMD_VM_CONSOLE		"vsh"
#define SHELL_CMD_VM_CONSOLE_PARAM	"<vm id>"
#define SHELL_CMD_VM_CONSOLE_HELP	"switch to the vm console. type `CTRL-D` switch to BEAU"

#define SHELL_CMD_VM_LOG		"vlog"
#define SHELL_CMD_VM_LOG_PARAM		"<vm id>"
#define SHELL_CMD_VM_LOG_HELP		"show buffered vm console history without switching to the vm"

void shell_puts(const char *string_ptr);
void shell_item_begin(const char *fmt, ...);
void shell_item_section(const char *fmt, ...);
void shell_item_line(const char *fmt, ...);
void shell_item_end(void);
uint16_t sanitize_vmid(uint16_t vmid);

#endif /* SHELL_PRIV_H */
