/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vcpu.h>
#include <vm.h>
#include <event.h>
#include <errno.h>
#include <logmsg.h>
#include <schedule.h>
#include <trace.h>
#include <asm/irq.h>
#include <asm/cpu.h>
#include <asm/pmu.h>
#include <asm/security.h>
#include <asm/sysreg.h>
#include <asm/trap.h>
#include <asm/guest/vcpu_priv.h>
#include <asm/guest/vm_reset.h>
#include <asm/guest/vmpu.h>
#include <asm/guest/virq.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vgicv3.h>

/* [20260630] vCPU scheduling coverage:
 *
 * vCPU virtualization principle:
 *
 * BEAU models each vCPU as a scheduler thread. The thread owns a durable guest
 * state image, while the physical CPU holds a live copy only between
 * arch_context_switch_in() and arch_context_switch_out().
 *
 *   durable vCPU state                         live pCPU state
 *   +----------------------+    switch in     +----------------------+
 *   | vcpu->arch.regs      | ---------------> | EL1 GPR/sysregs      |
 *   | vcpu->arch.gctx      |                  | HCR/VTTBR/VTCR       |
 *   | vcpu->arch.vgic      |                  | GIC list registers   |
 *   | vtimer shadow state  |                  | CNTV registers       |
 *   +----------------------+                  +----------------------+
 *             ^                                         |
 *             |                  switch out             |
 *             +-----------------------------------------+
 *
 * Guest execution is therefore a repeated ownership handoff:
 *
 *   scheduler picks vCPU thread
 *          |
 *          v
 *   load_vcpu()
 *     - install stage-2 root through VTTBR/VTCR
 *     - restore guest EL1 sysregs and saved GPR frame
 *     - load vtimer state and vGIC LRs
 *          |
 *          v
 *   ERET to EL1 guest
 *          |
 *          v
 *   trap / IRQ / request exits to EL2
 *          |
 *          v
 *   handle exit, sync vGIC/vtimer, update vcpu->arch.regs
 *          |
 *          v
 *   either re-enter guest or save state on scheduler switch-out
 *
 * The persistent register block is not used as the live EL2 stack. Guest entry
 * builds a temporary restore frame on the vCPU thread stack, and guest exits
 * copy the hardware frame back into vcpu->arch.regs. This keeps scheduler
 * context switches independent from guest register save/restore mechanics.
 */
#define SPSR_EL2_MODE_EL1H	0x5UL
#define ARM64_GUEST_SCTLR_EL1_INIT	0x00c50078UL

void vcpu_set_elr(struct acrn_vcpu *vcpu, uint64_t val)
{
	vcpu->arch.regs.elr = val;
}

static void arm64_init_guest_regs(struct cpu_regs *regs, uint64_t entry, uint64_t x0)
{
	uint64_t host_tpidr = regs->host_tpidr;
	uint64_t exc_sp = regs->exc_sp;

	/*
	 * Keep the EL2-private return fields intact; guest-visible GPRs and
	 * exception state are reset to the Linux boot ABI before EL1 entry.
	 */
	(void)memset(regs, 0U, sizeof(*regs));
	regs->host_tpidr = host_tpidr;
	regs->exc_sp = exc_sp;
	regs->x0 = x0;
	regs->elr = entry;
	regs->spsr = SPSR_EL2_MODE_EL1H | DAIF_ALL;
}

static void arm64_init_guest_control_context(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	/*
	 * Linux enters with EL1 MMU/data cache disabled and with architected EL1
	 * state known, not inherited from the pCPU that happened to create the
	 * vCPU. Values here mirror the reset-style context used by established ARM
	 * hypervisors while keeping BEAU's stage-2 and timer virtualization state.
	 */
	(void)memset(gctx, 0U, sizeof(*gctx));
	gctx->vttbr_el2 = arm64_stage2_vttbr(vcpu->vm);
	gctx->vtcr_el2 = VTCR_EL2_VALUE;
	/* Guest MTE is not virtualized; HCR_EL2.ATA must remain fail-closed. */
	gctx->hcr_el2 = (HCR_VM | HCR_RW | HCR_IMO | HCR_FMO | HCR_AMO |
		HCR_TSC) & ~HCR_ATA;
	gctx->cntvoff_el2 = (uint64_t)vcpu->vm->arch_vm.time_delta;
	gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	gctx->sctlr_el1 = ARM64_GUEST_SCTLR_EL1_INIT;
	arm64_vcpu_mpu_init(vcpu);
}

void arm64_prepare_linux_vcpu_context(struct acrn_vcpu *vcpu, uint64_t entry, uint64_t x0)
{
	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return;
	}

	arm64_vtimer_cancel_all(vcpu);
	vcpu->pending_req = 0UL;
	vcpu->arch.irqs_pending = 0UL;
	vcpu->arch.irqs_pending_mask = 0UL;
	vcpu->arch.trap.esr = EXCEPTION_INVALID;
	arm64_init_guest_regs(&vcpu->arch.regs, entry, x0);
	arm64_init_guest_control_context(vcpu);
	arm64_vgicv3_reset_vcpu_boot_state(vcpu);
}

uint64_t arch_vcpu_get_entry(const struct acrn_vcpu *vcpu)
{
	return (vcpu != NULL) ? vcpu->arch.regs.elr : 0UL;
}

void arm64_vtimer_diag_mark_pre_eret(struct acrn_vcpu *vcpu,
	bool flushed, bool masked_expired)
{
	struct arm64_vcpu_vtimer_diag *diag;

	if (vcpu == NULL) {
		return;
	}

	diag = &vcpu->arch.vtimer_diag;
	if (flushed) {
		diag->pre_eret_flush++;
		if (masked_expired) {
			diag->pre_eret_flush_expired++;
		}
	}
}

/*
 * Restore the guest-owned EL1 system register image before returning to EL1.
 * Translation attributes and base registers are written before SCTLR_EL1 so
 * the MMU enable state observes a complete address-space definition. The local
 * TLB flush removes translations that may have been cached for the previous
 * vCPU that occupied this pCPU.
 */
static void restore_el1_sysregs(const struct arm64_vcpu_guest_ctx *gctx)
{
	write_ttbr0_el1(gctx->ttbr0_el1);
	write_ttbr1_el1(gctx->ttbr1_el1);
	write_tcr_el1(gctx->tcr_el1);
	write_mair_el1(gctx->mair_el1);
	write_amair_el1(gctx->amair_el1);
	write_vbar_el1(gctx->vbar_el1);
	write_contextidr_el1(gctx->contextidr_el1);
	write_cpacr_el1(gctx->cpacr_el1);
	write_tpidr_el0(gctx->tpidr_el0);
	write_tpidrro_el0(gctx->tpidrro_el0);
	write_tpidr_el1(gctx->tpidr_el1);
	write_sp_el0(gctx->sp_el0);
	write_elr_el1(gctx->elr_el1);
	write_spsr_el1(gctx->spsr_el1);
	write_esr_el1(gctx->esr_el1);
	write_far_el1(gctx->far_el1);
	write_afsr0_el1(gctx->afsr0_el1);
	write_afsr1_el1(gctx->afsr1_el1);
	write_par_el1(gctx->par_el1);
	write_cntkctl_el1(gctx->cntkctl_el1);
	write_sctlr_el1(gctx->sctlr_el1);
	flush_tlb_local();
}

/*
 * Snapshot all EL1 state that can affect guest execution after a context
 * switch. This is required for shared-pCPU scheduling: the hardware registers
 * are physically per-core, but the architectural state must follow the vCPU
 * thread as it is descheduled and later resumed.
 */
static void save_el1_sysregs(struct arm64_vcpu_guest_ctx *gctx)
{
	gctx->sctlr_el1 = read_sctlr_el1();
	gctx->ttbr0_el1 = read_ttbr0_el1();
	gctx->ttbr1_el1 = read_ttbr1_el1();
	gctx->tcr_el1 = read_tcr_el1();
	gctx->mair_el1 = read_mair_el1();
	gctx->amair_el1 = read_amair_el1();
	gctx->vbar_el1 = read_vbar_el1();
	gctx->contextidr_el1 = read_contextidr_el1();
	gctx->cpacr_el1 = read_cpacr_el1();
	gctx->tpidr_el0 = read_tpidr_el0();
	gctx->tpidrro_el0 = read_tpidrro_el0();
	gctx->tpidr_el1 = read_tpidr_el1();
	gctx->sp_el0 = read_sp_el0();
	gctx->elr_el1 = read_elr_el1();
	gctx->spsr_el1 = read_spsr_el1();
	gctx->esr_el1 = read_esr_el1();
	gctx->far_el1 = read_far_el1();
	gctx->afsr0_el1 = read_afsr0_el1();
	gctx->afsr1_el1 = read_afsr1_el1();
	gctx->par_el1 = read_par_el1();
	gctx->cntkctl_el1 = read_cntkctl_el1();
}

void load_vcpu(__unused struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	arm64_core_pmu_vcpu_load(vcpu);

	/*
	 * VTTBR/VTCR select the VM's stage-2 table and VMPIDR gives the guest its
	 * virtual CPU identity. CNTV is direct guest hardware state saved/restored
	 * on vCPU switches, while guest CNTP stays trapped and is emulated with
	 * host software timers. The host scheduler tick uses CNTHP and is not part
	 * of the EL1 timer image.
	 *
	 * The EL1 register image and vGIC state must be loaded before guest entry
	 * so address translation, exception return state, and pending virtual
	 * interrupts are visible immediately after ERET.
	 */
	arm64_vtimer_cancel_all(vcpu);
	write_vtcr_el2(gctx->vtcr_el2);
	write_vttbr_el2(gctx->vttbr_el2);
	write_vmpidr_el2(vcpu_get_vmpidr(vcpu));
	/* [20260626] vCPU/vtimer principle:
	 *
	 *   CNTVCT_EL0 read  -> hardware CNTVCT = CNTPCT - CNTVOFF_EL2
	 *   CNTV timer regs  -> live CNTV saved/restored on vCPU switch
	 *   CNTP timer regs  -> EL2 trap -> guest shadow -> host timer
	 *
	 * A57/A73-class CPUs cannot rely on virtual timer traps for CNTV_CTL_EL0.
	 * Keep the virtual timer architectural and avoid hiding ISTATUS from
	 * Linux; leave EL1PCEN clear so physical timer accesses still trap for
	 * CNTP emulation.
	 */
	write_cnthctl_el2(CNTHCTL_EL2_EL1PCTEN);
	arm64_sysreg_write_sync(cntvoff_el2, gctx->cntvoff_el2);
	restore_el1_sysregs(gctx);
	arm64_vcpu_mpu_load(vcpu);
	arm64_vtimer_load_current(vcpu);
	arm64_vgicv3_load_vcpu(vcpu);
	uint64_t hcr = gctx->hcr_el2;

	/* EL2-only PAC: zero API/APK denies guest PAC instructions and key access. */
	hcr &= ~(HCR_API | HCR_APK);
	write_hcr_el2(hcr);
}

void unload_vcpu(__unused struct acrn_vcpu *vcpu)
{
	/*
	 * Save guest-owned EL1 state before clearing EL2 virtualization state.
	 * CNTV is disabled after its compare/control registers are captured so an
	 * expired deadline from this vCPU cannot interrupt the next vCPU scheduled
	 * on the same pCPU. Clearing HCR/VTTBR removes guest execution context before
	 * the host scheduler continues; stage-2 TLBI is reserved for map/unmap
	 * changes because VTTBR now carries a per-VM VMID.
	 */
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	save_el1_sysregs(gctx);
	arm64_vcpu_mpu_unload(vcpu);
	arm64_vtimer_save_current(vcpu);
	arm64_vgicv3_update_current_vtimer(vcpu);
	arm64_vtimer_disable_current();
	arm64_vgicv3_save_vcpu(vcpu);
	arm64_vgicv3_arm_cntv_timer(vcpu);
	write_hcr_el2(0UL);
	write_vttbr_el2(0UL);
	arm64_core_pmu_vcpu_unload(vcpu);
}

void flush_vcpu_context(__unused struct acrn_vcpu *vcpu)
{
}

bool is_vcpu_context_updated(__unused struct acrn_vcpu *vcpu)
{
	return false;
}

void vcpu_mark_context_dirty(__unused struct acrn_vcpu *vcpu)
{
}

int32_t arm64_process_vcpu_requests(struct acrn_vcpu *vcpu)
{
	if (vcpu_has_pending_request(vcpu)) {
		struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint64_t flags;

		/*
		 * Requests are the cross-CPU handoff path for work that must be
		 * materialized before returning to EL1. Trap injection updates the
		 * saved guest frame; virtual IRQ injection syncs software IRQ state
		 * with the hardware list registers under the VM vGIC lock.
		 */
		if (vcpu_take_request(vcpu, ARM64_VCPU_REQUEST_EXCEPTION)) {
			vcpu_set_trap(vcpu, &vcpu->arch.trap);
			memset(&vcpu->arch.trap, 0, sizeof(struct arm64_vcpu_trap_info));
			vcpu->arch.trap.esr = EXCEPTION_INVALID;
		}

		spinlock_irqsave_obtain(&vgic->lock, &flags);
		arm64_vgicv3_sync_current_vcpu(vcpu);
		(void)vcpu_inject_pending_intr(vcpu);
		arm64_vgicv3_flush_current_vcpu(vcpu);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}

	return 0;
}

int32_t arch_init_vcpu(struct acrn_vcpu *vcpu)
{
	/*
	 * Each vCPU points at the VM's stage-2 root and starts with EL1 AArch64
	 * enabled. HCR routes guest physical interrupts to EL2 and traps PSCI.
	 * WFI/WFE are left to the virtual CPU interface by default; trapping them
	 * on every guest idle/spin instruction is too expensive for the QEMU 3OS
	 * scenario and is only kept as a handler for future diagnostic modes.
	 */
	reset_vcpu(vcpu);
	return 0;
}

void arch_deinit_vcpu(__unused struct acrn_vcpu *vcpu)
{
	arm64_vtimer_cancel_all(vcpu);
}

void arch_vcpu_thread(struct thread_object *obj)
{
	struct acrn_vcpu *vcpu = container_of(obj, struct acrn_vcpu, thread_obj);
	int32_t ret;

	/* [20260714] vCPU thread run loop
	 *
	 * The scheduler sees this as a normal thread. The thread body is the guest
	 * run loop: materialize pending EL2 work, enter EL1, then return here through
	 * the assembly exit path when a trap or physical IRQ occurs.
	 *
	 *   scheduler pick
	 *        |
	 *        v
	 *   arch_context_switch_in()
	 *     - install VTTBR/VTCR
	 *     - restore EL1 sysregs, vGIC, vtimer
	 *        |
	 *        v
	 *   arch_vcpu_thread()
	 *     - process pending requests
	 *     - trace guest-entry boundary
	 *     - arm64_run_vcpu()
	 *        |
	 *        v
	 *   ERET to guest EL1
	 *        |
	 *        v
	 *   vcpu_exit.c dispatch
	 *     - emulate trap or handle IRQ
	 *     - maybe schedule another vCPU
	 *     - restore trap frame for return
	 *
	 * Key rule: durable state lives in vcpu->arch. Guest entry/exit uses a
	 * temporary stack frame so scheduler context switches stay independent from
	 * guest register save/restore.
	 */
	while (true) {
		while (!is_vcpu_running(vcpu)) {
			sleep_thread(obj);
			schedule();
		}

		ret = arm64_process_vcpu_requests(vcpu);
		if (ret < 0) {
			pause_vcpu(vcpu);
			continue;
		}
		TRACE_4I(TRACE_VM_ENTER, vcpu->vm->vm_id, vcpu->vcpu_id, 0U, 0U);
		arm64_run_vcpu(&vcpu->arch);
	}
}

void arch_reset_vcpu(struct acrn_vcpu *vcpu)
{
	arm64_vtimer_cancel_all(vcpu);
	memset(&vcpu->arch, 0, sizeof(vcpu->arch));
	arm64_prepare_linux_vcpu_context(vcpu, 0UL, 0UL);
}

void arch_context_switch_out(struct thread_object *prev)
{
	struct acrn_vcpu *vcpu = container_of(prev, struct acrn_vcpu, thread_obj);

	unload_vcpu(vcpu);
}

void arch_context_switch_in(struct thread_object *next)
{
	struct acrn_vcpu *vcpu = container_of(next, struct acrn_vcpu, thread_obj);

	load_vcpu(vcpu);
}

uint64_t arch_setup_thread_stack(struct thread_object *obj, uint8_t *stack, uint64_t stack_size)
{
	uint64_t stack_base = (uint64_t)stack;
	uint64_t stacktop;
	struct stack_frame *frame;

	if (obj != NULL) {
		obj->host_stack_base = 0UL;
		obj->host_stack_size = 0UL;
	}
	if ((obj == NULL) || (stack == NULL) ||
		(stack_size < sizeof(struct stack_frame)) ||
		(stack_base > (UINT64_MAX - stack_size))) {
		return 0UL;
	}
	stacktop = stack_base + stack_size;
	if (((stack_base | stacktop) & (CPU_STACK_ALIGN - 1UL)) != 0UL) {
		return 0UL;
	}
	#if CONFIG_ARM64_PTRAUTH
	if (!arm64_ptrauth_prepare_thread(obj)) {
		return 0UL;
	}
	#endif
	obj->host_stack_base = stack_base;
	obj->host_stack_size = stack_size;
	frame = (struct stack_frame *)stacktop;

	frame -= 1;
	memset(frame, 0, sizeof(struct stack_frame));

	frame->magic = SP_BOTTOM_MAGIC;
	frame->lr = (uint64_t)obj->thread_entry;
	frame->x0 = (uint64_t)obj;
	frame->spsr = read_daif();

	return (uint64_t)&frame->x19;
}

uint64_t arch_build_stack_frame(struct acrn_vcpu *vcpu)
{
	return arch_setup_thread_stack(&vcpu->thread_obj, vcpu->stack, CONFIG_STACK_SIZE);
}
