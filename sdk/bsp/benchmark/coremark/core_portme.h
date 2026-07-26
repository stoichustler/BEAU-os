/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <types.h>
#include "coremark_port.h"

#define HAS_FLOAT 0
#define HAS_TIME_H 0
#define USE_CLOCK 0
#define HAS_STDIO 0
#define HAS_PRINTF 1
#define COMPILER_VERSION "BEAU GCC " __VERSION__
#define COMPILER_FLAGS "-O2 -ffreestanding -mgeneral-regs-only"
#define MEM_LOCATION "BEAU static context pool"

typedef int16_t ee_s16;
typedef uint16_t ee_u16;
typedef int32_t ee_s32;
typedef uint32_t ee_f32;
typedef uint8_t ee_u8;
typedef uint32_t ee_u32;
typedef uint64_t ee_ptr_int;
typedef size_t ee_size_t;

#define align_mem(x) ((void *)(4UL + ((((ee_ptr_int)(x)) - 1UL) & ~3UL)))
#define CORETIMETYPE uint64_t
typedef uint64_t CORE_TICKS;

#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD MEM_MALLOC
#define MULTITHREAD COREMARK_CONTEXTS
#define PARALLEL_METHOD "BEAU-pCPU"
#define MAIN_HAS_NOARGC 1
#define MAIN_HAS_NORETURN 0
#define printf coremark_printf

extern ee_u32 default_num_contexts;

struct CORE_PORTABLE_S {
	ee_u8 portable_id;
};
typedef struct CORE_PORTABLE_S core_portable;

void portable_init(core_portable *portable, int *argc, char *argv[]);
void portable_fini(core_portable *portable);

#endif /* CORE_PORTME_H */
