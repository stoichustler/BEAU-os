/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SECURITY_H
#define ARM64_SECURITY_H

#ifndef ASSEMBLER

#include <types.h>
#include <random.h>

struct thread_object;
struct arm64_ptrauth_key;

#define ARM64_SECURITY_FEATURE_PAUTH	(1UL << 0U)
#define ARM64_SECURITY_FEATURE_BTI	(1UL << 1U)
#define ARM64_SECURITY_FEATURE_RNG	(1UL << 2U)

void arm64_security_early_init(void) __attribute__((no_stack_protector));
void arm64_security_validate_pcpu(uint16_t pcpu_id);
void arm64_security_finalize(void);
void arm64_security_log_bsp_info(void);
uint64_t arm64_security_host_features(void);

#if CONFIG_ARM64_PTRAUTH
void arm64_ptrauth_finalize(uint64_t host_features);
bool arm64_ptrauth_enabled(void);
bool arm64_ptrauth_prepare_current_cpu(void);
bool arm64_ptrauth_prepare_thread(struct thread_object *obj);
void arm64_ptrauth_bind_idle_thread(struct thread_object *obj);
bool arm64_ptrauth_active_current(void);
const struct arm64_ptrauth_key *arm64_ptrauth_current_cpu_key(void);
void arm64_ptrauth_activate_and_run_idle(const struct arm64_ptrauth_key *key)
	__attribute__((noreturn));
#endif

#ifdef STACK_PROTECTOR
extern unsigned long __stack_chk_guard;

static inline __attribute__((__always_inline__)) void init_stack_canary(void)
{
	__stack_chk_guard = get_random_value();
}
#endif

static inline bool check_cpu_security_cap(void)
{
	return arm64_security_host_features() != 0UL;
}

static inline void cpu_internal_buffers_clear(void)
{
}

static inline bool is_ept_force_4k_ipage(void)
{
	return false;
}

#endif /* ASSEMBLER */

#endif /* ARM64_SECURITY_H */
