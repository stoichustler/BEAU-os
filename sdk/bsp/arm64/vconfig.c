/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <bare.h>
#include <vconfig.h>
#include <logmsg.h>
#include <rtl.h>
#include <arm64_platform_dts.h>

#include "bimage.h"

extern const uint8_t arm64_lk_image_start[];
extern const uint8_t arm64_lk_image_size[];
extern const uint8_t arm64_zephyr_image_start[];
extern const uint8_t arm64_zephyr_image_size[];
#ifdef CONFIG_STATIC_QEMU_PLATFORM
extern const uint8_t arm64_beau_linux_vm1_dtb_start[];
extern const uint8_t arm64_beau_linux_vm1_dtb_size[];
extern const uint8_t arm64_beau_linux_vm2_dtb_start[];
extern const uint8_t arm64_beau_linux_vm2_dtb_size[];
extern const uint8_t arm64_beau_linux_vm3_dtb_start[];
extern const uint8_t arm64_beau_linux_vm3_dtb_size[];
#endif

struct acrn_vm_config vm_configs[CONFIG_MAX_VM_NUM];
struct acrn_vm_config *const service_vm_config = &vm_configs[0];

static struct vm_hpa_regions arm64_vm_memory_regions[CONFIG_MAX_VM_NUM];
static struct acrn_vm_pci_dev_config
	arm64_vm_pci_devs[CONFIG_MAX_VM_NUM][CONFIG_MAX_PCI_DEV_NUM];

struct bare_boot_option bare_boot_options[MAX_MODULE_NUM];
uint16_t n_bare_boot_options;

static bool arm64_symbol_matches(const char *symbol, const char *arm64,
	const char *qemu, const char *rk356x)
{
	return (strcmp(symbol, arm64) == 0) ||
		((qemu != NULL) && (strcmp(symbol, qemu) == 0)) ||
		((rk356x != NULL) && (strcmp(symbol, rk356x) == 0));
}

static const uint8_t *arm64_dts_module_addr(const char *symbol)
{
	const uint8_t *addr = NULL;

	if (arm64_symbol_matches(symbol, "arm64_zephyr_image_start",
		"qemu_zephyr_image_start", "rk356x_zephyr_image_start")) {
		addr = arm64_zephyr_image_start;
	} else if (arm64_symbol_matches(symbol, "arm64_lk_image_start",
		"qemu_lk_image_start", "rk356x_lk_image_start")) {
		addr = arm64_lk_image_start;
	}
#ifdef CONFIG_STATIC_QEMU_PLATFORM
	else if (arm64_symbol_matches(symbol, "arm64_beau_linux_vm1_dtb_start",
		"qemu_beau_linux_vm1_dtb_start", NULL)) {
		addr = arm64_beau_linux_vm1_dtb_start;
	} else if (arm64_symbol_matches(symbol, "arm64_beau_linux_vm2_dtb_start",
		"qemu_beau_linux_vm2_dtb_start", NULL) ||
		arm64_symbol_matches(symbol, "arm64_beau_linux_dtb_start",
		"qemu_beau_linux_dtb_start", NULL)) {
		addr = arm64_beau_linux_vm2_dtb_start;
	} else if (arm64_symbol_matches(symbol, "arm64_beau_linux_vm3_dtb_start",
		"qemu_beau_linux_vm3_dtb_start", NULL)) {
		addr = arm64_beau_linux_vm3_dtb_start;
	}
#endif
	else {
		panic("unknown arm64 image addr symbol '%s'", symbol);
	}

	return addr;
}

static uint64_t arm64_dts_module_size(const char *symbol)
{
	uint64_t size;

	if (arm64_symbol_matches(symbol, "arm64_zephyr_image_size",
		"qemu_zephyr_image_size", "rk356x_zephyr_image_size")) {
		size = (uint64_t)arm64_zephyr_image_size;
	} else if (arm64_symbol_matches(symbol, "arm64_lk_image_size",
		"qemu_lk_image_size", "rk356x_lk_image_size")) {
		size = (uint64_t)arm64_lk_image_size;
	}
#ifdef CONFIG_STATIC_QEMU_PLATFORM
	else if (arm64_symbol_matches(symbol, "arm64_beau_linux_vm1_dtb_size",
		"qemu_beau_linux_vm1_dtb_size", NULL)) {
		size = (uint64_t)arm64_beau_linux_vm1_dtb_size;
	} else if (arm64_symbol_matches(symbol, "arm64_beau_linux_vm2_dtb_size",
		"qemu_beau_linux_vm2_dtb_size", NULL) ||
		arm64_symbol_matches(symbol, "arm64_beau_linux_dtb_size",
		"qemu_beau_linux_dtb_size", NULL)) {
		size = (uint64_t)arm64_beau_linux_vm2_dtb_size;
	} else if (arm64_symbol_matches(symbol, "arm64_beau_linux_vm3_dtb_size",
		"qemu_beau_linux_vm3_dtb_size", NULL)) {
		size = (uint64_t)arm64_beau_linux_vm3_dtb_size;
	}
#endif
	else if (strcmp(symbol, "BEAU_LINUX_VM1_IMAGE_SIZE") == 0) {
		size = BEAU_LINUX_VM1_IMAGE_SIZE;
	} else if (strcmp(symbol, "BEAU_LINUX_VM2_IMAGE_SIZE") == 0) {
		size = BEAU_LINUX_VM2_IMAGE_SIZE;
	} else if (strcmp(symbol, "BEAU_LINUX_VM3_IMAGE_SIZE") == 0) {
		size = BEAU_LINUX_VM3_IMAGE_SIZE;
	} else if (strcmp(symbol, "BEAU_LINUX_INITRAMFS_SIZE") == 0) {
		size = BEAU_LINUX_INITRAMFS_SIZE;
	} else {
		panic("unknown arm64 image size symbol '%s'", symbol);
	}

	return size;
}

void arm64_parse_vm_config_from_dts(const void *fdt)
{
	static const struct arm64_platform_dts_ops arm64_dts_ops = {
		.module_addr = arm64_dts_module_addr,
		.module_size = arm64_dts_module_size,
	};
	struct arm64_platform_dts_vm_storage storage = {
		.vm_configs = vm_configs,
		.memory_regions = arm64_vm_memory_regions,
		.pci_devs = arm64_vm_pci_devs,
		.vm_config_count = ARRAY_SIZE(vm_configs),
		.service_vm_id = 0U,
		.boot_options = bare_boot_options,
		.boot_option_capacity = ARRAY_SIZE(bare_boot_options),
		.boot_option_count = &n_bare_boot_options,
	};

	arm64_platform_dts_parse_vms(fdt, &arm64_dts_ops, &storage);
}
