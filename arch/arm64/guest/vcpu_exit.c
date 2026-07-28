/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <rtl.h>
#include <vcpu.h>
#include <vm.h>
#include <cpu.h>
#include <schedule.h>
#include <errno.h>
#include <logmsg.h>
#include <bsp/io_req.h>
#include <guest_memory.h>
#include <acrn_common.h>
#include <hv_pm.h>
#include <softirq.h>
#include <ticks.h>
#include <trace.h>
#include <vconfig.h>
#include <hwtdbg.h>
#include <asm/platform.h>
#include <asm/cpu.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/pmu.h>
#if CONFIG_ARM64_SPE
#include <asm/spe.h>
#endif
#include <asm/sysreg.h>
#include <asm/trap.h>
#include <asm/guest/vcpu_priv.h>
#include <asm/guest/vm_reset.h>
#include <asm/guest/vmpu.h>
#include <asm/guest/vgicv3.h>

/* [20260630] vCPU exit principle:
 *
 * Guest exits are entered from the EL2 vector table. Assembly saves the live
 * CPU state into a temporary struct cpu_regs frame on the vCPU thread stack;
 * this file converts that architectural exit state into common ACRN concepts
 * such as MMIO requests, PSCI CPU control, and virtual interrupt delivery.
 *
 * Interrupt and timer return paths:
 *
 *   EL1 ICC_* sysreg trap -> handle_sysreg() -> arm64_vgicv3_*()
 *   EL1 CNT* sysreg trap -> arm64_vtimer_handle_sysreg() -> update_current_vtimer()
 *   EL1 WFI/WFE trap     -> poll/update vtimer -> pending check -> maybe yield
 *   physical IRQ at EL2  -> dispatch IRQ/softirq -> maybe schedule
 *                         -> poll/update vtimer -> process vCPU requests
 *
 * [20260710] synchronous exit demux:
 *
 *   ESR_EL2.EC
 *      |
 *      +-- HVC64      -> explicit hypercall ABI
 *      +-- SYSREG     -> vGIC/vtimer/diagnostic sysreg emulation
 *      +-- WFI/WFE    -> idle hint with vtimer/vGIC refresh before yield
 *      +-- IABT S2    -> instruction-fetch fault diagnostics, no MMIO value
 *      +-- DABT S2    -> load/store MMIO request when the IPA is registered
 *      +-- unknown    -> fatal guest-exit diagnostic
 *
 * Instruction aborts and data aborts are deliberately separated. An
 * instruction abort means the guest tried to fetch from unmapped or non-
 * executable memory; a data abort can carry access width, direction, and target
 * register information, so only data aborts may become MMIO emulation.
 */
#define HPFAR_EL2_FIPA_MASK	0xfffffffff0UL
#define ARM64_TRACE_EXIT_SYNC	1U
#define ARM64_TRACE_EXIT_IRQ	2U
#define FAR_EL2_PAGE_MASK	0xfffUL
#define VM_STACK_TRACE_DEPTH	16U

#define PSCI_0_2_FN_PSCI_VERSION	0x84000000U
#define PSCI_0_2_FN_CPU_SUSPEND	0x84000001U
#define PSCI_0_2_FN_CPU_OFF		0x84000002U
#define PSCI_0_2_FN_CPU_ON		0x84000003U
#define PSCI_0_2_FN_AFFINITY_INFO	0x84000004U
#define PSCI_0_2_FN_MIGRATE_INFO_TYPE	0x84000006U
#define PSCI_0_2_FN_SYSTEM_OFF		0x84000008U
#define PSCI_0_2_FN_SYSTEM_RESET	0x84000009U
#define PSCI_1_0_FN_PSCI_FEATURES	0x8400000aU
#define PSCI_1_0_FN_SYSTEM_SUSPEND	0x8400000eU
#define PSCI_1_1_FN_SYSTEM_RESET2	0x84000012U
#define ARM_SMCCC_VERSION_FUNC_ID	0x80000000U
#define PSCI_0_2_FN64_CPU_SUSPEND	0xc4000001U
#define PSCI_0_2_FN64_CPU_ON		0xc4000003U
#define PSCI_0_2_FN64_AFFINITY_INFO	0xc4000004U
#define PSCI_1_0_FN64_SYSTEM_SUSPEND	0xc400000eU
#define PSCI_0_2_TOS_MP		2L
#define TRUSTY_SMC_FC_API_VERSION	0xbc00000bU
#define SMC_UNK				UINT64_MAX

#define PSCI_RET_SUCCESS		0L
#define PSCI_RET_NOT_SUPPORTED		(-1L)
#define PSCI_RET_INVALID_PARAMS		(-2L)
#define PSCI_RET_DENIED			(-3L)
#define PSCI_AFFINITY_LEVEL_ON		0UL
#define PSCI_AFFINITY_LEVEL_OFF		1UL
#define SPSR_MODE_MASK			0xfUL
#define SPSR_MODE_EL1H			0x5UL

uint64_t beau_trusty_smc(struct acrn_vcpu *vcpu, uint64_t function_id,
	uint64_t requested_version);

#define ESR_SYSREG_DIR_READ		1UL
#define ESR_SYSREG_OP0_SHIFT		20U
#define ESR_SYSREG_OP0_MASK		0x3UL
#define ESR_SYSREG_OP2_SHIFT		17U
#define ESR_SYSREG_OP2_MASK		0x7UL
#define ESR_SYSREG_OP1_SHIFT		14U
#define ESR_SYSREG_OP1_MASK		0x7UL
#define ESR_SYSREG_CRN_SHIFT		10U
#define ESR_SYSREG_CRN_MASK		0xfUL
#define ESR_SYSREG_RT_SHIFT		5U
#define ESR_SYSREG_RT_MASK		0x1fUL
#define ESR_SYSREG_CRM_SHIFT		1U
#define ESR_SYSREG_CRM_MASK		0xfUL

#define SYSREG_ENC(op0, op1, crn, crm, op2) \
	(((op0) << 20U) | ((op2) << 17U) | ((op1) << 14U) | ((crn) << 10U) | ((crm) << 1U))
#define SYSREG_ICC_PMR_EL1		SYSREG_ENC(3UL, 0UL, 4UL, 6UL, 0UL)
#define SYSREG_ICC_DIR_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 11UL, 1UL)
#define SYSREG_ICC_RPR_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 11UL, 3UL)
#define SYSREG_ICC_CTLR_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 12UL, 4UL)
#define SYSREG_ICC_SRE_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 12UL, 5UL)
#define SYSREG_ICC_IGRPEN1_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 12UL, 7UL)
#define SYSREG_ICC_SGI1R_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 11UL, 5UL)
#define SYSREG_ICC_ASGI1R_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 11UL, 6UL)
#define SYSREG_ICC_SGI0R_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 11UL, 7UL)
#define SYSREG_CNTP_CTL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 1UL)
#define SYSREG_CNTP_CVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 2UL)
#define SYSREG_CNTP_TVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 0UL)
#define SYSREG_CNTPCT_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 0UL, 1UL)
#define SYSREG_CNTV_CTL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 1UL)
#define SYSREG_CNTV_CVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 2UL)
#define SYSREG_CNTV_TVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 0UL)
#define SYSREG_CNTVCT_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 0UL, 2UL)

/*
 * Linux scheduling hint:
 *
 *   SP_EL0(current task) -> task.thread_info.preempt_count
 *      SOFTIRQ/HARDIRQ/NMI set -> EL1 still owns IRQ bottom-half progress
 *
 * The local Linux image has CONFIG_THREAD_INFO_IN_TASK=y and no SW_TTBR0_PAN,
 * so thread_info is the first task_struct field and preempt_count sits after
 * flags. This is a best-effort hint: failed translation/read falls back to the
 * vGIC-only policy used for RTOS guests.
 */
#define LINUX_TI_PREEMPT_COUNT_OFFSET	8UL
#define LINUX_PREEMPT_MASK		0x000000ffU
#define LINUX_SOFTIRQ_MASK		0x0000ff00U
#define LINUX_HARDIRQ_MASK		0x000f0000U
#define LINUX_NMI_MASK			0x00f00000U
#define LINUX_IRQ_CONTEXT_MASK \
	(LINUX_SOFTIRQ_MASK | LINUX_HARDIRQ_MASK | LINUX_NMI_MASK)
#define ARM64_IRQ_PROGRESS_MAX_BLOCKS	8U
#define ARM64_IRQ_PROGRESS_MAX_US	2000U

struct arm64_guest_stack_frame {
	uint64_t fp;
	uint64_t lr;
};

static uint64_t arm64_fault_ipa(const struct cpu_regs *regs)
{
	/*
	 * For guest stage-2 memory aborts, HPFAR_EL2 contains the faulting IPA
	 * page number and FAR_EL2 provides the byte offset inside that page. This
	 * helper is shared by instruction abort diagnostics and data-abort MMIO
	 * emulation; only data aborts can be turned into load/store MMIO requests.
	 */
	return ((regs->hpfar & HPFAR_EL2_FIPA_MASK) << 8U) |
		(regs->far & FAR_EL2_PAGE_MASK);
}

static uint32_t arm64_abort_fsc(uint64_t esr)
{
	return (uint32_t)(esr & ESR_ABORT_FSC_MASK);
}

static void dump_vm_stack_trace(struct acrn_vcpu *vcpu, const struct cpu_regs *regs,
	const char *reason)
{
	struct arm64_guest_stack_frame frame;
	uint64_t fp = regs->x29;
	uint64_t lr = regs->lr;
	uint32_t idx;

	/*
	 * This is a raw AArch64 frame-pointer unwind. AAPCS64 frames save the
	 * previous x29 and return address at [x29, x29 + 8]. BEAU reads those
	 * words through stage-2 guest memory, so the trace is available only when
	 * the saved guest FP values are directly readable as GPAs, which matches
	 * the current static 1:1 RTOS layout. Guests using high virtual kernel
	 * stacks need guest VA translation before deeper frames can be decoded.
	 */
	LOG_ERR("arm64 %s vm%u:vcpu%u stack pc=0x%lx sp=0x%lx fp=0x%lx lr=0x%lx",
		reason, vcpu->vm->vm_id, vcpu->vcpu_id, regs->elr, regs->sp, fp, lr);

	if ((fp == 0UL) && (lr == 0UL)) {
		LOG_ERR("arm64 %s vm%u:vcpu%u stack trace unavailable: empty frame registers",
			reason, vcpu->vm->vm_id, vcpu->vcpu_id);
		return;
	}

	for (idx = 0U; idx < VM_STACK_TRACE_DEPTH; idx++) {
		LOG_ERR("arm64 %s vm%u:vcpu%u frame[%02u] fp=0x%lx lr=0x%lx",
			reason, vcpu->vm->vm_id, vcpu->vcpu_id, idx, fp, lr);

		if (fp == 0UL) {
			break;
		}

		if (copy_from_gpa(vcpu->vm, &frame, fp, sizeof(frame)) != 0) {
			LOG_ERR("arm64 %s vm%u:vcpu%u stack trace stopped: guest fp is not directly readable as GPA",
				reason, vcpu->vm->vm_id, vcpu->vcpu_id);
			break;
		}

		if ((frame.fp == 0UL) || (frame.fp <= fp)) {
			break;
		}

		fp = frame.fp;
		lr = frame.lr;
	}
}

static struct acrn_vcpu *get_exit_vcpu(uint16_t pcpu_id)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(pcpu_id);

	if (vcpu == NULL) {
		LOG_FTL("arm64 vcpu exit without current vcpu on pcpu%hu", pcpu_id);
		cpu_dead();
	}

	return vcpu;
}

static void save_exit_regs(struct acrn_vcpu *vcpu, const struct cpu_regs *regs)
{
	/*
	 * Keep the persistent guest image in vcpu->arch.regs. The vector stack
	 * frame is temporary and may be rebuilt after scheduling or softirq work.
	 */
	(void)memcpy_s(&vcpu->arch.regs, sizeof(vcpu->arch.regs),
		regs, sizeof(*regs));
}

static void restore_exit_regs(struct cpu_regs *regs, const struct acrn_vcpu *vcpu)
{
	(void)memcpy_s(regs, sizeof(*regs),
		&vcpu->arch.regs, sizeof(vcpu->arch.regs));
}

static void refresh_current_vtimer(struct acrn_vcpu *vcpu)
{
	arm64_vgicv3_poll_current_vtimer(vcpu);
	arm64_vgicv3_update_current_vtimer(vcpu);
}

static bool vcpu_has_pending_guest_irq(struct acrn_vcpu *vcpu)
{
	return arm64_vgicv3_pending_irq_blocks_reschedule(vcpu);
}

static int32_t arm64_translate_live_guest_va(struct acrn_vcpu *vcpu, uint64_t gva,
	uint64_t *gpa)
{
	uint64_t old_par;
	uint64_t par;
	uint64_t page;
	int32_t ret = -EINVAL;

	if ((vcpu == NULL) || (gpa == NULL) ||
		(get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return ret;
	}

	/*
	 * AT S1E1R consumes the live guest EL1 translation registers and stage-2
	 * context already installed for this vCPU:
	 *
	 *   guest VA --S1+S2--> IPA/GPA page -> copy_from_gpa()
	 *
	 * PAR_EL1 is guest-visible state, so preserve it around the diagnostic
	 * translation.
	 */
	old_par = read_par_el1();
	arm64_at_s1e1r(gva);
	par = read_par_el1();
	write_par_el1(old_par);
	if ((par & PAR_EL1_F) == 0UL) {
		page = par & PAR_EL1_PA_MASK;
		*gpa = page | (gva & (PAGE_SIZE - 1UL));
		ret = 0;
	}

	return ret;
}

static bool vcpu_guest_is_linux(const struct acrn_vcpu *vcpu)
{
	const struct acrn_vm_config *vm_config;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return false;
	}

	vm_config = get_vm_config(vcpu->vm->vm_id);
	return vm_config->os_config.os_family == VM_OS_LINUX;
}

static bool sample_guest_linux_preempt_count(struct acrn_vcpu *vcpu,
	uint32_t *preempt_count)
{
	uint64_t current;
	uint64_t gpa;
	bool ret = false;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (preempt_count == NULL)) {
		return false;
	}
	if (!vcpu_guest_is_linux(vcpu)) {
		return false;
	}
	if ((vcpu->arch.regs.spsr & SPSR_MODE_MASK) != SPSR_MODE_EL1H) {
		return false;
	}

	current = read_sp_el0();
	if (current == 0UL) {
		return false;
	}

	if ((arm64_translate_live_guest_va(vcpu,
			current + LINUX_TI_PREEMPT_COUNT_OFFSET, &gpa) == 0) &&
		(copy_from_gpa(vcpu->vm, preempt_count, gpa, sizeof(*preempt_count)) == 0)) {
		ret = true;
	}

	return ret;
}

static bool vcpu_guest_irq_context_blocks_reschedule(struct acrn_vcpu *vcpu)
{
	uint32_t preempt_count = 0U;
	bool blocks = false;

	if (sample_guest_linux_preempt_count(vcpu, &preempt_count)) {
		/*
		 * Hardirq/softirq/NMI contexts are forward-progress critical because
		 * Linux raises TIMER/SCHED work from the timer IRQ and runs it after
		 * irq_exit. Plain PREEMPT_MASK is observed for diagnosis but is not a
		 * standalone block; idle and scheduler sections can keep it set long
		 * enough to hurt shared-pCPU fairness.
		 */
		if ((preempt_count & LINUX_IRQ_CONTEXT_MASK) != 0U) {
			blocks = true;
		} else if ((preempt_count & LINUX_PREEMPT_MASK) != 0U) {
			blocks = arm64_vgicv3_pending_irq_blocks_reschedule(vcpu);
		}
	}

	return blocks;
}

static bool vcpu_forward_progress_blocks_reschedule(struct acrn_vcpu *vcpu)
{
	return vcpu_has_pending_guest_irq(vcpu) ||
		vcpu_guest_irq_context_blocks_reschedule(vcpu);
}

static void vcpu_reset_irq_forward_progress(struct acrn_vcpu *vcpu)
{
	vcpu->arch.irq_forward_progress_start_ticks = 0UL;
	vcpu->arch.irq_forward_progress_blocks = 0U;
}

static bool vcpu_irq_forward_progress_budget_available(struct acrn_vcpu *vcpu)
{
	uint64_t now = cpu_ticks();
	uint64_t start = vcpu->arch.irq_forward_progress_start_ticks;

	if (start == 0UL) {
		vcpu->arch.irq_forward_progress_start_ticks = now;
		vcpu->arch.irq_forward_progress_blocks = 1U;
		return true;
	}

	if ((now - start) >= us_to_ticks(ARM64_IRQ_PROGRESS_MAX_US)) {
		return false;
	}

	if (vcpu->arch.irq_forward_progress_blocks >= ARM64_IRQ_PROGRESS_MAX_BLOCKS) {
		return false;
	}

	vcpu->arch.irq_forward_progress_blocks++;
	return true;
}

/* [20260708] bounded guest IRQ progress window:
 *
 *   physical IRQ exit
 *        -> refresh current CNTV/vGIC state
 *        -> guest IRQ or Linux IRQ context still needs progress?
 *             yes -> return to the same vCPU for a short budget
 *             no/expired -> let schedule() pick the next runnable vCPU
 *
 * A guest timer LR can remain deliverable across many exits. The bypass is
 * therefore bounded so a shared-pCPU peer cannot be starved indefinitely.
 */
static bool vcpu_should_defer_reschedule_for_irq_progress(struct acrn_vcpu *vcpu)
{
	bool blocks = vcpu_forward_progress_blocks_reschedule(vcpu);

	if (!blocks) {
		vcpu_reset_irq_forward_progress(vcpu);
		return false;
	}

	return vcpu_irq_forward_progress_budget_available(vcpu);
}

static void prepare_current_guest_resume(struct acrn_vcpu *vcpu)
{
	bool expired;

	/*
	 * Sample the live CNTV line before every ERET, update PPI27's level state,
	 * and flush it into an LR if it is deliverable. This keeps CNTV/PPI27
	 * deterministic and avoids relying on stale LR state as the normal timer
	 * delivery path.
	 */
	arm64_vgicv3_update_current_vtimer(vcpu);
	expired = arm64_vtimer_guest_expired(vcpu);
	arm64_vtimer_diag_mark_pre_eret(vcpu, true, expired);
}

static struct acrn_vcpu *schedule_without_guest_resume(uint16_t pcpu_id,
	struct acrn_vcpu *vcpu)
{
	if (!is_vcpu_running(vcpu) || vcpu->thread_obj.be_blocking) {
		schedule();
		return get_exit_vcpu(pcpu_id);
	}

	refresh_current_vtimer(vcpu);
	if (need_reschedule(pcpu_id) && !vcpu_has_pending_request(vcpu)) {
		(void)sched_clear_reschedule_if_current_only(pcpu_id);
	}

	if (need_reschedule(pcpu_id) && !vcpu_has_pending_request(vcpu)) {
		if (!vcpu_should_defer_reschedule_for_irq_progress(vcpu)) {
			struct acrn_vcpu *prev = vcpu;

			schedule();
			vcpu_reset_irq_forward_progress(prev);
			vcpu = get_exit_vcpu(pcpu_id);
			if (vcpu != prev) {
				vcpu_reset_irq_forward_progress(vcpu);
			}
			refresh_current_vtimer(vcpu);
		}
	}

	return vcpu;
}

static uint64_t *arm64_gpr(struct cpu_regs *regs, uint32_t idx)
{
	return (idx < 31U) ? (&regs->x0 + idx) : NULL;
}

static bool arm64_sysreg_zero_rt(uint32_t rt, bool read)
{
	/*
	 * AArch64 system-register traps encode Rt=31 when the guest uses XZR/WZR.
	 * Writes with Rt=31 supply a zero value; reads discard the result, so the
	 * emulator can complete them without trying to write back a GPR.
	 */
	return (rt == 31U) && !read;
}

static uint64_t mmio_size_mask(uint64_t size)
{
	return (size >= sizeof(uint64_t)) ? ~0UL : ((1UL << (size * 8U)) - 1UL);
}

static uint64_t extend_mmio_read(uint64_t value, uint64_t size, uint64_t esr)
{
	uint64_t mask = mmio_size_mask(size);

	value &= mask;
	if (((esr & ESR_DABT_SSE) != 0UL) && (size < sizeof(uint64_t))) {
		uint64_t sign = 1UL << ((size * 8U) - 1U);

		if ((value & sign) != 0UL) {
			value |= ~mask;
		}
	}
	if ((esr & ESR_DABT_SF) == 0UL) {
		value &= 0xffffffffUL;
	}

	return value;
}

static void advance_vcpu_elr(struct acrn_vcpu *vcpu)
{
	struct cpu_regs *regs = &vcpu->arch.regs;

	regs->elr += ((regs->esr & ESR_EL2_IL) != 0UL) ? 4UL : 2UL;
}

static int32_t handle_mmio_abort(struct acrn_vcpu *vcpu)
{
	struct arm64_core_pmu_path_token pmu_token;
	struct cpu_regs *regs = &vcpu->arch.regs;
	struct io_request *io_req = &vcpu->req;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	uint64_t esr = regs->esr;
	uint64_t ipa;
	uint64_t size;
	uint32_t reg_idx;
	uint64_t *reg;
	int32_t ret;

	/*
	 * Data aborts are load/store memory aborts. BEAU intentionally leaves
	 * guest device IPA windows unmapped at stage-2 so reads and writes trap
	 * here and become MMIO requests. Instruction aborts are fetch failures and
	 * must not use this path because ESR.DABT fields describe data access size,
	 * direction, and source/target GPR.
	 */
	if ((esr & ESR_DABT_ISV) == 0UL) {
		return -EINVAL;
	}

	ipa = arm64_fault_ipa(regs);
	size = 1UL << ((esr >> ESR_DABT_SAS_SHIFT) & ESR_DABT_SAS_MASK);
	reg_idx = (uint32_t)((esr >> ESR_DABT_SRT_SHIFT) & ESR_DABT_SRT_MASK);
	reg = arm64_gpr(regs, reg_idx);

	(void)memset(io_req, 0U, sizeof(*io_req));
	io_req->io_type = ACRN_IOREQ_TYPE_MMIO;
	mmio->address = ipa;
	mmio->size = size;
	if ((esr & ESR_DABT_WNR) != 0UL) {
		mmio->direction = ACRN_IOREQ_DIR_WRITE;
		mmio->value = (reg != NULL) ? (*reg & mmio_size_mask(size)) : 0UL;
	} else {
		mmio->direction = ACRN_IOREQ_DIR_READ;
	}

	/*
	 * Performance bottleneck: every device MMIO access on an unmapped IPA
	 * takes a full EL1->EL2 exit, handler lookup, device emulation, register
	 * writeback, and ERET. Guest console and GIC polling loops can therefore
	 * dominate boot/runtime cost here even when each emulated register access
	 * is individually simple.
	 */
	arm64_core_pmu_path_begin(&pmu_token);
	ret = emulate_io(vcpu, io_req);
	arm64_core_pmu_path_end(ARM64_CORE_PMU_PATH_MMIO, &pmu_token);
	if (ret == 0) {
		if (((esr & ESR_DABT_WNR) == 0UL) && (reg != NULL)) {
			*reg = extend_mmio_read(mmio->value, size, esr);
		}
		advance_vcpu_elr(vcpu);
	} else {
		LOG_ERR("arm64 mmio abort failed vm%u:vcpu%u ipa=0x%lx size=%lu dir=%s srt=%u esr=0x%lx ret=%d",
			vcpu->vm->vm_id, vcpu->vcpu_id, ipa, size,
			((esr & ESR_DABT_WNR) != 0UL) ? "write" : "read", reg_idx, esr, ret);
	}

	return ret;
}

static int32_t handle_instruction_abort(struct acrn_vcpu *vcpu)
{
	const struct cpu_regs *regs = &vcpu->arch.regs;
	uint64_t ipa = arm64_fault_ipa(regs);
	uint32_t fsc = arm64_abort_fsc(regs->esr);

	/*
	 * Instruction aborts are guest instruction-fetch failures, such as
	 * executing from an unmapped stage-2 IPA, an execute-never mapping, or a
	 * permission fault. They are captured for diagnostics but are not emulated
	 * as MMIO because no load/store value or target register exists.
	 */
	LOG_ERR("arm64 instruction abort vm%u:vcpu%u ipa=0x%lx far=0x%lx hpfar=0x%lx fsc=0x%x esr=0x%lx elr=0x%lx",
		vcpu->vm->vm_id, vcpu->vcpu_id, ipa, regs->far, regs->hpfar,
		fsc, regs->esr, regs->elr);
	dump_vm_stack_trace(vcpu, regs, "instruction abort");

	return -EFAULT;
}

static int32_t handle_serror(struct acrn_vcpu *vcpu)
{
	const struct cpu_regs *regs = &vcpu->arch.regs;
	struct arm64_ras_snapshot ras;

	/*
	 * Guest SError is asynchronous and may be reported after the instruction
	 * that caused it. The ELR/SP/FP snapshot still identifies where the VM was
	 * interrupted, which is usually the best handoff point for manual triage.
	 */
	if (arm64_ras_capture(&ras)) {
		hwtdbg_capture_ras(vcpu, regs, &ras);
	}
	LOG_ERR("arm64 serror vm%u:vcpu%u esr=0x%lx elr=0x%lx far=0x%lx hpfar=0x%lx",
		vcpu->vm->vm_id, vcpu->vcpu_id, regs->esr, regs->elr, regs->far,
		regs->hpfar);
	dump_vm_stack_trace(vcpu, regs, "serror");

	return -EFAULT;
}

static struct acrn_vcpu *psci_target_vcpu(struct acrn_vm *vm, uint64_t mpidr)
{
	uint16_t idx;
	struct acrn_vcpu *vcpu;

	foreach_vcpu(idx, vm, vcpu) {
		if (vcpu_get_vmpidr(vcpu) == (mpidr & MPIDR_AFFINITY_MASK)) {
			return vcpu;
		}
	}

	return NULL;
}

static int64_t handle_psci_cpu_on(struct acrn_vcpu *vcpu)
{
	struct acrn_vcpu *target = psci_target_vcpu(vcpu->vm, vcpu->arch.regs.x1);
	int64_t ret = PSCI_RET_INVALID_PARAMS;

	if (hv_pm_begin_vm_topology_change(vcpu->vm->vm_id) != 0) {
		ret = PSCI_RET_DENIED;
		return ret;
	}
	if (target != NULL) {
		get_vm_lock(vcpu->vm);
		if (is_vcpu_running(target)) {
			ret = PSCI_RET_DENIED;
		} else if ((vcpu_get_state(target) == VCPU_INIT) ||
			is_vcpu_powered_off(target)) {
			arm64_prepare_linux_vcpu_context(target,
				vcpu->arch.regs.x2, vcpu->arch.regs.x3);
			ret = launch_vcpu(target) ? PSCI_RET_SUCCESS : PSCI_RET_DENIED;
		} else {
			ret = PSCI_RET_DENIED;
		}
		put_vm_lock(vcpu->vm);
	}
	hv_pm_end_vm_topology_change(vcpu->vm->vm_id);

	return ret;
}

static int64_t handle_psci_features(uint32_t fn)
{
	switch (fn) {
	case PSCI_0_2_FN_PSCI_VERSION:
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN_CPU_OFF:
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN_AFFINITY_INFO:
	case PSCI_0_2_FN_MIGRATE_INFO_TYPE:
	case PSCI_0_2_FN_SYSTEM_OFF:
	case PSCI_0_2_FN_SYSTEM_RESET:
	case PSCI_1_0_FN_PSCI_FEATURES:
	case PSCI_0_2_FN64_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_ON:
	case PSCI_0_2_FN64_AFFINITY_INFO:
		return PSCI_RET_SUCCESS;
	default:
		return PSCI_RET_NOT_SUPPORTED;
	}
}

static int32_t handle_psci64(struct acrn_vcpu *vcpu, bool advance_elr)
{
	uint32_t fn = (uint32_t)vcpu->arch.regs.x0;
	struct acrn_vcpu *target;
	uint64_t power_state;
	uint64_t entry_point;
	uint64_t context_id;
	int64_t ret;
	bool response_prepared = false;

	/*
	 * PSCI is the guest-visible CPU power-management ABI. The current QEMU
	 * model handles the calls needed to boot and stop vCPUs locally instead of
	 * forwarding SMC/HVC requests to firmware.
	 */
	switch (fn) {
	case PSCI_0_2_FN_PSCI_VERSION:
		ret = 0x00010000L;
		break;
	case PSCI_1_0_FN_PSCI_FEATURES:
		ret = handle_psci_features((uint32_t)vcpu->arch.regs.x1);
		break;
	case PSCI_0_2_FN_MIGRATE_INFO_TYPE:
		ret = PSCI_0_2_TOS_MP;
		break;
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_SUSPEND:
		power_state = (uint64_t)(uint32_t)vcpu->arch.regs.x1;
		entry_point = (fn == PSCI_0_2_FN64_CPU_SUSPEND) ?
			vcpu->arch.regs.x2 : (uint64_t)(uint32_t)vcpu->arch.regs.x2;
		context_id = (fn == PSCI_0_2_FN64_CPU_SUSPEND) ?
			vcpu->arch.regs.x3 : (uint64_t)(uint32_t)vcpu->arch.regs.x3;
		ret = arm64_vpsci_cpu_suspend(vcpu, power_state, entry_point,
			context_id, advance_elr);
		response_prepared = (ret == PSCI_RET_SUCCESS);
		break;
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN64_CPU_ON:
		ret = handle_psci_cpu_on(vcpu);
		break;
	case PSCI_0_2_FN_CPU_OFF:
		if (hv_pm_begin_vm_topology_change(vcpu->vm->vm_id) != 0) {
			ret = PSCI_RET_DENIED;
		} else {
			ret = poweroff_vcpu(vcpu) ? PSCI_RET_SUCCESS : PSCI_RET_DENIED;
			hv_pm_end_vm_topology_change(vcpu->vm->vm_id);
		}
		break;
	case PSCI_0_2_FN_AFFINITY_INFO:
	case PSCI_0_2_FN64_AFFINITY_INFO:
		target = psci_target_vcpu(vcpu->vm, vcpu->arch.regs.x1);
		ret = ((target != NULL) && is_vcpu_running(target)) ?
			PSCI_AFFINITY_LEVEL_ON : PSCI_AFFINITY_LEVEL_OFF;
		break;
	case PSCI_0_2_FN_SYSTEM_OFF:
		ret = arm64_vpsci_system_off(vcpu);
		break;
	case PSCI_0_2_FN_SYSTEM_RESET:
		ret = arm64_vpsci_system_reset(vcpu);
		break;
	case PSCI_1_0_FN_SYSTEM_SUSPEND:
	case PSCI_1_0_FN64_SYSTEM_SUSPEND:
		entry_point = (fn == PSCI_1_0_FN64_SYSTEM_SUSPEND) ?
			vcpu->arch.regs.x1 : (uint64_t)(uint32_t)vcpu->arch.regs.x1;
		context_id = (fn == PSCI_1_0_FN64_SYSTEM_SUSPEND) ?
			vcpu->arch.regs.x2 : (uint64_t)(uint32_t)vcpu->arch.regs.x2;
		ret = arm64_vpsci_system_suspend(vcpu, entry_point, context_id);
		response_prepared = (ret == PSCI_RET_SUCCESS);
		break;
	default:
		LOG_WRN("vm%u:vcpu%u unsupported psci call 0x%x",
			vcpu->vm->vm_id, vcpu->vcpu_id, fn);
		ret = PSCI_RET_NOT_SUPPORTED;
		break;
	}

	if (!response_prepared) {
		vcpu->arch.regs.x0 = (uint64_t)ret;
		if (advance_elr) {
			advance_vcpu_elr(vcpu);
		}
	}
	return 0;
}

static bool is_local_psci_smc(uint64_t function_id)
{
	switch (function_id) {
	case PSCI_0_2_FN_PSCI_VERSION:
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN_CPU_OFF:
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN_AFFINITY_INFO:
	case PSCI_0_2_FN_MIGRATE_INFO_TYPE:
	case PSCI_0_2_FN_SYSTEM_OFF:
	case PSCI_0_2_FN_SYSTEM_RESET:
	case PSCI_1_0_FN_PSCI_FEATURES:
	case PSCI_1_0_FN_SYSTEM_SUSPEND:
	case PSCI_1_0_FN64_SYSTEM_SUSPEND:
	case PSCI_0_2_FN64_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_ON:
	case PSCI_0_2_FN64_AFFINITY_INFO:
		return true;
	default:
		return false;
	}
}

static int32_t handle_smc64(struct acrn_vcpu *vcpu)
{
	uint64_t function_id = vcpu->arch.regs.x0;

	if (is_local_psci_smc(function_id)) {
		return handle_psci64(vcpu, true);
	}

	if (function_id == (uint64_t)TRUSTY_SMC_FC_API_VERSION) {
		vcpu->arch.regs.x0 = beau_trusty_smc(vcpu, function_id,
			vcpu->arch.regs.x1);
	} else {
		vcpu->arch.regs.x0 = SMC_UNK;
	}
	advance_vcpu_elr(vcpu);

	return 0;
}

static int32_t handle_hvc64(struct acrn_vcpu *vcpu)
{
	int32_t ret;

	if (arm64_is_acrn_hypercall(vcpu->arch.regs.x0)) {
		ret = arm64_dispatch_hypercall(vcpu);
	} else {
		ret = handle_psci64(vcpu, false);
	}

	return ret;
}

static int32_t handle_sysreg(struct acrn_vcpu *vcpu)
{
	struct cpu_regs *regs = &vcpu->arch.regs;
	uint64_t esr = regs->esr;
	uint64_t iss = esr & ((1UL << 25U) - 1UL);
	uint64_t sysreg = iss & ~(ESR_SYSREG_RT_MASK << ESR_SYSREG_RT_SHIFT) &
		~ESR_SYSREG_DIR_READ;
	uint32_t rt = (uint32_t)((iss >> ESR_SYSREG_RT_SHIFT) & ESR_SYSREG_RT_MASK);
	bool read = ((iss & ESR_SYSREG_DIR_READ) != 0UL);
	uint64_t *reg = arm64_gpr(regs, rt);
	uint64_t zero_reg = 0UL;
	int32_t ret = -EINVAL;

	if ((reg == NULL) && arm64_sysreg_zero_rt(rt, read)) {
		reg = &zero_reg;
	}

	/*
	 * ICC_SGI1R_EL1 is trapped so guest SGI sends become vGIC software state
	 * updates. Other system registers are left unsupported until a guest needs
	 * them; failing closed makes missing virtualization explicit.
	 *
	 *   ICC_SGI* -> SGI injection
	 *   ICC control -> vCPU interface shadow/control
	 *   CNT* -> timer shadow + vtimer update
	 */
	ret = arm64_vm_mpu_handle_sysreg(vcpu, sysreg, read, reg);
	if (ret == 0) {
		advance_vcpu_elr(vcpu);
	} else if (ret != -ENODATA) {
		/* vMPU owns the diagnostic for policy-denied architectural features. */
	} else if (((iss & ESR_SYSREG_DIR_READ) == 0UL) &&
		((sysreg == SYSREG_ICC_SGI1R_EL1) || (sysreg == SYSREG_ICC_SGI0R_EL1) ||
		(sysreg == SYSREG_ICC_ASGI1R_EL1)) &&
		(reg != NULL)) {
		struct arm64_core_pmu_path_token pmu_token;

		arm64_core_pmu_path_begin(&pmu_token);
		ret = arm64_vgicv3_handle_sgi1r(vcpu, *reg);
		arm64_core_pmu_path_end(ARM64_CORE_PMU_PATH_VGIC, &pmu_token);
		if (ret == 0) {
			advance_vcpu_elr(vcpu);
		}
	} else if ((sysreg == SYSREG_ICC_PMR_EL1) || (sysreg == SYSREG_ICC_CTLR_EL1) ||
		(sysreg == SYSREG_ICC_SRE_EL1) || (sysreg == SYSREG_ICC_IGRPEN1_EL1) ||
		(sysreg == SYSREG_ICC_DIR_EL1) || (sysreg == SYSREG_ICC_RPR_EL1)) {
		struct arm64_core_pmu_path_token pmu_token;
		uint32_t vgic_sysreg;

		switch (sysreg) {
		case SYSREG_ICC_PMR_EL1:
			vgic_sysreg = ARM64_VGIC_SYSREG_ICC_PMR_EL1;
			break;
		case SYSREG_ICC_DIR_EL1:
			vgic_sysreg = ARM64_VGIC_SYSREG_ICC_DIR_EL1;
			break;
		case SYSREG_ICC_RPR_EL1:
			vgic_sysreg = ARM64_VGIC_SYSREG_ICC_RPR_EL1;
			break;
		case SYSREG_ICC_CTLR_EL1:
			vgic_sysreg = ARM64_VGIC_SYSREG_ICC_CTLR_EL1;
			break;
		case SYSREG_ICC_SRE_EL1:
			vgic_sysreg = ARM64_VGIC_SYSREG_ICC_SRE_EL1;
			break;
		default:
			vgic_sysreg = ARM64_VGIC_SYSREG_ICC_IGRPEN1_EL1;
			break;
		}
		arm64_core_pmu_path_begin(&pmu_token);
		ret = arm64_vgicv3_handle_cpuif_sysreg(vcpu, vgic_sysreg, read, reg);
		arm64_core_pmu_path_end(ARM64_CORE_PMU_PATH_VGIC, &pmu_token);
		if (ret == 0) {
			advance_vcpu_elr(vcpu);
		}
	} else if (arm64_core_pmu_guest_sysreg(sysreg)
#if CONFIG_ARM64_SPE
		|| arm64_spe_guest_sysreg(sysreg)
#endif
		) {
		if (read && (reg != NULL)) {
			*reg = 0UL;
		}
		arm64_core_pmu_record_guest_access(vcpu);
#if CONFIG_ARM64_SPE
		if (arm64_spe_guest_sysreg(sysreg)) {
			arm64_spe_record_guest_access();
		}
#endif
		advance_vcpu_elr(vcpu);
		ret = 0;
	} else if (arm64_vtimer_sysreg(sysreg)) {
		ret = arm64_vtimer_handle_sysreg(vcpu, sysreg, read, reg);
		if (ret == 0) {
			advance_vcpu_elr(vcpu);
			if (!read) {
				arm64_vgicv3_update_current_vtimer(vcpu);
			}
		}
	}

	if (ret != 0) {
		uint32_t op0 = (uint32_t)((iss >> ESR_SYSREG_OP0_SHIFT) & ESR_SYSREG_OP0_MASK);
		uint32_t op1 = (uint32_t)((iss >> ESR_SYSREG_OP1_SHIFT) & ESR_SYSREG_OP1_MASK);
		uint32_t crn = (uint32_t)((iss >> ESR_SYSREG_CRN_SHIFT) & ESR_SYSREG_CRN_MASK);
		uint32_t crm = (uint32_t)((iss >> ESR_SYSREG_CRM_SHIFT) & ESR_SYSREG_CRM_MASK);
		uint32_t op2 = (uint32_t)((iss >> ESR_SYSREG_OP2_SHIFT) & ESR_SYSREG_OP2_MASK);

		LOG_ERR("unsupported arm64 sysreg trap vm%u:vcpu%u %s rt=%u op=%u:%u:c%u:c%u:%u esr=0x%lx elr=0x%lx",
			vcpu->vm->vm_id, vcpu->vcpu_id, read ? "read" : "write",
			rt, op0, op1, crn, crm, op2, esr, regs->elr);
	}

	return ret;
}

static int32_t handle_wfx(struct acrn_vcpu *vcpu)
{
	bool is_wfe = ESR_WFX_IS_WFE(vcpu->arch.regs.esr);
	bool pending_irq;
	bool irq_masked;
	bool request_pending;
	bool should_yield;

	advance_vcpu_elr(vcpu);

	/*
	 * WFI observes pending interrupts at the instruction boundary. Sample the
	 * loaded CNTV state before deciding whether this trapped WFI can yield;
	 * otherwise an expired guest timer could be missed until another exit.
	 *
	 *   sample vtimer -> update vGIC timer line -> complete stale active LRs
	 *        -> check LR/software pending state -> yield only if no event
	 */
	arm64_vgicv3_poll_current_vtimer(vcpu);
	arm64_vgicv3_update_current_vtimer(vcpu);
	arm64_vgicv3_complete_wfi_irqs(vcpu);
	pending_irq = arm64_vgicv3_has_pending_irq(vcpu);
	irq_masked = ((vcpu->arch.regs.spsr & DAIF_IRQ) != 0UL);
	request_pending = vcpu_has_pending_request(vcpu);
	/*
	 * WFI is allowed to complete on a pending interrupt even when EL1 still has
	 * PSTATE.I set. Linux relies on that: idle returns from WFI with IRQs masked,
	 * exits the idle accounting path, and only then executes local_irq_enable().
	 * Yield only when no virtual event is visible; otherwise return to EL1 so it
	 * can make forward progress to the unmask point.
	 */
	should_yield = is_wfe || (!request_pending && !pending_irq);

	/*
	 * Keep only cumulative predicates needed by vmstat/health. WFI can be a hot
	 * path, so it must not populate a per-vCPU trace or register snapshot.
	 */
	if (!is_wfe) {
		vcpu->arch.vtimer_diag.wfi_trap++;
		if (irq_masked) {
			vcpu->arch.vtimer_diag.wfi_irq_masked++;
		}
		if (pending_irq) {
			vcpu->arch.vtimer_diag.wfi_pending_irq++;
		}
	}

	/*
	 * This handler is normally reached only when a diagnostic enables WFI/WFE
	 * trapping. Keep the behavior lightweight: WFE yields, and WFI yields only
	 * when no virtual event is visible. A masked pending IRQ still has to return
	 * to EL1 so Linux can run out of the idle path and unmask interrupts.
	 */
	if (should_yield) {
		yield_current();
	}

	return 0;
}

static enum hwtdbg_guest_fault_reason vcpu_exit_fault_reason(uint64_t ec)
{
	if (ec == ESR_EL2_EC_IABT_LOW) {
		return HWTDBG_GUEST_FAULT_IABT;
	}
	if (ec == ESR_EL2_EC_SERROR) {
		return HWTDBG_GUEST_FAULT_SERROR;
	}
	return HWTDBG_GUEST_FAULT_UNHANDLED_EXIT;
}

int32_t vcpu_exit_handler(struct acrn_vcpu *vcpu)
{
	uint64_t ec = ESR_EL2_EC(vcpu->arch.regs.esr);
	int32_t ret = -EINVAL;

	/*
	 * ESR.EC identifies the architectural reason for the EL1-to-EL2 exit.
	 * Each handled class must either update the saved guest frame and advance
	 * ELR, or deliberately leave the vCPU stopped on failure.
	 */
	switch (ec) {
	case ESR_EL2_EC_IABT_LOW:
		ret = handle_instruction_abort(vcpu);
		break;
	case ESR_EL2_EC_SERROR:
		ret = handle_serror(vcpu);
		break;
	case ESR_EL2_EC_DABT_LOW:
		ret = handle_mmio_abort(vcpu);
		break;
	case ESR_EL2_EC_HVC64:
		ret = handle_hvc64(vcpu);
		break;
	case ESR_EL2_EC_SMC64:
		ret = handle_smc64(vcpu);
		break;
	case ESR_EL2_EC_SYSREG:
		ret = handle_sysreg(vcpu);
		break;
	case ESR_EL2_EC_SVE:
		ret = arm64_vm_mpu_handle_sve_trap(vcpu);
		break;
	case ESR_EL2_EC_WFI_WFE:
		ret = handle_wfx(vcpu);
		break;
	default:
		break;
	}

	TRACE_4I(TRACE_VM_EXIT, vcpu->vm->vm_id, vcpu->vcpu_id,
		ARM64_TRACE_EXIT_SYNC,
		(uint32_t)ret);

	if (ret == 0) {
		return 0;
	}

	LOG_ERR("unhandled arm64 vcpu exit vm%u:vcpu%u ec=0x%lx esr=0x%lx elr=0x%lx far=0x%lx hpfar=0x%lx",
		vcpu->vm->vm_id, vcpu->vcpu_id, ec, vcpu->arch.regs.esr,
		vcpu->arch.regs.elr, vcpu->arch.regs.far, vcpu->arch.regs.hpfar);
	hwtdbg_capture_guest_fault(vcpu, vcpu_exit_fault_reason(ec), ret);
	return -EINVAL;
}

void dispatch_vcpu_trap(struct cpu_regs *regs)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct acrn_vcpu *vcpu = get_exit_vcpu(pcpu_id);
	int32_t ret;

	/*
	 * Synchronous guest exits are handled on the current vCPU thread. The vector
	 * frame is temporary; emulation updates vcpu->arch.regs, and the final selected
	 * vCPU's saved frame is copied back before returning to assembly.
	 *
	 *   EL1 sync exit
	 *        |
	 *        v
	 *   save vector frame -> vcpu->arch.regs
	 *        |
	 *        v
	 *   vcpu_exit_handler()
	 *     - HVC/PSCI
	 *     - sysreg/vGIC/vtimer
	 *     - stage-2 data abort -> MMIO emulation
	 *        |
	 *        v
	 *   optional schedule / request processing
	 *        |
	 *        v
	 *   prepare vtimer/vGIC for next ERET
	 *        |
	 *        v
	 *   restore selected vCPU regs -> vector frame
	 */
	save_exit_regs(vcpu, regs);

	ret = vcpu_exit_handler(vcpu);
	if (ret < 0) {
		LOG_ERR("failed to handle arm64 vcpu exit vm%u:vcpu%u ret=%d",
			vcpu->vm->vm_id, vcpu->vcpu_id, ret);
		get_vm_lock(vcpu->vm);
		pause_vcpu(vcpu);
		put_vm_lock(vcpu->vm);
	}

	local_irq_disable();

	/*
	 * Do not preempt a vCPU that already has visible guest work. Linux can be
	 * sitting just after WFI with PSTATE.I still set; it needs a short return to
	 * EL1 to leave the idle path and unmask the pending virtual IRQ.
	 */
	vcpu = schedule_without_guest_resume(pcpu_id, vcpu);
	ret = arm64_process_vcpu_requests(vcpu);
	if (ret < 0) {
		LOG_FTL("failed to process arm64 vcpu requests vm%u:vcpu%u ret=%d",
			vcpu->vm->vm_id, vcpu->vcpu_id, ret);
		get_vm_lock(vcpu->vm);
		pause_vcpu(vcpu);
		put_vm_lock(vcpu->vm);
		schedule();
	}

	prepare_current_guest_resume(vcpu);
	restore_exit_regs(regs, vcpu);
}

void dispatch_vcpu_irq(struct cpu_regs *regs)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct acrn_vcpu *vcpu = get_exit_vcpu(pcpu_id);
	int32_t ret;

	/*
	 * Physical IRQs taken while a guest is running are handled without enabling
	 * nested softirq processing in the low-level IRQ path. Softirqs run once
	 * the interrupt is acknowledged. If the timer tick made another vCPU
	 * runnable, schedule before restoring the trap frame; otherwise a vCPU on a
	 * shared pCPU can immediately re-enter EL1 and starve the peer vCPU that is
	 * waiting for its time slice. Virtual interrupt state is resynced after the
	 * possible context switch and before returning to EL1.
	 *
	 *   physical IRQ -> IRQ handler/softirq -> optional schedule
	 *        -> poll/update vtimer -> process vCPU requests -> ERET
	 */
	save_exit_regs(vcpu, regs);
	TRACE_4I(TRACE_VM_EXIT, vcpu->vm->vm_id, vcpu->vcpu_id,
		ARM64_TRACE_EXIT_IRQ, 0U);

	dispatch_interrupt_no_softirq((const struct intr_excp_ctx *)regs);
	local_irq_disable();
	do_softirq_no_irqenable();

	/*
	 * A host tick often arrives while the guest is in the WFI return window.
	 * If a virtual IRQ is already materialized, let the guest retire enough
	 * instructions to unmask and handle it before honoring host reschedule.
	 */
	vcpu = schedule_without_guest_resume(pcpu_id, vcpu);
	ret = arm64_process_vcpu_requests(vcpu);
	if (ret < 0) {
		LOG_FTL("failed to process arm64 vcpu requests vm%u:vcpu%u ret=%d",
			vcpu->vm->vm_id, vcpu->vcpu_id, ret);
		get_vm_lock(vcpu->vm);
		pause_vcpu(vcpu);
		put_vm_lock(vcpu->vm);
		schedule();
	}

	prepare_current_guest_resume(vcpu);
	restore_exit_regs(regs, vcpu);
}
