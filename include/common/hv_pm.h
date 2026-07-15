/*
 * Copyright (C) 2018-2026 Intel Corporation.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HV_PM_H
#define HV_PM_H

#include <asm/hv_pm.h>
#include <logmsg.h>

enum beau_pm_system_state {
	PM_RUNNING = 0U,
	PM_PREPARING,
	PM_GUESTS_QUIESCED,
	PM_FREEZING_HOST,
	PM_SUSPENDED,
	PM_RESTORING_HOST,
	PM_RESUMING_GUESTS,
	PM_ABORTING,
	PM_FAILED,
};

const char *hv_pm_state_to_str(enum beau_pm_system_state state);

void arch_shutdown_host(void);
void arch_reset_host(bool warm);

static inline void shutdown_host(void) {
	LOG_INF("shutting down BEAU");
	arch_shutdown_host();
}

static inline void reset_host(bool warm) {
	LOG_INF("%s rebooting BEAU", warm ? "warm" : "cold");
	arch_reset_host(warm);
}

#endif /* HV_PM_H */
