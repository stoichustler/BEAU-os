/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <schedule.h>
#include <cpu.h>
#include <logmsg.h>
#include <asm/irq.h>
#include <asm/trap.h>

/* [20260630] SMP kick principle:
 *
 * Common scheduler and vCPU code express remote work as state bits: pending
 * vCPU requests, NEED_RESCHEDULE, or other per-pCPU flags. ARM64's job here is
 * only to make the target pCPU leave idle or guest execution so it can observe
 * those bits at a safe boundary.
 *
 *   common code sets request bit
 *          |
 *          v
 *   arch_smp_call_kick_pcpu()
 *          |
 *          v
 *   GIC SGI to target pCPU
 *          |
 *          v
 *   EL2 IRQ path handles request / schedules
 */
void arch_init_smp_call(void)
{
	uint32_t acrn_irq;

	if (get_pcpu_id() == BSP_CPU_ID) {
		acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, ARM64_GIC_SGI_SMP_CALL);
		if ((acrn_irq == IRQ_INVALID) ||
			(request_irq(acrn_irq, arm64_smp_call_irq_handler, NULL, IRQF_NONE) < 0)) {
			LOG_ERR("software interrupt irq setup failed");
		}
	}
}

void arch_smp_call_kick_pcpu(uint16_t pcpu_id)
{
	arm64_gicv3_send_sgi(pcpu_id, ARM64_GIC_SGI_SMP_CALL);
}

void arch_send_reschedule_request(uint16_t pcpu_id)
{
	arch_smp_call_kick_pcpu(pcpu_id);
}
