/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PLATFORM_H
#define ARM64_PLATFORM_H

#include <types.h>

#ifndef ASSEMBLER
struct arm64_mem_region {
	uint64_t base;
	uint64_t size;
};

struct beau_config {
	uint64_t ram_start;
	uint64_t ram_size;

	uint64_t console_mmio_base;

	uint64_t gicd_base;
	uint64_t gicd_size;
	uint64_t gicr_base;
	uint64_t gicr_stride;
	uint64_t gicr_size;
	uint64_t gits_base;
	uint64_t gits_size;
	uint64_t smmu_base;
	uint64_t smmu_size;
	uint32_t gic_iidr;
	uint32_t spe_ppi;
};

extern struct beau_config beau_config;

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count);
void arm64_platform_init(uint64_t fdt_paddr);
void arm64_platform_init_post_console(void);
void arm64_platform_init_smmu(void);
#endif

#endif /* ARM64_PLATFORM_H */
