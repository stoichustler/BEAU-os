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

#define ARM64_VCPU_DEBUG_EXIT_NONE		0U
#define ARM64_VCPU_DEBUG_EXIT_SYNC		1U
#define ARM64_VCPU_DEBUG_EXIT_IRQ		2U
#define ARM64_VCPU_DEBUG_EXIT_EC_INVALID	UINT32_MAX
#define ARM64_VCPU_DEBUG_ABORT_NONE		0U
#define ARM64_VCPU_DEBUG_ABORT_INSTRUCTION	1U
#define ARM64_VCPU_DEBUG_ABORT_DATA		2U
#define ARM64_VCPU_DEBUG_INVALID_VCPU_ID	0xffffU
#define ARM64_VCPU_DEBUG_VGIC_SYNC		1U
#define ARM64_VCPU_DEBUG_VGIC_MAINTENANCE	2U

/*
 * ARM64 dumpstat trace event IDs.
 *
 * vtimer events record semantic edges in virtual timer delivery, not every
 * guest exit:
 * - LOAD/UNLOAD: vCPU switch-in/out loads or saves guest timer state.
 * - SYSREG: guest CNT{P,V}_* system-register access updated shadow state.
 * - PPI/POLL/UPDATE: EL2 found an expired timer via host PPI, bounded poll,
 *   or explicit sync point and synchronized the vGIC timer line.
 * - INJECT/EOI/REQUEUE: guest-visible timer PPI was injected, completed by
 *   EOI, or requeued after timer/vGIC state changed.
 * - BACKUP: offline backup timer for an unloaded vCPU.
 * - PENDING_LR/LOST_LR/MASK/STALL: diagnostics for LR ownership and host PPI
 *   masking when investigating lost or repeated timer interrupts.
 *
 * Guest trace events describe EL1/EL2 boundaries: ENTER enters guest EL1, EXIT
 * records a trap/IRQ back to EL2, and RESUME records return to EL1 after EL2
 * has handled that exit.
 */
#define ARM64_VCPU_VTIMER_TRACE_NUM		6U
#define ARM64_VTIMER_TRACE_LOAD		1U
#define ARM64_VTIMER_TRACE_UNLOAD		2U
#define ARM64_VTIMER_TRACE_SYSREG		3U
#define ARM64_VTIMER_TRACE_PPI			4U
#define ARM64_VTIMER_TRACE_POLL		5U
#define ARM64_VTIMER_TRACE_UPDATE		6U
#define ARM64_VTIMER_TRACE_INJECT		7U
#define ARM64_VTIMER_TRACE_EOI			8U
#define ARM64_VTIMER_TRACE_REQUEUE		9U
#define ARM64_VTIMER_TRACE_BACKUP		10U
#define ARM64_VTIMER_TRACE_WFI			11U /* reserved */
#define ARM64_VTIMER_TRACE_PENDING_LR		12U
#define ARM64_VTIMER_TRACE_LOST_LR		13U
#define ARM64_VTIMER_TRACE_MASK			14U
#define ARM64_VTIMER_TRACE_STALL		15U
#define ARM64_VCPU_GUEST_TRACE_NUM		6U
#define ARM64_VCPU_GUEST_TRACE_ENTER		1U
#define ARM64_VCPU_GUEST_TRACE_EXIT		2U
#define ARM64_VCPU_GUEST_TRACE_RESUME		3U

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

struct arm64_vcpu_last_exit {
	uint64_t tsc;
	uint64_t esr;
	uint64_t elr;
	uint64_t far;
	uint64_t hpfar;
	uint32_t ec;
	uint32_t abort_type;
	uint32_t abort_fsc;
	uint32_t source;
	int32_t status;
};

struct arm64_vcpu_last_irq {
	uint64_t tsc;
	uint32_t virq;
	int32_t status;
	uint16_t source_vcpu_id;
	uint16_t target_vcpu_id;
	bool level;
};

struct arm64_vcpu_last_timer {
	uint64_t tsc;
	uint64_t cval;
	uint32_t ctl;
	uint32_t virq;
	uint32_t sysreg;
	int32_t status;
	bool write;
	bool injected;
};

struct arm64_vcpu_last_sgi {
	uint64_t tsc;
	uint64_t value;
	uint32_t intid;
	int32_t status;
	uint16_t source_vcpu_id;
	uint16_t target_mask;
	uint16_t delivered_mask;
};

/*
 * Target-side SGI state is recorded after a trapped ICC_SGI1R_EL1 injection.
 * It lets dumpstat distinguish "source sent an IPI" from "target has a
 * pending/requested virtual interrupt ready to wake an idle guest CPU".
 */
struct arm64_vcpu_last_sgi_target {
	uint64_t tsc;
	uint64_t source_value;
	uint64_t lr0;
	uint64_t lr1;
	uint64_t hcr;
	uint64_t misr;
	uint32_t intid;
	int32_t status;
	uint16_t source_vcpu_id;
	uint16_t target_vcpu_id;
	uint16_t local_enabled;
	uint16_t local_pending;
	uint16_t local_active;
	uint8_t used_lrs;
	bool request_pending;
	bool target_running;
	bool target_current;
	bool desc_enabled;
	bool desc_pending;
	bool desc_active;
	bool desc_level;
};

struct arm64_vcpu_last_psci {
	uint64_t tsc;
	uint64_t target_mpidr;
	uint64_t entry;
	uint64_t context;
	uint32_t fn;
	int64_t ret;
	uint16_t source_vcpu_id;
	uint16_t target_vcpu_id;
};

/*
 * CPU-interface sysreg traps show the guest's view of interrupt lifecycle
 * control. Keeping the last access lets dumpstat correlate ICC_DIR/CTL/enable
 * writes with stuck LR active/pending state without enabling noisy tracing.
 */
struct arm64_vcpu_last_cpuif {
	uint64_t tsc;
	uint64_t value;
	uint32_t sysreg;
	int32_t status;
	bool read;
};

/*
 * A vGIC maintenance snapshot captures live EL2 CPU-interface state at the
 * boundary where hardware LR state is reconciled with BEAU's software IRQ
 * model. This lets dumpstat distinguish "maintenance never fired" from
 * "maintenance fired but the LR lifecycle was not consumed".
 */
struct arm64_vcpu_last_vgic {
	uint64_t tsc;
	uint64_t misr;
	uint64_t eisr;
	uint64_t elrsr;
	uint64_t hcr;
	uint64_t vmcr;
	uint64_t ap0r0;
	uint64_t ap1r0;
	uint64_t lr0;
	uint64_t lr1;
	uint32_t source;
	uint32_t count;
	uint8_t used_lrs;
};

/*
 * WFI/WFE debug state records the final decision made by the trapped idle path.
 * It is intentionally a single snapshot so dumpstat can explain a stalled vCPU
 * without adding noisy per-exit logs to guest console output.
 */
struct arm64_vcpu_last_wfx {
	uint64_t tsc;
	uint64_t esr;
	uint64_t elr;
	uint64_t cntvct;
	uint64_t cntv_cval;
	uint64_t hcr;
	uint64_t misr;
	uint64_t ap0r0;
	uint64_t ap1r0;
	uint64_t lr0;
	uint64_t lr1;
	uint64_t live_lr0;
	uint64_t live_lr1;
	uint32_t cntv_ctl;
	uint8_t used_lrs;
	bool is_wfe;
	bool pending_irq;
	bool irq_masked;
	bool request_pending;
	bool yielded;
	bool cntv_expired;
	bool cntv_el2_masked;
};

/*
 * The guest-resume snapshot is recorded after exit handling has synchronized
 * vtimer/vGIC state and immediately before EL2 restores the frame for ERET.
 * It shows what the guest is about to observe, which can differ from the state
 * captured at the earlier physical IRQ entry point.
 */
struct arm64_vcpu_guest_resume {
	uint64_t tsc;
	uint64_t elr;
	uint64_t spsr;
	uint64_t host_cntpct;
	uint64_t host_timer_cval;
	uint64_t cntvct;
	uint64_t cntv_cval;
	uint64_t hcr;
	uint64_t vmcr;
	uint64_t misr;
	uint64_t eisr;
	uint64_t elrsr;
	uint64_t ap0r0;
	uint64_t ap1r0;
	uint64_t sw_lr0;
	uint64_t sw_lr1;
	uint64_t live_lr0;
	uint64_t live_lr1;
	uint32_t cntv_ctl;
	uint32_t host_timer_ctl;
	uint32_t source;
	uint32_t timer_virq;
	uint8_t used_lrs;
	bool cntv_expired;
	bool cntv_el2_masked;
};

struct arm64_vcpu_guest_trace_entry {
	uint64_t tsc;
	uint64_t elr;
	uint64_t esr;
	uint64_t far;
	uint64_t hpfar;
	uint32_t ec;
	uint32_t source;
	int32_t status;
	uint16_t pcpu_id;
	uint8_t event;
};

struct arm64_vcpu_guest_trace {
	uint32_t head;
	uint32_t count;
	struct arm64_vcpu_guest_trace_entry entry[ARM64_VCPU_GUEST_TRACE_NUM];
};

/*
 * The vtimer trace ring is intentionally small and per-vCPU. It records only
 * lifecycle edges that can explain a lost or repeated timer interrupt without
 * turning every guest exit into a log event.
 */
struct arm64_vcpu_vtimer_trace_entry {
	uint64_t tsc;
	uint64_t cntvct;
	uint64_t cval;
	uint64_t lr0;
	uint64_t hcr;
	uint64_t misr;
	uint32_t ctl;
	uint32_t virq;
	uint16_t pcpu_id;
	uint8_t event;
	uint8_t used_lrs;
	bool expired;
	bool masked;
	bool pending;
	bool active;
	bool level;
	bool write;
	bool injected;
};

struct arm64_vcpu_vtimer_trace {
	uint32_t head;
	uint32_t count;
	/*
	 * The trace ring is for state changes; high-rate repetitions are counted
	 * by vtimer_diag. Suppressing consecutive duplicates keeps dumpstat useful
	 * when an interrupt stays in the same pending/masked state for many exits.
	 */
	uint64_t last_key;
	struct arm64_vcpu_vtimer_trace_entry entry[ARM64_VCPU_VTIMER_TRACE_NUM];
};

/*
 * vtimer/vGIC diagnosis counters are intentionally summaries, not another raw
 * register dump. dumpstat already prints the current CNTV, vGIC descriptor and
 * LR state; these counters answer which timer-forward-progress transition kept
 * repeating before Linux reported timer-softirq/RCU stalls.
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
	 * before returning to EL1. They stay out of the trace ring because this path
	 * can be hot and would otherwise overwrite rarer pending-only LR evidence.
	 */
	uint64_t pre_eret_flush;
	uint64_t pre_eret_flush_expired;
	uint64_t max_el2_mask_ticks;
	uint64_t el2_mask_since_ticks;
};

struct arm64_vcpu_debug_info {
	struct arm64_vcpu_last_exit last_exit;
	struct arm64_vcpu_last_irq last_irq;
	struct arm64_vcpu_last_timer last_timer;
	struct arm64_vcpu_last_sgi last_sgi;
	struct arm64_vcpu_last_sgi_target last_sgi_target;
	struct arm64_vcpu_last_psci last_psci;
	struct arm64_vcpu_last_cpuif last_cpuif;
	struct arm64_vcpu_last_vgic last_vgic_sync;
	struct arm64_vcpu_last_vgic last_vgic_maintenance;
	struct arm64_vcpu_last_wfx last_wfx;
	struct arm64_vcpu_guest_resume last_resume;
	struct arm64_vcpu_guest_trace guest_trace;
	struct arm64_vcpu_vtimer_trace vtimer_trace;
	struct arm64_vcpu_vtimer_diag vtimer_diag;
};

struct acrn_vcpu_arch {
	/*
	 * This must be the first member of acrn_vcpu_arch so low-level assembly
	 * can treat struct acrn_vcpu_arch as a struct cpu_regs when building the
	 * guest-entry frame.
	 */
	struct cpu_regs regs;

	struct arm64_vcpu_guest_ctx gctx;
	struct arm64_vcpu_sve_state sve;
	struct arm64_vgicv3_vcpu_ctx vgic;
	struct arm64_vcpu_trap_info trap;
	struct arm64_vcpu_debug_info debug;
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
uint64_t arch_vcpu_get_entry(const struct acrn_vcpu *vcpu);
uint64_t arm64_vcpu_trace_guest_boundary(struct acrn_vcpu *vcpu, uint8_t event,
	uint32_t source, int32_t status);
void arm64_vcpu_trace_vtimer(struct acrn_vcpu *vcpu, uint32_t event,
	uint32_t virq, uint32_t ctl, uint64_t cval, bool write, bool injected);
void arm64_vtimer_diag_mark_pre_eret(struct acrn_vcpu *vcpu,
	bool flushed, bool masked_expired);

#endif /* ASSEMBLER */

#endif /* ARM64_GUEST_VCPU_H */
