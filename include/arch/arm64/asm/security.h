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

#define ARM64_SECURITY_FEATURE_PAUTH	(1UL << 0U)
#define ARM64_SECURITY_FEATURE_BTI	(1UL << 1U)
#define ARM64_SECURITY_FEATURE_RNG	(1UL << 2U)

void arm64_security_early_init(void) __attribute__((no_stack_protector));
void arm64_security_validate_pcpu(uint16_t pcpu_id);
void arm64_security_finalize(void);
void arm64_security_log_bsp_info(void);
uint64_t arm64_security_host_features(void);

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
