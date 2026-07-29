/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <logmsg.h>
#include <asm/esr.h>
#include <asm/trap.h>

#define ARM64_ESR_ISS_MASK		((1UL << 25U) - 1UL)
#define ARM64_ESR_EC_BRK64		0x3cU
#define ARM64_ESR_DABT_VNCR		(1UL << 13U)
#define ARM64_ESR_DABT_FNV		(1UL << 10U)
#define ARM64_ESR_DABT_EA		(1UL << 9U)
#define ARM64_ESR_DABT_CM		(1UL << 8U)
#define ARM64_ESR_DABT_S1PTW		(1UL << 7U)
#define ARM64_ESR_DABT_AR		(1UL << 14U)
#define ARM64_ESR_ABORT_SET_SHIFT	11U
#define ARM64_ESR_ABORT_SET_MASK	0x3U
#define ARM64_ESR_SERROR_IDS		(1UL << 24U)
#define ARM64_ESR_SERROR_IESB		(1UL << 13U)
#define ARM64_ESR_SERROR_AET_SHIFT	10U
#define ARM64_ESR_SERROR_AET_MASK	0x7U
#define ARM64_ESR_SERROR_EA		(1UL << 9U)
#define ARM64_ESR_SYSREG_DIR_READ	(1UL << 0U)
#define ARM64_ESR_SYSREG_OP0_SHIFT	20U
#define ARM64_ESR_SYSREG_OP1_SHIFT	14U
#define ARM64_ESR_SYSREG_OP2_SHIFT	17U
#define ARM64_ESR_SYSREG_CRN_SHIFT	10U
#define ARM64_ESR_SYSREG_CRM_SHIFT	1U
#define ARM64_ESR_SYSREG_RT_SHIFT	5U
#define ARM64_ESR_FIELD_BINARY_MAX	27U

/* [20260729] ESR_EL2 decode boundary
 *
 *   ESR_EL2 raw value
 *       |
 *       v
 *   fixed header: RES0 | ISS2 | EC | IL | ISS
 *       |
 *       v
 *   EC selects an ISS schema -> arm64_esr_info -> VM policy or diagnostics
 *       |
 *       +--> unknown EC: preserve raw fields and mark class-specific data absent
 *
 * Data Abort uses ISV to decide whether [23:14] describes the trapped
 * instruction or must be reported as RES0. Data and Instruction Abort use
 * [12:11] as SET only when DFSC/IFSC is synchronous external abort (0x10).
 * SError uses IDS to select either implementation-defined ISS or platform
 * fields, and IESB is meaningful only for its defined DFSC value. This keeps
 * the log faithful to the architecture when a reserved bit is unexpectedly
 * nonzero.
 *
 * Reference: Arm DDI0601 2026-06, ESR_EL2 -- Exception Syndrome Register,
 * EL2: https://support.arm.com/documentation/ddi0601/2026-06/AArch64-Registers/ESR-EL2--Exception-Syndrome-Register--EL2-?lang=en
 *
 * Key rule:
 *   - this layer owns only architectural field extraction, never VM policy;
 *   - callers consume structured fields instead of duplicating bit layouts;
 *   - an unknown or future EC remains observable and cannot block panic,
 *     coredump publication, or guest-exit failure handling.
 */
static bool arm64_esr_is_data_abort(uint32_t ec)
{
	return (ec == ESR_EL2_EC_DABT_LOW) || (ec == ESR_EL2_EC_DABT_CUR);
}

static bool arm64_esr_is_instruction_abort(uint32_t ec)
{
	return (ec == ESR_EL2_EC_IABT_LOW) || (ec == ESR_EL2_EC_IABT_CUR);
}

static uint64_t arm64_esr_get_field(uint64_t value, uint8_t low, uint8_t width)
{
	return (value >> low) & ((1UL << width) - 1UL);
}

/* [20260729] ESR_EL2 EC architecture index
 *
 * EC selects the ISS schema. Unless noted below, ISS2 uses the "all other
 * exceptions" encoding. IABT, DABT, and Watchpoint exceptions have their own
 * ISS2 encoding. This is an architecture reference, not a claim that every
 * class below has a dedicated BEAU ISS renderer.
 *
 *   EC          Class                                      Applies when
 *   000000 00   Unknown reason                             -
 *   000001 01   Trapped WF* instruction                    -
 *   000011 03   MCR/MRC, coproc 0b1111                     FEAT_AA32
 *   000100 04   MCRR/MRRC, coproc 0b1111                   FEAT_AA32
 *   000101 05   MCR/MRC, coproc 0b1110                     FEAT_AA32
 *   000110 06   LDC/STC access                             FEAT_AA32
 *   000111 07   FP/SIMD/SVE/SME access trap                FPEN/TFP trap
 *   001000 08   VMRS ID-group trap                         FEAT_AA32
 *   001001 09   Pointer Authentication instruction         FEAT_PAuth
 *   001010 0a   Other trapped instruction                  LS64/SPEv1p5/TRBEv1p1
 *   001100 0c   MRRC, coproc 0b1110                        FEAT_AA32
 *   001101 0d   Branch Target Exception                    FEAT_BTI
 *   001110 0e   Illegal execution state                    -
 *   010001 11   AArch32 SVC                                FEAT_AA32, HCR_EL2.TGE
 *   010010 12   AArch32 HVC                                FEAT_AA32
 *   010011 13   AArch32 SMC                                FEAT_AA32, HCR_EL2.TSC
 *   010100 14   AArch64 MSRR/MRRS/128-bit system           SYSREG128/SYSINSTR128
 *   010101 15   AArch64 SVC                                FEAT_AA64
 *   010110 16   AArch64 HVC                                FEAT_AA64
 *   010111 17   AArch64 SMC                                FEAT_AA64, HCR_EL2.TSC
 *   011000 18   AArch64 MSR/MRS/system instruction         FEAT_AA64
 *   011001 19   SVE functionality trap                     FEAT_SVE
 *   011010 1a   ERET/ERETAA/ERETAB trap                    FEAT_FGT or FEAT_NV
 *   011100 1c   PAC Fail                                   FEAT_FPAC
 *   011101 1d   SME functionality trap                     FEAT_SME
 *   100000 20   Instruction Abort, lower EL                IABT ISS2
 *   100001 21   Instruction Abort, same EL                 IABT ISS2
 *   100010 22   PC alignment fault                         -
 *   100100 24   Data Abort, lower EL                       DABT ISS2
 *   100101 25   Data Abort, same EL or VNCR access         DABT ISS2
 *   100110 26   SP alignment fault                         -
 *   100111 27   Memory Operation Exception                 FEAT_MOPS
 *   101000 28   AArch32 floating-point exception           FEAT_AA32
 *   101100 2c   AArch64 floating-point exception           FEAT_AA64
 *   101101 2d   GCS exception                              FEAT_GCS
 *   101111 2f   SError exception                           -
 *   110000 30   Breakpoint, lower EL                       -
 *   110001 31   Breakpoint, same EL                        -
 *   110010 32   Software Step, lower EL                    -
 *   110011 33   Software Step, same EL                     -
 *   110100 34   Watchpoint, lower EL                       Watchpoint ISS2
 *   110101 35   Watchpoint, same EL or VNCR access         Watchpoint ISS2
 *   111000 38   AArch32 BKPT                               FEAT_AA32
 *   111010 3a   AArch32 Vector Catch                       FEAT_AA32
 *   111100 3c   AArch64 BRK                                FEAT_AA64
 *   111101 3d   Profiling exception                        EBEP/SPE_EXC/TRBE_EXC
 *
 * Source: Arm DDI0601 2026-06, ESR_EL2 -- Exception Syndrome Register,
 * EL2: https://support.arm.com/documentation/ddi0601/2026-06/AArch64-Registers/ESR-EL2--Exception-Syndrome-Register--EL2-?lang=en
 */
static const char *arm64_esr_ec_name(uint32_t ec)
{
	const char *name;

	switch (ec) {
	case ESR_EL2_EC_UNKNOWN:
		name = "N/A";
		break;
	case ESR_EL2_EC_WFI_WFE:
		name = "wfi-wfe";
		break;
	case ESR_EL2_EC_HVC64:
		name = "hvc64";
		break;
	case ESR_EL2_EC_SMC64:
		name = "smc64";
		break;
	case ESR_EL2_EC_SYSREG:
		name = "sysreg";
		break;
	case ESR_EL2_EC_SVE:
		name = "sve";
		break;
	case ESR_EL2_EC_IABT_LOW:
		name = "instruction-abort-lower-el";
		break;
	case ESR_EL2_EC_IABT_CUR:
		name = "instruction-abort-current-el";
		break;
	case ESR_EL2_EC_DABT_LOW:
		name = "data-abort-lower-el";
		break;
	case ESR_EL2_EC_DABT_CUR:
		name = "data-abort-current-el";
		break;
	case ESR_EL2_EC_SERROR:
		name = "serror";
		break;
	case ARM64_ESR_EC_BRK64:
		name = "brk64";
		break;
	default:
		name = "reserved-or-unhandled";
		break;
	}

	return name;
}

static const char *arm64_esr_fsc_name(uint8_t fsc)
{
	const char *name;

	switch (fsc) {
	case 0x00U:
		name = "address-size-l0";
		break;
	case 0x01U:
		name = "address-size-l1";
		break;
	case 0x02U:
		name = "address-size-l2";
		break;
	case 0x03U:
		name = "address-size-l3";
		break;
	case 0x04U:
		name = "translation-l0";
		break;
	case 0x05U:
		name = "translation-l1";
		break;
	case 0x06U:
		name = "translation-l2";
		break;
	case 0x07U:
		name = "translation-l3";
		break;
	case 0x08U:
		name = "access-flag-l0";
		break;
	case 0x09U:
		name = "access-flag-l1";
		break;
	case 0x0aU:
		name = "access-flag-l2";
		break;
	case 0x0bU:
		name = "access-flag-l3";
		break;
	case 0x0cU:
		name = "permission-l0";
		break;
	case 0x0dU:
		name = "permission-l1";
		break;
	case 0x0eU:
		name = "permission-l2";
		break;
	case 0x0fU:
		name = "permission-l3";
		break;
	case 0x10U:
		name = "sync-external-abort";
		break;
	case 0x11U:
		name = "tag-check";
		break;
	case 0x13U:
		name = "sync-external-abort-ttw-l-minus-1";
		break;
	case 0x14U:
		name = "sync-external-abort-ttw-l0";
		break;
	case 0x15U:
		name = "sync-external-abort-ttw-l1";
		break;
	case 0x16U:
		name = "sync-external-abort-ttw-l2";
		break;
	case 0x17U:
		name = "sync-external-abort-ttw-l3";
		break;
	case 0x18U:
		name = "sync-parity-ecc";
		break;
	case 0x1bU:
		name = "sync-parity-ecc-ttw-l-minus-1";
		break;
	case 0x1cU:
		name = "sync-parity-ecc-ttw-l0";
		break;
	case 0x1dU:
		name = "sync-parity-ecc-ttw-l1";
		break;
	case 0x1eU:
		name = "sync-parity-ecc-ttw-l2";
		break;
	case 0x1fU:
		name = "sync-parity-ecc-ttw-l3";
		break;
	case 0x21U:
		name = "alignment";
		break;
	case 0x23U:
		name = "granule-protection-ttw-l-minus-1";
		break;
	case 0x24U:
		name = "granule-protection-ttw-l0";
		break;
	case 0x25U:
		name = "granule-protection-ttw-l1";
		break;
	case 0x26U:
		name = "granule-protection-ttw-l2";
		break;
	case 0x27U:
		name = "granule-protection-ttw-l3";
		break;
	case 0x28U:
		name = "granule-protection";
		break;
	case 0x29U:
		name = "address-size-l-minus-1";
		break;
	case 0x2bU:
		name = "translation-l-minus-1";
		break;
	case 0x30U:
		name = "tlb-conflict";
		break;
	case 0x31U:
		name = "unsupported-atomic-hardware-update";
		break;
	case 0x34U:
		name = "implementation-defined-lockdown";
		break;
	case 0x35U:
		name = "implementation-defined-atomic";
		break;
	default:
		name = "reserved-or-implementation-defined";
		break;
	}

	return name;
}

static const char *arm64_esr_set_name(uint8_t set)
{
	const char *name;

	switch (set) {
	case 0U:
		name = "recoverable-state-uer";
		break;
	case 2U:
		name = "uncontainable-uc";
		break;
	case 3U:
		name = "restartable-state-ueo";
		break;
	default:
		name = "reserved";
		break;
	}

	return name;
}

static const char *arm64_esr_serror_fsc_name(uint8_t fsc)
{
	const char *name;

	switch (fsc) {
	case 0x00U:
		name = "uncategorized";
		break;
	case 0x11U:
		name = "asynchronous-serror";
		break;
	default:
		name = "reserved-or-implementation-defined";
		break;
	}

	return name;
}

static const char *arm64_esr_aet_name(uint8_t aet)
{
	const char *name;

	switch (aet) {
	case 0U:
		name = "uncontainable-uc";
		break;
	case 1U:
		name = "unrecoverable-ueu";
		break;
	case 2U:
		name = "restartable-ueo";
		break;
	case 3U:
		name = "recoverable-uer";
		break;
	case 6U:
		name = "corrected-ce";
		break;
	default:
		name = "reserved";
		break;
	}

	return name;
}

/* [20260729] Bounded ESR field emission
 *
 *   ESR bit slice -> fixed local binary buffer -> one durable log record
 *
 * Key rule:
 *   - the renderer owns no persistent state and allocates no memory;
 *   - every field is emitted separately so panic/coredump logs stay within
 *     the fixed log-record limit;
 *   - nonzero reserved fields remain visible for hardware and firmware
 *     diagnosis instead of being discarded by the semantic decoder.
 */
static void arm64_esr_log_field(uint32_t severity, const char *scope, uint8_t high,
	uint8_t low, const char *name, uint64_t value, const char *description)
{
	char binary[ARM64_ESR_FIELD_BINARY_MAX + 1U];
	uint32_t width = (uint32_t)high - (uint32_t)low + 1U;
	uint32_t index;

	for (index = 0U; index < width; index++) {
		binary[index] = ((value >> (width - index - 1U)) & 1UL) != 0UL ? '1' : '0';
	}
	binary[width] = '\0';
	if (high == low) {
		do_logmsg(severity, "%s [   %2u] %s:0x%lx 0b%s%s%s", scope, high, name,
			value, binary, description == NULL ? "" : " ",
			description == NULL ? "" : description);
	} else {
		do_logmsg(severity, "%s [%2u:%2u] %s:0x%lx 0b%s%s%s", scope, high, low,
			name, value, binary, description == NULL ? "" : " ",
			description == NULL ? "" : description);
	}
}

bool arm64_esr_decode(uint64_t esr, struct arm64_esr_info *info)
{
	uint32_t sas;

	if (info == NULL) {
		return false;
	}

	*info = (struct arm64_esr_info){ 0U };
	info->raw = esr;
	info->res0_63_37 = (uint32_t)arm64_esr_get_field(esr, 37U, 27U);
	info->iss2 = (uint8_t)arm64_esr_get_field(esr, 32U, 5U);
	info->ec = (uint32_t)ESR_EL2_EC(esr);
	info->il = (esr & ESR_EL2_IL) != 0UL;
	info->iss = (uint32_t)(esr & ARM64_ESR_ISS_MASK);

	if (arm64_esr_is_data_abort(info->ec) || arm64_esr_is_instruction_abort(info->ec)) {
		info->abort.valid = true;
		info->abort.data = arm64_esr_is_data_abort(info->ec);
		info->abort.fnv = (esr & ARM64_ESR_DABT_FNV) != 0UL;
		info->abort.ea = (esr & ARM64_ESR_DABT_EA) != 0UL;
		info->abort.s1ptw = (esr & ARM64_ESR_DABT_S1PTW) != 0UL;
		info->abort.fsc = (uint8_t)(esr & ESR_ABORT_FSC_MASK);
		info->abort.set = (uint8_t)arm64_esr_get_field(esr,
			ARM64_ESR_ABORT_SET_SHIFT, 2U);
		info->abort.set_valid = info->abort.fsc == 0x10U;
		if (info->abort.data) {
			info->abort.isv = (esr & ESR_DABT_ISV) != 0UL;
			info->abort.sse = (esr & ESR_DABT_SSE) != 0UL;
			info->abort.sf = (esr & ESR_DABT_SF) != 0UL;
			info->abort.ar = (esr & ARM64_ESR_DABT_AR) != 0UL;
			info->abort.vncr = (esr & ARM64_ESR_DABT_VNCR) != 0UL;
			info->abort.cm = (esr & ARM64_ESR_DABT_CM) != 0UL;
			info->abort.write = (esr & ESR_DABT_WNR) != 0UL;
			if (info->abort.isv) {
				sas = (uint32_t)((esr >> ESR_DABT_SAS_SHIFT) & ESR_DABT_SAS_MASK);
				info->abort.access_size = (uint8_t)(1U << sas);
				info->abort.srt = (uint8_t)((esr >> ESR_DABT_SRT_SHIFT) &
					ESR_DABT_SRT_MASK);
			} else {
				info->abort.data_res0_23_14 = (uint16_t)arm64_esr_get_field(esr,
					14U, 10U);
			}
		}
	}

	if (info->ec == ESR_EL2_EC_SERROR) {
		info->serror = true;
		info->serror_ids = (esr & ARM64_ESR_SERROR_IDS) != 0UL;
		info->serror_ea = (esr & ARM64_ESR_SERROR_EA) != 0UL;
		info->serror_fsc = (uint8_t)(esr & ESR_ABORT_FSC_MASK);
		if (info->serror_ids) {
			info->serror_impdef = (uint32_t)arm64_esr_get_field(esr, 0U, 24U);
		} else {
			info->serror_iesb = (esr & ARM64_ESR_SERROR_IESB) != 0UL;
			info->serror_iesb_valid = info->serror_fsc == 0x11U;
			info->serror_aet = (uint8_t)((esr >> ARM64_ESR_SERROR_AET_SHIFT) &
				ARM64_ESR_SERROR_AET_MASK);
		}
	}

	return true;
}

static void arm64_esr_log_abort(uint32_t severity, const char *scope,
	const struct arm64_esr_info *info)
{
	uint64_t esr = info->raw;
	const char *fsc_name = arm64_esr_fsc_name(info->abort.fsc);

	if (info->abort.data) {
		arm64_esr_log_field(severity, scope, 24U, 24U, "ISV", info->abort.isv,
			info->abort.isv ? "valid-instruction-syndrome" :
			"no-valid-instruction-syndrome");
		if (info->abort.isv) {
			arm64_esr_log_field(severity, scope, 23U, 22U, "SAS",
				arm64_esr_get_field(esr, 22U, 2U), "syndrome-access-size");
			arm64_esr_log_field(severity, scope, 21U, 21U, "SSE", info->abort.sse,
				"syndrome-sign-extend");
			arm64_esr_log_field(severity, scope, 20U, 16U, "SRT", info->abort.srt,
				"syndrome-register-transfer");
			arm64_esr_log_field(severity, scope, 15U, 15U, "SF", info->abort.sf,
				info->abort.sf ? "64-bit-register" : "32-bit-register");
			arm64_esr_log_field(severity, scope, 14U, 14U, "AR", info->abort.ar,
				info->abort.ar ? "acquire-release" : "no-acquire-release");
		} else {
			arm64_esr_log_field(severity, scope, 23U, 14U, "RES0",
				info->abort.data_res0_23_14, "reserved-when-isv-zero");
		}
		arm64_esr_log_field(severity, scope, 13U, 13U, "VNCR", info->abort.vncr,
			"virtual-nested-control-register");
		if (info->abort.set_valid) {
			arm64_esr_log_field(severity, scope, 12U, 11U, "SET", info->abort.set,
				arm64_esr_set_name(info->abort.set));
		} else {
			arm64_esr_log_field(severity, scope, 12U, 11U, "RES0",
				info->abort.set, "reserved-for-dfsc");
		}
		arm64_esr_log_field(severity, scope, 10U, 10U, "FnV", info->abort.fnv,
			info->abort.fnv ? "far-not-valid" : "far-valid");
		arm64_esr_log_field(severity, scope, 9U, 9U, "EA", info->abort.ea,
			"external-abort-type");
		arm64_esr_log_field(severity, scope, 8U, 8U, "CM", info->abort.cm,
			"cache-maintenance");
		arm64_esr_log_field(severity, scope, 7U, 7U, "S1PTW", info->abort.s1ptw,
			"stage-1-translation-table-walk");
		arm64_esr_log_field(severity, scope, 6U, 6U, "WnR", info->abort.write,
			info->abort.write ? "write" : "read");
		arm64_esr_log_field(severity, scope, 5U, 0U, "DFSC", info->abort.fsc,
			fsc_name);
	} else {
		arm64_esr_log_field(severity, scope, 24U, 13U, "RES0",
			arm64_esr_get_field(esr, 13U, 12U), "reserved");
		if (info->abort.set_valid) {
			arm64_esr_log_field(severity, scope, 12U, 11U, "SET", info->abort.set,
				arm64_esr_set_name(info->abort.set));
		} else {
			arm64_esr_log_field(severity, scope, 12U, 11U, "RES0",
				info->abort.set, "reserved-for-ifsc");
		}
		arm64_esr_log_field(severity, scope, 10U, 10U, "FnV", info->abort.fnv,
			info->abort.fnv ? "far-not-valid" : "far-valid");
		arm64_esr_log_field(severity, scope, 9U, 9U, "EA", info->abort.ea,
			"external-abort-type");
		arm64_esr_log_field(severity, scope, 8U, 8U, "RES0",
			arm64_esr_get_field(esr, 8U, 1U), "reserved");
		arm64_esr_log_field(severity, scope, 7U, 7U, "S1PTW", info->abort.s1ptw,
			"stage-1-translation-table-walk");
		arm64_esr_log_field(severity, scope, 6U, 6U, "RES0",
			arm64_esr_get_field(esr, 6U, 1U), "reserved");
		arm64_esr_log_field(severity, scope, 5U, 0U, "IFSC", info->abort.fsc,
			fsc_name);
	}
}

static void arm64_esr_log_serror(uint32_t severity, const char *scope,
	const struct arm64_esr_info *info)
{
	uint64_t esr = info->raw;

	arm64_esr_log_field(severity, scope, 24U, 24U, "IDS", info->serror_ids,
		info->serror_ids ? "implementation-defined-syndrome" : "platform-syndrome");
	if (info->serror_ids) {
		arm64_esr_log_field(severity, scope, 23U, 0U, "IMPDEF",
			info->serror_impdef, "implementation-defined");
		return;
	}
	arm64_esr_log_field(severity, scope, 23U, 14U, "RES0",
		arm64_esr_get_field(esr, 14U, 10U), "reserved");
	if (info->serror_iesb_valid) {
		arm64_esr_log_field(severity, scope, 13U, 13U, "IESB", info->serror_iesb,
			"implicit-error-synchronization");
	} else {
		arm64_esr_log_field(severity, scope, 13U, 13U, "RES0",
			arm64_esr_get_field(esr, 13U, 1U), "reserved-for-dfsc");
	}
	arm64_esr_log_field(severity, scope, 12U, 10U, "AET", info->serror_aet,
		arm64_esr_aet_name(info->serror_aet));
	arm64_esr_log_field(severity, scope, 9U, 9U, "EA", info->serror_ea,
		"external-abort-type");
	arm64_esr_log_field(severity, scope, 8U, 6U, "RES0",
		arm64_esr_get_field(esr, 6U, 3U), "reserved");
	arm64_esr_log_field(severity, scope, 5U, 0U, "DFSC", info->serror_fsc,
		arm64_esr_serror_fsc_name(info->serror_fsc));
}

static void arm64_esr_log_wfx(uint32_t severity, const char *scope, uint64_t esr)
{
	uint32_t ti = (uint32_t)arm64_esr_get_field(esr, 0U, 2U);
	const char *ti_name = ti == 0U ? "wfi" : ti == 1U ? "wfe" :
		ti == 2U ? "wfit" : "wfet";

	arm64_esr_log_field(severity, scope, 24U, 24U, "CV",
		arm64_esr_get_field(esr, 24U, 1U), "condition-code-valid");
	arm64_esr_log_field(severity, scope, 23U, 20U, "COND",
		arm64_esr_get_field(esr, 20U, 4U), "condition-code");
	arm64_esr_log_field(severity, scope, 19U, 10U, "RES0",
		arm64_esr_get_field(esr, 10U, 10U), "reserved");
	arm64_esr_log_field(severity, scope, 9U, 5U, "RN",
		arm64_esr_get_field(esr, 5U, 5U), "register-number");
	arm64_esr_log_field(severity, scope, 4U, 3U, "RES0",
		arm64_esr_get_field(esr, 3U, 2U), "reserved");
	arm64_esr_log_field(severity, scope, 2U, 2U, "RV",
		arm64_esr_get_field(esr, 2U, 1U), "register-valid");
	arm64_esr_log_field(severity, scope, 1U, 0U, "TI", ti, ti_name);
}

static void arm64_esr_log_sysreg(uint32_t severity, const char *scope, uint64_t esr)
{
	arm64_esr_log_field(severity, scope, 24U, 22U, "RES0",
		arm64_esr_get_field(esr, 22U, 3U), "reserved");
	arm64_esr_log_field(severity, scope, 21U, 20U, "Op0",
		arm64_esr_get_field(esr, ARM64_ESR_SYSREG_OP0_SHIFT, 2U), NULL);
	arm64_esr_log_field(severity, scope, 19U, 17U, "Op2",
		arm64_esr_get_field(esr, ARM64_ESR_SYSREG_OP2_SHIFT, 3U), NULL);
	arm64_esr_log_field(severity, scope, 16U, 14U, "Op1",
		arm64_esr_get_field(esr, ARM64_ESR_SYSREG_OP1_SHIFT, 3U), NULL);
	arm64_esr_log_field(severity, scope, 13U, 10U, "CRn",
		arm64_esr_get_field(esr, ARM64_ESR_SYSREG_CRN_SHIFT, 4U), NULL);
	arm64_esr_log_field(severity, scope, 9U, 5U, "Rt",
		arm64_esr_get_field(esr, ARM64_ESR_SYSREG_RT_SHIFT, 5U), NULL);
	arm64_esr_log_field(severity, scope, 4U, 1U, "CRm",
		arm64_esr_get_field(esr, ARM64_ESR_SYSREG_CRM_SHIFT, 4U), NULL);
	arm64_esr_log_field(severity, scope, 0U, 0U, "Direction",
		(esr & ARM64_ESR_SYSREG_DIR_READ) != 0UL,
		(esr & ARM64_ESR_SYSREG_DIR_READ) != 0UL ? "mrs-read" : "msr-write");
}

void arm64_esr_log(uint32_t severity, const char *scope, uint64_t esr)
{
	struct arm64_esr_info info;

	if (scope == NULL) {
		scope = "esr";
	}
	if (!arm64_esr_decode(esr, &info)) {
		do_logmsg(severity, "%s [63: 0] 0x%016lx (FAILED)", scope, esr);
		return;
	}

	do_logmsg(severity, "%s [63: 0] 0x%016lx", scope, info.raw);
	arm64_esr_log_field(severity, scope, 63U, 37U, "RES0", info.res0_63_37,
		"reserved");
	arm64_esr_log_field(severity, scope, 36U, 32U, "ISS2", info.iss2, NULL);
	arm64_esr_log_field(severity, scope, 31U, 26U, "EC", info.ec,
		arm64_esr_ec_name(info.ec));
	arm64_esr_log_field(severity, scope, 25U, 25U, "IL", info.il,
		info.il ? "32-bit-instruction" : "16-bit-instruction");
	arm64_esr_log_field(severity, scope, 24U, 0U, "ISS", info.iss,
		"instruction-specific-syndrome");

	if (info.abort.valid) {
		arm64_esr_log_abort(severity, scope, &info);
	} else if (info.serror) {
		arm64_esr_log_serror(severity, scope, &info);
	} else if ((info.ec == ESR_EL2_EC_HVC64) || (info.ec == ESR_EL2_EC_SMC64) ||
		(info.ec == ARM64_ESR_EC_BRK64)) {
		arm64_esr_log_field(severity, scope, 24U, 16U, "RES0",
			arm64_esr_get_field(esr, 16U, 9U), "reserved");
		arm64_esr_log_field(severity, scope, 15U, 0U, "imm16",
			arm64_esr_get_field(esr, 0U, 16U), "immediate");
	} else if (info.ec == ESR_EL2_EC_WFI_WFE) {
		arm64_esr_log_wfx(severity, scope, esr);
	} else if (info.ec == ESR_EL2_EC_SYSREG) {
		arm64_esr_log_sysreg(severity, scope, esr);
	} else {
		arm64_esr_log_field(severity, scope, 24U, 0U, "ISS", info.iss,
			"unhandled-class-or-implementation-defined");
	}
}
