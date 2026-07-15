/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <bsp/pm.h>

#define BSP_PM_MAX_WAKE_IRQS	8U

static uint32_t bsp_pm_wakeup_irqs[BSP_PM_MAX_WAKE_IRQS];
static uint16_t bsp_pm_wakeup_irq_count;

int32_t bsp_pm_set_wakeup_irqs(const uint32_t *irqs, uint16_t count)
{
	uint16_t idx;

	if ((irqs == NULL) || (count == 0U) || (count > BSP_PM_MAX_WAKE_IRQS)) {
		return -EINVAL;
	}

	for (idx = 0U; idx < count; idx++) {
		bsp_pm_wakeup_irqs[idx] = irqs[idx];
	}
	bsp_pm_wakeup_irq_count = count;

	return 0;
}

int32_t bsp_pm_request_suspend(void)
{
	struct beau_pm_snapshot snapshot;

	hv_pm_get_snapshot(&snapshot);
	return hv_pm_request_suspend(snapshot.controller_vmid);
}

int32_t bsp_pm_abort(int32_t reason)
{
	struct beau_pm_snapshot snapshot;

	hv_pm_get_snapshot(&snapshot);
	return hv_pm_abort(snapshot.epoch, reason);
}

int32_t bsp_pm_request_wake(uint32_t wake_source)
{
	uint16_t idx;

	for (idx = 0U; idx < bsp_pm_wakeup_irq_count; idx++) {
		if (bsp_pm_wakeup_irqs[idx] == wake_source) {
			return hv_pm_record_wake(wake_source, idx);
		}
	}

	return -EACCES;
}
