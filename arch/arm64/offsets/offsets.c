/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/gen_offset.h>
#include <asm/cpu.h>
#include <asm/hv_pm.h>
#include <asm/guest/vcpu.h>

/* [20260716] Compiler-owned assembly structure layout
 *
 * C structure definitions
 *         |
 *         v
 * global absolute ELF symbols
 *         |
 *         v
 * generated/offsets.h -> ARM64 assembly
 *
 * Key rule:
 *   - C structure definitions own every offset and size value;
 *   - the build publishes the generated header before compiling assembly;
 *   - generation failure leaves no partial layout for assembly to consume.
 */
GEN_ABS_SYM_BEGIN(arm64_offset_abs_syms)

GEN_OFFSET_SYM(struct cpu_regs, x0, CPU_REGS_OFFSET_X0);
GEN_OFFSET_SYM(struct cpu_regs, x1, CPU_REGS_OFFSET_X1);
GEN_OFFSET_SYM(struct cpu_regs, x2, CPU_REGS_OFFSET_X2);
GEN_OFFSET_SYM(struct cpu_regs, x3, CPU_REGS_OFFSET_X3);
GEN_OFFSET_SYM(struct cpu_regs, x4, CPU_REGS_OFFSET_X4);
GEN_OFFSET_SYM(struct cpu_regs, x5, CPU_REGS_OFFSET_X5);
GEN_OFFSET_SYM(struct cpu_regs, x6, CPU_REGS_OFFSET_X6);
GEN_OFFSET_SYM(struct cpu_regs, x7, CPU_REGS_OFFSET_X7);
GEN_OFFSET_SYM(struct cpu_regs, x8, CPU_REGS_OFFSET_X8);
GEN_OFFSET_SYM(struct cpu_regs, x9, CPU_REGS_OFFSET_X9);
GEN_OFFSET_SYM(struct cpu_regs, x10, CPU_REGS_OFFSET_X10);
GEN_OFFSET_SYM(struct cpu_regs, x11, CPU_REGS_OFFSET_X11);
GEN_OFFSET_SYM(struct cpu_regs, x12, CPU_REGS_OFFSET_X12);
GEN_OFFSET_SYM(struct cpu_regs, x13, CPU_REGS_OFFSET_X13);
GEN_OFFSET_SYM(struct cpu_regs, x14, CPU_REGS_OFFSET_X14);
GEN_OFFSET_SYM(struct cpu_regs, x15, CPU_REGS_OFFSET_X15);
GEN_OFFSET_SYM(struct cpu_regs, x16, CPU_REGS_OFFSET_X16);
GEN_OFFSET_SYM(struct cpu_regs, x17, CPU_REGS_OFFSET_X17);
GEN_OFFSET_SYM(struct cpu_regs, x18, CPU_REGS_OFFSET_X18);
GEN_OFFSET_SYM(struct cpu_regs, x19, CPU_REGS_OFFSET_X19);
GEN_OFFSET_SYM(struct cpu_regs, x20, CPU_REGS_OFFSET_X20);
GEN_OFFSET_SYM(struct cpu_regs, x21, CPU_REGS_OFFSET_X21);
GEN_OFFSET_SYM(struct cpu_regs, x22, CPU_REGS_OFFSET_X22);
GEN_OFFSET_SYM(struct cpu_regs, x23, CPU_REGS_OFFSET_X23);
GEN_OFFSET_SYM(struct cpu_regs, x24, CPU_REGS_OFFSET_X24);
GEN_OFFSET_SYM(struct cpu_regs, x25, CPU_REGS_OFFSET_X25);
GEN_OFFSET_SYM(struct cpu_regs, x26, CPU_REGS_OFFSET_X26);
GEN_OFFSET_SYM(struct cpu_regs, x27, CPU_REGS_OFFSET_X27);
GEN_OFFSET_SYM(struct cpu_regs, x28, CPU_REGS_OFFSET_X28);
GEN_OFFSET_SYM(struct cpu_regs, x29, CPU_REGS_OFFSET_X29);
GEN_OFFSET_SYM(struct cpu_regs, lr, CPU_REGS_OFFSET_LR);
GEN_OFFSET_SYM(struct cpu_regs, sp, CPU_REGS_OFFSET_SP);
GEN_OFFSET_SYM(struct cpu_regs, elr, CPU_REGS_OFFSET_ELR);
GEN_OFFSET_SYM(struct cpu_regs, spsr, CPU_REGS_OFFSET_SPSR);
GEN_OFFSET_SYM(struct cpu_regs, esr, CPU_REGS_OFFSET_ESR);
GEN_OFFSET_SYM(struct cpu_regs, far, CPU_REGS_OFFSET_FAR);
GEN_OFFSET_SYM(struct cpu_regs, hpfar, CPU_REGS_OFFSET_HPFAR);
GEN_OFFSET_SYM(struct cpu_regs, host_tpidr, CPU_REGS_OFFSET_HOST_TPIDR);
GEN_OFFSET_SYM(struct cpu_regs, exc_sp, CPU_REGS_OFFSET_EXC_SP);
GEN_OFFSET_SYM(struct cpu_regs, reserved, CPU_REGS_OFFSET_RESERVED);
GEN_SIZE_SYM(struct cpu_regs, CPU_REGS_OFFSET_LAST);

GEN_OFFSET_SYM(struct stack_frame, x19, STACK_FRAME_OFFSET_X19);
GEN_OFFSET_SYM(struct stack_frame, x20, STACK_FRAME_OFFSET_X20);
GEN_OFFSET_SYM(struct stack_frame, x21, STACK_FRAME_OFFSET_X21);
GEN_OFFSET_SYM(struct stack_frame, x22, STACK_FRAME_OFFSET_X22);
GEN_OFFSET_SYM(struct stack_frame, x23, STACK_FRAME_OFFSET_X23);
GEN_OFFSET_SYM(struct stack_frame, x24, STACK_FRAME_OFFSET_X24);
GEN_OFFSET_SYM(struct stack_frame, x25, STACK_FRAME_OFFSET_X25);
GEN_OFFSET_SYM(struct stack_frame, x26, STACK_FRAME_OFFSET_X26);
GEN_OFFSET_SYM(struct stack_frame, x27, STACK_FRAME_OFFSET_X27);
GEN_OFFSET_SYM(struct stack_frame, x28, STACK_FRAME_OFFSET_X28);
GEN_OFFSET_SYM(struct stack_frame, x29, STACK_FRAME_OFFSET_X29);
GEN_OFFSET_SYM(struct stack_frame, lr, STACK_FRAME_OFFSET_LR);
GEN_OFFSET_SYM(struct stack_frame, x0, STACK_FRAME_OFFSET_X0);
GEN_OFFSET_SYM(struct stack_frame, spsr, STACK_FRAME_OFFSET_SPSR);
GEN_OFFSET_SYM(struct stack_frame, magic, STACK_FRAME_OFFSET_MAGIC);
GEN_OFFSET_SYM(struct stack_frame, reserved, STACK_FRAME_OFFSET_RESERVED);
GEN_SIZE_SYM(struct stack_frame, STACK_FRAME_SIZE);

GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X19,
	offsetof(struct arm64_suspend_callee_context, x19_x30[0U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X20,
	offsetof(struct arm64_suspend_callee_context, x19_x30[1U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X21,
	offsetof(struct arm64_suspend_callee_context, x19_x30[2U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X22,
	offsetof(struct arm64_suspend_callee_context, x19_x30[3U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X23,
	offsetof(struct arm64_suspend_callee_context, x19_x30[4U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X24,
	offsetof(struct arm64_suspend_callee_context, x19_x30[5U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X25,
	offsetof(struct arm64_suspend_callee_context, x19_x30[6U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X26,
	offsetof(struct arm64_suspend_callee_context, x19_x30[7U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X27,
	offsetof(struct arm64_suspend_callee_context, x19_x30[8U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X28,
	offsetof(struct arm64_suspend_callee_context, x19_x30[9U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X29,
	offsetof(struct arm64_suspend_callee_context, x19_x30[10U]));
GEN_ABSOLUTE_SYM(ARM64_SUSPEND_CALLEE_OFFSET_X30,
	offsetof(struct arm64_suspend_callee_context, x19_x30[11U]));
GEN_OFFSET_SYM(struct arm64_suspend_callee_context, sp,
	ARM64_SUSPEND_CALLEE_OFFSET_SP);
GEN_SIZE_SYM(struct arm64_suspend_callee_context, ARM64_SUSPEND_CALLEE_SIZE);

GEN_OFFSET_SYM(struct arm64_el2_pm_context, vbar_el2, ARM64_EL2_PM_OFFSET_VBAR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, sctlr_el2, ARM64_EL2_PM_OFFSET_SCTLR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, tcr_el2, ARM64_EL2_PM_OFFSET_TCR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, ttbr0_el2, ARM64_EL2_PM_OFFSET_TTBR0_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, mair_el2, ARM64_EL2_PM_OFFSET_MAIR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, hcr_el2, ARM64_EL2_PM_OFFSET_HCR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, vtcr_el2, ARM64_EL2_PM_OFFSET_VTCR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, vttbr_el2, ARM64_EL2_PM_OFFSET_VTTBR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, cptr_el2, ARM64_EL2_PM_OFFSET_CPTR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, cnthctl_el2, ARM64_EL2_PM_OFFSET_CNTHCTL_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, cntvoff_el2, ARM64_EL2_PM_OFFSET_CNTVOFF_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, mdcr_el2, ARM64_EL2_PM_OFFSET_MDCR_EL2);
GEN_OFFSET_SYM(struct arm64_el2_pm_context, tpidr_el2, ARM64_EL2_PM_OFFSET_TPIDR_EL2);
GEN_SIZE_SYM(struct arm64_el2_pm_context, ARM64_EL2_PM_SIZE);

GEN_OFFSET_SYM(struct arm64_host_pm_context, callee, ARM64_HOST_PM_OFFSET_CALLEE);
GEN_OFFSET_SYM(struct arm64_host_pm_context, el2, ARM64_HOST_PM_OFFSET_EL2);
GEN_OFFSET_SYM(struct arm64_host_pm_context, epoch, ARM64_HOST_PM_OFFSET_EPOCH);
GEN_OFFSET_SYM(struct arm64_host_pm_context, valid, ARM64_HOST_PM_OFFSET_VALID);
GEN_SIZE_SYM(struct arm64_host_pm_context, ARM64_HOST_PM_SIZE);

GEN_OFFSET_SYM(struct acrn_vcpu_arch, regs, ACRN_VCPU_ARCH_OFFSET_REGS);

GEN_ABS_SYM_END
