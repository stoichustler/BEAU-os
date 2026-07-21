/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <logmsg.h>
#include <rtl.h>
#include <vcpu.h>
#include <vm.h>
#include <asm/sve.h>
#include <asm/sysreg.h>
#include <asm/guest/vmpu.h>
#include <asm/guest/vsve.h>

#define ARM64_SYSREG_ENC(op0, op1, crn, crm, op2) \
	(((op0) << 20U) | ((op2) << 17U) | ((op1) << 14U) | ((crn) << 10U) | ((crm) << 1U))

#define SYSREG_ID_AA64PFR0_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 4UL, 0UL)
#define SYSREG_ID_AA64PFR1_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 4UL, 1UL)
#define SYSREG_ID_AA64PFR2_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 4UL, 2UL)
#define SYSREG_ID_AA64ZFR0_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 4UL, 4UL)
#define SYSREG_ID_AA64DFR0_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 5UL, 0UL)
#define SYSREG_ID_AA64DFR1_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 5UL, 1UL)
#define SYSREG_ID_AA64ISAR0_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 6UL, 0UL)
#define SYSREG_ID_AA64ISAR1_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 6UL, 1UL)
#define SYSREG_ID_AA64ISAR2_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 6UL, 2UL)
#define SYSREG_ID_AA64MMFR0_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 7UL, 0UL)
#define SYSREG_ID_AA64MMFR1_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 7UL, 1UL)
#define SYSREG_ID_AA64MMFR2_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 7UL, 2UL)
#define SYSREG_ID_AA64MMFR3_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 7UL, 3UL)
#define SYSREG_ID_AA64MMFR4_EL1		ARM64_SYSREG_ENC(3UL, 0UL, 0UL, 7UL, 4UL)
#define SYSREG_ZCR_EL1			ARM64_SYSREG_ENC(3UL, 0UL, 1UL, 2UL, 0UL)

/* [20260712] vSVE framework:
 *
 * Guest SVE virtualization has three jobs:
 *
 *   1. expose truthful ID registers to EL1;
 *   2. gate actual SVE execution with CPTR_EL2.TZ;
 *   3. save/restore the scalable Z/P/FFR state on vCPU switches.
 *
 * Flow:
 *
 *   vMPU policy says SVE disabled
 *        -> ID_AA64PFR0_EL1.SVE is hidden
 *        -> ID_AA64ZFR0_EL1 reads as zero
 *        -> CPTR_EL2.TZ remains set, SVE execution traps
 *
 *   vMPU policy says SVE enabled
 *        -> guest sees SVE ID fields
 *        -> vCPU load clears CPTR_EL2.TZ
 *        -> ZCR_EL2 caps hardware VL to VM configured VL
 *        -> guest ZCR_EL1 writes are clamped to that cap
 *        -> vCPU unload saves Z0-Z31, P0-P15 and FFR
 *
 * This supports vector-length agnostic guest software while still giving EL2 a
 * deterministic upper bound for context size and scheduling cost.
 */
static inline uint64_t read_id_aa64pfr1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c4_1);
	return val;
}

static inline uint64_t read_id_aa64dfr1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c5_1);
	return val;
}

static inline uint64_t read_id_aa64isar0_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c6_0);
	return val;
}

static inline uint64_t read_id_aa64isar1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c6_1);
	return val;
}

static inline uint64_t read_id_aa64mmfr0_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c7_0);
	return val;
}

static inline uint64_t read_id_aa64mmfr1_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c7_1);
	return val;
}

static inline uint64_t read_id_aa64mmfr2_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c0_c7_2);
	return val;
}

void arm64_vcpu_vsve_init(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_sve_state *sve;
	uint32_t vl_bits;

	if (vcpu == NULL) {
		return;
	}

	/*
	 * Initialize the per-vCPU SVE context with the VM configured vector length.
	 * A zero VM setting falls back to the platform default. The context is
	 * valid even before the VM is allowed to execute SVE, so later policy
	 * changes do not need to rebuild the save area.
	 */
	sve = &vcpu->arch.sve;
	(void)memset(sve, 0U, sizeof(*sve));
	vl_bits = arm64_vm_mpu_sve_vl_bits(vcpu->vm);
	if (vl_bits == 0U) {
		vl_bits = ARM64_SVE_VL_BITS_DEFAULT;
	}
	sve->vl_bits = vl_bits;
	sve->vl_bytes = vl_bits / 8U;
	sve->regs.zcr_el1 = arm64_sve_zcr_len_from_bits(vl_bits);
	sve->valid = true;
}

void arm64_vcpu_vsve_load(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_sve_state *sve;
	uint64_t cptr;
	uint64_t zcr_len;

	if (vcpu == NULL) {
		return;
	}

	cptr = read_cptr_el2();
	if (!arm64_vcpu_mpu_sve_enabled(vcpu)) {
		/*
		 * CPTR_EL2.TZ forces SVE/FP-related access to trap instead of executing
		 * with stale host or previous-guest state. Disabled RTOS VMs stay on
		 * this path.
		 */
		write_cptr_el2(cptr | CPTR_EL2_TZ);
		return;
	}

	/*
	 * Load order:
	 *
	 *   allow EL2 SVE use -> set ZCR_EL2 VM maximum -> restore guest ZCR_EL1
	 *   within that maximum -> restore scalable register state
	 */
	sve = &vcpu->arch.sve;
	zcr_len = arm64_sve_zcr_len_from_bits(sve->vl_bits);
	write_cptr_el2(cptr & ~CPTR_EL2_TZ);
	write_zcr_el2(zcr_len);
	write_zcr_el1(arm64_sve_clamp_zcr(sve->regs.zcr_el1, sve->vl_bits));
	arm64_sve_restore_state(&sve->regs);
}

void arm64_vcpu_vsve_unload(struct acrn_vcpu *vcpu)
{
	uint64_t cptr;

	if (vcpu == NULL) {
		return;
	}

	if (arm64_vcpu_mpu_sve_enabled(vcpu)) {
		arm64_sve_save_state(&vcpu->arch.sve.regs);
	}

	/*
	 * Always re-enable SVE trapping on unload. The next vCPU must explicitly
	 * pass vMPU policy before SVE instructions can run.
	 */
	cptr = read_cptr_el2();
	write_cptr_el2(cptr | CPTR_EL2_TZ);
}

static bool arm64_vsve_feature_id_sysreg(uint64_t sysreg)
{
	uint64_t op0 = (sysreg >> 20U) & 0x3UL;
	uint64_t op1 = (sysreg >> 14U) & 0x7UL;
	uint64_t crn = (sysreg >> 10U) & 0xfUL;
	uint64_t crm = (sysreg >> 1U) & 0xfUL;

	return (op0 == 3UL) && (op1 == 0UL) && (crn == 0UL) &&
		(crm >= 1UL) && (crm <= 7UL);
}

static bool arm64_vsve_read_id_value(const struct acrn_vcpu *vcpu,
	uint64_t sysreg, uint64_t *value)
{
	uint64_t val;

	if (value == NULL) {
		return false;
	}

	switch (sysreg) {
	case SYSREG_ID_AA64PFR0_EL1:
		val = read_id_aa64pfr0_el1();
		/*
		 * ID filtering is the architectural contract with the guest. If SVE is
		 * not active for this VM, EL1 must not discover SVE even if the host
		 * CPU supports it.
		 */
		if (!arm64_vcpu_mpu_sve_enabled(vcpu)) {
			val &= ~ID_AA64PFR0_SVE_MASK;
		}
			*value = val;
			break;
		case SYSREG_ID_AA64PFR1_EL1:
			/*
			 * BEAU does not virtualize guest allocation tags. Hide the base,
			 * fractional, and extended MTE fields together so EL1 cannot infer
			 * a feature whose state HCR_EL2 deliberately blocks.
			 */
			val = read_id_aa64pfr1_el1();
			val &= ~(ID_AA64PFR1_MTE_MASK |
				ID_AA64PFR1_MTE_FRAC_MASK | ID_AA64PFR1_MTEX_MASK);
			*value = val;
			break;
	case SYSREG_ID_AA64ZFR0_EL1:
		*value = arm64_vcpu_mpu_sve_enabled(vcpu) ? read_id_aa64zfr0_el1() : 0UL;
		break;
	case SYSREG_ID_AA64DFR0_EL1:
		*value = read_id_aa64dfr0_el1() &
			~(ID_AA64DFR0_PMUVER_MASK | ID_AA64DFR0_PMSVER_MASK);
		break;
	case SYSREG_ID_AA64DFR1_EL1:
		*value = read_id_aa64dfr1_el1();
		break;
	case SYSREG_ID_AA64ISAR0_EL1:
		*value = read_id_aa64isar0_el1();
		break;
	case SYSREG_ID_AA64ISAR1_EL1:
		*value = read_id_aa64isar1_el1();
		break;
	case SYSREG_ID_AA64MMFR0_EL1:
		*value = read_id_aa64mmfr0_el1();
		break;
	case SYSREG_ID_AA64MMFR1_EL1:
		*value = read_id_aa64mmfr1_el1();
		break;
	case SYSREG_ID_AA64MMFR2_EL1:
		*value = read_id_aa64mmfr2_el1();
		break;
	case SYSREG_ID_AA64PFR2_EL1:
	case SYSREG_ID_AA64ISAR2_EL1:
	case SYSREG_ID_AA64MMFR3_EL1:
	case SYSREG_ID_AA64MMFR4_EL1:
		*value = 0UL;
		break;
	default:
		if (!arm64_vsve_feature_id_sysreg(sysreg)) {
			return false;
		}
		*value = 0UL;
		break;
	}

	return true;
}

static int32_t arm64_vsve_handle_zcr_el1(struct acrn_vcpu *vcpu, bool read,
	uint64_t *reg)
{
	struct arm64_vcpu_sve_state *sve;

	if ((vcpu == NULL) || (reg == NULL) || !arm64_vcpu_mpu_sve_enabled(vcpu)) {
		return -EACCES;
	}

	sve = &vcpu->arch.sve;
	if (read) {
		*reg = sve->regs.zcr_el1;
	} else {
		/*
		 * Guest ZCR_EL1 selects a VL at or below the VM maximum. Clamping keeps
		 * guest software free to request a smaller VL while preventing it from
		 * expanding the EL2 context beyond the configured bound.
		 */
		sve->regs.zcr_el1 = arm64_sve_clamp_zcr(*reg, sve->vl_bits);
		write_zcr_el1(sve->regs.zcr_el1);
	}

	return 0;
}

int32_t arm64_vsve_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg)
{
	uint64_t value;

	if (sysreg == SYSREG_ZCR_EL1) {
		return arm64_vsve_handle_zcr_el1(vcpu, read, reg);
	}

	if (!read) {
		return -ENODATA;
	}
	if (arm64_vsve_read_id_value(vcpu, sysreg, &value)) {
		if (reg == NULL) {
			return -EINVAL;
		}
		*reg = value;
		return 0;
	}

	return -ENODATA;
}

int32_t arm64_vsve_handle_trap(struct acrn_vcpu *vcpu)
{
	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return -EINVAL;
	}

	LOG_ERR("arm64 vsve denied SVE access vm%u:vcpu%u esr=0x%lx elr=0x%lx",
		vcpu->vm->vm_id, vcpu->vcpu_id, vcpu->arch.regs.esr,
		vcpu->arch.regs.elr);
	return -EACCES;
}
