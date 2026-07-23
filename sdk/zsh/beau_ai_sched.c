/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "beau_ai_model.h"
#include "beau_ai_sched.h"
#include "hcall.h"

LOG_MODULE_REGISTER(beau_ai_sched);

#define BEAU_AI_SCHED_PERIOD_MS	100U
#define BEAU_AI_SCHED_STACK_SIZE	1536U
#define BEAU_AI_SCHED_PRIORITY		12

static struct beau_ai_sched_ioc beau_ai_sched_ioc __aligned(64);
static uint64_t beau_ai_sched_capability;

static int beau_ai_sched_call(uint32_t op)
{
	long ret;

	memset(&beau_ai_sched_ioc, 0, sizeof(beau_ai_sched_ioc));
	beau_ai_sched_ioc.op = op;
	beau_ai_sched_ioc.abi_version = BEAU_AI_SCHED_ABI_VERSION;
	beau_ai_sched_ioc.ioc_size = sizeof(beau_ai_sched_ioc);
	beau_ai_sched_ioc.capability = beau_ai_sched_capability;
	ret = beau_hcall_ai_sched(&beau_ai_sched_ioc);
	if (ret != 0) {
		return (int)ret;
	}
	return beau_ai_sched_ioc.status == BEAU_AI_SCHED_STATUS_OK ? 0 : -EACCES;
}

static void beau_ai_sched_thread(void *arg1, void *arg2, void *arg3)
{
	int ret;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	ret = beau_ai_sched_call(BEAU_AI_SCHED_OP_REGISTER);
	if (ret != 0) {
		LOG_WRN("AI advisor registration unavailable:%d", ret);
		return;
	}
	beau_ai_sched_capability = beau_ai_sched_ioc.capability;
	LOG_INF("AI advisor registered; model=%s", BEAU_AI_MODEL_AVAILABLE ? "ready" : "untrained");

	for (;;) {
		ret = beau_ai_sched_call(BEAU_AI_SCHED_OP_SNAPSHOT);
		if (ret != 0) {
			LOG_WRN("AI advisor snapshot failed:%d; re-register required after reset", ret);
			return;
		}
#if BEAU_AI_MODEL_AVAILABLE
		/* A generated model may fill a bounded PROPOSE request here. */
#endif
		k_sleep(K_MSEC(BEAU_AI_SCHED_PERIOD_MS));
	}
}

K_THREAD_DEFINE(beau_ai_sched_tid, BEAU_AI_SCHED_STACK_SIZE, beau_ai_sched_thread,
	NULL, NULL, NULL, BEAU_AI_SCHED_PRIORITY, 0, 0);

void beau_ai_sched_start(void)
{
	/* K_THREAD_DEFINE starts the service during normal Zephyr initialization. */
}
