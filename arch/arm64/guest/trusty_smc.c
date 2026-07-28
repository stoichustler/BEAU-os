/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <memory.h>
#include <acrn_common.h>
#include <vconfig.h>
#include <vcpu.h>
#include <vm.h>
#include <asm/trusty.h>

#define TRUSTY_SMC_FC_GET_VERSION_STR	0xbc00000aUL
#define TRUSTY_SMC_FC_API_VERSION	0xbc00000bUL
#define TRUSTY_SMC_FC_GET_SMP_MAX_CPUS	0xbc00000dUL
#define TRUSTY_API_VERSION_MIN		1UL
#define TRUSTY_API_VERSION_MAX		5UL
#define TRUSTY_SMC_UNK			UINT64_MAX
#define TRUSTY_VERSION_LENGTH_INDEX	0xffffffffUL
#define TRUSTY_VERSION_MAX_LEN		128U

static uint64_t trusty_api_version_cache;

/* [20260727] Restricted Trusty API-version forwarding
 *
 * VM2 guest x0/x1             BEAU EL2                  TF-A / Trusty
 *       |                        |                            |
 *       +-- exact API request -->+-- client/version gate ------>+
 *                                |                            |
 *                                +-- reject ------------------> SMC_UNK
 *
 * Key rule:
 *   - BEAU owns guest policy; TF-A owns secure-world state;
 *   - only a Trusty-client VM may request API versions 1 through 5;
 *   - x2 through x7 are zeroed, so no guest pointer or unapproved argument
 *     crosses into secure world; every validation or TF-A failure is SMC_UNK.
 */
static uint64_t trusty_fastcall(uint64_t function_id, uint64_t argument)
{
	register uint64_t x0 asm("x0") = function_id;
	register uint64_t x1 asm("x1") = argument;
	register uint64_t x2 asm("x2") = 0UL;
	register uint64_t x3 asm("x3") = 0UL;
	register uint64_t x4 asm("x4") = 0UL;
	register uint64_t x5 asm("x5") = 0UL;
	register uint64_t x6 asm("x6") = 0UL;
	register uint64_t x7 asm("x7") = 0UL;

	asm volatile ("smc #0"
		: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4),
		  "+r" (x5), "+r" (x6), "+r" (x7)
		:
		: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		  "x16", "x17", "memory");

	return x0;
}

/* [20260728] Read-only Trusty version query
 *
 * BEAU shell -> bounded SMC index -> TF-A / Trusty LK character
 *                                      |
 *                                      +--> invalid reply: clear buffer
 *                                      |
 *                                      v
 *                              printable version output
 *
 * Key rule:
 *   - BEAU owns the caller buffer and Trusty owns the immutable build string;
 *   - the length is validated before each fixed-index fastcall and before the
 *     NUL terminator is published;
 *   - no pointer, shared buffer, or API-version negotiation crosses the
 *     secure boundary, preventing a diagnostic query from changing Trusty
 *     state or exposing arbitrary secure-world data.
 */
int32_t arm64_trusty_get_version(char *version, size_t version_size)
{
	if ((version == NULL) || (version_size < 2U) ||
		(version_size > (TRUSTY_VERSION_MAX_LEN + 1U))) {
		return -EINVAL;
	}
	(void)memset(version, 0U, version_size);

#if defined(CONFIG_PLATFORM_QEMU)
	{
		uint64_t version_length;
		uint64_t value;
		size_t index;
		int32_t status = -EIO;

		version_length = trusty_fastcall(TRUSTY_SMC_FC_GET_VERSION_STR,
			TRUSTY_VERSION_LENGTH_INDEX);
		if ((version_length == TRUSTY_SMC_UNK) || (version_length == 0UL) ||
			(version_length > TRUSTY_VERSION_MAX_LEN) ||
			(version_length >= version_size)) {
			return status;
		}

		for (index = 0U; index < (size_t)version_length; index++) {
			value = trusty_fastcall(TRUSTY_SMC_FC_GET_VERSION_STR, index);
			if ((value == TRUSTY_SMC_UNK) || (value < 0x20UL) ||
				(value > 0x7eUL)) {
				(void)memset(version, 0U, version_size);
				return status;
			}
			version[index] = (char)value;
		}

		return 0;
	}
#else
	return -ENOTSUP;
#endif
}

/* [20260728] Trusty system-info snapshot
 *
 * VM2 API-version success -> release-store ABI cache
 *                                        |
 * shell dump -> fixed CPU SMC -> acquire-load cache -> complete snapshot
 *                    |
 *                    +--> invalid reply: no snapshot output
 *
 * Key rule:
 *   - Trusty owns the reported CPU limit while BEAU owns the ABI cache;
 *   - the VM2 policy validation completes before it publishes the cached ABI
 *     version, and the shell acquires that value only after its CPU query;
 *   - dump never invokes the stateful API-version SMC, preventing a diagnostic
 *     command from selecting or downgrading Trusty's global ABI state.
 */
int32_t arm64_trusty_get_system_info(struct arm64_trusty_system_info *info)
{
	if (info == NULL) {
		return -EINVAL;
	}
	(void)memset(info, 0U, sizeof(*info));

#if defined(CONFIG_PLATFORM_QEMU)
	{
		uint64_t max_cpus = trusty_fastcall(TRUSTY_SMC_FC_GET_SMP_MAX_CPUS, 0UL);
		uint64_t api_version;

		if ((max_cpus == TRUSTY_SMC_UNK) || (max_cpus == 0UL) ||
			(max_cpus > UINT32_MAX)) {
			return -EIO;
		}

		api_version = __atomic_load_n(&trusty_api_version_cache,
			__ATOMIC_ACQUIRE);
		info->smp_max_cpus = (uint32_t)max_cpus;
		if ((api_version >= TRUSTY_API_VERSION_MIN) &&
			(api_version <= TRUSTY_API_VERSION_MAX)) {
			info->api_version = (uint32_t)api_version;
			info->api_version_valid = true;
		}

		return 0;
	}
#else
	return -ENOTSUP;
#endif
}

uint64_t beau_trusty_smc(struct acrn_vcpu *vcpu, uint64_t function_id,
	uint64_t requested_version)
{
	const struct acrn_vm_config *vm_config;
	uint64_t returned_version;

	if ((vcpu == NULL) || (vcpu->vm == NULL) ||
		(function_id != TRUSTY_SMC_FC_API_VERSION) ||
		(requested_version < TRUSTY_API_VERSION_MIN) ||
		(requested_version > TRUSTY_API_VERSION_MAX)) {
		return TRUSTY_SMC_UNK;
	}

	vm_config = get_vm_config(vcpu->vm->vm_id);
	if ((vm_config == NULL) ||
		((vm_config->guest_flags & GUEST_FLAG_TRUSTY_CLIENT) == 0UL)) {
		return TRUSTY_SMC_UNK;
	}

	returned_version = trusty_fastcall(TRUSTY_SMC_FC_API_VERSION, requested_version);
	if ((returned_version < TRUSTY_API_VERSION_MIN) ||
		(returned_version > requested_version)) {
		return TRUSTY_SMC_UNK;
	}
	__atomic_store_n(&trusty_api_version_cache, returned_version, __ATOMIC_RELEASE);

	return returned_version;
}
