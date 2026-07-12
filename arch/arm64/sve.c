/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <asm/sve.h>
#include <asm/sysreg.h>

static bool host_vl_valid;
static uint32_t host_vl_bits;

bool arm64_sve_host_supported(void)
{
	return (read_id_aa64pfr0_el1() & ID_AA64PFR0_SVE_MASK) != 0UL;
}

uint64_t arm64_sve_zcr_len_from_bits(uint32_t vl_bits)
{
	uint32_t chunks = vl_bits / ARM64_SVE_VL_BITS_MIN;

	if (chunks == 0U) {
		chunks = 1U;
	}

	return (uint64_t)(chunks - 1U) & ZCR_ELx_LEN_MASK;
}

uint64_t arm64_sve_clamp_zcr(uint64_t zcr, uint32_t vl_bits)
{
	uint64_t max_len = arm64_sve_zcr_len_from_bits(vl_bits);
	uint64_t len = zcr & ZCR_ELx_LEN_MASK;

	return (len > max_len) ? max_len : len;
}

static uint32_t arm64_sve_read_live_vl_bits(void)
{
	uint64_t old_cptr;
	uint64_t old_zcr;
	uint64_t bytes = 0UL;

	old_cptr = read_cptr_el2();
	old_zcr = read_zcr_el2();
	write_cptr_el2(old_cptr & ~CPTR_EL2_TZ);
	write_zcr_el2(arm64_sve_zcr_len_from_bits(ARM64_SVE_VL_BITS_MAX));
	asm volatile (
		".arch_extension sve\n"
		"cntb %0\n"
		: "=r" (bytes)
		:
		: "memory");
	write_zcr_el2(old_zcr);
	write_cptr_el2(old_cptr);

	if ((bytes < (ARM64_SVE_VL_BITS_MIN / 8U)) ||
		(bytes > ARM64_SVE_VL_BYTES_MAX)) {
		return 0U;
	}

	return (uint32_t)(bytes * 8UL);
}

uint32_t arm64_sve_host_vl_bits(void)
{
	if (!host_vl_valid) {
		host_vl_bits = arm64_sve_host_supported() ?
			arm64_sve_read_live_vl_bits() : 0U;
		host_vl_valid = true;
	}

	return host_vl_bits;
}

void arm64_sve_save_state(struct arm64_sve_state *state)
{
	if (state == NULL) {
		return;
	}

	state->zcr_el1 = read_zcr_el1() & ZCR_ELx_LEN_MASK;
	state->fpsr = read_fpsr();
	state->fpcr = read_fpcr();
	asm volatile (
		".arch_extension sve\n"
		"str z0,  [%[z], #0,  mul vl]\n"
		"str z1,  [%[z], #1,  mul vl]\n"
		"str z2,  [%[z], #2,  mul vl]\n"
		"str z3,  [%[z], #3,  mul vl]\n"
		"str z4,  [%[z], #4,  mul vl]\n"
		"str z5,  [%[z], #5,  mul vl]\n"
		"str z6,  [%[z], #6,  mul vl]\n"
		"str z7,  [%[z], #7,  mul vl]\n"
		"str z8,  [%[z], #8,  mul vl]\n"
		"str z9,  [%[z], #9,  mul vl]\n"
		"str z10, [%[z], #10, mul vl]\n"
		"str z11, [%[z], #11, mul vl]\n"
		"str z12, [%[z], #12, mul vl]\n"
		"str z13, [%[z], #13, mul vl]\n"
		"str z14, [%[z], #14, mul vl]\n"
		"str z15, [%[z], #15, mul vl]\n"
		"str z16, [%[z], #16, mul vl]\n"
		"str z17, [%[z], #17, mul vl]\n"
		"str z18, [%[z], #18, mul vl]\n"
		"str z19, [%[z], #19, mul vl]\n"
		"str z20, [%[z], #20, mul vl]\n"
		"str z21, [%[z], #21, mul vl]\n"
		"str z22, [%[z], #22, mul vl]\n"
		"str z23, [%[z], #23, mul vl]\n"
		"str z24, [%[z], #24, mul vl]\n"
		"str z25, [%[z], #25, mul vl]\n"
		"str z26, [%[z], #26, mul vl]\n"
		"str z27, [%[z], #27, mul vl]\n"
		"str z28, [%[z], #28, mul vl]\n"
		"str z29, [%[z], #29, mul vl]\n"
		"str z30, [%[z], #30, mul vl]\n"
		"str z31, [%[z], #31, mul vl]\n"
		"str p0,  [%[p], #0,  mul vl]\n"
		"str p1,  [%[p], #1,  mul vl]\n"
		"str p2,  [%[p], #2,  mul vl]\n"
		"str p3,  [%[p], #3,  mul vl]\n"
		"str p4,  [%[p], #4,  mul vl]\n"
		"str p5,  [%[p], #5,  mul vl]\n"
		"str p6,  [%[p], #6,  mul vl]\n"
		"str p7,  [%[p], #7,  mul vl]\n"
		"str p8,  [%[p], #8,  mul vl]\n"
		"str p9,  [%[p], #9,  mul vl]\n"
		"str p10, [%[p], #10, mul vl]\n"
		"str p11, [%[p], #11, mul vl]\n"
		"str p12, [%[p], #12, mul vl]\n"
		"str p13, [%[p], #13, mul vl]\n"
		"str p14, [%[p], #14, mul vl]\n"
		"str p15, [%[p], #15, mul vl]\n"
		"rdffr p0.b\n"
		"str p0, [%[ffr]]\n"
		:
		: [z] "r" (&state->z[0U][0U]),
		  [p] "r" (&state->p[0U][0U]),
		  [ffr] "r" (&state->ffr[0U])
		: "memory");
}

void arm64_sve_restore_state(const struct arm64_sve_state *state)
{
	if (state == NULL) {
		return;
	}

	write_fpcr(state->fpcr);
	write_fpsr(state->fpsr);
	asm volatile (
		".arch_extension sve\n"
		"ldr z0,  [%[z], #0,  mul vl]\n"
		"ldr z1,  [%[z], #1,  mul vl]\n"
		"ldr z2,  [%[z], #2,  mul vl]\n"
		"ldr z3,  [%[z], #3,  mul vl]\n"
		"ldr z4,  [%[z], #4,  mul vl]\n"
		"ldr z5,  [%[z], #5,  mul vl]\n"
		"ldr z6,  [%[z], #6,  mul vl]\n"
		"ldr z7,  [%[z], #7,  mul vl]\n"
		"ldr z8,  [%[z], #8,  mul vl]\n"
		"ldr z9,  [%[z], #9,  mul vl]\n"
		"ldr z10, [%[z], #10, mul vl]\n"
		"ldr z11, [%[z], #11, mul vl]\n"
		"ldr z12, [%[z], #12, mul vl]\n"
		"ldr z13, [%[z], #13, mul vl]\n"
		"ldr z14, [%[z], #14, mul vl]\n"
		"ldr z15, [%[z], #15, mul vl]\n"
		"ldr z16, [%[z], #16, mul vl]\n"
		"ldr z17, [%[z], #17, mul vl]\n"
		"ldr z18, [%[z], #18, mul vl]\n"
		"ldr z19, [%[z], #19, mul vl]\n"
		"ldr z20, [%[z], #20, mul vl]\n"
		"ldr z21, [%[z], #21, mul vl]\n"
		"ldr z22, [%[z], #22, mul vl]\n"
		"ldr z23, [%[z], #23, mul vl]\n"
		"ldr z24, [%[z], #24, mul vl]\n"
		"ldr z25, [%[z], #25, mul vl]\n"
		"ldr z26, [%[z], #26, mul vl]\n"
		"ldr z27, [%[z], #27, mul vl]\n"
		"ldr z28, [%[z], #28, mul vl]\n"
		"ldr z29, [%[z], #29, mul vl]\n"
		"ldr z30, [%[z], #30, mul vl]\n"
		"ldr z31, [%[z], #31, mul vl]\n"
		"ldr p0, [%[ffr]]\n"
		"wrffr p0.b\n"
		"ldr p0,  [%[p], #0,  mul vl]\n"
		"ldr p1,  [%[p], #1,  mul vl]\n"
		"ldr p2,  [%[p], #2,  mul vl]\n"
		"ldr p3,  [%[p], #3,  mul vl]\n"
		"ldr p4,  [%[p], #4,  mul vl]\n"
		"ldr p5,  [%[p], #5,  mul vl]\n"
		"ldr p6,  [%[p], #6,  mul vl]\n"
		"ldr p7,  [%[p], #7,  mul vl]\n"
		"ldr p8,  [%[p], #8,  mul vl]\n"
		"ldr p9,  [%[p], #9,  mul vl]\n"
		"ldr p10, [%[p], #10, mul vl]\n"
		"ldr p11, [%[p], #11, mul vl]\n"
		"ldr p12, [%[p], #12, mul vl]\n"
		"ldr p13, [%[p], #13, mul vl]\n"
		"ldr p14, [%[p], #14, mul vl]\n"
		"ldr p15, [%[p], #15, mul vl]\n"
		:
		: [z] "r" (&state->z[0U][0U]),
		  [p] "r" (&state->p[0U][0U]),
		  [ffr] "r" (&state->ffr[0U])
		: "memory");
}
