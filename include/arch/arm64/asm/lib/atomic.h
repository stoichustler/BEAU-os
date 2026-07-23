/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_ATOMIC_H
#define ARM64_LIB_ATOMIC_H

/* [20260723] ARM64 typed atomic memory-order contract
 *
 * publisher writes private state
 *     |
 *     v
 * store-release publishes the state
 *     |
 *     v
 * load-acquire observes publication before consuming the state
 *
 * Key rule:
 *   - callers select ordering from ownership transfer, not convenience;
 *   - relaxed operations are only for counters with no publication role;
 *   - this layer uses compiler builtins so the configured ARM64 target keeps
 *     its LL/SC fallback without assuming runtime LSE support.
 */
static inline uint32_t arch_atomic_load_acquire32(const volatile uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline uint64_t arch_atomic_load_acquire64(const volatile uint64_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void arch_atomic_store_release32(volatile uint32_t *ptr,
	uint32_t value)
{
	__atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

static inline void arch_atomic_store_release64(volatile uint64_t *ptr,
	uint64_t value)
{
	__atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

static inline uint32_t arch_atomic_fetch_add_relaxed32(volatile uint32_t *ptr,
	uint32_t value)
{
	return __atomic_fetch_add(ptr, value, __ATOMIC_RELAXED);
}

static inline uint64_t arch_atomic_fetch_add_relaxed64(volatile uint64_t *ptr,
	uint64_t value)
{
	return __atomic_fetch_add(ptr, value, __ATOMIC_RELAXED);
}

static inline uint32_t arch_atomic_cmpxchg_acqrel32(volatile uint32_t *ptr,
	uint32_t old, uint32_t new)
{
	uint32_t expected = old;

	(void)__atomic_compare_exchange_n(ptr, &expected, new, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	return expected;
}

static inline uint64_t arch_atomic_cmpxchg_acqrel64(volatile uint64_t *ptr,
	uint64_t old, uint64_t new)
{
	uint64_t expected = old;

	(void)__atomic_compare_exchange_n(ptr, &expected, new, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	return expected;
}

static inline void arch_atomic_inc32(uint32_t *ptr)
{
	(void)__atomic_add_fetch(ptr, 1U, __ATOMIC_SEQ_CST);
}

static inline void arch_atomic_inc64(uint64_t *ptr)
{
	(void)__atomic_add_fetch(ptr, 1UL, __ATOMIC_SEQ_CST);
}

static inline void arch_atomic_dec32(uint32_t *ptr)
{
	(void)__atomic_sub_fetch(ptr, 1U, __ATOMIC_SEQ_CST);
}

static inline void arch_atomic_dec64(uint64_t *ptr)
{
	(void)__atomic_sub_fetch(ptr, 1UL, __ATOMIC_SEQ_CST);
}

static inline uint32_t arch_atomic_cmpxchg32(volatile uint32_t *ptr, uint32_t old, uint32_t new)
{
	uint32_t expected = old;

	(void)__atomic_compare_exchange_n(ptr, &expected, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

static inline uint64_t arch_atomic_cmpxchg64(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
	uint64_t expected = old;

	(void)__atomic_compare_exchange_n(ptr, &expected, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

static inline uint32_t arch_atomic_swap32(uint32_t *ptr, uint32_t v)
{
	return __atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST);
}

static inline uint64_t arch_atomic_swap64(uint64_t *ptr, uint64_t v)
{
	return __atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int32_t arch_atomic_add_return(int32_t *ptr, int32_t v)
{
	return __atomic_add_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int32_t arch_atomic_sub_return(int32_t *ptr, int32_t v)
{
	return __atomic_sub_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int32_t arch_atomic_inc_return(int32_t *ptr)
{
	return arch_atomic_add_return(ptr, 1);
}

static inline int32_t arch_atomic_dec_return(int32_t *ptr)
{
	return arch_atomic_sub_return(ptr, 1);
}

static inline int64_t arch_atomic_add64_return(int64_t *ptr, int64_t v)
{
	return __atomic_add_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int64_t arch_atomic_sub64_return(int64_t *ptr, int64_t v)
{
	return __atomic_sub_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int64_t arch_atomic_inc64_return(int64_t *ptr)
{
	return arch_atomic_add64_return(ptr, 1L);
}

static inline int64_t arch_atomic_dec64_return(int64_t *ptr)
{
	return arch_atomic_sub64_return(ptr, 1L);
}

#endif /* ARM64_LIB_ATOMIC_H */
