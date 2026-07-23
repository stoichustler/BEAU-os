/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/cpu.h>
#include <asm/boot/macro.h>
#include <asm/trap.h>
#include <generated/offsets.h>

#define VCPU_RUN_STACK_SIZE	CPU_REGS_OFFSET_LAST

/*
 * [20260714] ARM64 guest entry principle
 *
 *   arch_vcpu_thread()
 *       |
 *       +--> arm64_process_vcpu_requests()
 *       |       materialize pending EL2 work in vcpu->arch
 *       |
 *       +--> arch_context_switch_in()
 *       |       already installed stage-2, EL1 sysregs, vGIC, and vtimer
 *       |
 *       v
 *   arm64_run_vcpu(vcpu->arch)
 *       |
 *       +--> copy durable vcpu->arch.regs into a temporary cpu_regs frame
 *       |    on the current vCPU thread's EL2 stack
 *       |
 *       +--> branch to vcpu_exit_return
 *       |       program ELR_EL2/SPSR_EL2/SP_EL1 and guest GPRs
 *       |
 *       v
 *   ERET to guest EL1
 *       |
 *       v
 *   guest trap/IRQ -> vector.S guest_vector_entry -> vcpu_exit.c
 *       |
 *       +--> save temporary vector frame back into vcpu->arch.regs
 *       +--> emulate, inject IRQs, process requests, or schedule
 *       +--> restore selected vCPU regs into the vector frame
 *       |
 *       v
 *   vcpu_exit_return -> ERET to guest EL1 again
 *
 * Design principle:
 *   - vcpu->arch.regs is the durable guest register image;
 *   - the EL2 stack frame is disposable and exists only to reuse the same
 *     restore code as trap return;
 *   - first guest entry and later guest resume share vcpu_exit_return, so ELR,
 *     SPSR, SP_EL1, and GPR restore ordering has one implementation;
 *   - this file does not own stage-2, vGIC, vtimer, SVE, or EL1 sysregs beyond
 *     the cpu_regs payload copied below; those are prepared by vCPU switch-in
 *     and resume paths before ERET.
 */

	.text
FUNC_ALIGN(arm64_run_vcpu, 4)
	/*
	 * Build the same cpu_regs layout that guest_vector_entry builds on an EL1
	 * exit.  Keeping entry and exit on one frame format prevents two subtly
	 * different definitions of "guest architectural state" from drifting apart.
	 */
	sub	sp, sp, #VCPU_RUN_STACK_SIZE
	/*
	 * x0 points at struct acrn_vcpu_arch.  Resolve its register image through a
	 * compiler-generated member offset so structure growth cannot silently move
	 * the source consumed here.  x17 names the temporary stack frame consumed by
	 * vcpu_exit_return.
	 */
	add	x16, x0, #ACRN_VCPU_ARCH_OFFSET_REGS
	mov	x17, sp
	/*
	 * Copy the durable guest register image pairwise into the stack frame.
	 * Pair loads intentionally follow cpu_regs order:
	 *
	 *   x0..x30, SP_EL1, ELR_EL2, SPSR_EL2, ESR/FAR/HPFAR diagnostics
	 *
	 * Diagnostic fields are copied too because the same frame is later visible
	 * to trap/IRQ code and shell diagnostics; they are not reinterpreted here.
	 */
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X0]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X0]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X2]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X2]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X4]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X4]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X6]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X6]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X8]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X8]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X10]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X10]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X12]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X12]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X14]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X14]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X16]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X16]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X18]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X18]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X20]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X20]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X22]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X22]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X24]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X24]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X26]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X26]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X28]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X28]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_LR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_LR]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_ELR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_ELR]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_ESR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_ESR]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_HPFAR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_HPFAR]
	ldr	x0, [x16, #CPU_REGS_OFFSET_EXC_SP]
	str	x0, [x17, #CPU_REGS_OFFSET_EXC_SP]
	/*
	 * Do not return with RET.  vcpu_exit_return consumes the frame above and
	 * performs the architectural transition to guest EL1 with ERET.  The next
	 * time EL2 code runs, it will be through the guest exception vector, not by
	 * falling back to the instruction after arm64_run_vcpu().
	 */
	b	vcpu_exit_return
END(arm64_run_vcpu)
