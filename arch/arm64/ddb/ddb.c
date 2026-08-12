/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <atomic.h>
#include <cpu.h>
#include <errno.h>
#include <hv_pm.h>
#include <logmsg.h>
#include <rtl.h>
#include <serial.h>
#include <util.h>
#include <debug/symbol.h>
#include <asm/ddb.h>
#include <asm/trap.h>
#include "ddb_internal.h"

#define ESR_EL2_EC_BRK64	0x3cUL
#define ESR_EL2_BRK_IMM_MASK	0xffffUL

typedef int32_t (*ddb_command_fn_t)(struct ddb_session *session,
	uint32_t argc, char **argv);

struct ddb_command {
	const char *name;
	ddb_command_fn_t handler;
};

enum ddb_entry_reason {
	DDB_ENTRY_NONE = 0U,
	DDB_ENTRY_BREAK,
	DDB_ENTRY_PANIC,
};

static volatile uint64_t ddb_owner;

static int32_t ddb_cmd_help(struct ddb_session *session, uint32_t argc,
	char **argv);
static int32_t ddb_cmd_regs(struct ddb_session *session, uint32_t argc,
	char **argv);
static int32_t ddb_cmd_symbol(struct ddb_session *session, uint32_t argc,
	char **argv);
static int32_t ddb_cmd_continue(struct ddb_session *session, uint32_t argc,
	char **argv);
static int32_t ddb_cmd_reboot(struct ddb_session *session, uint32_t argc,
	char **argv);

static const struct ddb_command ddb_commands[] = {
	{ "help", ddb_cmd_help },
	{ "regs", ddb_cmd_regs },
	{ "bt", ddb_cmd_backtrace },
	{ "xd", ddb_cmd_examine },
	{ "symbol", ddb_cmd_symbol },
	{ "cpu", ddb_cmd_cpu },
	{ "continue", ddb_cmd_continue },
	{ "c", ddb_cmd_continue },
	{ "reboot", ddb_cmd_reboot },
};

static uint64_t ddb_owner_token(uint16_t pcpu_id)
{
	return (pcpu_id < MAX_PCPU_NUM) ? (uint64_t)pcpu_id + 1UL : 0UL;
}

static enum ddb_entry_reason ddb_get_entry_reason(
	const struct intr_excp_ctx *ctx)
{
	enum ddb_entry_reason reason = DDB_ENTRY_NONE;

	if ((ESR_EL2_EC(ctx->regs.esr) == ESR_EL2_EC_BRK64) &&
		((ctx->regs.esr & ESR_EL2_IL) != 0UL)) {
		switch (ctx->regs.esr & ESR_EL2_BRK_IMM_MASK) {
		case ARM64_DDB_BRK_IMM:
			reason = DDB_ENTRY_BREAK;
			break;
		case ARM64_DDB_PANIC_BRK_IMM:
			reason = DDB_ENTRY_PANIC;
			break;
		default:
			reason = DDB_ENTRY_NONE;
			break;
		}
	}

	return reason;
}

static const char *ddb_entry_reason_name(enum ddb_entry_reason reason)
{
	const char *name;

	switch (reason) {
	case DDB_ENTRY_BREAK:
		name = "break";
		break;
	case DDB_ENTRY_PANIC:
		name = "panic";
		break;
	case DDB_ENTRY_NONE:
	default:
		name = "unknown";
		break;
	}

	return name;
}

static int32_t ddb_cmd_help(__unused struct ddb_session *session,
	uint32_t argc, __unused char **argv)
{
	if (argc != 1U) {
		ddb_puts("usage: help\n");
		return -EINVAL;
	}
	ddb_puts("help                 list commands\n");
	ddb_puts("regs                 print saved Host registers\n");
	ddb_puts("bt                   unwind the current EL2 stack\n");
	ddb_puts("xd <addr> [count]    read 1-256 bytes of Normal memory\n");
	ddb_puts("symbol <addr>        resolve a Host text address\n");
	ddb_puts("cpu                  sample active pCPUs\n");
	ddb_puts("continue | c         resume after the DDB breakpoint\n");
	ddb_puts("reboot               cold-reset the Host\n");
	return 0;
}

static int32_t ddb_cmd_regs(struct ddb_session *session, uint32_t argc,
	__unused char **argv)
{
	const uint64_t *gpr = &session->ctx->regs.x0;
	uint32_t index;

	if (argc != 1U) {
		ddb_puts("usage: regs\n");
		return -EINVAL;
	}
	for (index = 0U; index < 30U; index += 2U) {
		ddb_printf("x%02u:0x%016lx  x%02u:0x%016lx\n",
			index, gpr[index], index + 1U, gpr[index + 1U]);
	}
	ddb_printf("x30:0x%016lx   sp:0x%016lx\n",
		session->ctx->regs.lr, session->ctx->regs.sp);
	ddb_printf("elr:0x%016lx spsr:0x%016lx\n",
		session->ctx->regs.elr, session->ctx->regs.spsr);
	ddb_printf("esr:0x%016lx  far:0x%016lx hpfar:0x%016lx\n",
		session->ctx->regs.esr, session->ctx->regs.far,
		session->ctx->regs.hpfar);
	return 0;
}

static int32_t ddb_cmd_symbol(__unused struct ddb_session *session,
	uint32_t argc, char **argv)
{
	char symbol[96U];
	uint64_t address;

	if ((argc != 2U) || !ddb_parse_u64(argv[1], 16U, &address)) {
		ddb_puts("usage: symbol <hex-address>\n");
		return -EINVAL;
	}
	dbg_format_symbol(address, symbol, sizeof(symbol));
	ddb_printf("0x%016lx %s\n", address, symbol);
	return 0;
}

static int32_t ddb_cmd_continue(struct ddb_session *session, uint32_t argc,
	__unused char **argv)
{
	if (argc != 1U) {
		ddb_puts("usage: continue\n");
		return -EINVAL;
	}
	session->done = true;
	return 0;
}

static int32_t ddb_cmd_reboot(struct ddb_session *session, uint32_t argc,
	__unused char **argv)
{
	if (argc != 1U) {
		ddb_puts("usage: reboot\n");
		return -EINVAL;
	}
	ddb_puts("ddb: rebooting\n");
	reset_host(false);
	session->done = true;
	return 0;
}

static void ddb_run_session(struct ddb_session *session,
	enum ddb_entry_reason reason)
{
	char line[DDB_LINE_SIZE];
	char *argv[DDB_MAX_ARGS];

	ddb_printf("\nBEAU DDB cpu%hu reason:%s elr:0x%016lx esr:0x%016lx\n",
		session->pcpu_id, ddb_entry_reason_name(reason),
		session->ctx->regs.elr, session->ctx->regs.esr);
	ddb_puts("type 'help' for commands\n");
	while (!session->done) {
		int32_t read_status;
		uint32_t argc;
		uint32_t index;
		bool found = false;

		ddb_printf("ddb[cpu%hu]> ", session->pcpu_id);
		read_status = ddb_read_line(line, sizeof(line));
		if (read_status == -ETIMEDOUT) {
			ddb_puts("ddb: idle timeout\n");
			session->done = true;
			continue;
		}
		if (read_status < 0) {
			continue;
		}
		argc = ddb_split_line(line, argv, ARRAY_SIZE(argv));
		if (argc == 0U) {
			continue;
		}
		if (argc > ARRAY_SIZE(argv)) {
			ddb_puts("ddb: too many arguments\n");
			continue;
		}
		for (index = 0U; index < ARRAY_SIZE(ddb_commands); index++) {
			if (strcmp(argv[0], ddb_commands[index].name) == 0) {
				(void)ddb_commands[index].handler(session, argc, argv);
				found = true;
				break;
			}
		}
		if (!found) {
			ddb_printf("ddb: unknown command '%s'\n", argv[0]);
		}
		(void)memset(line, 0U, sizeof(line));
		(void)memset(argv, 0U, sizeof(argv));
	}
}

/* [20260720] FreeBSD-derived Host DDB exception ownership
 *
 *   manual/panic Host BRK -> atomic owner -> raw UART -> command loop
 *          |                                |
 *          |                                `--> continue -> release -> ERET
 *          |
 *          +--> busy/nested -> skip only the reserved BRK
 *          `--> all other traps -> existing coredump/reset path
 *
 * Design origin:
 *   - derived from the FreeBSD ARM64 KDB/DDB trap entry in
 *     sys/arm64/arm64/trap.c and breakpoint skip contract in
 *     sys/arm64/include/db_machdep.h;
 *   - reimplemented for BEAU Host EL2 ownership and Guest isolation.
 *
 * Key rule:
 *   - Guest exceptions never call this interface;
 *   - only the reserved manual and panic immediates create a session;
 *   - an unexpected DDB-time fault retains raw UART ownership so fatal logging
 *     cannot deadlock on a lock held below the exception frame.
 */
bool arm64_ddb_handle_trap(struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t token = ddb_owner_token(pcpu_id);
	uint64_t current_owner;
	uint64_t irq_flags;
	enum ddb_entry_reason reason;
	struct ddb_session session;

	if ((ctx == NULL) || (trap_type != ARM64_TRAP_SYNC) || (token == 0UL)) {
		return false;
	}
	if (ddb_mem_handle_fault(ctx)) {
		return true;
	}

	reason = ddb_get_entry_reason(ctx);
	current_owner = __atomic_load_n(&ddb_owner, __ATOMIC_ACQUIRE);
	if (reason == DDB_ENTRY_NONE) {
		if (current_owner == token) {
			ddb_mem_cancel(pcpu_id);
			__atomic_store_n(&ddb_owner, 0UL, __ATOMIC_RELEASE);
		}
		return false;
	}
	ctx->regs.elr += 4UL;
	if (current_owner == token) {
		ddb_printf("ddb: nested %s breakpoint skipped\n",
			ddb_entry_reason_name(reason));
		return true;
	}
	if (atomic_cmpxchg64(&ddb_owner, 0UL, token) != 0UL) {
		return true;
	}
	if (!serial_debug_claim()) {
		__atomic_store_n(&ddb_owner, 0UL, __ATOMIC_RELEASE);
		LOG_ERR("DDB cpu%hu cannot claim the debug UART", pcpu_id);
		return true;
	}

	local_irq_save(&irq_flags);
	session.ctx = ctx;
	session.pcpu_id = pcpu_id;
	session.done = false;
	ddb_run_session(&session, reason);
	ddb_mem_cancel(pcpu_id);
	serial_debug_release();
	__atomic_store_n(&ddb_owner, 0UL, __ATOMIC_RELEASE);
	local_irq_restore(irq_flags);
	return true;
}
