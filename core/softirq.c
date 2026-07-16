/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <bits.h>
#include <asm/cpu.h>
#include <per_cpu.h>
#include <softirq.h>
#include <cpu.h>
#include <notify.h>

/*
 * Deferred interrupt-work principle:
 *
 * Hard IRQ handlers should keep the architecture interrupt window short. They
 * acknowledge or mask the source, set a per-pCPU softirq bit, and return. The
 * softirq pass then runs the heavier common callback outside the immediate IRQ
 * dispatch path.
 *
 *   IRQ handler
 *      |
 *      v
 *   fire_softirq(nr)
 *      |
 *      v
 *   per_cpu(softirq_pending) bit
 *      |
 *      v
 *   do_softirq()
 *     - prevent nested servicing on the same pCPU
 *     - optionally re-enable local IRQs while callbacks run
 *     - drain bits raised during callback execution
 *
 * The ARM64 host timer uses this path: the CNTHP PPI fires in EL2, the handler
 * raises SOFTIRQ_TIMER, and core/timer.c advances software timers before
 * programming the next architecture-timer deadline.
 */
static softirq_handler softirq_handlers[NR_SOFTIRQS];

void init_softirq(void)
{
}

void register_softirq(uint16_t nr, softirq_handler handler)
{
	softirq_handlers[nr] = handler;
}

void fire_softirq(uint16_t nr)
{
	fire_softirq_on(nr, get_pcpu_id());
}

void fire_softirq_on(uint16_t nr, uint16_t pcpu_id)
{
	/* [20260716] Target-pCPU deferred work
	 *
	 * A vSMMU MMIO exit can occur on any owner-VM pCPU, while its bounded
	 * command worker is pinned by static policy. Atomically publish the target
	 * softirq bit before the SGI so the remote exit path observes complete work.
	 */
	if ((nr < NR_SOFTIRQS) && (pcpu_id < get_pcpu_nums())) {
		bitmap_set(nr, &per_cpu(softirq_pending, pcpu_id));
		if (pcpu_id != get_pcpu_id()) {
			arch_smp_call_kick_pcpu(pcpu_id);
		}
	}
}

static void do_softirq_internal(uint16_t cpu_id)
{
	volatile uint64_t *softirq_pending_bitmap =
			&per_cpu(softirq_pending, cpu_id);
	uint16_t nr = ffs64(*softirq_pending_bitmap);

	while (nr < NR_SOFTIRQS) {
		bitmap_clear(nr, softirq_pending_bitmap);
		(*softirq_handlers[nr])(cpu_id);
		nr = ffs64(*softirq_pending_bitmap);
	}
}

void do_softirq(void)
{
	uint16_t cpu_id = get_pcpu_id();

	if (per_cpu(softirq_servicing, cpu_id) == 0U) {
		per_cpu(softirq_servicing, cpu_id) = 1U;

		local_irq_enable();
		do_softirq_internal(cpu_id);
		local_irq_disable();

		do_softirq_internal(cpu_id);
		per_cpu(softirq_servicing, cpu_id) = 0U;
	}
}

void do_softirq_no_irqenable(void)
{
	uint16_t cpu_id = get_pcpu_id();

	if (per_cpu(softirq_servicing, cpu_id) == 0U) {
		per_cpu(softirq_servicing, cpu_id) = 1U;
		do_softirq_internal(cpu_id);
		per_cpu(softirq_servicing, cpu_id) = 0U;
	}
}
