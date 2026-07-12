/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <pgtable.h>
#include <boot.h>
#include <rtl.h>
#include <logmsg.h>
#include <bare.h>

static struct acrn_boot_info acrn_bi = { 0 };

/*
 * acrn_boot_info is the normalized handoff from the host boot environment to
 * VM loading. Multiboot, Multiboot2, and bare boot all feed the same structure:
 * module source addresses, module tags, bootloader command line, memory map,
 * and optional EFI/ACPI pointers. Per-VM loading code should consume this
 * normalized view instead of reading bootloader-specific records directly.
 */

/**
 * @pre (p_start != NULL) && (p_end != NULL)
 */
void get_boot_mods_range(uint64_t *p_start, uint64_t *p_end)
{
	uint32_t i;
	uint64_t start = ~0UL, end = 0UL;
	struct acrn_boot_info *abi = get_acrn_boot_info();

	for (i = 0; i < abi->mods_count; i++) {
		if (hva2hpa(abi->mods[i].start) < start) {
			start = hva2hpa(abi->mods[i].start);
		}
		if (hva2hpa(abi->mods[i].start + abi->mods[i].size) > end) {
			end = hva2hpa(abi->mods[i].start + abi->mods[i].size);
		}
	}
	*p_start = start;
	*p_end = end;
}

void init_acrn_boot_info(uint32_t *registers)
{
	int32_t ret;

	/* [20260630] boot-info normalization principle:
	 *
	 * The BSP reaches this point before VM creation, while bootloader records
	 * are still available through the raw entry registers. Convert that
	 * machine/protocol-specific handoff into the single global acrn_boot_info
	 * object, then let the rest of the hypervisor consume only the normalized
	 * module list, memory map, command line, loader name, and optional EFI/ACPI
	 * pointers.
	 *
	 *   entry registers
	 *        |
	 *        v
	 *   Multiboot/Multiboot2 parser
	 *        |
	 *        +-- success --> acrn_bi modules/mmap/EFI/ACPI
	 *        |
	 *        +-- absent  --> bare_boot_options[] -> acrn_bi modules
	 *
	 * x86 normally arrives through a Multiboot protocol and is later sanitized.
	 * ARM64 QEMU/rk356x static builds pass zeroed boot_regs today, so the
	 * Multiboot probe intentionally fails and bare boot supplies the compiled
	 * image table used by the raw-image loader.
	 */
	ret = init_multiboot_info(registers);
	if (ret < 0) {
		init_bare_boot_info();
	}
}

int32_t sanitize_acrn_boot_info(struct acrn_boot_info *abi)
{
	int32_t abi_status = 0;

	/*
	 * VM launch depends on both modules and a memory map. The modules provide
	 * guest images and firmware payloads; the memory map feeds legacy x86
	 * loaders and still documents host boot ownership for diagnostics.
	 */
	if (abi->mods_count == 0U) {
		LOG_ERR("no boot module info found");
		abi_status = -EINVAL;
	}

	if (abi->mmap_entries == 0U) {
		LOG_ERR("no boot mmap info found");
		abi_status = -EINVAL;
	}

	printf("%s environment detected.\n", boot_from_uefi(abi) ? "uefi" : "non-uefi");
	if (boot_from_uefi(abi) && ((abi->uefi_info.memmap == 0U) || (abi->uefi_info.memmap_hi != 0U))) {
		LOG_ERR("no efi memmap found below 4gb space!");
		abi_status = -EINVAL;
	}

	if (abi->loader_name[0] == '\0') {
		LOG_ERR("no bootloader name found!");
		abi_status = -EINVAL;
	} else {
	printf("%s bootloader: %s\n", abi->protocol_name, abi->loader_name);
	}

	return abi_status;
}

/*
 * @post retval != NULL
 */
struct acrn_boot_info *get_acrn_boot_info(void)
{
	return &acrn_bi;
}
