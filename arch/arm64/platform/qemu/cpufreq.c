/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <logmsg.h>
#include <bsp/cpufreq.h>

static int32_t qemu_cpufreq_init(void)
{
	return 0;
}

static int32_t qemu_cpufreq_set_pstate(const struct cpufreq_platform_domain *domain,
	const struct cpufreq_pstate *pstate)
{
	(void)domain;
	(void)pstate;
	return 0;
}

static const struct cpufreq_platform_ops qemu_cpufreq_ops = {
	.name = "stub",
	.init = qemu_cpufreq_init,
	.set_pstate = qemu_cpufreq_set_pstate,
};

const struct cpufreq_platform_ops *cpufreq_get_platform_ops(void)
{
	return &qemu_cpufreq_ops;
}
