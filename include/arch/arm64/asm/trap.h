/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_TRAP_H
#define ARM64_TRAP_H

#define ARM64_TRAP_SYNC		0
#define ARM64_TRAP_IRQ		1
#define ARM64_TRAP_FIQ		2
#define ARM64_TRAP_SERROR	3

#ifndef ASSEMBLER
#include <irq.h>

#define ESR_EL2_EC_SHIFT	26U
#define ESR_EL2_EC_MASK		0x3fUL
#define ESR_EL2_EC(esr)		(((esr) >> ESR_EL2_EC_SHIFT) & ESR_EL2_EC_MASK)
#define ESR_EL2_IL		(1UL << 25U)

#define ESR_EL2_EC_UNKNOWN	0x00UL
#define ESR_EL2_EC_WFI_WFE	0x01UL
#define ESR_EL2_EC_HVC64	0x16UL
#define ESR_EL2_EC_SMC64	0x17UL
#define ESR_EL2_EC_SYSREG	0x18UL
#define ESR_EL2_EC_SVE		0x19UL
#define ESR_EL2_EC_IABT_LOW	0x20UL
#define ESR_EL2_EC_IABT_CUR	0x21UL
#define ESR_EL2_EC_DABT_LOW	0x24UL
#define ESR_EL2_EC_DABT_CUR	0x25UL
#define ESR_EL2_EC_SERROR	0x2fUL

/*
 * For EC=0x01, ISS.TI distinguishes WFI from WFE. WFI waits for an interrupt;
 * WFE waits for an event, so the vCPU exit path can give them different
 * scheduler behavior without decoding the original instruction stream.
 */
#define ESR_WFX_TI		(1UL << 0U)
#define ESR_WFX_IS_WFE(esr)	(((esr) & ESR_WFX_TI) != 0UL)

#define ESR_DABT_ISV		(1UL << 24U)
#define ESR_DABT_SAS_SHIFT	22U
#define ESR_DABT_SAS_MASK	0x3UL
#define ESR_DABT_SSE		(1UL << 21U)
#define ESR_DABT_SRT_SHIFT	16U
#define ESR_DABT_SRT_MASK	0x1fUL
#define ESR_DABT_SF		(1UL << 15U)
#define ESR_DABT_WNR		(1UL << 6U)
#define ESR_ABORT_FSC_MASK	0x3fUL
#define ESR_ABORT_FSC_TAG_CHECK	0x11UL

extern char arm64_exception_vectors[];
extern void vcpu_exit_return(void);

void dispatch_trap(struct intr_excp_ctx *ctx, uint64_t trap_type);
void dispatch_interrupt(const struct intr_excp_ctx *ctx);
void dispatch_interrupt_no_softirq(const struct intr_excp_ctx *ctx);
void arm64_smp_call_irq_handler(__unused uint32_t irq, __unused void *data);
#endif

#endif /* ARM64_TRAP_H */
