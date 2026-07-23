/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/arm64/arm-smccc.h>
#include <zephyr/kernel/internal/mm.h>

#include "hcall.h"

#define BEAU_HC_ID			0x80UL
#define BEAU_HC_ID_DBG_BASE		0x60UL
#define BEAU_HC_ID_MAKE(x, y)		(((x) << 24) | (y))
#define BEAU_HC_VM_WDT_KICK		BEAU_HC_ID_MAKE(BEAU_HC_ID, BEAU_HC_ID_DBG_BASE + 0x04UL)
#define BEAU_HC_IPC			BEAU_HC_ID_MAKE(BEAU_HC_ID, BEAU_HC_ID_DBG_BASE + 0x06UL)
#define BEAU_HC_AI_SCHED		BEAU_HC_ID_MAKE(BEAU_HC_ID, BEAU_HC_ID_DBG_BASE + 0x07UL)

static long beau_hcall1(unsigned long hcall_id, unsigned long param1)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(hcall_id, param1, 0, 0, 0, 0, 0, 0, &res);

	return (long)res.a0;
}

long beau_hcall_vm_wdt_kick(unsigned long token)
{
	return beau_hcall1(BEAU_HC_VM_WDT_KICK, token);
}

long beau_hcall_ipc(struct beau_ipc_ioc *ioc)
{
	return beau_hcall1(BEAU_HC_IPC, k_mem_phys_addr(ioc));
}

long beau_hcall_ai_sched(struct beau_ai_sched_ioc *ioc)
{
	return beau_hcall1(BEAU_HC_AI_SCHED, k_mem_phys_addr(ioc));
}
