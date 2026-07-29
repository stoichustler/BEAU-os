/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <logmsg.h>
#include <random.h>
#include <schedule.h>
#include <asm/security.h>

/* [20260729] EL2 PAC activation ownership
 *
 * BSP capability intersection + strong RNDR publication
 *     |
 *     +--> unsupported or mixed --> keep PAC disabled
 *     |
 *     v
 * per-pCPU bootstrap key -> no-return EL2 activation -> idle context
 *     |
 *     v
 * per-thread key -> scheduler loads key -> authenticate saved LR
 *
 * Key rule:
 *   - security.c owns the all-pCPU capability decision before this module
 *     publishes PAC policy;
 *   - a key is generated before its thread can become runnable;
 *   - an entropy failure leaves PAC disabled before activation, preventing a
 *     predictable key from protecting a live EL2 return path.
 */
struct arm64_ptrauth_cpu_state {
	struct arm64_ptrauth_key key;
	bool ready;
};

static struct arm64_ptrauth_cpu_state ptrauth_cpu_state[MAX_PCPU_NUM];
static bool ptrauth_policy_enabled;

static bool arm64_ptrauth_generate_key(struct arm64_ptrauth_key *key)
{
	if ((key == NULL) || !arch_get_random_strong_value(&key->lo) ||
		!arch_get_random_strong_value(&key->hi)) {
		return false;
	}

	return true;
}

void arm64_ptrauth_finalize(uint64_t host_features)
{
	ptrauth_policy_enabled =
		((host_features & (ARM64_SECURITY_FEATURE_PAUTH | ARM64_SECURITY_FEATURE_RNG)) ==
		 (ARM64_SECURITY_FEATURE_PAUTH | ARM64_SECURITY_FEATURE_RNG)) &&
		arch_random_strong_ready();
}

bool arm64_ptrauth_enabled(void)
{
	return ptrauth_policy_enabled;
}

bool arm64_ptrauth_prepare_current_cpu(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_ptrauth_cpu_state *state;

	if (!ptrauth_policy_enabled || (pcpu_id >= MAX_PCPU_NUM)) {
		return false;
	}

	state = &ptrauth_cpu_state[pcpu_id];
	if (state->ready) {
		return true;
	}
	if (!arm64_ptrauth_generate_key(&state->key)) {
		LOG_ERR("SEC:    CPU%hu PAC key generation failed", pcpu_id);
		return false;
	}

	state->ready = true;
	return true;
}

bool arm64_ptrauth_prepare_thread(struct thread_object *obj)
{
	if (!ptrauth_policy_enabled) {
		return true;
	}
	if ((obj == NULL) || !arm64_ptrauth_generate_key(&obj->ptrauth_key)) {
		LOG_ERR("SEC:    PAC thread key generation failed");
		return false;
	}

	obj->ptrauth_return_authenticated = false;
	return true;
}

void arm64_ptrauth_bind_idle_thread(struct thread_object *obj)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (!ptrauth_policy_enabled || (obj == NULL) || (pcpu_id >= MAX_PCPU_NUM) ||
		!ptrauth_cpu_state[pcpu_id].ready) {
		return;
	}

	obj->ptrauth_key = ptrauth_cpu_state[pcpu_id].key;
	obj->ptrauth_return_authenticated = true;
}

bool arm64_ptrauth_active_current(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	return ptrauth_policy_enabled && (pcpu_id < MAX_PCPU_NUM) &&
		ptrauth_cpu_state[pcpu_id].ready;
}

const struct arm64_ptrauth_key *arm64_ptrauth_current_cpu_key(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (!arm64_ptrauth_active_current()) {
		return NULL;
	}

	return &ptrauth_cpu_state[pcpu_id].key;
}
