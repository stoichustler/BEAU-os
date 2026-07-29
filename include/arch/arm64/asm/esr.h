/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_ESR_H
#define ARM64_ESR_H

#include <types.h>

/* [20260729] ESR_EL2 public decode contract
 *
 * ESR_EL2 records the exception class and class-specific syndrome at EL2:
 *
 *   [63:37] RES0 | [36:32] ISS2 | [31:26] EC | [25] IL | [24:0] ISS
 *
 * raw preserves every architectural bit for diagnostics. EC selects the ISS
 * schema, so abort fields are meaningful only for an Instruction Abort or
 * Data Abort. In particular, Data Abort [23:14] is an instruction syndrome
 * only when ISV is set, and [12:11] is SET only for the external-abort FSC.
 * Other values remain observable as RES0 instead of being silently discarded.
 *
 * Reference: Arm DDI0601 2026-06, ESR_EL2 -- Exception Syndrome Register,
 * EL2: https://support.arm.com/documentation/ddi0601/2026-06/AArch64-Registers/ESR-EL2--Exception-Syndrome-Register--EL2-?lang=en
 *
 * Key rule:
 *   - structured fields are decoded values, while raw remains the diagnostic
 *     source of truth for future architectural extensions;
 *   - consumers must test valid/data/ISV and class-specific validity flags
 *     before assigning policy to an ISS field.
 */
struct arm64_esr_abort_info {
	uint8_t access_size;
	uint8_t srt;
	uint8_t fsc;
	uint8_t set;
	uint16_t data_res0_23_14;
	bool valid;
	bool data;
	bool isv;
	bool sse;
	bool sf;
	bool ar;
	bool vncr;
	bool fnv;
	bool ea;
	bool cm;
	bool s1ptw;
	bool write;
	bool set_valid;
};

struct arm64_esr_info {
	uint64_t raw;
	uint32_t ec;
	uint32_t iss;
	uint32_t res0_63_37;
	uint32_t serror_impdef;
	bool il;
	uint8_t iss2;
	bool serror;
	bool serror_ids;
	bool serror_ea;
	bool serror_iesb;
	bool serror_iesb_valid;
	uint8_t serror_aet;
	uint8_t serror_fsc;
	struct arm64_esr_abort_info abort;
};

bool arm64_esr_decode(uint64_t esr, struct arm64_esr_info *info);
void arm64_esr_log(uint32_t severity, const char *scope, uint64_t esr);

#endif /* ARM64_ESR_H */
