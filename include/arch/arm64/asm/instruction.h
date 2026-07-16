/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_INSTRUCTION_H
#define ARM64_INSTRUCTION_H

#include <types.h>

/* [20260716] ARM64 C system-instruction ownership
 *
 * subsystem C code -> typed instruction primitive -> compiler asm boundary
 *
 * Key rules:
 *   - this header owns C implementations of event, hint, cache, translation,
 *     and generic SYS instructions;
 *   - every primitive is volatile and carries a compiler memory clobber;
 *   - register operands are evaluated once before entering inline assembly;
 *   - architecture barrier sequences remain visible at the semantic caller.
 */
#define ARM64_INSN_STRINGIFY_INNER(token) #token
#define ARM64_INSN_STRINGIFY(token) ARM64_INSN_STRINGIFY_INNER(token)

static inline void arm64_wfi(void)
{
	asm volatile ("wfi" ::: "memory");
}

static inline void arm64_wfe(void)
{
	asm volatile ("wfe" ::: "memory");
}

static inline void arm64_sev(void)
{
	asm volatile ("sev" ::: "memory");
}

static inline void arm64_sevl(void)
{
	asm volatile ("sevl" ::: "memory");
}

static inline void arm64_yield(void)
{
	asm volatile ("yield" ::: "memory");
}

#define arm64_at(operation, value) do { \
	uint64_t _arm64_instruction_value = (uint64_t)(value); \
	asm volatile ("at " ARM64_INSN_STRINGIFY(operation) ", %0" \
		: : "r" (_arm64_instruction_value) : "memory"); \
} while (0)

#define arm64_tlbi(operation) \
	asm volatile ("tlbi " ARM64_INSN_STRINGIFY(operation) ::: "memory")

#define arm64_dc(operation, value) do { \
	uint64_t _arm64_instruction_value = (uint64_t)(value); \
	asm volatile ("dc " ARM64_INSN_STRINGIFY(operation) ", %0" \
		: : "r" (_arm64_instruction_value) : "memory"); \
} while (0)

#define arm64_sys(op1, crn, crm, op2) \
	asm volatile ("sys #" ARM64_INSN_STRINGIFY(op1) ", c" \
		ARM64_INSN_STRINGIFY(crn) ", c" ARM64_INSN_STRINGIFY(crm) \
		", #" ARM64_INSN_STRINGIFY(op2) ::: "memory")

#define arm64_sys_write(op1, crn, crm, op2, value) do { \
	uint64_t _arm64_instruction_value = (uint64_t)(value); \
	asm volatile ("sys #" ARM64_INSN_STRINGIFY(op1) ", c" \
		ARM64_INSN_STRINGIFY(crn) ", c" ARM64_INSN_STRINGIFY(crm) \
		", #" ARM64_INSN_STRINGIFY(op2) ", %0" \
		: : "r" (_arm64_instruction_value) : "memory"); \
} while (0)

#define arm64_sysl_read(op1, crn, crm, op2) ({ \
	uint64_t _arm64_instruction_value; \
	asm volatile ("sysl %0, #" ARM64_INSN_STRINGIFY(op1) ", c" \
		ARM64_INSN_STRINGIFY(crn) ", c" ARM64_INSN_STRINGIFY(crm) \
		", #" ARM64_INSN_STRINGIFY(op2) \
		: "=r" (_arm64_instruction_value) : : "memory"); \
	_arm64_instruction_value; \
})

#endif /* ARM64_INSTRUCTION_H */
