/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <cpu.h>
#include <logmsg.h>
#include <notify.h>
#include <host_pm.h>
#include <debug/dump.h>

#include <asm/trap.h>

/* [20260710] EL2 trap routing principle:
 *
 * This file owns host-origin EL2 traps and the physical IRQ handoff into the
 * common IRQ core. Guest-origin exits use the vector path in vcpu_exit.c so
 * they can synchronize vCPU, vGIC, and vtimer state before returning to EL1.
 *
 *   exception taken at EL2
 *          |
 *          v
 *   vector origin
 *      |
 *      +-- host EL2 context  -> dispatch_trap()
 *      |                         - IRQ through GIC domain
 *      |                         - unexpected sync/FIQ/SError dump + reset
 *      |
 *      +-- guest EL1 context -> vcpu_exit.c
 *                                - save guest frame
 *                                - emulate or inject
 *                                - restore/resume guest frame
 *
 * Physical GIC INTIDs are hardware-local. They must be translated into ACRN
 * IRQ numbers before common dispatch, and EOI'd only after the selected handler
 * has consumed the interrupt.
 */
void arm64_smp_call_irq_handler(__unused uint32_t irq, __unused void *data)
{
	handle_smp_call();
}

static void unexpected_trap_handler(const struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	char *exception = NULL;

	switch (trap_type) {
	case ARM64_TRAP_IRQ:
		exception = "IRQ";
		break;
	case ARM64_TRAP_SYNC:
		exception = "SYNC";
		break;
	case ARM64_TRAP_FIQ:
		exception = "FIQ";
		break;
	case ARM64_TRAP_SERROR:
		exception = "SABT";
		break;
	default:
		exception = "N/A";
		break;
	}

	LOG_FTL("unexpected <%-4s> esr:0x%lx elr:0x%lx far:0x%lx\r\n",
		exception, ctx->regs.esr, ctx->regs.elr, ctx->regs.far);
	LOG_FTL("──────────────── [end here] ────────────────\r\n");
	LOG_FTL("method: auto reboot\r\n");
	reset_host(false);
}

static bool dispatch_wfi_wfe_trap(const struct intr_excp_ctx *ctx)
{
	struct cpu_regs *regs = (struct cpu_regs *)&ctx->regs;
	bool handled = false;

	/*
	 * HCR_EL2.TWI/TWE can also trap WFI/WFE executed while the host is at EL1
	 * in future configurations. WFI/WFE has no architectural side effect other
	 * than waiting, so the safe EL2 fallback is to skip the instruction and
	 * return. Guest WFI/WFE exits are handled in vcpu_exit.c, where vGIC/timer
	 * synchronization is available.
	 */
	if (ESR_EL2_EC(regs->esr) == ESR_EL2_EC_WFI_WFE) {
		regs->elr += ((regs->esr & ESR_EL2_IL) != 0UL) ? 4UL : 2UL;
		handled = true;
	}

	return handled;
}

static void dispatch_exception(const struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	uint16_t pcpu_id = get_pcpu_id();

	if ((trap_type == ARM64_TRAP_SYNC) && dispatch_wfi_wfe_trap(ctx)) {
		return;
	}

	dump_exception(ctx, pcpu_id);
	unexpected_trap_handler(ctx, trap_type);
}

static void dispatch_interrupt_common(const struct intr_excp_ctx *ctx, bool handle_softirq)
{
	uint32_t intid = arm64_gicv3_ack_irq();
	uint32_t acrn_irq;
	const char *domain = ARM64_IRQD_GIC;
	uint32_t src_id = intid;

	/*
	 * GIC INTIDs are hardware-local. Convert them to ACRN IRQ numbers before
	 * invoking generic handlers, then EOI the physical interrupt after the
	 * handler has consumed it.
	 */
	if (intid == ARM64_GIC_SPURIOUS_INTID) {
		acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, intid);
		if (arm64_is_valid_acrn_irq(acrn_irq)) {
			count_irq(acrn_irq);
		}
		return;
	}

	/*
	 * GICv3 LPIs are architecturally separate from SGI/PPI/SPI INTIDs. Keep
	 * common IRQ ids dense by subtracting the LPI base before entering the
	 * LPI domain.
	 */
	if (intid >= ARM64_GIC_FIRST_LPI) {
		domain = ARM64_IRQD_GIC_LPI;
		src_id = intid - ARM64_GIC_FIRST_LPI;
	}

	acrn_irq = arm64_domain_get_acrn_irq(domain, src_id);
	if (arm64_is_valid_acrn_irq(acrn_irq)) {
		if (handle_softirq) {
			do_irq(acrn_irq);
		} else {
			do_irq_no_softirq(acrn_irq);
		}
	} else {
		unexpected_trap_handler(ctx, ARM64_TRAP_IRQ);
	}

	arm64_gicv3_eoi_irq(intid);
}

void dispatch_interrupt(const struct intr_excp_ctx *ctx)
{
	dispatch_interrupt_common(ctx, true);
}

void dispatch_interrupt_no_softirq(const struct intr_excp_ctx *ctx)
{
	dispatch_interrupt_common(ctx, false);
}

void dispatch_trap(const struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	switch (trap_type) {
	case ARM64_TRAP_IRQ:
		dispatch_interrupt(ctx);
		break;
	case ARM64_TRAP_SYNC:
	case ARM64_TRAP_FIQ:
	case ARM64_TRAP_SERROR:
	default:
		dispatch_exception(ctx, trap_type);
		break;
	}
}
