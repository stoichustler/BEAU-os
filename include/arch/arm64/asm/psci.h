/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PSCI_H
#define ARM64_PSCI_H

#include <types.h>

#define PSCI_RET_SUCCESS		0L
#define PSCI_RET_NOT_SUPPORTED		(-1L)
#define PSCI_RET_INVALID_PARAMS		(-2L)
#define PSCI_RET_DENIED			(-3L)
#define PSCI_RET_ALREADY_ON		(-4L)
#define PSCI_RET_ON_PENDING		(-5L)
#define PSCI_RET_INTERNAL_FAILURE	(-6L)
#define PSCI_RET_NOT_PRESENT		(-7L)
#define PSCI_RET_DISABLED		(-8L)
#define PSCI_RET_INVALID_ADDRESS	(-9L)

int64_t psci_cpu_on(uint64_t target_cpu, uint64_t entry_point, uint64_t context_id);
int64_t psci_system_off(void);
int64_t psci_system_reset(void);
int64_t psci_system_suspend(uint64_t resume_entry, uint64_t context_id);

#endif /* ARM64_PSCI_H */
