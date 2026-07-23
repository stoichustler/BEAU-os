/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_MEMORY_H
#define ARM64_LIB_MEMORY_H

/* [20260723] C contract versus ARM64 memory backend
 *
 * C API: memcpy/memset/memmove
 *     - owns caller-visible semantics: arbitrary alignment, overlap direction,
 *       normal-memory-only use, and return-value contract;
 *     - remains the portable interface for BEAU C code.
 *
 * ASM backend: arch_memcpy/arch_memset/arch_memcpy_backwards
 *     - owns AAPCS64 registers, byte alignment prologue, and optional wide
 *       ARM64 transfers after alignment is proven;
 *     - must preserve the C API contract and must not add user-copy, MMIO, or
 *       fault-recovery behavior.
 */
#define HAS_ARCH_MEMORY_LIB

#endif /* ARM64_LIB_MEMORY_H */
