/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SYSREG_H
#define ARM64_SYSREG_H

#include <types.h>

/*
 * ARM64 system-register quick reference for BEAU:
 *
 * MPIDR_EL1: CPU affinity identifier. The low affinity levels are used to map
 *            physical CPUs, redistributors, and guest vMPIDR values.
 * CurrentEL: current exception level. Boot code requires EL2 before enabling
 *            the hypervisor runtime.
 * DAIF:      PSTATE interrupt masks for Debug, SError, IRQ, and FIQ. Context
 *            switch code saves it so each task keeps its interrupt-mask state.
 * VBAR_EL2:  EL2 exception-vector base. All host and guest exits branch through
 *            the vector table installed here.
 * ELR_ELx:   exception link register. ERET resumes execution at this address.
 * SPSR_ELx:  saved PSTATE for exception return, including target EL and masks.
 * ESR_ELx:   exception syndrome, used to decode traps such as system-register
 *            accesses and data aborts.
 * FAR_ELx:   faulting virtual address for abort exceptions.
 * HPFAR_EL2: stage-2 fault IPA fragment for guest memory abort handling.
 *
 * Timer registers:
 * CNTFRQ_EL0 reports counter frequency, CNTP* is the EL1 physical timer,
 * CNTV* is the EL1 virtual timer, and CNTHP* is the EL2 host timer.
 * CNTHCTL_EL2 decides which EL1 timer/counter accesses are allowed or trapped.
 *
 * GIC system registers:
 * ICC_* registers are the EL1 CPU-interface view. ICH_* registers are the EL2
 * virtual CPU-interface state: HCR enables vGIC delivery, VMCR mirrors guest
 * control, LR<n> entries hold pending/active virtual interrupts, AP registers
 * hold active-priority state, and VTR reports hardware vGIC capacity.
 *
 * Virtualization registers:
 * HCR_EL2 enables stage-2 translation and controls EL1 trap routing. VTCR_EL2
 * describes the stage-2 table format, VTTBR_EL2 selects the VM's stage-2 root,
 * and VMPIDR_EL2 provides the guest-visible MPIDR value.
 *
 * Translation registers:
 * SCTLR_ELx enables MMU/cache behavior. TTBR*_ELx selects translation tables,
 * TCR_ELx describes address-size/cacheability/shareability, and MAIR_ELx maps
 * page-table attribute indexes to memory types.
 */
#define MPIDR_AFFINITY_MASK	0x00FFFFFFUL

/* CNTV_CTL_EL0 bits shared by virtual and emulated guest timer control paths. */
#define CNTV_CTL_ENABLE		(1U << 0U)
#define CNTV_CTL_IMASK		(1U << 1U)
#define CNTV_CTL_ISTATUS	(1U << 2U)

/*
 * CNTHCTL_EL2 controls EL1 timer access when HCR_EL2.E2H is clear. Bits 8/9
 * belong to the EL1 CNTKCTL view; EL1 virtual timer trapping uses EL1TVT.
 */
#define CNTHCTL_EL2_EL1PCTEN	(1UL << 0U)
#define CNTHCTL_EL2_EL1PCEN	(1UL << 1U)
#define CNTHCTL_EL2_ECV		(1UL << 12U)
#define CNTHCTL_EL2_EL1TVT	(1UL << 13U)
#define CNTHCTL_EL2_EL1TVCT	(1UL << 14U)

/* HCR_EL2 controls virtualization, interrupt routing, guest width, and traps. */
#define HCR_VM			(1UL << 0U)
#define HCR_IMO			(1UL << 4U)
#define HCR_FMO			(1UL << 3U)
#define HCR_AMO			(1UL << 5U)
#define HCR_TWI			(1UL << 13U)
#define HCR_TWE			(1UL << 14U)
#define HCR_RW			(1UL << 31U)
#define HCR_VI			(1UL << 7U)
#define HCR_VF			(1UL << 6U)
#define HCR_TSC			(1UL << 19U)

/* ICH_HCR_EL2 controls the virtual GIC CPU interface exposed to a vCPU. */
#define ICH_HCR_EN		(1UL << 0U)
#define ICH_HCR_UIE		(1UL << 1U)
#define ICH_HCR_LRENPIE		(1UL << 2U)
#define ICH_HCR_NPIE		(1UL << 3U)
#define ICH_HCR_TC		(1UL << 10U)
#define ICH_HCR_EOICOUNT_SHIFT	27U
/* EOIcount is hardware completion status, not control state to restore. */
#define ICH_HCR_EOICOUNT_MASK	(0x1fUL << ICH_HCR_EOICOUNT_SHIFT)

/* ICH_MISR_EL2 reports which virtual GIC maintenance conditions are asserted. */
#define ICH_MISR_EOI		(1UL << 0U)
#define ICH_MISR_U		(1UL << 1U)
#define ICH_MISR_LRENP		(1UL << 2U)
#define ICH_MISR_NP		(1UL << 3U)
#define ICH_MISR_VGRP1E		(1UL << 6U)
#define ICH_MISR_VGRP1D		(1UL << 7U)

/* ICH_VMCR_EL2 mirrors guest-visible GIC CPU-interface control state. */
#define ICH_VMCR_VENG1		(1UL << 1U)
#define ICH_VMCR_EOIM		(1UL << 9U)
#define ICH_VMCR_PRIORITY_SHIFT 24U
#define ICH_VMCR_PRIORITY_MASK	(0xffUL << ICH_VMCR_PRIORITY_SHIFT)
#define ICH_VMCR_DEFAULT_MASK	(0xf8UL << 24U)

/* ICH_LR<n> encodes one virtual interrupt presented through a list register. */
#define ICH_LR_VINTID_MASK	0xffffffffUL
#define ICH_LR_PINTID_SHIFT	32U
#define ICH_LR_EOI		(1UL << 41U)
#define ICH_LR_PRIORITY_SHIFT	48U
#define ICH_LR_GROUP1		(1UL << 60U)
#define ICH_LR_HW		(1UL << 61U)
#define ICH_LR_STATE_SHIFT	62U
#define ICH_LR_STATE_INVALID	0UL
#define ICH_LR_STATE_PENDING	1UL
#define ICH_LR_STATE_ACTIVE	2UL
#define ICH_LR_STATE_ACTIVE_PENDING 3UL

/* PAR_EL1 result fields used after an EL2 AT S1E1R guest-VA translation. */
#define PAR_EL1_F		(1UL << 0U)
#define PAR_EL1_PA_MASK		0x000ffffffffff000UL

/* ICC_SRE enables system-register access to the GIC CPU interface. */
#define ICC_CTLR_EL1_EOIMODE	(1UL << 1U)
#define ICC_SRE_SRE		(1UL << 0U)
#define ICC_SRE_DFB		(1UL << 1U)
#define ICC_SRE_DIB		(1UL << 2U)
#define ICC_SRE_ENABLE		(1UL << 3U)

#ifndef ASSEMBLER

static inline uint64_t read_mpidr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, mpidr_el1" : "=r" (val));
	return val;
}

static inline uint64_t read_currentel(void)
{
	uint64_t val;

	asm volatile ("mrs %0, CurrentEL" : "=r" (val));
	return val;
}

static inline uint64_t read_cntfrq_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntfrq_el0" : "=r" (val));
	return val;
}

static inline uint64_t read_cntpct_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntpct_el0" : "=r" (val));
	return val;
}

static inline uint64_t read_cntvct_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntvct_el0" : "=r" (val));
	return val;
}

static inline void write_cntv_tval_el0(uint32_t val)
{
	asm volatile ("msr cntv_tval_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_cntv_cval_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntv_cval_el0" : "=r" (val));
	return val;
}

static inline void write_cntv_cval_el0(uint64_t val)
{
	asm volatile ("msr cntv_cval_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint32_t read_cntv_ctl_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntv_ctl_el0" : "=r" (val));
	return (uint32_t)val;
}

static inline void write_cntv_ctl_el0(uint32_t val)
{
	asm volatile ("msr cntv_ctl_el0, %0; isb" : : "r" (val) : "memory");
}

static inline void write_cnthctl_el2(uint64_t val)
{
	asm volatile ("msr cnthctl_el2, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_cntvoff_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntvoff_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_cnthctl_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cnthctl_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_cnthp_cval_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cnthp_cval_el2" : "=r" (val));
	return val;
}

static inline void write_cnthp_cval_el2(uint64_t val)
{
	asm volatile ("msr cnthp_cval_el2, %0" : : "r" (val) : "memory");
}

static inline uint32_t read_cnthp_ctl_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cnthp_ctl_el2" : "=r" (val));
	return (uint32_t)val;
}

static inline void write_cnthp_ctl_el2(uint32_t val)
{
	asm volatile ("msr cnthp_ctl_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_cntp_tval_el0(uint32_t val)
{
	asm volatile ("msr cntp_tval_el0, %0" : : "r" (val));
}

static inline uint64_t read_cntp_cval_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntp_cval_el0" : "=r" (val));
	return val;
}

static inline void write_cntp_cval_el0(uint64_t val)
{
	asm volatile ("msr cntp_cval_el0, %0" : : "r" (val));
}

static inline uint32_t read_cntp_ctl_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntp_ctl_el0" : "=r" (val));
	return (uint32_t)val;
}

static inline void write_cntp_ctl_el0(uint32_t val)
{
	asm volatile ("msr cntp_ctl_el0, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_sgi1r_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c11_5, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_icc_iar1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_0" : "=r" (val));
	return val;
}

static inline void write_icc_eoir1_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_1, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_pmr_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c4_c6_0, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_bpr1_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_3, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_ctlr_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_4, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_igrpen1_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_7, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_icc_igrpen1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_7" : "=r" (val));
	return val;
}

static inline void write_icc_sre_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c9_5, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_sre_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_5, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_icc_sre_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_5" : "=r" (val));
	return val;
}

static inline uint64_t read_icc_pmr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c4_c6_0" : "=r" (val));
	return val;
}

static inline uint64_t read_icc_ctlr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_4" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_vtr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_1" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_hcr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_0" : "=r" (val));
	return val;
}

static inline void write_ich_hcr_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c11_0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ich_vmcr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_7" : "=r" (val));
	return val;
}

static inline void write_ich_vmcr_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c11_7, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ich_eisr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_3" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_misr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_2" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_elrsr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_5" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_lr_el2(uint8_t idx)
{
	uint64_t val = 0UL;

	switch (idx) {
	case 0U:
		asm volatile ("mrs %0, s3_4_c12_c12_0" : "=r" (val));
		break;
	case 1U:
		asm volatile ("mrs %0, s3_4_c12_c12_1" : "=r" (val));
		break;
	case 2U:
		asm volatile ("mrs %0, s3_4_c12_c12_2" : "=r" (val));
		break;
	case 3U:
		asm volatile ("mrs %0, s3_4_c12_c12_3" : "=r" (val));
		break;
	case 4U:
		asm volatile ("mrs %0, s3_4_c12_c12_4" : "=r" (val));
		break;
	case 5U:
		asm volatile ("mrs %0, s3_4_c12_c12_5" : "=r" (val));
		break;
	case 6U:
		asm volatile ("mrs %0, s3_4_c12_c12_6" : "=r" (val));
		break;
	case 7U:
		asm volatile ("mrs %0, s3_4_c12_c12_7" : "=r" (val));
		break;
	default:
		break;
	}

	return val;
}

static inline void write_ich_lr_el2(uint8_t idx, uint64_t val)
{
	switch (idx) {
	case 0U:
		asm volatile ("msr s3_4_c12_c12_0, %0; isb" : : "r" (val) : "memory");
		break;
	case 1U:
		asm volatile ("msr s3_4_c12_c12_1, %0; isb" : : "r" (val) : "memory");
		break;
	case 2U:
		asm volatile ("msr s3_4_c12_c12_2, %0; isb" : : "r" (val) : "memory");
		break;
	case 3U:
		asm volatile ("msr s3_4_c12_c12_3, %0; isb" : : "r" (val) : "memory");
		break;
	case 4U:
		asm volatile ("msr s3_4_c12_c12_4, %0; isb" : : "r" (val) : "memory");
		break;
	case 5U:
		asm volatile ("msr s3_4_c12_c12_5, %0; isb" : : "r" (val) : "memory");
		break;
	case 6U:
		asm volatile ("msr s3_4_c12_c12_6, %0; isb" : : "r" (val) : "memory");
		break;
	case 7U:
		asm volatile ("msr s3_4_c12_c12_7, %0; isb" : : "r" (val) : "memory");
		break;
	default:
		break;
	}
}

static inline uint64_t read_ich_ap0r0_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c8_0" : "=r" (val));
	return val;
}

static inline void write_ich_ap0r0_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c8_0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ich_ap1r0_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c9_0" : "=r" (val));
	return val;
}

static inline void write_ich_ap1r0_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c9_0, %0; isb" : : "r" (val) : "memory");
}

static inline void write_hcr_el2(uint64_t val)
{
	asm volatile ("msr hcr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vtcr_el2(uint64_t val)
{
	asm volatile ("msr vtcr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vttbr_el2(uint64_t val)
{
	asm volatile ("msr vttbr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vmpidr_el2(uint64_t val)
{
	asm volatile ("msr vmpidr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_sctlr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, sctlr_el1" : "=r" (val));
	return val;
}

static inline void write_sctlr_el1(uint64_t val)
{
	asm volatile ("msr sctlr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_cntkctl_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntkctl_el1" : "=r" (val));
	return val;
}

static inline void write_cntkctl_el1(uint64_t val)
{
	asm volatile ("msr cntkctl_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ttbr0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, ttbr0_el1" : "=r" (val));
	return val;
}

static inline void write_ttbr0_el1(uint64_t val)
{
	asm volatile ("msr ttbr0_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ttbr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, ttbr1_el1" : "=r" (val));
	return val;
}

static inline void write_ttbr1_el1(uint64_t val)
{
	asm volatile ("msr ttbr1_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tcr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tcr_el1" : "=r" (val));
	return val;
}

static inline void write_tcr_el1(uint64_t val)
{
	asm volatile ("msr tcr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_mair_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, mair_el1" : "=r" (val));
	return val;
}

static inline void write_mair_el1(uint64_t val)
{
	asm volatile ("msr mair_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_amair_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, amair_el1" : "=r" (val));
	return val;
}

static inline void write_amair_el1(uint64_t val)
{
	asm volatile ("msr amair_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_vbar_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, vbar_el1" : "=r" (val));
	return val;
}

static inline void write_vbar_el1(uint64_t val)
{
	asm volatile ("msr vbar_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_contextidr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, contextidr_el1" : "=r" (val));
	return val;
}

static inline void write_contextidr_el1(uint64_t val)
{
	asm volatile ("msr contextidr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_cpacr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cpacr_el1" : "=r" (val));
	return val;
}

static inline void write_cpacr_el1(uint64_t val)
{
	asm volatile ("msr cpacr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tpidr_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tpidr_el0" : "=r" (val));
	return val;
}

static inline void write_tpidr_el0(uint64_t val)
{
	asm volatile ("msr tpidr_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tpidrro_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tpidrro_el0" : "=r" (val));
	return val;
}

static inline void write_tpidrro_el0(uint64_t val)
{
	asm volatile ("msr tpidrro_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tpidr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tpidr_el1" : "=r" (val));
	return val;
}

static inline void write_tpidr_el1(uint64_t val)
{
	asm volatile ("msr tpidr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_sp_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, sp_el0" : "=r" (val));
	return val;
}

static inline void write_sp_el0(uint64_t val)
{
	asm volatile ("msr sp_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_elr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, elr_el1" : "=r" (val));
	return val;
}

static inline void write_elr_el1(uint64_t val)
{
	asm volatile ("msr elr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_spsr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, spsr_el1" : "=r" (val));
	return val;
}

static inline void write_spsr_el1(uint64_t val)
{
	asm volatile ("msr spsr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_esr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, esr_el1" : "=r" (val));
	return val;
}

static inline void write_esr_el1(uint64_t val)
{
	asm volatile ("msr esr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_far_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, far_el1" : "=r" (val));
	return val;
}

static inline void write_far_el1(uint64_t val)
{
	asm volatile ("msr far_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_afsr0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, afsr0_el1" : "=r" (val));
	return val;
}

static inline void write_afsr0_el1(uint64_t val)
{
	asm volatile ("msr afsr0_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_afsr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, afsr1_el1" : "=r" (val));
	return val;
}

static inline void write_afsr1_el1(uint64_t val)
{
	asm volatile ("msr afsr1_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_par_el1(void)
{
	uint64_t val;

	asm volatile ("dmb sy; mrs %0, par_el1; dmb sy" : "=r" (val) : : "memory");
	return val;
}

static inline void write_par_el1(uint64_t val)
{
	asm volatile ("msr par_el1, %0; isb" : : "r" (val) : "memory");
}

static inline void arm64_at_s1e1r(uint64_t va)
{
	asm volatile ("at s1e1r, %0; isb" : : "r" (va) : "memory");
}

static inline void write_vbar_el2(uint64_t val)
{
	asm volatile ("msr vbar_el2, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_elr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, elr_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_spsr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, spsr_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_esr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, esr_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_far_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, far_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_hpfar_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, hpfar_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_hcr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, hcr_el2" : "=r" (val));
	return val;
}

static inline uint64_t read_vttbr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, vttbr_el2" : "=r" (val));
	return val;
}

static inline void write_ttbr0_el2(uint64_t val)
{
	asm volatile ("msr ttbr0_el2, %0" : : "r" (val) : "memory");
}

static inline void write_tcr_el2(uint64_t val)
{
	asm volatile ("msr tcr_el2, %0" : : "r" (val) : "memory");
}

static inline void write_mair_el2(uint64_t val)
{
	asm volatile ("msr mair_el2, %0" : : "r" (val) : "memory");
}

static inline uint64_t read_sctlr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, sctlr_el2" : "=r" (val));
	return val;
}

static inline void write_sctlr_el2(uint64_t val)
{
	asm volatile ("msr sctlr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void flush_tlb_local(void)
{
	asm volatile ("dsb ishst; tlbi alle2; dsb ish; isb" ::: "memory");
}

static inline void flush_stage2_tlb_local(void)
{
	asm volatile ("dsb ishst; tlbi vmalls12e1; dsb ish; isb" ::: "memory");
}

#endif /* ASSEMBLER */

#endif /* ARM64_SYSREG_H */
