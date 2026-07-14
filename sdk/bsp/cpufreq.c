/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <libfdt.h>
#include <logmsg.h>
#include <rtl.h>
#include <sprintf.h>
#include <fdt_api.h>
#include <bsp/cpufreq.h>
#include "shell_priv.h"

struct cpufreq_domain {
	struct cpufreq_platform_domain platform;
	uint32_t min_khz;
	uint32_t max_khz;
	uint32_t pstate_count;
	uint32_t current_index;
	uint32_t transition_count;
	uint32_t fail_count;
	int32_t last_error;
	struct cpufreq_pstate pstates[CPUFREQ_MAX_PSTATES];
};

struct cpufreq_context {
	bool initialized;
	bool enabled;
	const char *policy;
	const char *backend;
	const struct cpufreq_platform_ops *ops;
	uint32_t domain_count;
	uint32_t fail_count;
	int32_t last_error;
	struct cpufreq_domain domains[CPUFREQ_MAX_DOMAINS];
};

static struct cpufreq_context cpufreq_ctx;

#ifdef CONFIG_CPUFREQ

/* [20260714] BSP CPUFreq policy split
 *
 * The BSP layer owns static policy and validation; platform code owns the
 * hardware transition path.
 *
 *   platform.dts
 *        |
 *        v
 *   sdk/bsp/cpufreq.c
 *     - parse domains and P-states
 *     - performance policy selects max frequency
 *     - dispatch selected P-state to platform ops
 *        |
 *        v
 *   arch/arm64/platform/<platform>/cpufreq.c
 *     - stub today
 *     - CRU/OPP/regulator sequence later
 *
 * Key rule:
 *   - DTS errors fail closed before VM launch;
 *   - guests never own host P-state transitions;
 *   - platform backends can change without changing the BSP policy contract.
 */

static void cpufreq_dts_panic(const char *op, int32_t ret)
{
	panic("failed to parse cpufreq dts: %s ret=%d", op, ret);
}

static uint64_t cpufreq_expected_mask(void)
{
	uint32_t expected = CONFIG_CPUFREQ_EXPECTED_CPUS;

	if ((expected == 0U) || (expected > MAX_PCPU_NUM)) {
		panic("invalid cpufreq expected cpu count %u max=%u",
			expected, MAX_PCPU_NUM);
	}

	return (expected >= 64U) ? UINT64_MAX : ((1UL << expected) - 1UL);
}

static const char *cpufreq_string_prop(const void *fdt, int32_t node,
	const char *name, const char *fallback)
{
	const char *value = fdt_getprop(fdt, node, name, NULL);

	return (value != NULL) ? value : fallback;
}

static uint32_t cpufreq_u32_prop(const void *fdt, int32_t node, const char *name)
{
	const fdt32_t *prop;
	int32_t len;

	prop = fdt_getprop(fdt, node, name, &len);
	if ((prop == NULL) || (len != (int32_t)sizeof(fdt32_t))) {
		cpufreq_dts_panic(name, prop == NULL ? len : -EINVAL);
	}

	return fdt32_to_cpu(prop[0]);
}

static uint32_t cpufreq_domain_id(const void *fdt, int32_t node, uint32_t fallback)
{
	const fdt32_t *reg;
	int32_t len;

	reg = fdt_getprop(fdt, node, "reg", &len);
	if (reg == NULL) {
		return fallback;
	}
	if (len != (int32_t)sizeof(fdt32_t)) {
		cpufreq_dts_panic("cpufreq domain reg", -EINVAL);
	}

	return fdt32_to_cpu(reg[0]);
}

static uint64_t cpufreq_parse_cpu_mask(const void *fdt, int32_t node,
	uint64_t *global_mask)
{
	const fdt32_t *prop;
	uint64_t mask = 0UL;
	int32_t len;
	int32_t cells;
	int32_t idx;

	prop = fdt_getprop(fdt, node, "cpus", &len);
	if ((prop == NULL) || (len == 0) || ((len % (int32_t)sizeof(fdt32_t)) != 0)) {
		cpufreq_dts_panic("cpufreq cpus", prop == NULL ? len : -EINVAL);
	}

	cells = len / (int32_t)sizeof(fdt32_t);
	for (idx = 0; idx < cells; idx++) {
		uint32_t pcpu_id = fdt32_to_cpu(prop[idx]);
		uint64_t bit;

		if ((pcpu_id >= CONFIG_CPUFREQ_EXPECTED_CPUS) || (pcpu_id >= MAX_PCPU_NUM)) {
			panic("invalid cpufreq pcpu id %u", pcpu_id);
		}

		bit = 1UL << pcpu_id;
		if ((mask & bit) != 0UL) {
			panic("duplicate cpufreq pcpu id %u in domain", pcpu_id);
		}
		if ((*global_mask & bit) != 0UL) {
			panic("duplicate cpufreq pcpu id %u across domains", pcpu_id);
		}

		mask |= bit;
		*global_mask |= bit;
	}

	return mask;
}

static void cpufreq_parse_pstates(const void *fdt, int32_t node,
	struct cpufreq_domain *domain)
{
	const fdt32_t *prop;
	uint32_t idx;
	int32_t len;
	int32_t entries;

	prop = fdt_getprop(fdt, node, "pstates", &len);
	if ((prop == NULL) || (len == 0) ||
		((len % (int32_t)(2U * sizeof(fdt32_t))) != 0)) {
		cpufreq_dts_panic("cpufreq pstates", prop == NULL ? len : -EINVAL);
	}

	entries = len / (int32_t)(2U * sizeof(fdt32_t));
	if ((entries <= 0) || ((uint32_t)entries > CPUFREQ_MAX_PSTATES)) {
		panic("invalid cpufreq pstate count %d max=%u", entries, CPUFREQ_MAX_PSTATES);
	}

	domain->pstate_count = (uint32_t)entries;
	for (idx = 0U; idx < domain->pstate_count; idx++) {
		domain->pstates[idx].freq_khz = fdt32_to_cpu(prop[idx * 2U]);
		domain->pstates[idx].platform_id = fdt32_to_cpu(prop[(idx * 2U) + 1U]);

		if (domain->pstates[idx].freq_khz == 0U) {
			panic("invalid cpufreq zero frequency in domain%u",
				domain->platform.id);
		}
	}
}

static uint32_t cpufreq_select_performance(const struct cpufreq_domain *domain)
{
	uint32_t idx;
	uint32_t best = UINT32_MAX;
	uint32_t best_freq = 0U;

	for (idx = 0U; idx < domain->pstate_count; idx++) {
		uint32_t freq = domain->pstates[idx].freq_khz;

		if ((freq >= domain->min_khz) && (freq <= domain->max_khz) &&
			(freq > best_freq)) {
			best = idx;
			best_freq = freq;
		}
	}

	if (best == UINT32_MAX) {
		panic("cpufreq domain%u has no performance pstate in %u-%u kHz",
			domain->platform.id, domain->min_khz, domain->max_khz);
	}

	return best;
}

static void cpufreq_validate_domain(struct cpufreq_domain *domain)
{
	uint32_t idx;
	bool has_min = false;
	bool has_max = false;

	if (domain->platform.cpu_mask == 0UL) {
		panic("cpufreq domain%u has empty cpu mask", domain->platform.id);
	}
	if ((domain->min_khz == 0U) || (domain->max_khz == 0U) ||
		(domain->min_khz > domain->max_khz)) {
		panic("invalid cpufreq domain%u limits min=%u max=%u",
			domain->platform.id, domain->min_khz, domain->max_khz);
	}

	for (idx = 0U; idx < domain->pstate_count; idx++) {
		if (domain->pstates[idx].freq_khz == domain->min_khz) {
			has_min = true;
		}
		if (domain->pstates[idx].freq_khz == domain->max_khz) {
			has_max = true;
		}
	}

	if (!has_min || !has_max) {
		panic("cpufreq domain%u limits must match pstates min=%u max=%u",
			domain->platform.id, domain->min_khz, domain->max_khz);
	}

	domain->current_index = UINT32_MAX;
}

static void cpufreq_parse_domain(const void *fdt, int32_t node, uint32_t domain_idx,
	uint64_t *global_mask)
{
	struct cpufreq_domain *domain = &cpufreq_ctx.domains[domain_idx];

	(void)memset(domain, 0U, sizeof(*domain));
	domain->platform.id = cpufreq_domain_id(fdt, node, domain_idx);
	domain->platform.cpu_mask = cpufreq_parse_cpu_mask(fdt, node, global_mask);
	domain->min_khz = cpufreq_u32_prop(fdt, node, "min-khz");
	domain->max_khz = cpufreq_u32_prop(fdt, node, "max-khz");
	cpufreq_parse_pstates(fdt, node, domain);
	cpufreq_validate_domain(domain);
}

static bool cpufreq_node_enabled(const void *fdt, int32_t node)
{
	const char *status = cpufreq_string_prop(fdt, node, "status", "okay");

	if (strcmp(status, "okay") == 0) {
		return true;
	}
	if (strcmp(status, "disabled") == 0) {
		return false;
	}

	panic("unknown cpufreq status '%s'", status);
	return false;
}

static void cpufreq_parse_dts(void)
{
	const void *fdt = get_host_fdt();
	int32_t platform;
	int32_t node;
	int32_t domain_node;
	uint64_t global_mask = 0UL;
	uint32_t expected_pcpus = CONFIG_CPUFREQ_EXPECTED_CPUS;

	platform = fdt_path_offset(fdt, "/beau,platform");
	if (platform < 0) {
		cpufreq_ctx.enabled = false;
		return;
	}

	node = fdt_subnode_offset(fdt, platform, "cpufreq");
	if (node < 0) {
		cpufreq_ctx.enabled = false;
		return;
	}
	if (!cpufreq_node_enabled(fdt, node)) {
		cpufreq_ctx.enabled = false;
		return;
	}

	if (get_pcpu_nums() != expected_pcpus) {
		panic("cpufreq expected %u pcpus but runtime has %hu",
			expected_pcpus, get_pcpu_nums());
	}

	cpufreq_ctx.policy = cpufreq_string_prop(fdt, node, "policy", "performance");
	if (strcmp(cpufreq_ctx.policy, "performance") != 0) {
		panic("unsupported cpufreq policy '%s'", cpufreq_ctx.policy);
	}

	cpufreq_ctx.ops = cpufreq_get_platform_ops();
	if ((cpufreq_ctx.ops == NULL) || (cpufreq_ctx.ops->name == NULL) ||
		(cpufreq_ctx.ops->set_pstate == NULL)) {
		panic("missing cpufreq platform ops");
	}

	cpufreq_ctx.backend = cpufreq_string_prop(fdt, node, "backend",
		cpufreq_ctx.ops->name);
	if (strcmp(cpufreq_ctx.backend, cpufreq_ctx.ops->name) != 0) {
		panic("cpufreq backend '%s' does not match platform ops '%s'",
			cpufreq_ctx.backend, cpufreq_ctx.ops->name);
	}

	fdt_for_each_subnode(domain_node, fdt, node) {
		if (cpufreq_ctx.domain_count >= CPUFREQ_MAX_DOMAINS) {
			panic("too many cpufreq domains max=%u", CPUFREQ_MAX_DOMAINS);
		}

		cpufreq_parse_domain(fdt, domain_node, cpufreq_ctx.domain_count,
			&global_mask);
		cpufreq_ctx.domain_count++;
	}

	if (cpufreq_ctx.domain_count == 0U) {
		panic("cpufreq enabled with no domains");
	}
	if (global_mask != cpufreq_expected_mask()) {
		panic("cpufreq cpu coverage mismatch mask=0x%lx expected=0x%lx",
			global_mask, cpufreq_expected_mask());
	}

	cpufreq_ctx.enabled = true;
}

static void cpufreq_apply_domain_performance(struct cpufreq_domain *domain)
{
	uint32_t target = cpufreq_select_performance(domain);
	int32_t ret;

	if (domain->current_index == target) {
		return;
	}

	ret = cpufreq_ctx.ops->set_pstate(&domain->platform, &domain->pstates[target]);
	if (ret != 0) {
		domain->fail_count++;
		domain->last_error = ret;
		cpufreq_ctx.fail_count++;
		cpufreq_ctx.last_error = ret;
		LOG_ERR("CPUFreq: domain%u backend failed ret=%d freq=%u kHz",
			domain->platform.id, ret, domain->pstates[target].freq_khz);
		return;
	}

	domain->current_index = target;
	domain->transition_count++;
}

void cpufreq_init(void)
{
	int32_t ret;

	if (cpufreq_ctx.initialized) {
		return;
	}

	(void)memset(&cpufreq_ctx, 0U, sizeof(cpufreq_ctx));
	cpufreq_parse_dts();
	if (!cpufreq_ctx.enabled) {
		cpufreq_ctx.initialized = true;
		LOG_INF("CPUFreq: disabled");
		return;
	}

	if (cpufreq_ctx.ops->init != NULL) {
		ret = cpufreq_ctx.ops->init();
		if (ret != 0) {
			panic("cpufreq platform init failed ret=%d", ret);
		}
	}

	cpufreq_apply_performance();
	cpufreq_ctx.initialized = true;
}

void cpufreq_apply_performance(void)
{
	uint32_t idx;

	if (!cpufreq_ctx.enabled) {
		return;
	}

	for (idx = 0U; idx < cpufreq_ctx.domain_count; idx++) {
		cpufreq_apply_domain_performance(&cpufreq_ctx.domains[idx]);
	}
}

bool cpufreq_is_enabled(void)
{
	return cpufreq_ctx.enabled;
}

void cpufreq_dump(void)
{
	uint32_t idx;

	shell_item_begin("CPUFreq");
	shell_item_line("enabled:%s initialized:%s", cpufreq_ctx.enabled ? "yes" : "no",
		cpufreq_ctx.initialized ? "yes" : "no");

	if (cpufreq_ctx.enabled) {
		shell_item_line("policy:%s backend:%s domains:%u failures:%u last-error:%d",
			cpufreq_ctx.policy, cpufreq_ctx.backend, cpufreq_ctx.domain_count,
			cpufreq_ctx.fail_count, cpufreq_ctx.last_error);

		for (idx = 0U; idx < cpufreq_ctx.domain_count; idx++) {
			const struct cpufreq_domain *domain = &cpufreq_ctx.domains[idx];
			const char *cur = "none";
			uint32_t cur_khz = 0U;

			if (domain->current_index < domain->pstate_count) {
				cur_khz = domain->pstates[domain->current_index].freq_khz;
				cur = "applied";
			}

			shell_item_line("domain%u cpus:0x%lx min:%u max:%u cur:%u %s trans:%u fail:%u",
				domain->platform.id, domain->platform.cpu_mask,
				domain->min_khz, domain->max_khz, cur_khz, cur,
				domain->transition_count, domain->fail_count);
		}
	}

	shell_item_end();
}

#else

void cpufreq_init(void)
{
}

void cpufreq_apply_performance(void)
{
}

bool cpufreq_is_enabled(void)
{
	return false;
}

void cpufreq_dump(void)
{
	shell_item_begin("cpufreq");
	shell_item_line("enabled:no");
	shell_item_end();
}

#endif
