/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GEN_OFFSET_H
#define ARM64_GEN_OFFSET_H

#include <lib/util.h>

/* GCC emits these constants as global absolute ELF symbols. */
#define GEN_ABSOLUTE_SYM(name, value) \
	__asm__ __volatile__(".globl\t" #name "\n\t.equ\t" #name \
		",%c0\n\t.type\t" #name ",@object" : : "n"(value))

#define GEN_OFFSET_SYM(type, member, name) \
	GEN_ABSOLUTE_SYM(name, offsetof(type, member))

#define GEN_SIZE_SYM(type, name) \
	GEN_ABSOLUTE_SYM(name, sizeof(type))

#define GEN_ABS_SYM_BEGIN(name) \
	void name(void); \
	void name(void) \
	{

#define GEN_ABS_SYM_END \
	}

#endif /* ARM64_GEN_OFFSET_H */
