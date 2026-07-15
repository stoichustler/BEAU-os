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
#include <hv_pm.h>
#include <asm/platform.h>

#define ARM64_PLATFORM_PM_MAX_WAKE_IRQS	8U

struct arm64_platform_pm_config {
	uint64_t required_vm_mask;
	uint32_t prepare_timeout_ms;
	uint32_t resume_timeout_ms;
	uint32_t event_virq;
	uint32_t wakeup_irqs[ARM64_PLATFORM_PM_MAX_WAKE_IRQS];
	uint16_t controller_vmid;
	uint16_t wakeup_irq_count;
	uint8_t enabled;
	uint8_t qemu_mode;
	uint8_t reserved[6U];
};

struct arm64_platform_dts_ops {
	const uint8_t *(*module_addr)(const char *symbol);
	uint64_t (*module_size)(const char *symbol);
};

struct arm64_platform_dts_vm_storage {
	struct acrn_vm_config *vm_configs;
	struct vm_hpa_regions *memory_regions;
	struct acrn_vm_pci_dev_config (*pci_devs)[CONFIG_MAX_PCI_DEV_NUM];
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
	struct arm64_platform_pm_config pm;
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
