/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BSP_CPUFREQ_H
#define BSP_CPUFREQ_H

#include <types.h>
#include <cpu.h>

#define CPUFREQ_NAME_LEN	16U
#define CPUFREQ_MAX_PSTATES	16U
#define CPUFREQ_MAX_DOMAINS	MAX_PCPU_NUM

struct cpufreq_pstate {
	uint32_t freq_khz;
	uint32_t platform_id;
};

struct cpufreq_platform_domain {
	uint32_t id;
	uint64_t cpu_mask;
};

struct cpufreq_platform_ops {
	const char *name;
	int32_t (*init)(void);
	int32_t (*set_pstate)(const struct cpufreq_platform_domain *domain,
		const struct cpufreq_pstate *pstate);
};

void cpufreq_init(void);
void cpufreq_apply_performance(void);
void cpufreq_dump(void);
bool cpufreq_is_enabled(void);
const struct cpufreq_platform_ops *cpufreq_get_platform_ops(void);

#endif /* BSP_CPUFREQ_H */
