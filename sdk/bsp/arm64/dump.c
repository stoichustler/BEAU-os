/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <logmsg.h>
#include <dump.h>
#include <asm/coredump.h>

void asm_assert(__unused int32_t line, __unused const char *file, __unused const char *txt)
{
	LOG_ERR("assertion failed in file %s,line %d : %s", file, line, txt);
	do {
		asm_pause();
	} while (1);
}

void dump_intr_excp_frame(const struct intr_excp_ctx *ctx)
{
	LOG_ERR("──────────────── [cut here] ────────────────");
	LOG_ERR("host registers:");
	LOG_ERR("elr:0x%016lx spsr:0x%016lx esr:0x%016lx far:0x%016lx",
		ctx->regs.elr, ctx->regs.spsr, ctx->regs.esr, ctx->regs.far);
	LOG_ERR("x0:0x%016lx x1:0x%016lx x2:0x%016lx x3:0x%016lx",
		ctx->regs.x0, ctx->regs.x1, ctx->regs.x2, ctx->regs.x3);
	LOG_ERR("x4:0x%016lx x5:0x%016lx x6:0x%016lx x7:0x%016lx",
		ctx->regs.x4, ctx->regs.x5, ctx->regs.x6, ctx->regs.x7);
	LOG_ERR("x8:0x%016lx x9:0x%016lx x10:0x%016lx x11:0x%016lx",
		ctx->regs.x8, ctx->regs.x9, ctx->regs.x10, ctx->regs.x11);
	LOG_ERR("x12:0x%016lx x13:0x%016lx x14:0x%016lx x15:0x%016lx",
		ctx->regs.x12, ctx->regs.x13, ctx->regs.x14, ctx->regs.x15);
	LOG_ERR("x16:0x%016lx x17:0x%016lx x18:0x%016lx x19:0x%016lx",
		ctx->regs.x16, ctx->regs.x17, ctx->regs.x18, ctx->regs.x19);
	LOG_ERR("x20:0x%016lx x21:0x%016lx x22:0x%016lx x23:0x%016lx",
		ctx->regs.x20, ctx->regs.x21, ctx->regs.x22, ctx->regs.x23);
	LOG_ERR("x24:0x%016lx x25:0x%016lx x26:0x%016lx x27:0x%016lx",
		ctx->regs.x24, ctx->regs.x25, ctx->regs.x26, ctx->regs.x27);
	LOG_ERR("x28:0x%016lx x29:0x%016lx lr:0x%016lx sp:0x%016lx",
		ctx->regs.x28, ctx->regs.x29, ctx->regs.lr, ctx->regs.sp);
}

void dump_exception(const struct intr_excp_ctx *ctx, uint16_t pcpu_id)
{
	struct arm64_coredump_context context;

	dump_intr_excp_frame(ctx);
	context.pc = ctx->regs.elr;
	context.lr = ctx->regs.lr;
	context.sp = ctx->regs.sp;
	context.fp = ctx->regs.x29;
	arm64_coredump_log(&context, pcpu_id, LOG_ERROR);
}
