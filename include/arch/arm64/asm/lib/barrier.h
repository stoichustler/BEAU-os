/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_BARRIER_H
#define ARM64_LIB_BARRIER_H

/* [20260716] ARM64 C barrier ownership
 *
 * subsystem C code -> scoped barrier helper -> one compiler asm boundary
 *
 * Key rules:
 *   - this header is the only C implementation point for DMB, DSB, and ISB;
 *   - the helper name exposes the architectural shareability/access scope;
 *   - every hardware barrier is also a compiler memory barrier;
 *   - assembly sources continue to use native instructions directly.
 */
#define ARM64_DMB(scope) asm volatile ("dmb " #scope ::: "memory")
#define ARM64_DSB(scope) asm volatile ("dsb " #scope ::: "memory")

static inline void arm64_isb(void)
{
	asm volatile ("isb" ::: "memory");
}

static inline void arm64_dmb_sy(void)
{
	ARM64_DMB(sy);
}

static inline void arm64_dmb_ish(void)
{
	ARM64_DMB(ish);
}

static inline void arm64_dmb_ishld(void)
{
	ARM64_DMB(ishld);
}

static inline void arm64_dmb_ishst(void)
{
	ARM64_DMB(ishst);
}

static inline void arm64_dsb_sy(void)
{
	ARM64_DSB(sy);
}

static inline void arm64_dsb_ish(void)
{
	ARM64_DSB(ish);
}

static inline void arm64_dsb_ishst(void)
{
	ARM64_DSB(ishst);
}

static inline void arch_cpu_read_memory_barrier(void)
{
	arm64_dmb_ishld();
}

static inline void arch_cpu_write_memory_barrier(void)
{
	arm64_dmb_ishst();
}

static inline void arch_cpu_memory_barrier(void)
{
	arm64_dmb_ish();
}

#endif /* ARM64_LIB_BARRIER_H */
