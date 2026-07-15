/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <hv_pm.h>
#include <asm/psci.h>

void arch_shutdown_host(void)
{
	int64_t ret = psci_system_off();

	panic("arm64 psci system off failed, ret=%ld", ret);
}

void arch_reset_host(__unused bool warm)
{
	int64_t ret = psci_system_reset();

	panic("arm64 psci system reset failed, ret=%ld", ret);
}
