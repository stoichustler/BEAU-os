/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PLATFORM_H
#define ARM64_PLATFORM_H

#include <types.h>
#include <cpu.h>

#define ARM64_PLATFORM_LLC_SOURCE_DTS		0U
#define ARM64_PLATFORM_LLC_SOURCE_MPIDR	1U

struct arm64_platform_cpu_topology {
	uint64_t mpidr;
	uint64_t llc_pcpu_mask;
	uint32_t llc_id;
	uint8_t llc_source;
	uint8_t reserved[3U];
};

#ifndef ASSEMBLER
struct arm64_mem_region {
	uint64_t base;
	uint64_t size;
};

struct beau_config {
	uint64_t ram_start;
	uint64_t ram_size;
	uint64_t ramlog_base;
	uint64_t ramlog_size;
	uint32_t ramlog_rtos_size;
	uint32_t ramlog_linux_size;
	uint16_t ramlog_rtos_vm_count;
	uint16_t ramlog_linux_vm_count;

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
	uint16_t dma_passthrough_count;
	bool dma_isolation_required;
	bool cpu_topology_valid;
	uint16_t cpu_topology_count;
	uint16_t llc_domain_count;
	struct arm64_platform_cpu_topology cpu_topology[MAX_PCPU_NUM];
};

extern struct beau_config beau_config;

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count);
void arm64_platform_init_early(void);
void arm64_platform_init(uint64_t fdt_paddr);
void arm64_platform_init_post_console(void);
void arm64_platform_init_smmu(void);
bool arm64_platform_dma_isolation_required(void);
#endif

#endif /* ARM64_PLATFORM_H */
