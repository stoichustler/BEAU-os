/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <softirq.h>
#include <timer.h>
#include <irq.h>
#include <cpu.h>
#include <logmsg.h>
#include <asm/sysreg.h>
#include <asm/irq.h>

#define ARM64_TIMER_MAX_DELTA	UINT32_MAX

struct arm64_pm_timer_state {
	uint64_t suspend_epoch;
	uint64_t cval;
	uint32_t ctl;
	bool valid;
};

static struct arm64_pm_timer_state arm64_pm_timer[MAX_PCPU_NUM];

static void arm64_stop_host_timer(void)
{
	/*
	 * The scheduler owns the EL2 hypervisor timer. CNTP is left for guest
	 * physical-timer emulation, so host ticks cannot be changed by EL1 CNTP
	 * traps or guest-visible PPI30 state.
	 */
	write_cnthp_ctl_el2(0U);
}

static void timer_irq_handler(__unused uint32_t irq, __unused void *data)
{
	arm64_stop_host_timer();
	fire_softirq(SOFTIRQ_TIMER);
}

void arch_init_timer(void)
{
	uint32_t acrn_irq;

	/*
	 * The scheduler uses the ARM EL2 hypervisor timer as a per-pCPU tick
	 * source. Architecturally the timer interrupt is a PPI: the IRQ action is
	 * a single global descriptor, but every pCPU has its own enable bit and
	 * comparator. Install the common handler on the BSP once, then enable and
	 * stop the local PPI on every pCPU as it initializes.
	 *
	 * Guest timer delivery is intentionally separate: VGICv3 uses the hardware
	 * virtual timer/CNTV path for PPI27 and software-emulates guest CNTP/PPI30.
	 * Do not use CNTP for host deadlines, or guest physical-timer state can
	 * race the host scheduler tick.
	 */
	acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, ARM64_GIC_PPI_HYPERVISOR_TIMER);
	if (get_pcpu_id() == BSP_CPU_ID) {
		if ((acrn_irq == IRQ_INVALID) || (request_irq(acrn_irq, timer_irq_handler, NULL, IRQF_NONE) < 0)) {
			LOG_ERR("timer irq setup failed");
		}
	}
	if (acrn_irq != IRQ_INVALID) {
		arm64_gicv3_enable_irq(ARM64_GIC_PPI_HYPERVISOR_TIMER);
		write_cntp_ctl_el0(0U);
		arm64_stop_host_timer();
	}
}

uint64_t arch_cpu_ticks(void)
{
	return read_cntpct_el0();
}

uint32_t arch_cpu_tickrate(void)
{
	return read_cntfrq_el0() / 1000U;
}

void arch_set_timer_count(uint64_t timeout)
{
	uint64_t now = arch_cpu_ticks();
	uint64_t delta = (timeout > now) ? (timeout - now) : 1UL;
	uint16_t pcpu_id = get_pcpu_id();

	if ((pcpu_id < MAX_PCPU_NUM) && arm64_pm_timer[pcpu_id].valid) {
		return;
	}

	if (delta > ARM64_TIMER_MAX_DELTA) {
		delta = ARM64_TIMER_MAX_DELTA;
	}

	arm64_stop_host_timer();
	write_cnthp_cval_el2(now + delta);
	write_cnthp_ctl_el2(CNTV_CTL_ENABLE);
}

int32_t arch_pm_suspend_timer(uint64_t epoch)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_pm_timer_state *state;

	if ((epoch == 0UL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return -EINVAL;
	}
	state = &arm64_pm_timer[pcpu_id];
	if (state->valid) {
		return (state->suspend_epoch == epoch) ? 0 : -EBUSY;
	}

	state->suspend_epoch = epoch;
	state->cval = read_cnthp_cval_el2();
	state->ctl = read_cnthp_ctl_el2();
	state->valid = true;
	arm64_stop_host_timer();

	return 0;
}

int32_t arch_pm_resume_timer(uint64_t epoch)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_pm_timer_state *state;

	if ((epoch == 0UL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return -EINVAL;
	}
	state = &arm64_pm_timer[pcpu_id];
	if (!state->valid) {
		return 0;
	}
	if (state->suspend_epoch != epoch) {
		return -EINVAL;
	}

	write_cnthp_ctl_el2(0U);
	write_cnthp_cval_el2(state->cval);
	write_cnthp_ctl_el2(state->ctl);
	state->suspend_epoch = 0UL;
	state->valid = false;

	return 0;
}
