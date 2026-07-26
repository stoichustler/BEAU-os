/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <rtl.h>
#include <ticks.h>
#include <util.h>

#include "coremark.h"

static uint8_t coremark_pool[COREMARK_CONTEXTS][COREMARK_TOTAL_DATA_SIZE]
	__aligned(16);
static uint16_t coremark_pool_next;
static uint64_t coremark_start_ticks;
static uint64_t coremark_stop_ticks;

volatile ee_s32 seed1_volatile;
volatile ee_s32 seed2_volatile;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile;
volatile ee_s32 seed5_volatile;
ee_u32 default_num_contexts = COREMARK_CONTEXTS;

int32_t coremark_printf(__unused const char *fmt, ...)
{
	return 0;
}

void start_time(void)
{
	coremark_start_ticks = cpu_ticks();
}

void stop_time(void)
{
	coremark_stop_ticks = cpu_ticks();
}

CORE_TICKS get_time(void)
{
	return coremark_stop_ticks - coremark_start_ticks;
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
	uint64_t tick_rate = (uint64_t)arch_cpu_tickrate() * 1000UL;

	return tick_rate == 0UL ? 0U : (secs_ret)(ticks / tick_rate);
}

void *portable_malloc(ee_size_t size)
{
	void *memory = NULL;

	if ((size == COREMARK_TOTAL_DATA_SIZE) &&
		(coremark_pool_next < COREMARK_CONTEXTS)) {
		memory = coremark_pool[coremark_pool_next];
		coremark_pool_next++;
	}
	return memory;
}

void portable_free(__unused void *memory)
{
}

void portable_init(core_portable *portable, int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	if (portable != NULL) {
		portable->portable_id = 1U;
	}
	default_num_contexts = COREMARK_CONTEXTS;
	if (sizeof(ee_ptr_int) != sizeof(ee_u8 *)) {
		(void)coremark_printf("ERROR! invalid BEAU CoreMark pointer type\n");
	}
}

void portable_fini(core_portable *portable)
{
	if (portable != NULL) {
		portable->portable_id = 0U;
	}
}
