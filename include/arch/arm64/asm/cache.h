/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_CACHE_H
#define ARM64_CACHE_H

#include <types.h>
#include <cpu.h>

#define ARM64_CACHE_MAX_LEAVES		16U
#define ARM64_CACHE_TYPE_NONE		0U
#define ARM64_CACHE_TYPE_INST		1U
#define ARM64_CACHE_TYPE_DATA		2U
#define ARM64_CACHE_TYPE_UNIFIED	3U

struct arm64_cache_leaf {
	uint8_t level;
	uint8_t type;
	uint16_t reserved;
	uint32_t line_size;
	uint32_t sets;
	uint32_t ways;
	uint64_t size;
	uint64_t shared_pcpu_mask;
};

struct arm64_cache_info {
	bool valid;
	uint32_t dcache_line_size;
	uint32_t icache_line_size;
	uint32_t leaf_count;
	uint32_t llc_domain_count;
	uint8_t llc_level;
	uint8_t llc_type;
	uint16_t reserved;
	uint64_t ctr_el0;
	uint64_t clidr_el1;
	uint64_t llc_size;
	uint64_t llc_pcpu_mask;
	struct arm64_cache_leaf leaves[ARM64_CACHE_MAX_LEAVES];
};

void arm64_cache_init(void);
void arm64_cache_get_info(struct arm64_cache_info *info);
const char *arm64_cache_type_str(uint8_t type);
uint32_t arm64_cache_llc_id_for_pcpu(uint16_t pcpu_id);

#endif /* ARM64_CACHE_H */
