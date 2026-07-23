/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hcall.h"

LOG_MODULE_REGISTER(beau_wdt);

#define BEAU_WDT_PERIOD_MS			5000
#define BEAU_WDT_STACK_SIZE			1024
#define BEAU_WDT_PRIORITY			10

static unsigned long beau_wdt_kicks;

static long beau_wdt_kick(void)
{
	unsigned long token = ++beau_wdt_kicks;

	return beau_hcall_vm_wdt_kick(token);
}

static void beau_wdt_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("[κ] beau wdt enabled, period:%d ms", BEAU_WDT_PERIOD_MS);

	for (;;) {
		long ret = beau_wdt_kick();

		if (ret != 0) {
			LOG_WRN("[κ] beau wdt kick failed:%ld count:%lu", ret, beau_wdt_kicks);
		}
		k_sleep(K_MSEC(BEAU_WDT_PERIOD_MS));
	}
}

K_THREAD_DEFINE(beau_wdt_tid, BEAU_WDT_STACK_SIZE, beau_wdt_thread,
		NULL, NULL, NULL, BEAU_WDT_PRIORITY, 0, 0);
