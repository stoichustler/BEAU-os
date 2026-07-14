/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <logmsg.h>
#include <bsp/cpufreq.h>

static int32_t rk356x_cpufreq_init(void)
{
	return 0;
}

static int32_t rk356x_cpufreq_set_pstate(const struct cpufreq_platform_domain *domain,
	const struct cpufreq_pstate *pstate)
{
	(void)domain;
	(void)pstate;
	return 0;
}

static const struct cpufreq_platform_ops rk356x_cpufreq_ops = {
	.name = "stub",
	.init = rk356x_cpufreq_init,
	.set_pstate = rk356x_cpufreq_set_pstate,
};

const struct cpufreq_platform_ops *cpufreq_get_platform_ops(void)
{
	return &rk356x_cpufreq_ops;
}
