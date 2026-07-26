/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ticks.h>

#include "dhrystone_port.h"

static volatile bool dhrystone_active;

bool dhrystone_port_acquire(void)
{
	bool expected = false;

	return __atomic_compare_exchange_n(&dhrystone_active, &expected, true, false,
		__ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void dhrystone_port_release(void)
{
	__atomic_store_n(&dhrystone_active, false, __ATOMIC_RELEASE);
}

uint64_t dhrystone_port_ticks(void)
{
	return cpu_ticks();
}

uint64_t dhrystone_port_ticks_per_second(void)
{
	uint64_t rate = (uint64_t)arch_cpu_tickrate() * 1000UL;

	return rate == 0UL ? 1UL : rate;
}
