/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SYSREG_H
#define ARM64_SYSREG_H

#include <types.h>
#include <asm/instruction.h>
#include <asm/lib/barrier.h>

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
#define HCR_TID3		(1UL << 18U)
#define HCR_TSC			(1UL << 19U)

/*
 * CPTR_EL2.TZ traps EL1/EL0 SVE access to EL2. BEAU keeps it set unless the
 * vMPU policy grants SVE to the current VM and a matching vCPU context image
 * is loaded.
 */
#define CPTR_EL2_TZ		(1UL << 8U)

#define ID_AA64PFR0_SVE_SHIFT	32U
#define ID_AA64PFR0_SVE_MASK	(0xfUL << ID_AA64PFR0_SVE_SHIFT)

#define ID_AA64DFR0_PMUVER_SHIFT	8U
#define ID_AA64DFR0_PMUVER_MASK		(0xfUL << ID_AA64DFR0_PMUVER_SHIFT)

#define MDCR_EL2_HPMN_MASK	0x1fUL
#define MDCR_EL2_TPMCR		(1UL << 5U)
#define MDCR_EL2_TPM		(1UL << 6U)
#define MDCR_EL2_HPME		(1UL << 7U)
#define MDCR_EL2_HPMD		(1UL << 17U)
#define MDCR_EL2_HCCD		(1UL << 23U)

#define PMCR_EL0_E		(1UL << 0U)
#define PMCR_EL0_P		(1UL << 1U)
#define PMCR_EL0_C		(1UL << 2U)
#define PMCR_EL0_D		(1UL << 3U)
#define PMCR_EL0_X		(1UL << 4U)
#define PMCR_EL0_DP		(1UL << 5U)
#define PMCR_EL0_LC		(1UL << 6U)
#define PMCR_EL0_LP		(1UL << 7U)
#define PMCR_EL0_N_SHIFT	11U
#define PMCR_EL0_N_MASK		0x1fUL
#define PMCR_EL0_WRITABLE_MASK	(PMCR_EL0_E | PMCR_EL0_P | PMCR_EL0_C | \
	PMCR_EL0_D | PMCR_EL0_X | PMCR_EL0_DP | PMCR_EL0_LC | PMCR_EL0_LP)

#define PMU_EVENT_INCLUDE_EL2	(1UL << 27U)

#define ZCR_ELx_LEN_MASK	0xfUL

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

/* [20260716] ARM64 C system-register ownership
 *
 * named register helper -> generic access primitive -> MRS/MSR
 *                                      |
 *                                      +--> optional trailing ISB
 *
 * Key rules:
 *   - subsystem code uses named helpers when one exists;
 *   - generic primitives evaluate write values once and block compiler memory
 *     motion across register accesses;
 *   - synchronization is explicit: ordinary writes do not silently gain ISB;
 *   - this header is the only C implementation point for MRS and MSR.
 */
#define ARM64_SYSREG_STRINGIFY_INNER(token) #token
#define ARM64_SYSREG_STRINGIFY(token) ARM64_SYSREG_STRINGIFY_INNER(token)

#define arm64_sysreg_read(reg) ({ \
	uint64_t _arm64_sysreg_value; \
	asm volatile ("mrs %0, " ARM64_SYSREG_STRINGIFY(reg) \
		: "=r" (_arm64_sysreg_value) : : "memory"); \
	_arm64_sysreg_value; \
})

#define arm64_sysreg_write(reg, value) do { \
	uint64_t _arm64_sysreg_value = (uint64_t)(value); \
	asm volatile ("msr " ARM64_SYSREG_STRINGIFY(reg) ", %0" \
		: : "r" (_arm64_sysreg_value) : "memory"); \
} while (0)

#define arm64_sysreg_write_imm(reg, value) do { \
	asm volatile ("msr " ARM64_SYSREG_STRINGIFY(reg) ", %0" \
		: : "i" (value) : "memory"); \
} while (0)

#define arm64_sysreg_write_sync(reg, value) do { \
	arm64_sysreg_write(reg, value); \
	arm64_isb(); \
} while (0)

static inline uint64_t read_mpidr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(mpidr_el1);
	return val;
}

static inline uint64_t read_currentel(void)
{
	uint64_t val;

	val = arm64_sysreg_read(CurrentEL);
	return val;
}

static inline uint64_t read_cntfrq_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntfrq_el0);
	return val;
}

static inline uint64_t read_cntpct_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntpct_el0);
	return val;
}

static inline uint64_t read_cntvct_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntvct_el0);
	return val;
}

static inline void write_cntv_tval_el0(uint32_t val)
{
	arm64_sysreg_write_sync(cntv_tval_el0, val);
}

static inline uint64_t read_cntv_cval_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntv_cval_el0);
	return val;
}

static inline void write_cntv_cval_el0(uint64_t val)
{
	arm64_sysreg_write_sync(cntv_cval_el0, val);
}

static inline uint32_t read_cntv_ctl_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntv_ctl_el0);
	return (uint32_t)val;
}

static inline void write_cntv_ctl_el0(uint32_t val)
{
	arm64_sysreg_write_sync(cntv_ctl_el0, val);
}

static inline void write_cnthctl_el2(uint64_t val)
{
	arm64_sysreg_write_sync(cnthctl_el2, val);
}

static inline uint64_t read_cntvoff_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntvoff_el2);
	return val;
}

static inline uint64_t read_cnthctl_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cnthctl_el2);
	return val;
}

static inline uint64_t read_cnthp_cval_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cnthp_cval_el2);
	return val;
}

static inline void write_cnthp_cval_el2(uint64_t val)
{
	arm64_sysreg_write(cnthp_cval_el2, val);
}

static inline uint32_t read_cnthp_ctl_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cnthp_ctl_el2);
	return (uint32_t)val;
}

static inline void write_cnthp_ctl_el2(uint32_t val)
{
	arm64_sysreg_write_sync(cnthp_ctl_el2, val);
}

static inline void write_cntp_tval_el0(uint32_t val)
{
	arm64_sysreg_write(cntp_tval_el0, val);
}

static inline uint64_t read_cntp_cval_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntp_cval_el0);
	return val;
}

static inline void write_cntp_cval_el0(uint64_t val)
{
	arm64_sysreg_write(cntp_cval_el0, val);
}

static inline uint32_t read_cntp_ctl_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntp_ctl_el0);
	return (uint32_t)val;
}

static inline void write_cntp_ctl_el0(uint32_t val)
{
	arm64_sysreg_write_sync(cntp_ctl_el0, val);
}

static inline void write_icc_sgi1r_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c11_5, val);
}

static inline uint64_t read_icc_iar1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c12_c12_0);
	return val;
}

static inline void write_icc_eoir1_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c12_1, val);
}

static inline void write_icc_pmr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c4_c6_0, val);
}

static inline void write_icc_bpr1_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c12_3, val);
}

static inline void write_icc_ctlr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c12_4, val);
}

static inline void write_icc_igrpen1_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c12_7, val);
}

static inline uint64_t read_icc_igrpen1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c12_c12_7);
	return val;
}

static inline void write_icc_sre_el2(uint64_t val)
{
	arm64_sysreg_write_sync(s3_4_c12_c9_5, val);
}

static inline void write_icc_sre_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c12_5, val);
}

static inline uint64_t read_icc_sre_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c12_c12_5);
	return val;
}

static inline uint64_t read_icc_pmr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c4_c6_0);
	return val;
}

static inline uint64_t read_icc_ctlr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c12_c12_4);
	return val;
}

static inline uint64_t read_ich_vtr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c11_1);
	return val;
}

static inline uint64_t read_ich_hcr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c11_0);
	return val;
}

static inline void write_ich_hcr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(s3_4_c12_c11_0, val);
}

static inline uint64_t read_ich_vmcr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c11_7);
	return val;
}

static inline void write_ich_vmcr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(s3_4_c12_c11_7, val);
}

static inline uint64_t read_ich_eisr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c11_3);
	return val;
}

static inline uint64_t read_ich_misr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c11_2);
	return val;
}

static inline uint64_t read_ich_elrsr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c11_5);
	return val;
}

static inline uint64_t read_ich_lr_el2(uint8_t idx)
{
	uint64_t val = 0UL;

	switch (idx) {
	case 0U:
		val = arm64_sysreg_read(s3_4_c12_c12_0);
		break;
	case 1U:
		val = arm64_sysreg_read(s3_4_c12_c12_1);
		break;
	case 2U:
		val = arm64_sysreg_read(s3_4_c12_c12_2);
		break;
	case 3U:
		val = arm64_sysreg_read(s3_4_c12_c12_3);
		break;
	case 4U:
		val = arm64_sysreg_read(s3_4_c12_c12_4);
		break;
	case 5U:
		val = arm64_sysreg_read(s3_4_c12_c12_5);
		break;
	case 6U:
		val = arm64_sysreg_read(s3_4_c12_c12_6);
		break;
	case 7U:
		val = arm64_sysreg_read(s3_4_c12_c12_7);
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
		arm64_sysreg_write_sync(s3_4_c12_c12_0, val);
		break;
	case 1U:
		arm64_sysreg_write_sync(s3_4_c12_c12_1, val);
		break;
	case 2U:
		arm64_sysreg_write_sync(s3_4_c12_c12_2, val);
		break;
	case 3U:
		arm64_sysreg_write_sync(s3_4_c12_c12_3, val);
		break;
	case 4U:
		arm64_sysreg_write_sync(s3_4_c12_c12_4, val);
		break;
	case 5U:
		arm64_sysreg_write_sync(s3_4_c12_c12_5, val);
		break;
	case 6U:
		arm64_sysreg_write_sync(s3_4_c12_c12_6, val);
		break;
	case 7U:
		arm64_sysreg_write_sync(s3_4_c12_c12_7, val);
		break;
	default:
		break;
	}
}

static inline uint64_t read_ich_ap0r0_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c8_0);
	return val;
}

static inline void write_ich_ap0r0_el2(uint64_t val)
{
	arm64_sysreg_write_sync(s3_4_c12_c8_0, val);
}

static inline uint64_t read_ich_ap1r0_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c12_c9_0);
	return val;
}

static inline void write_ich_ap1r0_el2(uint64_t val)
{
	arm64_sysreg_write_sync(s3_4_c12_c9_0, val);
}

static inline void write_hcr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(hcr_el2, val);
}

static inline uint64_t read_cptr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cptr_el2);
	return val;
}

static inline void write_cptr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(cptr_el2, val);
}

static inline uint64_t read_id_aa64pfr0_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(id_aa64pfr0_el1);
	return val;
}

static inline uint64_t read_id_aa64dfr0_el1(void)
{
	return arm64_sysreg_read(s3_0_c0_c5_0);
}

static inline uint64_t read_pmcr_el0(void)
{
	return arm64_sysreg_read(pmcr_el0);
}

static inline void write_pmcr_el0(uint64_t val)
{
	arm64_sysreg_write(pmcr_el0, val);
	arm64_isb();
}

static inline uint64_t read_pmceid0_el0(void)
{
	return arm64_sysreg_read(pmceid0_el0);
}

static inline uint64_t read_pmceid1_el0(void)
{
	return arm64_sysreg_read(pmceid1_el0);
}

static inline uint64_t read_pmccntr_el0(void)
{
	return arm64_sysreg_read(pmccntr_el0);
}

static inline void write_pmccntr_el0(uint64_t val)
{
	arm64_sysreg_write(pmccntr_el0, val);
}

static inline void write_pmcntenset_el0(uint64_t val)
{
	arm64_sysreg_write(pmcntenset_el0, val);
}

static inline void write_pmcntenclr_el0(uint64_t val)
{
	arm64_sysreg_write(pmcntenclr_el0, val);
}

static inline void write_pmintenclr_el1(uint64_t val)
{
	arm64_sysreg_write(pmintenclr_el1, val);
}

static inline uint64_t read_pmovsclr_el0(void)
{
	return arm64_sysreg_read(pmovsclr_el0);
}

static inline void write_pmovsclr_el0(uint64_t val)
{
	arm64_sysreg_write(pmovsclr_el0, val);
}

static inline void write_pmccfiltr_el0(uint64_t val)
{
	arm64_sysreg_write(pmccfiltr_el0, val);
}

static inline void write_pmuserenr_el0(uint64_t val)
{
	arm64_sysreg_write(pmuserenr_el0, val);
}

#define ARM64_PMU_COUNTER_HELPERS(n) \
static inline uint64_t read_pmevcntr##n##_el0(void) \
{ \
	return arm64_sysreg_read(pmevcntr##n##_el0); \
} \
static inline void write_pmevcntr##n##_el0(uint64_t val) \
{ \
	arm64_sysreg_write(pmevcntr##n##_el0, val); \
} \
static inline void write_pmevtyper##n##_el0(uint64_t val) \
{ \
	arm64_sysreg_write(pmevtyper##n##_el0, val); \
}

ARM64_PMU_COUNTER_HELPERS(0)
ARM64_PMU_COUNTER_HELPERS(1)
ARM64_PMU_COUNTER_HELPERS(2)
ARM64_PMU_COUNTER_HELPERS(3)
ARM64_PMU_COUNTER_HELPERS(4)
ARM64_PMU_COUNTER_HELPERS(5)

#undef ARM64_PMU_COUNTER_HELPERS

static inline uint64_t read_ctr_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(ctr_el0);
	return val;
}

static inline uint64_t read_clidr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(clidr_el1);
	return val;
}

static inline void write_csselr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(csselr_el1, val);
}

static inline uint64_t read_ccsidr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(ccsidr_el1);
	return val;
}

static inline uint64_t read_id_aa64zfr0_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c4_4);
	return val;
}

static inline uint64_t read_zcr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c1_c2_0);
	return val;
}

static inline void write_zcr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c1_c2_0, val);
}

static inline uint64_t read_zcr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_4_c1_c2_0);
	return val;
}

static inline void write_zcr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(s3_4_c1_c2_0, val);
}

static inline uint64_t read_fpsr(void)
{
	uint64_t val;

	val = arm64_sysreg_read(fpsr);
	return val;
}

static inline void write_fpsr(uint64_t val)
{
	arm64_sysreg_write(fpsr, val);
}

static inline uint64_t read_fpcr(void)
{
	uint64_t val;

	val = arm64_sysreg_read(fpcr);
	return val;
}

static inline void write_fpcr(uint64_t val)
{
	arm64_sysreg_write(fpcr, val);
}

static inline void write_vtcr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(vtcr_el2, val);
}

static inline void write_vttbr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(vttbr_el2, val);
}

static inline void write_vmpidr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(vmpidr_el2, val);
}

static inline uint64_t read_sctlr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(sctlr_el1);
	return val;
}

static inline void write_sctlr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(sctlr_el1, val);
}

static inline uint64_t read_cntkctl_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cntkctl_el1);
	return val;
}

static inline void write_cntkctl_el1(uint64_t val)
{
	arm64_sysreg_write_sync(cntkctl_el1, val);
}

static inline uint64_t read_ttbr0_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(ttbr0_el1);
	return val;
}

static inline void write_ttbr0_el1(uint64_t val)
{
	arm64_sysreg_write_sync(ttbr0_el1, val);
}

static inline uint64_t read_ttbr1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(ttbr1_el1);
	return val;
}

static inline void write_ttbr1_el1(uint64_t val)
{
	arm64_sysreg_write_sync(ttbr1_el1, val);
}

static inline uint64_t read_tcr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(tcr_el1);
	return val;
}

static inline void write_tcr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(tcr_el1, val);
}

static inline uint64_t read_mair_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(mair_el1);
	return val;
}

static inline void write_mair_el1(uint64_t val)
{
	arm64_sysreg_write_sync(mair_el1, val);
}

static inline uint64_t read_amair_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(amair_el1);
	return val;
}

static inline void write_amair_el1(uint64_t val)
{
	arm64_sysreg_write_sync(amair_el1, val);
}

static inline uint64_t read_vbar_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(vbar_el1);
	return val;
}

static inline void write_vbar_el1(uint64_t val)
{
	arm64_sysreg_write_sync(vbar_el1, val);
}

static inline uint64_t read_contextidr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(contextidr_el1);
	return val;
}

static inline void write_contextidr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(contextidr_el1, val);
}

static inline uint64_t read_cpacr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(cpacr_el1);
	return val;
}

static inline void write_cpacr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(cpacr_el1, val);
}

static inline uint64_t read_tpidr_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(tpidr_el0);
	return val;
}

static inline void write_tpidr_el0(uint64_t val)
{
	arm64_sysreg_write_sync(tpidr_el0, val);
}

static inline uint64_t read_tpidrro_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(tpidrro_el0);
	return val;
}

static inline void write_tpidrro_el0(uint64_t val)
{
	arm64_sysreg_write_sync(tpidrro_el0, val);
}

static inline uint64_t read_tpidr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(tpidr_el1);
	return val;
}

static inline void write_tpidr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(tpidr_el1, val);
}

static inline uint64_t read_sp_el0(void)
{
	uint64_t val;

	val = arm64_sysreg_read(sp_el0);
	return val;
}

static inline void write_sp_el0(uint64_t val)
{
	arm64_sysreg_write_sync(sp_el0, val);
}

static inline uint64_t read_elr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(elr_el1);
	return val;
}

static inline void write_elr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(elr_el1, val);
}

static inline uint64_t read_spsr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(spsr_el1);
	return val;
}

static inline void write_spsr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(spsr_el1, val);
}

static inline uint64_t read_esr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(esr_el1);
	return val;
}

static inline void write_esr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(esr_el1, val);
}

static inline uint64_t read_far_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(far_el1);
	return val;
}

static inline void write_far_el1(uint64_t val)
{
	arm64_sysreg_write_sync(far_el1, val);
}

static inline uint64_t read_afsr0_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(afsr0_el1);
	return val;
}

static inline void write_afsr0_el1(uint64_t val)
{
	arm64_sysreg_write_sync(afsr0_el1, val);
}

static inline uint64_t read_afsr1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(afsr1_el1);
	return val;
}

static inline void write_afsr1_el1(uint64_t val)
{
	arm64_sysreg_write_sync(afsr1_el1, val);
}

static inline uint64_t read_par_el1(void)
{
	uint64_t val;

	arm64_dmb_sy();
	val = arm64_sysreg_read(par_el1);
	arm64_dmb_sy();
	return val;
}

static inline void write_par_el1(uint64_t val)
{
	arm64_sysreg_write_sync(par_el1, val);
}

static inline void arm64_at_s1e1r(uint64_t va)
{
	arm64_at(s1e1r, va);
	arm64_isb();
}

static inline void write_vbar_el2(uint64_t val)
{
	arm64_sysreg_write_sync(vbar_el2, val);
}

static inline uint64_t read_elr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(elr_el2);
	return val;
}

static inline uint64_t read_spsr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(spsr_el2);
	return val;
}

static inline uint64_t read_esr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(esr_el2);
	return val;
}

static inline uint64_t read_far_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(far_el2);
	return val;
}

static inline uint64_t read_hpfar_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(hpfar_el2);
	return val;
}

static inline uint64_t read_hcr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(hcr_el2);
	return val;
}

static inline uint64_t read_vttbr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(vttbr_el2);
	return val;
}

static inline void write_ttbr0_el2(uint64_t val)
{
	arm64_sysreg_write(ttbr0_el2, val);
}

static inline void write_tcr_el2(uint64_t val)
{
	arm64_sysreg_write(tcr_el2, val);
}

static inline void write_mair_el2(uint64_t val)
{
	arm64_sysreg_write(mair_el2, val);
}

static inline uint64_t read_sctlr_el2(void)
{
	uint64_t val;

	val = arm64_sysreg_read(sctlr_el2);
	return val;
}

static inline void write_sctlr_el2(uint64_t val)
{
	arm64_sysreg_write_sync(sctlr_el2, val);
}

static inline void flush_tlb_local(void)
{
	arm64_dsb_ishst();
	arm64_tlbi(alle2);
	arm64_dsb_ish();
	arm64_isb();
}

static inline void flush_stage2_tlb_local(void)
{
	arm64_dsb_ishst();
	arm64_tlbi(vmalls12e1);
	arm64_dsb_ish();
	arm64_isb();
}

#endif /* ASSEMBLER */

#endif /* ARM64_SYSREG_H */
