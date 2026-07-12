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

static inline uint64_t read_id_aa64pfr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c4_1" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64dfr0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c5_0" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64dfr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c5_1" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64isar0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c6_0" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64isar1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c6_1" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64mmfr0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c7_0" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64mmfr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c7_1" : "=r" (val));
	return val;
}

static inline uint64_t read_id_aa64mmfr2_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c0_c7_2" : "=r" (val));
	return val;
}

void arm64_vcpu_vsve_init(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_sve_state *sve;
	uint32_t vl_bits;

	if (vcpu == NULL) {
		return;
	}

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
		write_cptr_el2(cptr | CPTR_EL2_TZ);
		return;
	}

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
		if (!arm64_vcpu_mpu_sve_enabled(vcpu)) {
			val &= ~ID_AA64PFR0_SVE_MASK;
		}
		*value = val;
		break;
	case SYSREG_ID_AA64PFR1_EL1:
		*value = read_id_aa64pfr1_el1();
		break;
	case SYSREG_ID_AA64ZFR0_EL1:
		*value = arm64_vcpu_mpu_sve_enabled(vcpu) ? read_id_aa64zfr0_el1() : 0UL;
		break;
	case SYSREG_ID_AA64DFR0_EL1:
		*value = read_id_aa64dfr0_el1();
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
