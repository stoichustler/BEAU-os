/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BSP_PM_H
#define BSP_PM_H

#include <types.h>

struct acrn_vcpu;

int32_t bsp_pm_set_wakeup_irqs(const uint32_t *irqs, uint16_t count);
int32_t bsp_pm_set_event_virq(uint32_t virq);
int32_t bsp_pm_request_suspend(void);
int32_t bsp_pm_abort(int32_t reason);
int32_t bsp_pm_request_wake(uint32_t wake_source);
int32_t bsp_pm_control_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa);

#endif /* BSP_PM_H */
