/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_HV_PM_H
#define ARM64_HV_PM_H

#include <types.h>

struct arm64_suspend_callee_context {
	uint64_t x19_x30[12U];
	uint64_t sp;
};

struct arm64_el2_pm_context {
	uint64_t vbar_el2;
	uint64_t sctlr_el2;
	uint64_t tcr_el2;
	uint64_t ttbr0_el2;
	uint64_t mair_el2;
	uint64_t hcr_el2;
	uint64_t vtcr_el2;
	uint64_t vttbr_el2;
	uint64_t cptr_el2;
	uint64_t cnthctl_el2;
	uint64_t cntvoff_el2;
	uint64_t mdcr_el2;
	uint64_t tpidr_el2;
};

struct arm64_host_pm_context {
	struct arm64_suspend_callee_context callee;
	struct arm64_el2_pm_context el2;
	uint64_t epoch;
	bool valid;
};

int64_t arm64_suspend_enter(struct arm64_suspend_callee_context *context,
	uint64_t resume_pa, uint64_t context_id);
void arm64_suspend_resume(void);
void arm64_save_el2_context(struct arm64_el2_pm_context *context);
void arm64_restore_el2_context(const struct arm64_el2_pm_context *context);
int32_t arm64_platform_pm_enter(uint64_t epoch,
	struct arm64_host_pm_context *context);
int32_t arch_pm_suspend_secondary_cpus(uint64_t epoch);
int32_t arch_pm_resume_secondary_cpus(uint64_t epoch);

#endif /* ARM64_HV_PM_H */
