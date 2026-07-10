/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#ifndef CONFIG_STATIC_ARM64_PLATFORM
#include <config.h>
#endif
#include <reloc.h>

uint64_t arch_get_hv_image_delta(void)
{
	uint64_t start;

	asm volatile (
		"adrp %0, _start\n"
		"add %0, %0, :lo12:_start\n"
		: "=r" (start));

	return start - CONFIG_HV_RAM_START;
}

#ifdef CONFIG_RELOC
void relocate(__unused struct Elf64_Dyn *dynamic)
{
	/* Dynamic relocation support is not part of the first-stage ARM64 port. */
}
#endif
