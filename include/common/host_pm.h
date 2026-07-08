/*
 * Copyright (C) 2018-2025 Intel Corporation.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef	HOST_PM_H
#define	HOST_PM_H

#include <asm/host_pm.h>
#include <logmsg.h>

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

#endif /* HOST_PM_H */
