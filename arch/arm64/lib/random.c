/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <asm/sysreg.h>

static uint64_t arch_random_boot_seed;
static bool arch_random_boot_seed_valid;
static bool arch_random_strong_published;
static bool arch_random_rng_supported;

/* [20260723] ARM64 hardware entropy publication
 *
 * ID_AA64ISAR0_EL1.RNDR
 *       |
 *       +--> unsupported: never execute RNDR/RNDRRS
 *       |
 *       v
 * bounded RNDR then RNDRRS retry
 *       |
 *       +--> fail: retain only non-cryptographic fallback API
 *       |
 *       v
 * BSP seed -> all-pCPU capability intersection -> publish strong API
 *
 * Key rule:
 *   - only a successful architectural random instruction can seed the strong
 *     entropy interface;
 *   - publication waits until every active pCPU is known to implement RNDR,
 *     preventing an unsupported CPU from executing the instruction;
 *   - the legacy value API remains available for stack-cookie degradation but
 *     is never reported as cryptographic entropy.
 */
#define ARM64_RNG_RETRY_MAX	16U

static bool arch_random_read_rndr(uint64_t *value)
{
	uint64_t random_value;
	uint32_t valid;

	if (value == NULL) {
		return false;
	}

	asm volatile(
		".inst 0xd53b2400\n\t"
		"mov %0, x0\n\t"
		"cset %w1, ne"
		: "=r" (random_value), "=r" (valid)
		:
		: "x0", "cc", "memory");
	if (valid == 0U) {
		return false;
	}

	*value = random_value;
	return true;
}

static bool arch_random_read_rndrrs(uint64_t *value)
{
	uint64_t random_value;
	uint32_t valid;

	if (value == NULL) {
		return false;
	}

	asm volatile(
		".inst 0xd53b2420\n\t"
		"mov %0, x0\n\t"
		"cset %w1, ne"
		: "=r" (random_value), "=r" (valid)
		:
		: "x0", "cc", "memory");
	if (valid == 0U) {
		return false;
	}

	*value = random_value;
	return true;
}

static bool arch_random_read_hardware(uint64_t *value)
{
	uint32_t retry;

	if (!arch_random_rng_supported || (value == NULL)) {
		return false;
	}

	for (retry = 0U; retry < ARM64_RNG_RETRY_MAX; retry++) {
		if (arch_random_read_rndr(value) || arch_random_read_rndrrs(value)) {
			return true;
		}
	}

	return false;
}

void arch_random_bootstrap_init(void)
{
	uint64_t isar0 = arm64_sysreg_read(s3_0_c0_c6_0);
	uint64_t seed;

	arch_random_rng_supported = (isar0 & ID_AA64ISAR0_RNDR_MASK) != 0UL;
	arch_random_strong_published = false;
	arch_random_boot_seed_valid = false;
	if (arch_random_read_hardware(&seed)) {
		arch_random_boot_seed = seed;
		arch_random_boot_seed_valid = true;
	}
}

void arch_random_publish_strong(void)
{
	if (arch_random_boot_seed_valid && arch_random_rng_supported) {
		__atomic_store_n(&arch_random_strong_published, true, __ATOMIC_RELEASE);
	}
}

bool arch_random_strong_ready(void)
{
	return __atomic_load_n(&arch_random_strong_published, __ATOMIC_ACQUIRE);
}

bool arch_get_random_strong_value(uint64_t *value)
{
	if (!arch_random_strong_ready()) {
		return false;
	}

	return arch_random_read_hardware(value);
}

uint64_t arch_get_random_value(void)
{
	uint64_t cnt = read_cntpct_el0();
	uint64_t mpidr = read_mpidr_el1();

	return cnt ^ (mpidr << 17U) ^ arch_random_boot_seed;
}
