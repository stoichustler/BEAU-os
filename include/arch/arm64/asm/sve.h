/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SVE_H
#define ARM64_SVE_H

#include <types.h>
#include <asm/vconfig.h>

#ifndef ASSEMBLER

#define ARM64_SVE_ZREG_NUM		32U
#define ARM64_SVE_PREG_NUM		16U
#define ARM64_SVE_VL_BYTES_MAX		(ARM64_SVE_VL_BITS_MAX / 8U)

struct arm64_sve_state {
	uint8_t z[ARM64_SVE_ZREG_NUM][ARM64_SVE_VL_BYTES_MAX] __aligned(16);
	uint8_t p[ARM64_SVE_PREG_NUM][ARM64_SVE_VL_BYTES_MAX] __aligned(16);
	uint8_t ffr[ARM64_SVE_VL_BYTES_MAX] __aligned(16);
	uint64_t fpsr;
	uint64_t fpcr;
	uint64_t zcr_el1;
};

bool arm64_sve_host_supported(void);
uint32_t arm64_sve_host_vl_bits(void);
uint64_t arm64_sve_zcr_len_from_bits(uint32_t vl_bits);
uint64_t arm64_sve_clamp_zcr(uint64_t zcr, uint32_t vl_bits);
void arm64_sve_save_state(struct arm64_sve_state *state);
void arm64_sve_restore_state(const struct arm64_sve_state *state);

#endif /* ASSEMBLER */

#endif /* ARM64_SVE_H */
