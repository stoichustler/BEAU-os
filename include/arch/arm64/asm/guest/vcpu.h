/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VCPU_H
#define ARM64_GUEST_VCPU_H

#include <types.h>
#include <timer.h>
#include <asm/page.h>
#include <cpu.h>
#include <asm/guest/vsve.h>
#include <asm/guest/vgicv3.h>

#ifndef ASSEMBLER

#define ARM64_VCPU_REQUEST_EXCEPTION		0U
#define ARM64_VCPU_REQUEST_EVENT		1U

#define ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT	0U

/*
 * EL2 control state that is programmed around vCPU scheduling. The guest GPRs
 * live in acrn_vcpu_arch::regs; this structure contains the translation,
 * execution-control, and timer-offset registers that define the EL1 virtual
 * CPU environment.
 *
 * When two VMs share one pCPU, EL1 state cannot be treated as pCPU-local
 * scratch state. Translation registers, exception registers, TPIDR values, and
 * generic-timer programming all belong to the vCPU that was running when the
 * guest left EL1. Saving them here prevents the next vCPU on the same pCPU
 * from inheriting another VM's address space, exception return state, or timer
 * deadline.
 */
struct arm64_vcpu_guest_ctx {
	uint64_t vttbr_el2;
	uint64_t vtcr_el2;
	uint64_t hcr_el2;
	uint64_t cntvoff_el2;
	uint64_t cntp_cval_el0;
	uint64_t cntv_cval_el0;
	uint32_t cntp_ctl_el0;
	uint32_t cntv_ctl_el0;
	uint32_t timer_virq;
	bool cntv_el2_masked;
	uint64_t cntkctl_el1;
	uint64_t sctlr_el1;
	uint64_t ttbr0_el1;
	uint64_t ttbr1_el1;
	uint64_t tcr_el1;
	uint64_t mair_el1;
	uint64_t amair_el1;
	uint64_t vbar_el1;
	uint64_t contextidr_el1;
	uint64_t cpacr_el1;
	uint64_t tpidr_el0;
	uint64_t tpidrro_el0;
	uint64_t tpidr_el1;
	uint64_t sp_el0;
	uint64_t elr_el1;
	uint64_t spsr_el1;
	uint64_t esr_el1;
	uint64_t far_el1;
	uint64_t afsr0_el1;
	uint64_t afsr1_el1;
	uint64_t par_el1;
};

/*
 * Deferred trap injection state. A producer records the target exception frame
 * here and raises ARM64_VCPU_REQUEST_EXCEPTION; the vCPU thread consumes it
 * just before returning to the guest.
 */
struct arm64_vcpu_trap_info {
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
};

enum arm64_vcpu_suspend_mode {
	ARM64_VCPU_SUSPEND_NONE = 0U,
	ARM64_VCPU_SUSPEND_STANDBY,
	ARM64_VCPU_SUSPEND_POWERDOWN,
};

/* CPU_SUSPEND is orthogonal to the management lifecycle in enum vcpu_state. */
struct arm64_vcpu_pm_state {
	uint64_t resume_entry;
	uint64_t resume_context;
	uint32_t power_state;
	enum arm64_vcpu_suspend_mode mode;
	bool blocked;
};

/*
 * vtimer/vGIC diagnosis counters are compact summaries consumed by vmstat and
 * health. They answer which timer-forward-progress transition kept repeating
 * without retaining per-exit or per-timer trace rings in every vCPU.
 */
struct arm64_vcpu_vtimer_diag {
	/*
	 * WFI tells whether the guest repeatedly slept with an IRQ already visible.
	 * irq_masked means EL1 returned from WFI with PSTATE.I set; pending_irq means
	 * EL2 believed a virtual interrupt should keep the vCPU running.
	 */
	uint64_t wfi_trap;
	uint64_t wfi_irq_masked;
	uint64_t wfi_pending_irq;
	/* [20260627] vtimer attribution:
	 *
	 *   host CNTV PPI27 -> running vCPU -> VM/vCPU vmstat counter
	 *
	 * irqstat counts pCPU handler entries. This counter is bumped while EL2
	 * still knows the owning vCPU, so vmstat can report guest ownership without
	 * guessing from CPU affinity.
	 */
	uint64_t cntv_ppi;
	/*
	 * backup/poll are non-IRQ CNTV sync points. They explain progress when
	 * Linux timer softirq recovery came from EL2 refresh rather than a fresh
	 * host PPI27.
	 */
	uint64_t cntv_backup;
	uint64_t cntv_poll;
	/*
	 * pending-only LR flow tracks the QEMU-sensitive path where a timer LR can
	 * wake WFI but disappear before Linux acknowledges PPI27. preserve means
	 * CNTV was still due and EL2 kept the virtual line asserted; drop means CNTV
	 * was no longer due and EL2 retired that pending-only state.
	 */
	uint64_t pending_only_lr_seen;
	uint64_t pending_only_lr_preserve;
	uint64_t pending_only_lr_drop;
	uint64_t lost_pending_lr;
	/*
	 * EL2 masks the host virtual-timer PPI while the interrupt is owned by vGIC
	 * state. Long mask age indicates that guest timer completion is not making
	 * normal LR/EOI progress.
	 */
	uint64_t el2_mask_set;
	uint64_t el2_mask_clear;
	/*
	 * pre-ERET flush counters summarize the last-chance vtimer/vGIC refresh
	 * before returning to EL1.
	 */
	uint64_t pre_eret_flush;
	uint64_t pre_eret_flush_expired;
	uint64_t max_el2_mask_ticks;
	uint64_t el2_mask_since_ticks;
};

/* Synchronous EL1-to-EL2 exits are classified by ESR_EL2.EC. Physical IRQ
 * exits have no EC and remain attributable through irqstat. */
enum arm64_vcpu_exit_class {
	ARM64_VCPU_EXIT_IABT = 0U,
	ARM64_VCPU_EXIT_SERROR,
	ARM64_VCPU_EXIT_DABT,
	ARM64_VCPU_EXIT_HVC,
	ARM64_VCPU_EXIT_SMC,
	ARM64_VCPU_EXIT_SYSREG,
	ARM64_VCPU_EXIT_SVE,
	ARM64_VCPU_EXIT_WFI_WFE,
	ARM64_VCPU_EXIT_UNKNOWN,
	ARM64_VCPU_EXIT_CLASS_NUM,
};

struct arm64_vcpu_exit_class_stats {
	uint64_t count;
	uint64_t total_ticks;
	uint64_t max_ticks;
};

struct arm64_vcpu_exit_stats {
	struct arm64_vcpu_exit_class_stats class[ARM64_VCPU_EXIT_CLASS_NUM];
};

struct acrn_vcpu_arch {
	/*
	 * Low-level guest-entry assembly locates this durable register image through
	 * the compiler-generated ACRN_VCPU_ARCH_OFFSET_REGS constant.
	 */
	struct cpu_regs regs;

	struct arm64_vcpu_guest_ctx gctx;
	struct arm64_vcpu_sve_state sve;
	struct arm64_vgicv3_vcpu_ctx vgic;
	struct arm64_vcpu_trap_info trap;
	struct arm64_vcpu_pm_state pm;
	struct arm64_vcpu_vtimer_diag vtimer_diag;
	/* [20260708] bounded IRQ forward progress:
	 *
	 *   pending guest IRQ -> short same-vCPU return window
	 *                     -> budget expires -> scheduler fairness resumes
	 *
	 * These fields account that temporary bypass across physical IRQ exits.
	 */
	uint64_t irq_forward_progress_start_ticks;
	uint32_t irq_forward_progress_blocks;
	uint64_t irqs_pending;
	uint64_t irqs_pending_mask;
	struct hv_timer cntv_timer;
	struct hv_timer cntp_timer;
	bool cntv_timer_initialized;
	bool cntp_timer_initialized;
} __aligned(PAGE_SIZE);

struct acrn_vcpu;

int32_t arm64_process_vcpu_requests(struct acrn_vcpu *vcpu);
bool arm64_is_acrn_hypercall(uint64_t hcall_id);
int32_t arm64_dispatch_hypercall(struct acrn_vcpu *vcpu);
void arm64_prepare_linux_vcpu_context(struct acrn_vcpu *vcpu, uint64_t entry, uint64_t x0);
bool arm64_vcpu_has_pending_event(struct acrn_vcpu *vcpu);
uint64_t arch_vcpu_get_entry(const struct acrn_vcpu *vcpu);
void arm64_vtimer_diag_mark_pre_eret(struct acrn_vcpu *vcpu,
	bool flushed, bool masked_expired);
void arm64_vcpu_exit_stats_reset(const struct acrn_vcpu *vcpu);
bool arm64_vcpu_exit_stats_snapshot(const struct acrn_vcpu *vcpu,
	struct arm64_vcpu_exit_stats *stats);
const char *arm64_vcpu_exit_class_name(enum arm64_vcpu_exit_class exit_class);

#endif /* ASSEMBLER */

#endif /* ARM64_GUEST_VCPU_H */
