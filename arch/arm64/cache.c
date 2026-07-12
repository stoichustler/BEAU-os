/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <memory.h>
#include <asm/cache.h>
#include <asm/sysreg.h>

/*
 * [20260712] ARM64 cache discovery framework:
 *
 *   CTR_EL0      -> minimum I/D cache line size
 *   CLIDR_EL1    -> cache type present at each architectural level
 *   CSSELR_EL1   -> selects level + I-cache/D-cache view
 *   CCSIDR_EL1   -> line size, associativity, set count
 *        |
 *        v
 *   arm64_cache_info
 *        |
 *        +-- shell cachestat
 *        +-- future LLC policy / cache-partition hooks
 *
 * The current QEMU and BEAU reference platform expose one shared LLC domain.
 * This file keeps the topology query explicit so platform-specific isolation
 * can be added without changing VM or shell code.
 */
#define ARM64_CACHE_LEVEL_MAX		7U
#define ARM64_CLIDR_CTYPE_MASK		0x7UL
#define ARM64_CLIDR_CTYPE_NONE		0UL
#define ARM64_CLIDR_CTYPE_INST		1UL
#define ARM64_CLIDR_CTYPE_DATA		2UL
#define ARM64_CLIDR_CTYPE_SEPARATE	3UL
#define ARM64_CLIDR_CTYPE_UNIFIED	4UL

#define ARM64_CCSIDR_LINESIZE_MASK	0x7UL
#define ARM64_CCSIDR_ASSOC_SHIFT	3U
#define ARM64_CCSIDR_ASSOC_MASK		0x3ffUL
#define ARM64_CCSIDR_NUMSETS_SHIFT	13U
#define ARM64_CCSIDR_NUMSETS_MASK	0x7fffUL

#define ARM64_CTR_IMINLINE_SHIFT	0U
#define ARM64_CTR_DMINLINE_SHIFT	16U
#define ARM64_CTR_LINE_MASK		0xfUL

static struct arm64_cache_info arm64_cache_state;
static bool arm64_cache_initialized;

static uint64_t arm64_cache_all_pcpu_mask(void)
{
	if (MAX_PCPU_NUM >= 64U) {
		return UINT64_MAX;
	}
	return (1UL << MAX_PCPU_NUM) - 1UL;
}

static uint32_t arm64_cache_line_from_ctr(uint64_t ctr_el0, uint32_t shift)
{
	uint32_t words = (uint32_t)((ctr_el0 >> shift) & ARM64_CTR_LINE_MASK);

	return 4U << words;
}

static uint32_t arm64_cache_line_from_ccsidr(uint64_t ccsidr)
{
	uint32_t words = (uint32_t)(ccsidr & ARM64_CCSIDR_LINESIZE_MASK);

	return 16U << words;
}

static bool arm64_cache_add_leaf(uint8_t level, uint8_t type, bool instruction)
{
	struct arm64_cache_leaf *leaf;
	uint64_t ccsidr;
	uint32_t idx = arm64_cache_state.leaf_count;

	if (idx >= ARM64_CACHE_MAX_LEAVES) {
		return false;
	}

	write_csselr_el1((((uint64_t)level - 1UL) << 1U) | (instruction ? 1UL : 0UL));
	ccsidr = read_ccsidr_el1();

	leaf = &arm64_cache_state.leaves[idx];
	leaf->level = level;
	leaf->type = type;
	leaf->line_size = arm64_cache_line_from_ccsidr(ccsidr);
	leaf->ways = (uint32_t)((ccsidr >> ARM64_CCSIDR_ASSOC_SHIFT) &
		ARM64_CCSIDR_ASSOC_MASK) + 1U;
	leaf->sets = (uint32_t)((ccsidr >> ARM64_CCSIDR_NUMSETS_SHIFT) &
		ARM64_CCSIDR_NUMSETS_MASK) + 1U;
	leaf->size = (uint64_t)leaf->line_size * leaf->ways * leaf->sets;
	leaf->shared_pcpu_mask = arm64_cache_all_pcpu_mask();
	arm64_cache_state.leaf_count++;

	arm64_cache_state.llc_level = level;
	arm64_cache_state.llc_type = type;
	arm64_cache_state.llc_size = leaf->size;
	arm64_cache_state.llc_pcpu_mask = leaf->shared_pcpu_mask;
	arm64_cache_state.llc_domain_count = 1U;
	return true;
}

static void arm64_cache_collect(void)
{
	uint64_t clidr;
	uint32_t level;

	(void)memset(&arm64_cache_state, 0U, sizeof(arm64_cache_state));
	arm64_cache_state.ctr_el0 = read_ctr_el0();
	arm64_cache_state.clidr_el1 = read_clidr_el1();
	arm64_cache_state.dcache_line_size = arm64_cache_line_from_ctr(
		arm64_cache_state.ctr_el0, ARM64_CTR_DMINLINE_SHIFT);
	arm64_cache_state.icache_line_size = arm64_cache_line_from_ctr(
		arm64_cache_state.ctr_el0, ARM64_CTR_IMINLINE_SHIFT);

	clidr = arm64_cache_state.clidr_el1;
	for (level = 1U; level <= ARM64_CACHE_LEVEL_MAX; level++) {
		uint64_t ctype = (clidr >> ((level - 1U) * 3U)) & ARM64_CLIDR_CTYPE_MASK;

		if (ctype == ARM64_CLIDR_CTYPE_NONE) {
			break;
		}
		if (ctype == ARM64_CLIDR_CTYPE_SEPARATE) {
			if (!arm64_cache_add_leaf((uint8_t)level, ARM64_CACHE_TYPE_DATA, false)) {
				break;
			}
			if (!arm64_cache_add_leaf((uint8_t)level, ARM64_CACHE_TYPE_INST, true)) {
				break;
			}
		} else if (ctype == ARM64_CLIDR_CTYPE_DATA) {
			if (!arm64_cache_add_leaf((uint8_t)level, ARM64_CACHE_TYPE_DATA, false)) {
				break;
			}
		} else if (ctype == ARM64_CLIDR_CTYPE_INST) {
			if (!arm64_cache_add_leaf((uint8_t)level, ARM64_CACHE_TYPE_INST, true)) {
				break;
			}
		} else if (ctype == ARM64_CLIDR_CTYPE_UNIFIED) {
			if (!arm64_cache_add_leaf((uint8_t)level, ARM64_CACHE_TYPE_UNIFIED, false)) {
				break;
			}
		} else {
			break;
		}
	}
	arm64_cache_state.valid = true;
}

void arm64_cache_init(void)
{
	if (!arm64_cache_initialized) {
		arm64_cache_collect();
		arm64_cache_initialized = true;
	}
}

void arm64_cache_get_info(struct arm64_cache_info *info)
{
	if (info == NULL) {
		return;
	}

	arm64_cache_init();
	*info = arm64_cache_state;
}

const char *arm64_cache_type_str(uint8_t type)
{
	if (type == ARM64_CACHE_TYPE_INST) {
		return "I";
	}
	if (type == ARM64_CACHE_TYPE_DATA) {
		return "D";
	}
	if (type == ARM64_CACHE_TYPE_UNIFIED) {
		return "Unified";
	}
	return "None";
}

uint32_t arm64_cache_llc_id_for_pcpu(uint16_t pcpu_id)
{
	if (pcpu_id >= MAX_PCPU_NUM) {
		return UINT32_MAX;
	}

	return 0U;
}
