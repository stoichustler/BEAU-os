/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <logmsg.h>
#include <asm/sysreg.h>
#include <asm/security.h>

#ifdef STACK_PROTECTOR
unsigned long __stack_chk_guard;
#endif

static uint64_t arm64_security_bsp_features;
static uint64_t arm64_security_host_feature_mask;
static bool arm64_security_initialized;
static bool arm64_security_finalized;

static uint64_t arm64_security_collect_features(void)
{
	uint64_t features = 0UL;
	uint64_t pfr1 = arm64_sysreg_read(s3_0_c0_c4_1);
	uint64_t isar0 = arm64_sysreg_read(s3_0_c0_c6_0);
	uint64_t isar1 = arm64_sysreg_read(s3_0_c0_c6_1);

	if ((isar1 & ID_AA64ISAR1_PAUTH_MASK) != 0UL) {
		features |= ARM64_SECURITY_FEATURE_PAUTH;
	}
	if ((pfr1 & ID_AA64PFR1_BT_MASK) != 0UL) {
		features |= ARM64_SECURITY_FEATURE_BTI;
	}
	if ((isar0 & ID_AA64ISAR0_RNDR_MASK) != 0UL) {
		features |= ARM64_SECURITY_FEATURE_RNG;
	}

	return features;
}

/* [20260723] EL2 security feature ownership
 *
 * BSP ID registers -> BSP capability snapshot -> early RNG seed
 *                                             |
 * AP ID registers  -> intersection ----------+
 *                                             v
 *                                 publish only supported host policy
 *                                             |
 *                                             +--> guest ID fields masked
 *
 * Key rule:
 *   - BSP owns the initial snapshot before the stack cookie is initialized;
 *   - every pCPU narrows the common mask before the BSP publishes strong RNG;
 *   - PAC and BTI remain detected-but-disabled until a later change owns key
 *     setup, toolchain branch protection, exception paths, and guest ABI.
 */
void arm64_security_early_init(void)
{
	arm64_security_bsp_features = arm64_security_collect_features();
	__atomic_store_n(&arm64_security_host_feature_mask,
		arm64_security_bsp_features, __ATOMIC_RELEASE);
	arch_random_bootstrap_init();
	__atomic_store_n(&arm64_security_initialized, true, __ATOMIC_RELEASE);
}

void arm64_security_validate_pcpu(uint16_t pcpu_id)
{
	uint64_t local_features;

	if (!__atomic_load_n(&arm64_security_initialized, __ATOMIC_ACQUIRE)) {
		return;
	}

	local_features = arm64_security_collect_features();
	(void)__atomic_fetch_and(&arm64_security_host_feature_mask, local_features,
		__ATOMIC_ACQ_REL);
	if ((local_features & arm64_security_bsp_features) !=
		arm64_security_bsp_features) {
		LOG_WRN("SEC:    CPU%hu security capability mismatch", pcpu_id);
	}
}

void arm64_security_finalize(void)
{
	uint64_t features;

	if (arm64_security_finalized) {
		return;
	}

	features = arm64_security_host_features();
	if ((features & ARM64_SECURITY_FEATURE_RNG) != 0UL) {
		arch_random_publish_strong();
	}
	arm64_security_finalized = true;
	LOG_INF("SEC:    PAC:%s BTI:%s RNG:%s",
		(features & ARM64_SECURITY_FEATURE_PAUTH) != 0UL ? "detected/off" : "off",
		(features & ARM64_SECURITY_FEATURE_BTI) != 0UL ? "detected/off" : "off",
		arch_random_strong_ready() ? "RNDR" : "degraded");
}

void arm64_security_log_bsp_info(void)
{
	LOG_INF("SEC:    BSP PAC:%s BTI:%s RNG:%s (global policy pending)",
		(arm64_security_bsp_features & ARM64_SECURITY_FEATURE_PAUTH) != 0UL ?
		"detected/off" : "off",
		(arm64_security_bsp_features & ARM64_SECURITY_FEATURE_BTI) != 0UL ?
		"detected/off" : "off",
		(arm64_security_bsp_features & ARM64_SECURITY_FEATURE_RNG) != 0UL ?
		"detected" : "off");
}

uint64_t arm64_security_host_features(void)
{
	return __atomic_load_n(&arm64_security_host_feature_mask, __ATOMIC_ACQUIRE);
}
