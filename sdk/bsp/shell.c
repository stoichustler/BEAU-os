/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bits.h>
#include "shell_priv.h"
#include <console.h>
#include <per_cpu.h>
#include <sprintf.h>
#include <util.h>
#include <logmsg.h>
#include <version.h>
#include <shell.h>
#include <cpu.h>
#include <ticks.h>
#include <schedule.h>
#include <irq.h>
#include <debug/symbol.h>
#include <bits.h>
#include <banner.h>

#define SHELL_PROMPT_STR	"console:\\> "
#define SHELL_ASCII_BS		'\b'
#define SHELL_ASCII_TAB		'\t'
#define SHELL_ASCII_DEL		0x7fU
#define SHELL_VT100_CLEAR_LINE	"\033[2K"
#define SHELL_SCHEDSTAT_MAX_THREADS	((CONFIG_MAX_VM_NUM * MAX_VCPUS_PER_VM) + MAX_PCPU_NUM + 8U)
#define SHELL_SCHEDSTAT_PERCENT_SCALE	1000UL

char shell_log_buf[SHELL_LOG_BUF_SIZE];

extern struct shell_cmd arch_shell_cmds[];
extern uint32_t arch_shell_cmds_sz;
/* Input Line Other - Switch to the "other" input line (there are only two
 * input lines total).
 */

static void shell_print_registered_commands(void);
static void shell_handle_tab_key(void);
static int32_t shell_version(__unused int32_t argc, __unused char **argv);
static int32_t shell_symtab(int32_t argc, __unused char **argv);
static int32_t shell_loglevel(int32_t argc, char **argv);
static int32_t shell_dump_host_mem(int32_t argc, char **argv);
static int32_t shell_list_vcpu(__unused int32_t argc, __unused char **argv);
static int32_t shell_list_threads(__unused int32_t argc, __unused char **argv);
static int32_t shell_schedstat(__unused int32_t argc, __unused char **argv);
static int32_t shell_irqstat(int32_t argc, char **argv);
static int32_t shell_to_vm_console(int32_t argc, char **argv);
static const char *thread_state_str(enum thread_object_state state);

static struct shell_cmd shell_cmds[] = {
	{
		.str		= SHELL_CMD_VERSION,
		.cmd_param	= SHELL_CMD_VERSION_PARAM,
		.help_str	= SHELL_CMD_VERSION_HELP,
		.fcn		= shell_version,
	},
	{
		.str		= SHELL_CMD_SYMTAB,
		.cmd_param	= SHELL_CMD_SYMTAB_PARAM,
		.help_str	= SHELL_CMD_SYMTAB_HELP,
		.fcn		= shell_symtab,
	},
	{
		.str		= SHELL_CMD_LOG_LVL,
		.cmd_param	= SHELL_CMD_LOG_LVL_PARAM,
		.help_str	= SHELL_CMD_LOG_LVL_HELP,
		.fcn		= shell_loglevel,
	},
	{
		.str		= SHELL_CMD_DUMP_HOST_MEM,
		.cmd_param	= SHELL_CMD_DUMP_HOST_MEM_PARAM,
		.help_str	= SHELL_CMD_DUMP_HOST_MEM_HELP,
		.fcn		= shell_dump_host_mem,
	},
	{
		.str		= SHELL_CMD_VCPU_LIST,
		.cmd_param	= SHELL_CMD_VCPU_LIST_PARAM,
		.help_str	= SHELL_CMD_VCPU_LIST_HELP,
		.fcn		= shell_list_vcpu,
	},
	{
		.str		= SHELL_CMD_THREAD_LIST,
		.cmd_param	= SHELL_CMD_THREAD_LIST_PARAM,
		.help_str	= SHELL_CMD_THREAD_LIST_HELP,
		.fcn		= shell_list_threads,
	},
	{
		.str		= SHELL_CMD_SCHED,
		.cmd_param	= SHELL_CMD_SCHED_PARAM,
		.help_str	= SHELL_CMD_SCHED_HELP,
		.fcn		= shell_schedstat,
	},
	{
		.str		= SHELL_CMD_IRQ_STATS,
		.cmd_param	= SHELL_CMD_IRQ_STATS_PARAM,
		.help_str	= SHELL_CMD_IRQ_STATS_HELP,
		.fcn		= shell_irqstat,
	},
	{
		.str		= SHELL_CMD_VM_CONSOLE,
		.cmd_param	= SHELL_CMD_VM_CONSOLE_PARAM,
		.help_str	= SHELL_CMD_VM_CONSOLE_HELP,
		.fcn		= shell_to_vm_console,
	},
};

/* for function key: up/down/right/left/home/end and delete key */
enum function_key {
	KEY_NONE,

	KEY_DELETE = 0x5B33,
	KEY_UP = 0x5B41,
	KEY_DOWN = 0x5B42,
	KEY_RIGHT = 0x5B43,
	KEY_LEFT = 0x5B44,
	KEY_END = 0x5B46,
	KEY_HOME = 0x5B48,
};

extern uint16_t mem_loglevel;
extern uint16_t console_loglevel;
extern uint16_t npk_loglevel;


static struct shell hv_shell;
static struct shell *p_shell = &hv_shell;
static struct thread_object shell_thread;
static uint8_t shell_stack[CONFIG_STACK_SIZE] __aligned(16);
static spinlock_t shell_tx_lock = {0U};
static bool shell_started;
static bool shell_prompt_enabled;
static bool shell_input_active;

static void shell_thread_main(__unused struct thread_object *obj);

struct shell_schedstat_thread_sample {
	const struct thread_object *thread;
	uint64_t runtime_ticks;
};

struct shell_schedstat_snapshot {
	bool valid;
	bool overflow;
	uint64_t sample_ticks;
	uint64_t idle_runtime_ticks[MAX_PCPU_NUM];
	bool idle_seen[MAX_PCPU_NUM];
	uint32_t thread_count;
	struct shell_schedstat_thread_sample thread[SHELL_SCHEDSTAT_MAX_THREADS];
};

static struct shell_schedstat_snapshot shell_schedstat_last;
static struct shell_schedstat_snapshot shell_schedstat_sample;

static int32_t string_to_argv(char *argv_str, void *p_argv_mem,
		__unused uint32_t argv_mem_size,
		uint32_t *p_argc, char ***p_argv)
{
	uint32_t argc;
	char **argv;
	char *p_ch;

	/* Setup initial argument values. */
	argc = 0U;
	argv = NULL;

	/* Ensure there are arguments to be processed. */
	if (argv_str == NULL) {
		*p_argc = argc;
		*p_argv = argv;
		return -EINVAL;
	}

	/* Process the argument string (there is at least one element). */
	argv = (char **)p_argv_mem;
	p_ch = argv_str;

	/* Remove all spaces at the beginning of cmd*/
	while (*p_ch == ' ') {
		p_ch++;
	}

	while (*p_ch != 0) {
		/* Add argument (string) pointer to the vector. */
		argv[argc] = p_ch;

		/* Move past the vector entry argument string (in the
		 * argument string).
		 */
		while ((*p_ch != ' ') && (*p_ch != ',') && (*p_ch != 0)) {
			p_ch++;
		}

		/* Count the argument just processed. */
		argc++;

		/* Check for the end of the argument string. */
		if (*p_ch != 0) {
			/* Terminate the vector entry argument string
			 * and move to the next.
			 */
			*p_ch = 0;
			/* Remove all space in middile of cmdline */
			p_ch++;
			while (*p_ch == ' ') {
				p_ch++;
			}
		}
	}

	/* Update return parameters */
	*p_argc = argc;
	*p_argv = argv;

	return 0;
}

static uint32_t shell_cmd_total(void)
{
	return p_shell->cmd_count + p_shell->arch_cmd_count;
}

static struct shell_cmd *shell_cmd_at(uint32_t idx)
{
	struct shell_cmd *p_cmd;

	if (idx < p_shell->cmd_count) {
		p_cmd = &p_shell->cmds[idx];
	} else {
		p_cmd = &p_shell->arch_cmds[idx - p_shell->cmd_count];
	}

	return p_cmd;
}

static struct shell_cmd *shell_find_cmd(const char *cmd_str)
{
	uint32_t i;
	struct shell_cmd *p_cmd = NULL;

	for (i = 0U; i < shell_cmd_total(); i++) {
		p_cmd = shell_cmd_at(i);
		if (strcmp(p_cmd->str, cmd_str) == 0) {
			return p_cmd;
		}
	}
	return NULL;
}

static char shell_getc(void)
{
	return console_getc();
}

static void shell_puts_unlocked(const char *string_ptr)
{
	/* Output the string */
	(void)console_write(string_ptr, strnlen_s(string_ptr,
				SHELL_STRING_MAX_LEN));
}

void shell_puts(const char *string_ptr)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_puts_unlocked(string_ptr);
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_show_prompt(bool leading_newline)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	if (leading_newline) {
		shell_puts_unlocked("\r\n");
	}
	shell_puts_unlocked(SHELL_PROMPT_STR);
	shell_input_active = true;
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_show_banner_prompt(void)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_puts_unlocked("\r\n" SHELL_COLOR_MAGENTA);
	shell_puts_unlocked(beau_banner);
	shell_puts_unlocked(SHELL_COLOR_RESET "\r\n" SHELL_PROMPT_STR);
	shell_input_active = true;
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_finish_input_line(void)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_input_active = false;
	shell_puts_unlocked("\r\n");
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_set_input_active(bool active)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_input_active = active;
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_item_vprint(const char *prefix, const char *fmt, va_list args)
{
	char body[MAX_STR_SIZE];
	char line[MAX_STR_SIZE];

	(void)vsnprintf(body, sizeof(body), fmt, args);
	(void)snprintf(line, sizeof(line), "%s%s\r\n", prefix, body);
	shell_puts(line);
}

void shell_item_begin(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	shell_item_vprint("\r\n┌─  ", fmt, args);
	va_end(args);
}

void shell_item_section(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	shell_item_vprint("├─  ", fmt, args);
	va_end(args);
}

void shell_item_line(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	shell_item_vprint("│   ", fmt, args);
	va_end(args);
}

void shell_item_end(void)
{
	shell_puts("└─\r\n");
}

static void clear_input_line(uint32_t len)
{
	while (len > 0) {
		len--;
		shell_puts("\b");
		shell_puts(" \b");
	}
}

static void set_cursor_pos(uint32_t left_offset)
{
	while (left_offset > 0) {
		left_offset--;
		shell_puts("\b");
	}
}

static void set_cursor_pos_unlocked(uint32_t left_offset)
{
	while (left_offset > 0) {
		left_offset--;
		shell_puts_unlocked("\b");
	}
}

static void shell_clear_current_line_unlocked(void)
{
	/*
	 * Async output borrows the active terminal row. Clear the prompt/input
	 * first, then redraw it after the background line.
	 */
	shell_puts_unlocked("\r" SHELL_VT100_CLEAR_LINE);
}

static void shell_restore_input_line_unlocked(void)
{
	shell_puts_unlocked(SHELL_PROMPT_STR);
	if (p_shell->input_line_len > 0U) {
		shell_puts_unlocked(p_shell->buffered_line[p_shell->input_line_active]);
		set_cursor_pos_unlocked(p_shell->input_line_len - p_shell->cursor_offset);
	}
}

static void shell_restore_input_line(void)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_restore_input_line_unlocked();
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static bool shell_cmd_matches_prefix(const struct shell_cmd *p_cmd, const char *prefix, uint32_t prefix_len)
{
	return (strnlen_s(p_cmd->str, SHELL_CMD_MAX_LEN) >= prefix_len) &&
		(strncmp(p_cmd->str, prefix, prefix_len) == 0);
}

static uint32_t shell_common_prefix_len(const char *a, const char *b, uint32_t min_len)
{
	uint32_t len = 0U;

	while ((len < min_len) && (a[len] != '\0') && (b[len] != '\0') &&
		(a[len] == b[len])) {
		len++;
	}

	return len;
}

static uint32_t shell_find_cmd_matches(const char *prefix, uint32_t prefix_len,
	const struct shell_cmd **first_match, uint32_t *common_len)
{
	const struct shell_cmd *p_cmd;
	uint32_t count = 0U;
	uint32_t i;

	*first_match = NULL;
	*common_len = 0U;
	for (i = 0U; i < shell_cmd_total(); i++) {
		p_cmd = shell_cmd_at(i);
		if (!shell_cmd_matches_prefix(p_cmd, prefix, prefix_len)) {
			continue;
		}

		if (count == 0U) {
			*first_match = p_cmd;
			*common_len = (uint32_t)strnlen_s(p_cmd->str, SHELL_CMD_MAX_LEN);
		} else {
			*common_len = shell_common_prefix_len((*first_match)->str, p_cmd->str, *common_len);
		}
		count++;
	}

	return count;
}

static void shell_append_completion(const char *completion, uint32_t completion_len)
{
	char *line = p_shell->buffered_line[p_shell->input_line_active];
	uint32_t appended = 0U;
	uint32_t idx;

	for (idx = 0U; (idx < completion_len) && (p_shell->input_line_len < SHELL_CMD_MAX_LEN);
		idx++) {
		line[p_shell->input_line_len] = completion[idx];
		p_shell->input_line_len++;
		p_shell->cursor_offset++;
		appended++;
	}
	line[p_shell->input_line_len] = '\0';
	(void)console_write(completion, appended);
}

static void shell_print_cmd_matches(const char *prefix, uint32_t prefix_len)
{
	struct shell_cmd *p_cmd;
	uint32_t i;

	shell_puts("\r\n");
	for (i = 0U; i < shell_cmd_total(); i++) {
		p_cmd = shell_cmd_at(i);
		if (shell_cmd_matches_prefix(p_cmd, prefix, prefix_len)) {
			shell_puts("  ");
			shell_puts(p_cmd->str);
			shell_puts("\r\n");
		}
	}
	shell_restore_input_line();
}

static bool shell_cursor_on_command_tail(uint32_t *cmd_start, uint32_t *prefix_len)
{
	char *line = p_shell->buffered_line[p_shell->input_line_active];
	uint32_t idx = 0U;

	/*
	 * Completion is intentionally limited to the command token. Parameter
	 * completion would need command-specific parsers, while the command token can
	 * be completed safely from the common command tables.
	 */
	if (p_shell->cursor_offset != p_shell->input_line_len) {
		return false;
	}

	while ((idx < p_shell->input_line_len) && (line[idx] == ' ')) {
		idx++;
	}
	*cmd_start = idx;
	while (idx < p_shell->cursor_offset) {
		if ((line[idx] == ' ') || (line[idx] == ',')) {
			return false;
		}
		idx++;
	}
	*prefix_len = p_shell->cursor_offset - *cmd_start;

	return true;
}

static void shell_handle_tab_key(void)
{
	const struct shell_cmd *first_match;
	char *line = p_shell->buffered_line[p_shell->input_line_active];
	uint32_t cmd_start;
	uint32_t prefix_len;
	uint32_t common_len;
	uint32_t match_count;
	uint32_t first_len;

	if (!shell_cursor_on_command_tail(&cmd_start, &prefix_len)) {
		return;
	}
	if (prefix_len == 0U) {
		shell_print_registered_commands();
		shell_restore_input_line();
		return;
	}

	match_count = shell_find_cmd_matches(&line[cmd_start], prefix_len, &first_match, &common_len);
	if (match_count == 0U) {
		return;
	}

	if (match_count == 1U) {
		first_len = (uint32_t)strnlen_s(first_match->str, SHELL_CMD_MAX_LEN);
		if (first_len > prefix_len) {
			shell_append_completion(&first_match->str[prefix_len], first_len - prefix_len);
		}
		if (p_shell->input_line_len < SHELL_CMD_MAX_LEN) {
			shell_append_completion(" ", 1U);
		}
	} else if (common_len > prefix_len) {
		shell_append_completion(&first_match->str[prefix_len], common_len - prefix_len);
	} else {
		shell_print_cmd_matches(&line[cmd_start], prefix_len);
	}
}

static void handle_delete_key(void)
{
	if (p_shell->cursor_offset < p_shell->input_line_len) {

		uint32_t delta = p_shell->input_line_len - p_shell->cursor_offset - 1;

		/* Send a space + backspace sequence to delete character */
		shell_puts(" \b");

		/* display the left input chars and remove former last one */
		shell_puts(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset + 1);
		shell_puts(" \b");

		set_cursor_pos(delta);

		memcpy(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset,
			p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset + 1, delta);

		/* Null terminate the last character to erase it */
		p_shell->buffered_line[p_shell->input_line_active][p_shell->input_line_len - 1] = 0;

		/* Reduce the length of the string by one */
		p_shell->input_line_len--;
	}
}

static void handle_updown_key(enum function_key key_value)
{
	int32_t to_select, current_select = p_shell->to_select_index;

	/* update current_select and p_shell->to_select_index as up/down key */
	if (key_value == KEY_UP) {
		/* if the ring buffer not full, just decrease one until to 0; if full, need handle overflow case */
		to_select = p_shell->to_select_index - 1;
		if (to_select < 0) {
			to_select += MAX_BUFFERED_CMDS;
		}

		if (p_shell->buffered_line[to_select][0] != '\0') {
			current_select = to_select;
		}

	} else {
		/* if down key and current is active line, not need update */
		if (p_shell->to_select_index != p_shell->input_line_active) {
			current_select = (p_shell->to_select_index + 1) % MAX_BUFFERED_CMDS;
		}
	}

	/* go up/down until first buffered cmd or current input line: user will know it is end to select */
	if (current_select != p_shell->input_line_active) {
		p_shell->to_select_index = current_select;
	}

	if (strcmp(p_shell->buffered_line[current_select], p_shell->buffered_line[p_shell->input_line_active]) != 0) {
		/* reset cursor pos and clear current input line first, then output selected cmd */
		if (p_shell->cursor_offset < p_shell->input_line_len) {
			shell_puts(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset);
		}

		clear_input_line(p_shell->input_line_len);
		shell_puts(p_shell->buffered_line[current_select]);

		size_t len = strnlen_s(p_shell->buffered_line[current_select], SHELL_CMD_MAX_LEN);

		memcpy_s(p_shell->buffered_line[p_shell->input_line_active], SHELL_CMD_MAX_LEN,
			p_shell->buffered_line[current_select], len + 1);
		p_shell->input_line_len = len;
		p_shell->cursor_offset = len;
	}
}

static void shell_handle_special_char(char ch)
{
	enum function_key key_value = KEY_NONE;

	switch (ch) {
	/* original function key value: ESC + key (2/3 bytes), so consume the next 2/3 characters */
	case 0x1b:
		key_value = (shell_getc() << 8) | shell_getc();
		if (key_value == KEY_DELETE) {
			(void)shell_getc(); /* delete key has one more byte */
		}

		switch (key_value) {
		case KEY_DELETE:
			handle_delete_key();
			break;
		case KEY_UP:
		case KEY_DOWN:
			handle_updown_key(key_value);
			break;
		case KEY_RIGHT:
			if (p_shell->cursor_offset < p_shell->input_line_len) {
				shell_puts(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset);
				p_shell->cursor_offset++;
				set_cursor_pos(p_shell->input_line_len - p_shell->cursor_offset);
			}
			break;
		case KEY_LEFT:
			if (p_shell->cursor_offset > 0) {
				p_shell->cursor_offset--;
				shell_puts("\b");
			}
			break;
		case KEY_END:
			if (p_shell->cursor_offset < p_shell->input_line_len) {
				shell_puts(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset);
				p_shell->cursor_offset = p_shell->input_line_len;
			}
			break;
		case KEY_HOME:
			if (p_shell->cursor_offset > 0) {
				set_cursor_pos(p_shell->cursor_offset);
				p_shell->cursor_offset = 0;
			}
			break;
		default:
			break;
		}

		break;
	default:
		/*
		 * Only the Escape character is treated as special character.
		 * All the other characters have been handled properly in
		 * shell_input_line, so they will not be handled in this API.
		 * Gracefully return if prior case clauses have not been met.
		 */
		break;
	}
}

static void handle_backspace_key(void)
{
	/* Ensure length is not 0 */
	if (p_shell->cursor_offset > 0U) {
		/* Echo backspace */
		shell_puts("\b");
		/* Send a space + backspace sequence to delete character */
		shell_puts(" \b");

		if (p_shell->cursor_offset < p_shell->input_line_len) {
			uint32_t delta = p_shell->input_line_len - p_shell->cursor_offset;

			/* display the left input-chars and remove the former last one */
			shell_puts(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset);
			shell_puts(" \b");

			set_cursor_pos(delta);
			memcpy(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset - 1,
				p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset, delta);
		}

		/* Null terminate the last character to erase it */
		p_shell->buffered_line[p_shell->input_line_active][p_shell->input_line_len - 1] = 0;

		/* Reduce the length of the string by one */
		p_shell->input_line_len--;
		p_shell->cursor_offset--;
	}
}

static void handle_input_char(char ch)
{
	uint32_t delta = p_shell->input_line_len - p_shell->cursor_offset;

	/* move the input from cursor offset back first */
	if (delta > 0) {
		memcpy_backwards(p_shell->buffered_line[p_shell->input_line_active] + p_shell->input_line_len,
			p_shell->buffered_line[p_shell->input_line_active] + p_shell->input_line_len - 1, delta);
	}

	p_shell->buffered_line[p_shell->input_line_active][p_shell->cursor_offset] = ch;

	/* Echo back the input */
	shell_puts(p_shell->buffered_line[p_shell->input_line_active] + p_shell->cursor_offset);
	set_cursor_pos(delta);

	/* Move to next character in string */
	p_shell->input_line_len++;
	p_shell->cursor_offset++;
}

static bool shell_input_line(void)
{
	bool done = false;
	char ch;

	ch = shell_getc();

	/* Check character */
	switch (ch) {
	/* Backspace: terminals commonly send either BS or DEL. */
	case SHELL_ASCII_BS:
	case SHELL_ASCII_DEL:
		handle_backspace_key();
		break;

	case SHELL_ASCII_TAB:
		shell_handle_tab_key();
		break;

	/* Carriage-return */
	case '\r':
		shell_finish_input_line();

		/* Set flag showing line input done */
		done = true;

		/* Reset command length for next command processing */
		p_shell->input_line_len = 0U;
		p_shell->cursor_offset = 0U;
		break;

	/* Line feed */
	case '\n':
		/* Do nothing */
		break;

	/* All other characters */
	default:
		/* Ensure data doesn't exceed full terminal width */
		if (p_shell->input_line_len < SHELL_CMD_MAX_LEN) {
			/* See if a "standard" prINTable ASCII character received */
			if ((ch >= 32) && (ch <= 126)) {
				handle_input_char(ch);
			} else {
				/* call special character handler */
				shell_handle_special_char(ch);
			}
		} else {
			shell_finish_input_line();

			/* Set flag showing line input done */
			done = true;

			/* Reset command length for next command processing */
			p_shell->input_line_len = 0U;
			p_shell->cursor_offset = 0U;
		}
		break;
	}


	return done;
}

static int32_t shell_process_cmd(const char *p_input_line)
{
	int32_t status = -EINVAL;
	struct shell_cmd *p_cmd;
	char cmd_argv_str[SHELL_CMD_MAX_LEN + 1U];
	int32_t cmd_argv_mem[sizeof(char *) * ((SHELL_CMD_MAX_LEN + 1U) >> 1U)];
	int32_t cmd_argc;
	char **cmd_argv;

	/* Copy the input line INTo an argument string to become part of the
	 * argument vector.
	 */
	(void)strncpy_s(&cmd_argv_str[0], SHELL_CMD_MAX_LEN + 1U, p_input_line, SHELL_CMD_MAX_LEN);
	cmd_argv_str[SHELL_CMD_MAX_LEN] = 0;

	/* Build the argv vector from the string. The first argument in the
	 * resulting vector will be the command string itself.
	 */

	/* NOTE: This process is destructive to the argument string! */

	(void) string_to_argv(&cmd_argv_str[0],
			(void *) &cmd_argv_mem[0],
			sizeof(cmd_argv_mem), (void *)&cmd_argc, &cmd_argv);

	/* Determine if there is a command to process. */
	if (cmd_argc != 0) {
		/* See if command is in cmds supported */
		p_cmd = shell_find_cmd(cmd_argv[0]);
		if (p_cmd == NULL) {
			shell_puts("\r\n[ERR] Invalid Command.\r\n");
			return -EINVAL;
		}

		status = p_cmd->fcn(cmd_argc, &cmd_argv[0]);
		if (status == -EINVAL) {
			shell_puts("\r\n[ERR] Invalid Parameters.\r\n");
		} else if (status != 0) {
			shell_puts("\r\n[ERR] Command launch failed.\r\n");
		} else {
			/* No other state currently, do nothing */
		}
	}

	return status;
}

static int32_t shell_process(void)
{
	int32_t status, former_index;
	char *p_input_line;

	/* Process current command (using active input line). */
	p_input_line = p_shell->buffered_line[p_shell->input_line_active];

	former_index = (p_shell->input_line_active + MAX_BUFFERED_CMDS - 1) % MAX_BUFFERED_CMDS;

	/* just buffer current cmd if current is not empty and not same with last buffered one */
	if ((strnlen_s(p_input_line, SHELL_CMD_MAX_LEN) > 0) &&
		(strcmp(p_input_line, p_shell->buffered_line[former_index]) != 0)) {
		p_shell->input_line_active = (p_shell->input_line_active + 1) % MAX_BUFFERED_CMDS;
	}

	p_shell->to_select_index = p_shell->input_line_active;

	/* Process command */
	status = shell_process_cmd(p_input_line);

	/* Now that the command is processed, zero fill the input buffer */
	(void)memset(p_shell->buffered_line[p_shell->input_line_active], 0, SHELL_CMD_MAX_LEN + 1U);

	/* Process command and return result to caller */
	return status;
}


void shell_kick(void)
{
	static bool is_cmd_cmplt = false;

	if (console_vm_kick()) {
		return;
	}

	if (!shell_prompt_enabled) {
		char ch = shell_getc();

		/*
		 * Guest APs can still be brought up by PSCI after shell_start() because
		 * only VM BSPs are launched by the host autostart path. Keep the BEAU
		 * shell quiet until the user presses Enter so the first prompt does not
		 * appear in the middle of late vCPU scheduling logs.
		 */
		if ((ch == '\r') || (ch == '\n')) {
			shell_prompt_enabled = true;
			shell_show_banner_prompt();
			is_cmd_cmplt = false;
		}
		return;
	}

	/* At any given instance, UART may be owned by the HV
	 * OR by the guest that has enabled the vUart.
	 * Show HV shell prompt ONLY when HV owns the
	 * serial port.
	 */
	/* Prompt the user for a selection. */
	if (is_cmd_cmplt) {
		shell_show_prompt(false);
	}

	/* Get user's input */
	is_cmd_cmplt = shell_input_line();

	/* If user has pressed the ENTER then process
	 * the command
	 */
	if (is_cmd_cmplt) {
		/* Process current input line. */
		(void)shell_process();
	}
}

bool shell_is_open(void)
{
	return shell_prompt_enabled && console_is_hv();
}

bool shell_async_puts(const char *string_ptr)
{
	uint64_t rflags;

	if (!shell_is_open()) {
		return false;
	}

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	if (shell_input_active) {
		shell_clear_current_line_unlocked();
		shell_puts_unlocked(string_ptr);
		shell_restore_input_line_unlocked();
	} else {
		shell_puts_unlocked("\r\n");
		shell_puts_unlocked(string_ptr);
	}
	spinlock_irqrestore_release(&shell_tx_lock, rflags);

	return true;
}

bool shell_async_puts_raw(const char *string_ptr)
{
	uint64_t rflags;

	if (!shell_is_open()) {
		return false;
	}

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	if (shell_input_active) {
		/*
		 * 2026-07-08, shell/log interleave principle:
		 *
		 * System logs may arrive while the shell prompt is already visible.
		 * Treat the prompt and any partially typed command as an editable row:
		 *
		 *   prompt/input -> clear row -> async log line -> restore prompt/input
		 *
		 * This mirrors Zephyr shell console behavior: asynchronous output owns
		 * a complete line, never the tail of the current input row.
		 */
		shell_clear_current_line_unlocked();
		shell_puts_unlocked(string_ptr);
		shell_restore_input_line_unlocked();
	} else {
		shell_puts_unlocked(string_ptr);
	}
	spinlock_irqrestore_release(&shell_tx_lock, rflags);

	return true;
}

static void shell_thread_main(__unused struct thread_object *obj)
{
	while (true) {
		shell_kick();
		yield_current();
		schedule();
	}
}

void shell_init(void)
{
	p_shell->cmds = shell_cmds;
	p_shell->cmd_count = ARRAY_SIZE(shell_cmds);

	p_shell->arch_cmds = arch_shell_cmds;
	p_shell->arch_cmd_count = arch_shell_cmds_sz;

	p_shell->to_select_index = 0;

	/* Zero fill the input buffer */
	(void)memset(p_shell->buffered_line[p_shell->input_line_active], 0U, SHELL_CMD_MAX_LEN + 1U);
}

void shell_start(void)
{
	struct sched_params shell_params = {0U};

	if (shell_started) {
		return;
	}

	(void)strncpy_s(shell_thread.name, sizeof(shell_thread.name), "shell", sizeof(shell_thread.name));
	shell_thread.pcpu_id = BSP_CPU_ID;
	shell_thread.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	shell_thread.thread_entry = shell_thread_main;
	shell_thread.switch_out = NULL;
	shell_thread.switch_in = NULL;
	shell_thread.host_sp = arch_setup_thread_stack(&shell_thread, shell_stack, CONFIG_STACK_SIZE);

	shell_params.prio = PRIO_LOW;
	init_thread_data(&shell_thread, &shell_params);
	wake_thread(&shell_thread);
	shell_started = true;
}

#define MAX_OUTPUT_LEN  80
static void shell_print_registered_commands(void)
{
	struct shell_cmd *p_cmd = NULL;

	char str[MAX_STR_SIZE];
	uint32_t cmd_cnt = shell_cmd_total();
	/* Print title */
	shell_puts("\r\n\r\n────────── [BEAU commands] ──────────\r\n\r\n");

	/* Proceed based on the number of registered commands. */
	if (cmd_cnt == 0U) {
		/* No registered commands */
		shell_puts("none\r\n");
	} else {
		uint32_t j;

		for (j = 0U; j < cmd_cnt; j++) {
			const char *cmd_param;
			const char *help_str;

			p_cmd = shell_cmd_at(j);

			cmd_param = (p_cmd->cmd_param == NULL) ? " " : p_cmd->cmd_param;
			(void)memset(str, ' ', sizeof(str));
			/* Output the command & parameter string */
			snprintf(str, MAX_OUTPUT_LEN, " %-15s%-64s",
					p_cmd->str, cmd_param);
			shell_puts(str);
			shell_puts("\r\n");

			help_str = (p_cmd->help_str == NULL) ? "" : p_cmd->help_str;
			while (strnlen_s(help_str, MAX_OUTPUT_LEN) > 0U) {
				(void)memset(str, ' ', sizeof(str));
				if (strnlen_s(help_str, MAX_OUTPUT_LEN) > 65) {
					snprintf(str, MAX_OUTPUT_LEN, "         %-s", help_str);
					shell_puts(str);
					shell_puts("\r\n");
					help_str = help_str + 65;
				} else {
					snprintf(str, MAX_OUTPUT_LEN, "         %-s", help_str);
					shell_puts(str);
					shell_puts("\r\n");
					break;
				}
			}
		}
	}

	shell_puts("\r\n");
}

static int32_t shell_version(__unused int32_t argc, __unused char **argv)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE, "hv: %s-%s-%s %s%s%s%s %s@%s build by %s %s\r\n",
		HV_BRANCH_VERSION, HV_COMMIT_TIME, HV_COMMIT_DIRTY, HV_BUILD_TYPE,
		(sizeof(HV_COMMIT_TAGS) > 1) ? "(tag: " : "", HV_COMMIT_TAGS, 
		(sizeof(HV_COMMIT_TAGS) > 1) ? ")" : "",
		HV_BUILD_SCENARIO, HV_BUILD_BOARD, HV_BUILD_USER, HV_BUILD_TIME);
	shell_puts(temp_str);

	return 0;
}

static int32_t shell_symtab(int32_t argc, __unused char **argv)
{
	char temp_str[MAX_STR_SIZE];
	uint32_t i;

	if (argc != 1) {
		return -EINVAL;
	}

	if (dbg_symbol_count == 0U) {
		shell_puts("\r\nsymbol table is empty\r\n");
		return 0;
	}

	shell_puts("\r\noffset              symbol\r\n");
	shell_puts("──────────────────  ────────────────────────────────\r\n");
	for (i = 0U; i < dbg_symbol_count; i++) {
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

		(void)snprintf(temp_str, MAX_STR_SIZE, "0x%016lx  ", offset);
		shell_puts(temp_str);
		shell_puts((name == NULL) ? "<null>" : name);
		shell_puts("\r\n");
	}

	return 0;
}

static int32_t shell_loglevel(int32_t argc, char **argv)
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

static int32_t shell_dump_host_mem(int32_t argc, char **argv)
{
	uint64_t *hva;
	int32_t ret;
	uint32_t i, length, loop_cnt;
	char temp_str[MAX_STR_SIZE];

	/* User input invalidation */
	if (argc != 3) {
		ret = -EINVAL;
	} else	{
		hva = (uint64_t *)strtoul_hex(argv[1]);
		length = (uint32_t)strtol_deci(argv[2]);

		snprintf(temp_str, MAX_STR_SIZE, "dump physical memory addr: 0x%016lx, length %d:\r\n", hva, length);
		shell_puts(temp_str);
		/* Change the length to a multiple of 32 if the length is not */
		loop_cnt = ((length & 0x1fU) == 0U) ? ((length >> 5U)) : ((length >> 5U) + 1U);
		for (i = 0U; i < loop_cnt; i++) {
			snprintf(temp_str, MAX_STR_SIZE, "hva(0x%llx): 0x%016lx  0x%016lx  0x%016lx  0x%016lx\r\n",
					hva, *hva, *(hva + 1UL), *(hva + 2UL), *(hva + 3UL));
			hva += 4UL;
			shell_puts(temp_str);
		}
		ret = 0;
	}

	return ret;
}

static bool pcpu_is_shared_by_vcpus(uint16_t pcpu_id)
{
	struct acrn_vm *vm;
	struct acrn_vcpu *vcpu;
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint16_t count = 0U;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm = get_vm_from_vmid(vm_id);
		if (is_poweroff_vm(vm)) {
			continue;
		}

		foreach_vcpu(vcpu_id, vm, vcpu) {
			if (pcpuid_from_vcpu(vcpu) == pcpu_id) {
				count++;
				if (count > 1U) {
					return true;
				}
			}
		}
	}

	return false;
}

/*
 * 2026-06-30, vcpus monitor:
 *
 * This command reports vCPU scheduler ownership, not guest CPU topology.
 * It answers which pCPU owns each vCPU thread, whether that pCPU is shared by
 * multiple vCPUs, and how long the vCPU has waited in its current state.
 *
 *   VM/vCPU -> scheduler thread -> pCPU binding -> latency snapshot
 */
static int32_t shell_list_vcpu(__unused int32_t argc, __unused char **argv)
{
	char temp_str[MAX_STR_SIZE];
	struct acrn_vm *vm;
	struct acrn_vcpu *vcpu;
	uint16_t i;
	uint16_t idx;

	shell_puts("\r\nvcpu       pcpu  pcpu_mode  state     switches  lastwait.us  maxwait.us  since.us\r\n");
	shell_puts("─────────  ────  ─────────  ────────  ────────  ───────────  ──────────  ────────\r\n");

	for (idx = 0U; idx < CONFIG_MAX_VM_NUM; idx++) {
		vm = get_vm_from_vmid(idx);
		if (is_poweroff_vm(vm)) {
			continue;
		}
		foreach_vcpu(i, vm, vcpu) {
			struct sched_latency_stats stats = { 0U };
			char since_us[24U];
			uint64_t since_ticks;
			uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);
			bool shared_pcpu = pcpu_is_shared_by_vcpus(pcpu_id);

			sched_get_latency(&vcpu->thread_obj, &stats);
			if (shared_pcpu) {
				since_ticks = (stats.state_since != 0UL) ? (cpu_ticks() - stats.state_since) : 0UL;
				snprintf(since_us, sizeof(since_us), "%lu", ticks_to_us(since_ticks));
			} else {
				snprintf(since_us, sizeof(since_us), "-");
			}
			snprintf(temp_str, MAX_STR_SIZE,
				"%-9s  %-4hu  %-9s  %-8s  %-8lu  %-11lu  %-10lu  %-8s\r\n",
				vcpu->thread_obj.name,
				pcpu_id,
				shared_pcpu ? "shared" : "exclusive",
				thread_state_str(vcpu->thread_obj.status),
				stats.switches,
				ticks_to_us(stats.last_wait_ticks),
				ticks_to_us(stats.max_wait_ticks),
				since_us);
			shell_puts(temp_str);
		}
	}

	return 0;
}

static const char *thread_state_str(enum thread_object_state state)
{
	const char *str;

	switch (state) {
	case THREAD_STS_RUNNING:
		str = "running";
		break;
	case THREAD_STS_RUNNABLE:
		str = "runnable";
		break;
	case THREAD_STS_BLOCKED:
		str = "blocked";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

/*
 * 2026-06-30, threads monitor:
 *
 * This is the scheduler-wide inventory behind the VM-centric commands. It
 * includes idle, shell, helper, and vCPU threads so a stuck VM can be compared
 * with the actual runnable/current thread list on each pCPU.
 *
 *   scheduler thread list -> per-thread state -> per-pCPU current owner
 */
static int32_t shell_list_threads(__unused int32_t argc, __unused char **argv)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;
	struct thread_object *thread;
	struct thread_object *current;
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE, "\r\nthreads: %u\r\n", sched_get_thread_count());
	shell_puts(temp_str);
	shell_puts("name             pcpu    state       current    entry\r\n");
	shell_puts("───────────────  ────    ────────    ───────    ────────────────\r\n");

	list_for_each(pos, head) {
		thread = container_of(pos, struct thread_object, node);
		current = sched_get_current(thread->pcpu_id);
		snprintf(temp_str, MAX_STR_SIZE,
			"%-15s  %-4hu    %-8s    %-7s    0x%014lx\r\n",
			thread->name,
			thread->pcpu_id,
			thread_state_str(thread->status),
			(current == thread) ? "yes" : "no",
			(uint64_t)thread->thread_entry);
		shell_puts(temp_str);
	}

	return 0;
}

static uint32_t shell_sched_runqueue_count(uint16_t pcpu_id)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;
	uint32_t count = 0U;

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);

		if ((thread->pcpu_id == pcpu_id) && (thread->status == THREAD_STS_RUNNABLE)) {
			count++;
		}
	}

	return count;
}

static uint64_t shell_schedstat_delta(uint64_t current, uint64_t previous)
{
	return (current >= previous) ? (current - previous) : 0UL;
}

static void shell_schedstat_format_percent(char *buf, size_t size, uint64_t used_ticks,
	uint64_t window_ticks)
{
	uint64_t permille;

	if (window_ticks == 0UL) {
		snprintf(buf, size, "0.0");
		return;
	}

	if ((used_ticks >= window_ticks) || (used_ticks > (UINT64_MAX / SHELL_SCHEDSTAT_PERCENT_SCALE))) {
		permille = SHELL_SCHEDSTAT_PERCENT_SCALE;
	} else {
		permille = (used_ticks * SHELL_SCHEDSTAT_PERCENT_SCALE) / window_ticks;
		if (permille > SHELL_SCHEDSTAT_PERCENT_SCALE) {
			permille = SHELL_SCHEDSTAT_PERCENT_SCALE;
		}
	}

	snprintf(buf, size, "%lu.%01lu", permille / 10UL, permille % 10UL);
}

static const struct shell_schedstat_thread_sample *shell_schedstat_find_thread_sample(
	const struct shell_schedstat_snapshot *snapshot, const struct thread_object *thread)
{
	uint32_t idx;

	for (idx = 0U; idx < snapshot->thread_count; idx++) {
		if (snapshot->thread[idx].thread == thread) {
			return &snapshot->thread[idx];
		}
	}

	return NULL;
}

/*
 * 2026-06-30, schedstat monitor:
 *
 * schedstat combines absolute scheduler counters with a two-sample runtime
 * delta. The pCPU table shows whether ticks, reschedules, and switches are
 * moving; the CPU-usage table shows which non-idle thread consumed the sample
 * window since the previous schedstat command.
 *
 *   previous shell snapshot
 *            |
 *            v
 *   current scheduler snapshot -> pCPU counters + per-thread runtime delta
 */
static void shell_schedstat_take_snapshot(struct shell_schedstat_snapshot *snapshot)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;

	(void)memset(snapshot, 0U, sizeof(*snapshot));

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		struct sched_latency_stats stats = { 0U };
		uint16_t pcpu_id = thread->pcpu_id;

		sched_get_latency(thread, &stats);
		if (snapshot->thread_count < SHELL_SCHEDSTAT_MAX_THREADS) {
			snapshot->thread[snapshot->thread_count].thread = thread;
			snapshot->thread[snapshot->thread_count].runtime_ticks = stats.runtime_ticks;
			snapshot->thread_count++;
		} else {
			snapshot->overflow = true;
		}

		if ((pcpu_id < MAX_PCPU_NUM) && is_idle_thread(thread)) {
			snapshot->idle_runtime_ticks[pcpu_id] = stats.runtime_ticks;
			snapshot->idle_seen[pcpu_id] = true;
		}
	}

	snapshot->sample_ticks = cpu_ticks();
	snapshot->valid = true;
}

static void shell_schedstat_format_pcpu_busy(char *buf, size_t size, uint16_t pcpu_id,
	uint64_t window_ticks)
{
	uint64_t idle_delta;
	uint64_t idle_permille;
	uint64_t busy_permille;

	if (!shell_schedstat_last.valid ||
		!shell_schedstat_sample.idle_seen[pcpu_id] ||
		!shell_schedstat_last.idle_seen[pcpu_id] ||
		(window_ticks == 0UL)) {
		snprintf(buf, size, "0.0");
		return;
	}

	idle_delta = shell_schedstat_delta(shell_schedstat_sample.idle_runtime_ticks[pcpu_id],
		shell_schedstat_last.idle_runtime_ticks[pcpu_id]);
	if ((idle_delta >= window_ticks) || (idle_delta > (UINT64_MAX / SHELL_SCHEDSTAT_PERCENT_SCALE))) {
		idle_permille = SHELL_SCHEDSTAT_PERCENT_SCALE;
	} else {
		idle_permille = (idle_delta * SHELL_SCHEDSTAT_PERCENT_SCALE) / window_ticks;
		if (idle_permille > SHELL_SCHEDSTAT_PERCENT_SCALE) {
			idle_permille = SHELL_SCHEDSTAT_PERCENT_SCALE;
		}
	}

	busy_permille = SHELL_SCHEDSTAT_PERCENT_SCALE - idle_permille;
	snprintf(buf, size, "%lu.%01lu", busy_permille / 10UL, busy_permille % 10UL);
}

static void shell_schedstat_print_cpu_usage(const struct list_head *head, uint64_t window_ticks)
{
	struct list_head *pos;
	char temp_str[MAX_STR_SIZE];

	/*
	 * 2026-06-30 scheduler stats:
	 *   schedule() accumulates per-thread running ticks.
	 *   schedstat keeps the last shell snapshot and prints deltas.
	 *
	 *   previous snapshot --delta window--> current snapshot
	 *          runtime[N] ----------------> runtime[N] + run_delta
	 */
	shell_puts("\r\nCPU usage since previous schedstat:\r\n\r\n");
	shell_puts("name             pcpu  state     cpu%   run.us\r\n");
	shell_puts("───────────────  ────  ────────  ─────  ─────────\r\n");

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		const struct shell_schedstat_thread_sample *current;
		const struct shell_schedstat_thread_sample *previous;
		uint64_t run_delta;
		char percent[16U];

		if (is_idle_thread(thread)) {
			continue;
		}

		current = shell_schedstat_find_thread_sample(&shell_schedstat_sample, thread);
		previous = shell_schedstat_find_thread_sample(&shell_schedstat_last, thread);
		if ((current == NULL) || (previous == NULL) || (window_ticks == 0UL)) {
			snprintf(percent, sizeof(percent), "0.0");
			run_delta = 0UL;
		} else {
			run_delta = shell_schedstat_delta(current->runtime_ticks, previous->runtime_ticks);
			shell_schedstat_format_percent(percent, sizeof(percent), run_delta, window_ticks);
		}

		snprintf(temp_str, MAX_STR_SIZE,
			"%-15s  %-4hu  %-8s  %-5s  %-9lu\r\n",
			thread->name,
			thread->pcpu_id,
			thread_state_str(thread->status),
			percent,
			ticks_to_us(run_delta));
		shell_puts(temp_str);
	}

	if (shell_schedstat_sample.overflow || shell_schedstat_last.overflow) {
		shell_puts("\r\nwarning: schedstat thread sample overflow; cpu% may omit some threads.\r\n");
	}
}

static int32_t shell_schedstat(__unused int32_t argc, __unused char **argv)
{
	const struct list_head *head = sched_get_thread_list();
	char temp_str[MAX_STR_SIZE];
	struct list_head *pos;
	uint16_t pcpu_id;
	uint16_t pcpu_num = get_pcpu_nums();
	bool has_bvt_stats = false;
	bool has_rtds_stats = false;
	uint64_t window_ticks;

	shell_schedstat_take_snapshot(&shell_schedstat_sample);
	window_ticks = shell_schedstat_last.valid ?
		shell_schedstat_delta(shell_schedstat_sample.sample_ticks, shell_schedstat_last.sample_ticks) : 0UL;

	snprintf(temp_str, MAX_STR_SIZE,
		"\r\nschedstat pcpus:%hu\r\n", pcpu_num);
	shell_puts(temp_str);

	/*
	 * Per-pCPU counters answer whether the scheduler is ticking, whether
	 * context switches are happening, and which thread currently owns a CPU.
	 */
	shell_puts("\r\nPer-pCPU hybrid scheduler counters:\r\n\r\n");
	shell_puts("pcpu  role       scheduler    busy%  timer   switches  resched  runqueue  current\r\n");
	shell_puts("────  ─────────  ───────────  ─────  ──────  ────────  ───────  ────────  ─────────────────\r\n");

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		struct thread_object *current = sched_get_current(pcpu_id);
		const char *name = (current != NULL) ? current->name : "-";
		bool shared_pcpu = pcpu_is_shared_by_vcpus(pcpu_id);
		char busy[16U];

		shell_schedstat_format_pcpu_busy(busy, sizeof(busy), pcpu_id, window_ticks);

		snprintf(temp_str, MAX_STR_SIZE,
			"%-5hu %-10s %-12s %-6s %-7lu %-9lu %-8lu %-9u %s\r\n",
			pcpu_id,
			shared_pcpu ? "shared" : "exclusive",
			sched_get_scheduler_name(pcpu_id),
			busy,
			sched_get_ticks(pcpu_id),
			sched_get_context_switches(pcpu_id),
			sched_get_reschedule_requests(pcpu_id),
			shell_sched_runqueue_count(pcpu_id),
			name);
		shell_puts(temp_str);
	}

	shell_schedstat_print_cpu_usage(head, window_ticks);

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		struct sched_bvt_stats bvt;
		struct sched_rtds_stats rtds;

		if (sched_get_bvt_stats(thread, &bvt)) {
			has_bvt_stats = true;
		}
		if (sched_get_rtds_stats(thread, &rtds)) {
			has_rtds_stats = true;
		}
		if (has_bvt_stats && has_rtds_stats) {
			break;
		}
	}

	if (has_bvt_stats) {
		/*
		 * BVT stats expose virtual-time ordering. Lower avt/evt is more
		 * eligible; weight controls how quickly virtual time advances.
		 */
		shell_puts("\r\nBVT stats:\r\n\r\n");
		shell_puts("name             pcpu  state     weight  avt       evt\r\n");
		shell_puts("───────────────  ────  ────────  ──────  ────────  ────────\r\n");

		list_for_each(pos, head) {
			struct thread_object *thread = container_of(pos, struct thread_object, node);
			struct sched_bvt_stats bvt;

			if (sched_get_bvt_stats(thread, &bvt)) {
				snprintf(temp_str, MAX_STR_SIZE,
					"%-15s  %-4hu  %-8s  %-6u  %-8ld  %-8ld\r\n",
					thread->name,
					thread->pcpu_id,
					thread_state_str(thread->status),
					(uint32_t)bvt.weight,
					bvt.avt,
					bvt.evt);
				shell_puts(temp_str);
			}
		}
	}

	if (has_rtds_stats) {
		uint64_t now = cpu_ticks();

		/*
		 * RTDS stats show fixed-period budget accounting and the time
		 * left before the next scheduling deadline.
		 */
		shell_puts("\r\nRTDS stats:\r\n\r\n");
		shell_puts("name             pcpu  state     period.us  budget.us  remain.us  deadline-in.us\r\n");
		shell_puts("───────────────  ────  ────────  ─────────  ─────────  ─────────  ──────────────\r\n");

		list_for_each(pos, head) {
			struct thread_object *thread = container_of(pos, struct thread_object, node);
			struct sched_rtds_stats rtds;

			if (sched_get_rtds_stats(thread, &rtds)) {
				snprintf(temp_str, MAX_STR_SIZE,
					"%-15s  %-4hu  %-8s  %-9lu  %-9lu  %-9lu  %-11lu\r\n",
					thread->name,
					thread->pcpu_id,
					thread_state_str(thread->status),
					ticks_to_us(rtds.period_ticks),
					ticks_to_us(rtds.budget_ticks),
					ticks_to_us(rtds.remaining_ticks),
					(rtds.deadline_ticks > now) ?
						ticks_to_us(rtds.deadline_ticks - now) : 0UL);
				shell_puts(temp_str);
			}
		}
	}

	shell_schedstat_last = shell_schedstat_sample;

	return 0;
}

struct irqstat_total {
	uint64_t count;
	bool overflow;
};

struct irqstat_snapshot {
	uint64_t count[MAX_PCPU_NUM];
	struct irqstat_total total;
	bool show;
};

static void irqstat_add_count(struct irqstat_total *total, uint64_t count)
{
	if ((count == UINT64_MAX) || (total->count > (UINT64_MAX - count))) {
		total->count = UINT64_MAX;
		total->overflow = true;
	} else {
		total->count += count;
	}
}

static void irqstat_take_snapshot(uint32_t irq, uint16_t pcpu_num,
	struct irqstat_snapshot *snapshot)
{
	uint16_t pcpu_id;
	bool has_handler;

	has_handler = irq_desc_array[irq].action != NULL;
	(void)memset(snapshot, 0U, sizeof(*snapshot));

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		snapshot->count[pcpu_id] = per_cpu(irq_count, pcpu_id)[irq];
		irqstat_add_count(&snapshot->total, snapshot->count[pcpu_id]);
	}

	snapshot->show = (snapshot->total.count != 0UL) || has_handler;
}

static void shell_print_irq_cpu_headers(uint16_t pcpu_num)
{
	char temp_str[16U];
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		snprintf(temp_str, sizeof(temp_str), "cpu%-6hu", pcpu_id);
		shell_puts(temp_str);
	}
	shell_puts("\r\n");
}

static void shell_print_irq_cpu_counts(const struct irqstat_snapshot *snapshot,
	uint16_t pcpu_num)
{
	char token[32U];
	uint16_t pcpu_id;
	uint64_t count;

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		count = snapshot->count[pcpu_id];

		if (count == UINT64_MAX) {
			snprintf(token, sizeof(token), " %-8s", "sat");
		} else {
			snprintf(token, sizeof(token), " %-8lu", count);
		}
		shell_puts(token);
	}
	shell_puts("\r\n");
}

/*
 * 2026-06-30, irqstat monitor:
 *
 * irqstat is host-side interrupt accounting. It prints IRQ lines that either
 * have a registered handler or have observed counts, then keeps the per-pCPU
 * columns raw so interrupt affinity and unexpected routing stay visible.
 *
 *   irq_desc/action + per_cpu(irq_count) -> active/name + cpuN counts
 */
static int32_t shell_irqstat(int32_t argc, __unused char **argv)
{
	char temp_str[MAX_STR_SIZE];
	uint16_t pcpu_num = get_pcpu_nums();
	uint32_t irq;
	uint32_t shown = 0U;

	if (argc != 1) {
		shell_puts("usage: irqstat\r\n");
		return -EINVAL;
	}

	snprintf(temp_str, MAX_STR_SIZE,
		"\r\nirqstat: nr_irqs=%u, pcpus=%hu\r\n",
		NR_IRQS, pcpu_num);
	shell_puts(temp_str);
	shell_puts("irq   name             active ");
	shell_print_irq_cpu_headers(pcpu_num);
	shell_puts("───── ──────────────── ──────");
	for (uint16_t pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		shell_puts(" ────────");
	}
	shell_puts("\r\n");

	for (irq = 0U; irq < NR_IRQS; irq++) {
		struct irqstat_snapshot snapshot;
		bool allocated;

		irqstat_take_snapshot(irq, pcpu_num, &snapshot);
		if (!snapshot.show) {
			continue;
		}

		allocated = bitmap_test((uint16_t)(irq & 0x3FU), irq_alloc_bitmap + (irq >> 6U));
		snprintf(temp_str, MAX_STR_SIZE, "%-5u %-16s %-6s",
			irq,
			arch_irq_name(irq),
			allocated ? "Y" : "N");
		shell_puts(temp_str);
		shell_print_irq_cpu_counts(&snapshot, pcpu_num);
		shown++;
	}

	if (shown == 0U) {
		shell_puts("(no active irq handlers and no interrupt counts)\r\n");
	}

	return 0;
}

uint16_t sanitize_vmid(uint16_t vmid)
{
	uint16_t sanitized_vmid = vmid;
	char temp_str[TEMP_STR_SIZE];

	if (vmid >= CONFIG_MAX_VM_NUM) {
		snprintf(temp_str, TEMP_STR_SIZE,
			"vm id given exceeds the max_vm_num(%u), using 0 instead\r\n",
			CONFIG_MAX_VM_NUM);
		shell_puts(temp_str);
		sanitized_vmid = 0U;
	}

	return sanitized_vmid;
}

static int32_t shell_to_vm_console(int32_t argc, char **argv)
{
	char temp_str[TEMP_STR_SIZE];
	uint16_t vm_id = 0U;

	struct acrn_vm *vm;
	struct acrn_vuart *vu;

	if (argc == 2) {
		vm_id = sanitize_vmid((uint16_t)strtol_deci(argv[1]));
	}

	/* Get the virtual device node */
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

	/*
	 * Bind vsh as the single host vuart for this VM console. Guest output has
	 * already been staged in the per-VM vuart receive FIFO; binding marks that
	 * backlog pending for the periodic drain without adding a second command
	 * path or a separate log viewer.
	 */
	snprintf(temp_str, TEMP_STR_SIZE, "\r\n──────── [switch to vm-%d shell] ────────\r\n", vm_id);
	shell_puts(temp_str);
	shell_set_input_active(false);
	if (console_vmid != ACRN_INVALID_VMID) {
		console_vm_vuart_unbind(console_vmid);
	}
	console_vmid = vm_id;
	(void)console_vm_vuart_bind(vm_id);

	return 0;
}
