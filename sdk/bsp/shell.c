/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bits.h>
#include "shell_priv.h"
#include "cmds/shell_cmds.h"
#include <console.h>
#include <debug/ramlog.h>
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
#include <atomic.h>
#include <serial.h>
#include <debug/symbol.h>
#include <banner.h>
#if CONFIG_ARM64_SPE
#include <asm/spe.h>
#endif

/* [20260717] BEAU shell event ownership:
 *
 *   console timer                         shell thread
 *        |                                    |
 *        +-- RX/ownership event               |
 *        +-- atomic latch ------------------->| wake + bounded BVT warp
 *                                             |
 *                                             +-- edit / dispatch / prompt
 *                                             +-- bounded drain
 *                                             +-- sleep, then recheck latch/RX
 *
 * Key rules:
 *   - the timer owns bounded selected-VM I/O; the shell thread never drains it;
 *   - only the shell thread owns BEAU input editing and command dispatch;
 *   - the latch preserves an event that arrives before the thread blocks;
 *   - the post-sleep recheck preserves an event that races with blocking;
 *   - BVT warp changes short-term ordering without changing long-term weight;
 *   - guest ownership suppresses that prompt until BEAU owns the console again.
 *
 * Asynchronous logs still serialize terminal redraw through shell_tx_lock so
 * they can borrow and restore the active input row without owning shell state.
 */

/* [20260723] Shell core and command registry boundary
 *
 * console timer -> shell event latch -> shell thread -> registry handler
 *                                      |                    |
 *                                      |                    +-- command-local state
 *                                      v
 *                              shell-owned input/history state
 *
 * Key rule:
 *   - shell.c owns dispatch, terminal redraw, and input history;
 *   - shell_registry.c owns the command descriptions and handler association;
 *   - command modules never mutate shell input state directly, except the
 *     explicit console ownership transition through shell_set_input_active().
 */

#define SHELL_PROMPT_STR	"console:\\> "
#define SHELL_ASCII_BS		'\b'
#define SHELL_ASCII_TAB		'\t'
#define SHELL_ASCII_DEL		0x7fU
#define SHELL_VT100_CLEAR_LINE	"\033[2K"
#define SHELL_BANNER_SGR_BODY_MAX_LEN	32U
#define SHELL_EVENT_IDLE		0U
#define SHELL_EVENT_LATCHED	1U
#define SHELL_RX_DRAIN_BUDGET	64U
#define SHELL_BVT_WEIGHT		64U
#define SHELL_BVT_WARP_VALUE	8
#define SHELL_BVT_WARP_LIMIT	1U
#define SHELL_BVT_UNWARP_PERIOD	4U
#define SHELL_ITEM_STR_SIZE	(MAX_STR_SIZE * 2U)
#define SHELL_SENSITIVE_MASK	'*'

char shell_log_buf[SHELL_LOG_BUF_SIZE];

static void shell_print_registered_commands(void);
static void shell_handle_tab_key(void);
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
static uint32_t shell_event_pending;
static bool shell_started;
static bool shell_prompt_enabled;
static bool shell_input_active;

static void shell_thread_main(__unused struct thread_object *obj);

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

/* [20260720] Sensitive shell argument ownership
 *
 *   serial bytes -> bounded input buffer -> masked terminal rendering
 *                         |
 *                         +--> sensitive command: no history publication
 *                         |
 *                         `--> private argv copy -> command -> explicit clear
 *
 * Key rule:
 *   - only the shell thread owns plaintext while dispatching one command;
 *   - terminal redraw, cursor editing, and asynchronous logs use a masked copy;
 *   - sensitive input is never published into the command-history ring.
 */
static bool shell_line_sensitive(const char *line, uint32_t line_len,
	uint32_t *mask_start)
{
	uint32_t cmd_start = 0U;
	uint32_t index;

	if (line == NULL) {
		return false;
	}
	while ((cmd_start < line_len) && (line[cmd_start] == ' ')) {
		cmd_start++;
	}

	for (index = 0U; index < shell_cmd_total(); index++) {
		const struct shell_cmd *cmd = shell_cmd_at(index);
		uint32_t cmd_len;
		uint32_t delimiter;

		if ((cmd->flags & SHELL_CMD_FLAG_SENSITIVE_ARGS) == 0U) {
			continue;
		}
		cmd_len = (uint32_t)strnlen_s(cmd->str, SHELL_CMD_MAX_LEN);
		delimiter = cmd_start + cmd_len;
		if ((delimiter < line_len) &&
			((line[delimiter] == ' ') || (line[delimiter] == ',')) &&
			(strncmp(&line[cmd_start], cmd->str, cmd_len) == 0)) {
			if (mask_start != NULL) {
				*mask_start = delimiter + 1U;
			}
			return true;
		}
	}

	return false;
}

static bool shell_current_input_sensitive(void)
{
	const char *line = p_shell->buffered_line[p_shell->input_line_active];
	uint32_t mask_start;

	if (!p_shell->input_sensitive &&
		shell_line_sensitive(line, p_shell->input_line_len, &mask_start)) {
		p_shell->input_sensitive = true;
		p_shell->sensitive_mask_start = mask_start;
	}

	return p_shell->input_sensitive;
}

static void shell_mask_input_line(const char *line, uint32_t line_len,
	char masked[SHELL_CMD_MAX_LEN + 1U])
{
	uint32_t mask_start = line_len;
	bool sensitive = p_shell->input_sensitive;
	uint32_t index;

	if (sensitive) {
		mask_start = min(p_shell->sensitive_mask_start, line_len);
	} else {
		sensitive = shell_line_sensitive(line, line_len, &mask_start);
	}

	for (index = 0U; index < line_len; index++) {
		bool delimiter = (line[index] == ' ') || (line[index] == ',');

		masked[index] = (sensitive && (index >= mask_start) && !delimiter) ?
			SHELL_SENSITIVE_MASK : line[index];
	}
	masked[line_len] = '\0';
}

static char shell_getc(void)
{
	return console_getc();
}

static void shell_puts_unlocked(const char *string_ptr)
{
	(void)console_write(string_ptr, strnlen_s(string_ptr,
				SHELL_STRING_MAX_LEN));
}

/* [20260718] Banner SGR decoding boundary
 *
 * sdk/BANNER printable text
 *          |
 *          v
 * generated beau_banner[]
 *          |
 *          +-- U+241B "control picture" or "\\033" marker
 *          |          |
 *          |          +-- bounded [digits;...]m --> emit ESC + SGR
 *          |          +-- malformed -----------> emit printable source
 *          |
 *          +-- raw ESC ------------------------> emit printable "\\033"
 *
 * Key rule:
 *   - sdk/BANNER owns presentation text; the shell owns terminal control bytes;
 *   - only a bounded SGR sequence may cross the parser as a real ESC sequence;
 *   - malformed or non-SGR input cannot inject cursor, erase, or query commands.
 */
static size_t shell_banner_marker_len(const char *text, size_t remaining)
{
	size_t marker_len = 0U;

	if ((remaining >= 3U) && ((uint8_t)text[0] == 0xe2U) &&
		((uint8_t)text[1] == 0x90U) && ((uint8_t)text[2] == 0x9bU)) {
		marker_len = 3U;
	} else if ((remaining >= 4U) && (text[0] == '\\') &&
		(text[1] == '0') && (text[2] == '3') && (text[3] == '3')) {
		marker_len = 4U;
	}

	return marker_len;
}

static size_t shell_banner_sgr_len(const char *text, size_t remaining,
	size_t marker_len)
{
	size_t idx = marker_len;
	size_t scan_len = 0U;
	size_t limit = 0U;
	size_t sgr_len = 0U;
	bool have_digit = false;
	bool after_separator = false;

	if ((marker_len == 0U) || (remaining <= marker_len) ||
		(text[marker_len] != '[')) {
		return 0U;
	}

	idx++;
	scan_len = remaining - idx;
	if (scan_len > SHELL_BANNER_SGR_BODY_MAX_LEN) {
		scan_len = SHELL_BANNER_SGR_BODY_MAX_LEN;
	}
	limit = idx + scan_len;
	while (idx < limit) {
		char ch = text[idx];

		if ((ch >= '0') && (ch <= '9')) {
			have_digit = true;
			after_separator = false;
		} else if ((ch == ';') && have_digit && !after_separator) {
			after_separator = true;
		} else if ((ch == 'm') && have_digit && !after_separator) {
			sgr_len = idx + 1U;
			break;
		} else {
			break;
		}
		idx++;
	}

	return sgr_len;
}

static void shell_write_banner_unlocked(void)
{
	const size_t banner_len = sizeof(beau_banner) - 1U;
	size_t text_start = 0U;
	size_t offset = 0U;

	while (offset < banner_len) {
		size_t remaining = banner_len - offset;
		size_t marker_len = shell_banner_marker_len(&beau_banner[offset], remaining);
		size_t sgr_len = shell_banner_sgr_len(&beau_banner[offset], remaining,
			marker_len);

		if (sgr_len != 0U) {
			if (offset > text_start) {
				(void)console_write(&beau_banner[text_start], offset - text_start);
			}
			(void)console_write("\033", 1U);
			(void)console_write(&beau_banner[offset + marker_len],
				sgr_len - marker_len);
			offset += sgr_len;
			text_start = offset;
		} else if ((uint8_t)beau_banner[offset] == 0x1bU) {
			if (offset > text_start) {
				(void)console_write(&beau_banner[text_start], offset - text_start);
			}
			shell_puts_unlocked("\\033");
			offset++;
			text_start = offset;
		} else {
			offset++;
		}
	}

	if (text_start < banner_len) {
		(void)console_write(&beau_banner[text_start], banner_len - text_start);
	}
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
	shell_write_banner_unlocked();
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

void shell_set_input_active(bool active)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_input_active = active;
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_item_vprint(const char *prefix, const char *fmt, va_list args)
{
	char body[SHELL_ITEM_STR_SIZE];
	/* Leave room for the UTF-8 item prefix, CRLF and string terminator. */
	char line[SHELL_ITEM_STR_SIZE + 16U];

	(void)vsnprintf(body, sizeof(body), fmt, args);
	(void)snprintf(line, sizeof(line), "%s%s\r\n", prefix, body);
	shell_puts(line);
	shell_output_checkpoint();
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
	shell_output_checkpoint();
}

/* [20260717] Cooperative shell output scheduling
 *
 * long command -> print one complete row -> release output/subsystem locks
 *                                           |
 *                                           v
 *                                 honor NEED_RESCHEDULE
 *                                           |
 *                                           v
 *                                  resume the same command
 *
 * Key rule:
 *   - only the BSP shell thread may use this checkpoint;
 *   - callers place it where no subsystem or output lock is held;
 *   - bounded runtime charging prevents one long dump from leaving the shell
 *     far ahead of its BVT peers after the command completes.
 */
void shell_output_checkpoint(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	if ((pcpu_id == BSP_CPU_ID) &&
		(sched_get_current(pcpu_id) == &shell_thread) &&
		need_reschedule(pcpu_id)) {
		schedule();
	}
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
	char masked[SHELL_CMD_MAX_LEN + 1U];
	const char *line = p_shell->buffered_line[p_shell->input_line_active];

	shell_puts_unlocked(SHELL_PROMPT_STR);
	if (p_shell->input_line_len > 0U) {
		shell_mask_input_line(line, p_shell->input_line_len, masked);
		shell_puts_unlocked(masked);
		set_cursor_pos_unlocked(p_shell->input_line_len - p_shell->cursor_offset);
		(void)memset(masked, 0U, sizeof(masked));
	}
}

static void shell_restore_input_line(void)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_restore_input_line_unlocked();
	spinlock_irqrestore_release(&shell_tx_lock, rflags);
}

static void shell_redraw_input_line(void)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&shell_tx_lock, &rflags);
	shell_clear_current_line_unlocked();
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
		char *line = p_shell->buffered_line[p_shell->input_line_active];
		bool sensitive = shell_current_input_sensitive();
		uint32_t delta = p_shell->input_line_len - p_shell->cursor_offset - 1;

		if (sensitive) {
			if ((p_shell->cursor_offset < p_shell->sensitive_mask_start) &&
				(p_shell->sensitive_mask_start > 0U)) {
				p_shell->sensitive_mask_start--;
			}
			memcpy(line + p_shell->cursor_offset,
				line + p_shell->cursor_offset + 1U, delta);
			line[p_shell->input_line_len - 1U] = '\0';
			p_shell->input_line_len--;
			shell_redraw_input_line();
			return;
		}

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

	if (shell_current_input_sensitive()) {
		return;
	}

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
				p_shell->cursor_offset++;
				if (shell_current_input_sensitive()) {
					shell_redraw_input_line();
				} else {
					shell_puts(p_shell->buffered_line[p_shell->input_line_active] +
						p_shell->cursor_offset - 1U);
					set_cursor_pos(p_shell->input_line_len - p_shell->cursor_offset);
				}
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
				uint32_t old_offset = p_shell->cursor_offset;

				if (shell_current_input_sensitive()) {
					p_shell->cursor_offset = p_shell->input_line_len;
					shell_redraw_input_line();
				} else {
					shell_puts(p_shell->buffered_line[p_shell->input_line_active] +
						old_offset);
					p_shell->cursor_offset = p_shell->input_line_len;
				}
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
		char *line = p_shell->buffered_line[p_shell->input_line_active];
		bool sensitive = shell_current_input_sensitive();

		if (sensitive) {
			uint32_t delta = p_shell->input_line_len - p_shell->cursor_offset;
			uint32_t removed = p_shell->cursor_offset - 1U;

			if ((removed < p_shell->sensitive_mask_start) &&
				(p_shell->sensitive_mask_start > 0U)) {
				p_shell->sensitive_mask_start--;
			}

			if (delta > 0U) {
				memcpy(line + p_shell->cursor_offset - 1U,
					line + p_shell->cursor_offset, delta);
			}
			line[p_shell->input_line_len - 1U] = '\0';
			p_shell->input_line_len--;
			p_shell->cursor_offset--;
			shell_redraw_input_line();
			return;
		}

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
	char *line = p_shell->buffered_line[p_shell->input_line_active];
	uint32_t delta = p_shell->input_line_len - p_shell->cursor_offset;
	bool sensitive = shell_current_input_sensitive();

	if (sensitive &&
		(p_shell->cursor_offset < p_shell->sensitive_mask_start)) {
		p_shell->sensitive_mask_start++;
	}

	/* move the input from cursor offset back first */
	if (delta > 0) {
		memcpy_backwards(line + p_shell->input_line_len,
			line + p_shell->input_line_len - 1U, delta);
	}

	line[p_shell->cursor_offset] = ch;

	/* Move to next character in string */
	p_shell->input_line_len++;
	p_shell->cursor_offset++;
	line[p_shell->input_line_len] = '\0';

	if (shell_current_input_sensitive()) {
		shell_redraw_input_line();
	} else {
		/* Echo back the input */
		shell_puts(line + p_shell->cursor_offset - 1U);
		set_cursor_pos(delta);
	}
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

			done = true;

			p_shell->input_line_len = 0U;
			p_shell->cursor_offset = 0U;
		}
		break;
	}


	return done;
}

static void shell_put_error(const char *msg)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE, "%s[π] BEAU: %s%s\r\n",
		SHELL_COLOR_RED, msg, SHELL_COLOR_RESET);
	shell_puts(temp_str);
}

static int32_t shell_process_cmd(char *p_input_line)
{
	int32_t status = -EINVAL;
	struct shell_cmd *p_cmd;
	char cmd_argv_str[SHELL_CMD_MAX_LEN + 1U];
	int32_t cmd_argv_mem[sizeof(char *) * ((SHELL_CMD_MAX_LEN + 1U) >> 1U)];
	int32_t cmd_argc;
	char **cmd_argv;

	/* Dispatch mutates only this private argv copy, which is cleared on exit. */
	(void)strncpy_s(&cmd_argv_str[0], SHELL_CMD_MAX_LEN + 1U, p_input_line, SHELL_CMD_MAX_LEN);
	cmd_argv_str[SHELL_CMD_MAX_LEN] = 0;

	(void) string_to_argv(&cmd_argv_str[0],
			(void *) &cmd_argv_mem[0],
			sizeof(cmd_argv_mem), (void *)&cmd_argc, &cmd_argv);

	/* Determine if there is a command to process. */
	if (cmd_argc != 0) {
		p_cmd = shell_find_cmd(cmd_argv[0]);
		if (p_cmd == NULL) {
			shell_put_error("invalid command.");
			goto out;
		}
		if ((p_cmd->flags & SHELL_CMD_FLAG_SENSITIVE_ARGS) != 0U) {
			(void)memset(p_input_line, 0U, SHELL_CMD_MAX_LEN + 1U);
		}

		status = p_cmd->fcn(cmd_argc, &cmd_argv[0]);
		if (status == -EINVAL) {
			shell_put_error("invalid parameters.");
		} else if (status != 0) {
			shell_put_error("command launch failed.");
		}
	}

out:
	(void)memset(cmd_argv_str, 0U, sizeof(cmd_argv_str));
	(void)memset(cmd_argv_mem, 0U, sizeof(cmd_argv_mem));
	return status;
}

static int32_t shell_process(void)
{
	int32_t status, former_index;
	char *p_input_line;
	uint32_t input_len;
	bool sensitive;

	p_input_line = p_shell->buffered_line[p_shell->input_line_active];
	input_len = (uint32_t)strnlen_s(p_input_line, SHELL_CMD_MAX_LEN);
	sensitive = shell_current_input_sensitive() ||
		shell_line_sensitive(p_input_line, input_len, NULL);

	former_index = (p_shell->input_line_active + MAX_BUFFERED_CMDS - 1) % MAX_BUFFERED_CMDS;

	if (!sensitive && (input_len > 0U) &&
		(strcmp(p_input_line, p_shell->buffered_line[former_index]) != 0)) {
		p_shell->input_line_active = (p_shell->input_line_active + 1) % MAX_BUFFERED_CMDS;
	}

	p_shell->to_select_index = p_shell->input_line_active;

	status = shell_process_cmd(p_input_line);
	if (sensitive) {
		(void)memset(p_input_line, 0U, SHELL_CMD_MAX_LEN + 1U);
	}
	p_shell->input_sensitive = false;
	p_shell->sensitive_mask_start = 0U;

	(void)memset(p_shell->buffered_line[p_shell->input_line_active], 0, SHELL_CMD_MAX_LEN + 1U);

	return status;
}

static void shell_thread_kick(void)
{
	if (!console_is_hv()) {
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
		}
		return;
	}

	/* A guest-console command leaves the BEAU input row inactive. Restore it
	 * when Ctrl-D has returned ownership to the hypervisor.
	 */
	if (console_is_hv() && !shell_input_active) {
		shell_show_prompt(false);
	}

	if (shell_input_line()) {
		(void)shell_process();
		if (console_is_hv()) {
			shell_show_prompt(false);
		}
	}
}

void shell_kick(void)
{
	(void)atomic_cmpxchg32(&shell_event_pending,
		SHELL_EVENT_IDLE, SHELL_EVENT_LATCHED);
	if (shell_started) {
		wake_thread(&shell_thread);
	}
}

static bool shell_event_is_pending(void)
{
	return atomic_cmpxchg32(&shell_event_pending,
		SHELL_EVENT_IDLE, SHELL_EVENT_IDLE) == SHELL_EVENT_LATCHED;
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
		/* [20260708] shell/log interleave principle:
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

static void shell_thread_main(struct thread_object *obj)
{
	while (true) {
		uint32_t budget = SHELL_RX_DRAIN_BUDGET;

		(void)atomic_readandclear32(&shell_event_pending);
		do {
			shell_thread_kick();
			budget--;
		} while ((budget > 0U) && console_is_hv() && serial_rx_ready());

		sleep_thread(obj);
		if (shell_event_is_pending() ||
			(console_is_hv() && serial_rx_ready())) {
			wake_thread(obj);
		}
		schedule();
	}
}

void shell_init(void)
{
	shell_event_pending = SHELL_EVENT_IDLE;
	p_shell->cmds = shell_common_cmds;
	p_shell->cmd_count = shell_common_cmds_sz;

	p_shell->arch_cmds = shell_arch_cmds;
	p_shell->arch_cmd_count = shell_arch_cmds_sz;

	p_shell->to_select_index = 0;
	p_shell->input_sensitive = false;
	p_shell->sensitive_mask_start = 0U;

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
	shell_params.bvt_weight = SHELL_BVT_WEIGHT;
	shell_params.bvt_warp_value = SHELL_BVT_WARP_VALUE;
	shell_params.bvt_warp_limit = SHELL_BVT_WARP_LIMIT;
	shell_params.bvt_unwarp_period = SHELL_BVT_UNWARP_PERIOD;
	init_thread_data(&shell_thread, &shell_params);
	shell_started = true;
	if (shell_event_is_pending() ||
		(console_is_hv() && serial_rx_ready())) {
		wake_thread(&shell_thread);
	}
}

#define MAX_OUTPUT_LEN  80
static void shell_print_registered_commands(void)
{
	struct shell_cmd *p_cmd = NULL;

	char str[MAX_STR_SIZE];
	uint32_t cmd_cnt = shell_cmd_total();
	/* Print title */
	shell_puts("\r\n\r\n");

	/* Proceed based on the number of registered commands. */
	if (cmd_cnt == 0U) {
		/* No registered commands */
		shell_puts("NONE\r\n");
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
			shell_output_checkpoint();
		}
	}

	shell_puts("\r\n");
}
