/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VTIMER_H
#define ARM64_GUEST_VTIMER_H

#include <types.h>

struct acrn_vm;

int32_t arm64_vtimer_suspend_vm(struct acrn_vm *vm, uint64_t epoch);
int32_t arm64_vtimer_resume_vm(struct acrn_vm *vm, uint64_t epoch);

#endif /* ARM64_GUEST_VTIMER_H */
