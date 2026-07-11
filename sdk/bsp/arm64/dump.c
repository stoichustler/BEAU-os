/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <asm/page.h>
#include <logmsg.h>
#include <dump.h>
#include <debug/symbol.h>

#define CALL_TRACE_HIERARCHY_MAX	0x10U
#define DUMP_STACK_SIZE			0x800U

void asm_assert(__unused int32_t line, __unused const char *file, __unused const char *txt)
{
	LOG_ERR("assertion failed in file %s,line %d : %s", file, line, txt);
	do {
		asm_pause();
	} while (1);
}

static void show_host_call_trace(uint64_t stack_phy, uint64_t fp_arg, uint16_t pcpu_id)
{
	uint32_t i;
	uint32_t cb_hierarchy = 0U;
	uint64_t *fp = (uint64_t *)fp_arg;
	uint64_t *sp = (uint64_t *)stack_phy;
	uint64_t dump_size = min(roundup(stack_phy, PAGE_SIZE) - stack_phy, DUMP_STACK_SIZE);

	LOG_ERR("host stack: cpu%hu\r\n", pcpu_id);
	for (i = 0U; i < (dump_size >> 5U); i++) {
		LOG_ERR("addr(0x%lx)	0x%016lx  0x%016lx  0x%016lx  0x%016lx\r\n",
			(stack_phy + (i * 32U)), sp[i * 4U],
			sp[(i * 4U) + 1U], sp[(i * 4U) + 2U],
			sp[(i * 4U) + 3U]);
	}

	LOG_ERR("\r\nhost call trace:\r\n");
	while (cb_hierarchy < CALL_TRACE_HIERARCHY_MAX) {
		uint64_t lr = *(uint64_t *)(fp + 1);
		char sym[96U];

		dbg_format_symbol(lr, sym, sizeof(sym));
		LOG_ERR(" ✘  0x%016lx  %s\r\n", lr, sym);

		if (*fp == SP_BOTTOM_MAGIC) {
			break;
		}

		fp = (uint64_t *)(*fp);
		cb_hierarchy++;
	}

	LOG_ERR("\r\n");
}

void dump_intr_excp_frame(const struct intr_excp_ctx *ctx)
{
	LOG_ERR("\r\n──────────────── [cut here] ────────────────\r\n");

	LOG_ERR("host registers:\r\n");
	LOG_ERR("[κ]  elr:0x%016lx spsr:0x%016lx esr:0x%016lx far:0x%016lx\r\n",
		ctx->regs.elr, ctx->regs.spsr, ctx->regs.esr, ctx->regs.far);
	LOG_ERR("[κ]   x0:0x%016lx   x1:0x%016lx  x2:0x%016lx  x3:0x%016lx\r\n",
		ctx->regs.x0, ctx->regs.x1, ctx->regs.x2, ctx->regs.x3);
	LOG_ERR("[κ]   x4:0x%016lx   x5:0x%016lx  x6:0x%016lx  x7:0x%016lx\r\n",
		ctx->regs.x4, ctx->regs.x5, ctx->regs.x6, ctx->regs.x7);
	LOG_ERR("[κ]   x8:0x%016lx   x9:0x%016lx x10:0x%016lx x11:0x%016lx\r\n",
		ctx->regs.x8, ctx->regs.x9, ctx->regs.x10, ctx->regs.x11);
	LOG_ERR("[κ]  x12:0x%016lx  x13:0x%016lx x14:0x%016lx x15:0x%016lx\r\n",
		ctx->regs.x12, ctx->regs.x13, ctx->regs.x14, ctx->regs.x15);
	LOG_ERR("[κ]  x16:0x%016lx  x17:0x%016lx x18:0x%016lx x19:0x%016lx\r\n",
		ctx->regs.x16, ctx->regs.x17, ctx->regs.x18, ctx->regs.x19);
	LOG_ERR("[κ]  x20:0x%016lx  x21:0x%016lx x22:0x%016lx x23:0x%016lx\r\n",
		ctx->regs.x20, ctx->regs.x21, ctx->regs.x22, ctx->regs.x23);
	LOG_ERR("[κ]  x24:0x%016lx  x25:0x%016lx x26:0x%016lx x27:0x%016lx\r\n",
		ctx->regs.x24, ctx->regs.x25, ctx->regs.x26, ctx->regs.x27);
	LOG_ERR("[κ]  x28:0x%016lx  x29:0x%016lx  lr:0x%016lx  sp:0x%016lx\r\n",
		ctx->regs.x28, ctx->regs.x29, ctx->regs.lr, ctx->regs.sp);
	LOG_ERR("\r\n");
}

void dump_exception(const struct intr_excp_ctx *ctx, uint16_t pcpu_id)
{
	dump_intr_excp_frame(ctx);
	show_host_call_trace(ctx->regs.sp, ctx->regs.x29, pcpu_id);
}
