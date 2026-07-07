/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PLATFORM_DTS_H
#define ARM64_PLATFORM_DTS_H

#include <types.h>
#include <bare.h>
#include <vm_config.h>
#include <asm/platform.h>

struct arm64_platform_dts_ops {
	const uint8_t *(*module_addr)(const char *symbol);
	uint64_t (*module_size)(const char *symbol);
};

struct arm64_platform_dts_vm_storage {
	struct acrn_vm_config *vm_configs;
	struct vm_hpa_regions *memory_regions;
	uint16_t vm_config_count;
	uint16_t service_vm_id;
	struct bare_boot_option *boot_options;
	uint16_t boot_option_capacity;
	uint16_t *boot_option_count;
};

struct arm64_platform_dts_info {
	uint32_t gic_iidr;
	const char *guest_cpu_compatible;
	const char *vfdt_model;
	const char *vfdt_compatible;
	uint32_t uart_clock_hz;
	uint32_t uart_baud;
	bool service_vm_initrd;
};

void arm64_platform_dts_parse_info(const void *fdt,
	struct arm64_platform_dts_info *info);
const struct arm64_mem_region *arm64_platform_dts_mmio_regions(uint32_t *count);
void arm64_platform_dts_parse_board(const void *fdt,
	const struct arm64_platform_dts_info *info);
void arm64_platform_dts_parse_vms(const void *fdt,
	const struct arm64_platform_dts_ops *ops,
	const struct arm64_platform_dts_vm_storage *storage);

#endif /* ARM64_PLATFORM_DTS_H */
