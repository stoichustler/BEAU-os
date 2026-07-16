/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_HV_PM_H
#define ARM64_HV_PM_H

#include <types.h>

#define ARM64_PLATFORM_PM_CAP_SIMULATED	(1U << 0U)
#define ARM64_PLATFORM_PM_CAP_HARDWARE	(1U << 1U)

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

/* [20260716] ARM64 platform suspend ownership
 *
 * core PM preflight -> architecture retention -> platform ops -> restore
 *
 * Key rule:
 *   - immutable capability bits reject unsupported modes before an epoch starts;
 *   - prepare owns platform wake arming until wake or abort releases it;
 *   - architecture code reports whether host isolation was fully restored;
 *   - hardware backends keep this interface while unsupported operations fail closed.
 */
struct arm64_platform_pm_ops {
	const char *name;
	uint32_t capabilities;
	int32_t (*prepare)(uint64_t epoch, struct arm64_host_pm_context *context);
	int32_t (*enter)(uint64_t epoch, struct arm64_host_pm_context *context);
	int32_t (*wake)(uint64_t epoch, struct arm64_host_pm_context *context);
	int32_t (*abort)(uint64_t epoch, struct arm64_host_pm_context *context);
};

int64_t arm64_suspend_enter(struct arm64_suspend_callee_context *context,
	uint64_t resume_pa, uint64_t context_id);
void arm64_suspend_resume(void);
void arm64_save_el2_context(struct arm64_el2_pm_context *context);
void arm64_restore_el2_context(const struct arm64_el2_pm_context *context);
const struct arm64_platform_pm_ops *arm64_platform_pm_get_ops(void);
int32_t arch_pm_suspend_secondary_cpus(uint64_t epoch, bool *restored);
int32_t arch_pm_resume_secondary_cpus(uint64_t epoch);
void arch_pm_process_secondary_from_idle(uint16_t pcpu_id);

#endif /* ARM64_HV_PM_H */
