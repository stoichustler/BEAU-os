/*
 * Copyright (C) 2020-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <boot.h>
#include <pgtable.h>
#include <rtl.h>
#include "multiboot_priv.h"

/* [20260720] Multiboot2 tag-stream normalization
 *
 * total_size + reserved
 *        |
 *        v
 * 8-byte-aligned tag stream
 *        |
 *        +--> strings ---------> copied into acrn_boot_info
 *        +--> mmap/modules ----> bounded ABI entries
 *        `--> ACPI/EFI --------> retained early-boot pointers
 *        |
 *        +--> zero size or unsupported future type -> fail conversion
 *        |
 *        v
 * terminating tag -> record normalized module count
 *
 * Key rule:
 *   - the bootloader owns the tag buffer and all referenced payloads;
 *   - this parser copies only stable metadata and records borrowed payload
 *     mappings for later boot consumers;
 *   - tag size excludes alignment padding, so cursor movement rounds up to the
 *     protocol's 8-byte boundary before examining the next tag.
 */

/**
 * Copy the Multiboot2 memory-map payload into BEAU's fixed-capacity ABI table.
 * The tag header is excluded from the entry count and any excess records are
 * deliberately omitted rather than overrunning the normalized table.
 *
 * @pre abi != NULL && mb2_tag_mmap != NULL
 */
static void mb2_mmap_to_abi(struct acrn_boot_info *abi, const struct multiboot2_tag_mmap *mb2_tag_mmap)
{
	uint32_t i;
	struct multiboot2_mmap_entry *mb2_mmap = (struct multiboot2_mmap_entry *)mb2_tag_mmap->entries;

	/* multiboot2 mmap tag header occupied 16 bytes */
	abi->mmap_entries = (mb2_tag_mmap->size - 16U) / sizeof(struct multiboot2_mmap_entry);
	if (abi->mmap_entries > MAX_MMAP_ENTRIES) {
		abi->mmap_entries = MAX_MMAP_ENTRIES;
	}

	for (i = 0U; i < abi->mmap_entries; i++) {
		abi->mmap_entry[i].baseaddr = (mb2_mmap + i)->addr;
		abi->mmap_entry[i].length = (mb2_mmap + i)->len;
		abi->mmap_entry[i].type = (mb2_mmap + i)->type;
	}
}

/**
 * Normalize one module descriptor. The module label is copied, while the module
 * bytes stay in bootloader-owned memory and are referenced through an early
 * host virtual mapping until the VM image loader copies them.
 *
 * @pre abi != NULL && mb2_tag_mods != NULL
 */
static void mb2_mods_to_abi(struct acrn_boot_info *abi,
			uint32_t mbi_mod_idx, const struct multiboot2_tag_module *mb2_tag_mods)
{
	abi->mods[mbi_mod_idx].start = hpa2hva_early((uint64_t)mb2_tag_mods->mod_start);
	if (mb2_tag_mods->mod_end > mb2_tag_mods->mod_start) {
		abi->mods[mbi_mod_idx].size = mb2_tag_mods->mod_end - mb2_tag_mods->mod_start;
	}

	(void)strncpy_s((void *)(abi->mods[mbi_mod_idx].string), MAX_MOD_STRING_SIZE,
		(char *)hpa2hva_early((uint64_t)mb2_tag_mods->cmdline),
		strnlen_s((char *)hpa2hva_early((uint64_t)mb2_tag_mods->cmdline), MAX_MOD_STRING_SIZE));
}

/**
 * Preserve the 64-bit EFI system-table address in the split fields used by the
 * protocol-neutral boot ABI.
 *
 * @pre abi != NULL && mb2_tag_efi64 != 0
 */
static void mb2_efi64_to_abi(struct acrn_boot_info *abi, const struct multiboot2_tag_efi64 *mb2_tag_efi64)
{
	abi->uefi_info.systab = (uint32_t)(uint64_t)mb2_tag_efi64->pointer;
	abi->uefi_info.systab_hi = (uint32_t)((uint64_t)mb2_tag_efi64->pointer >> 32U);
}

/**
 * Record the EFI descriptor format and the map embedded in the Multiboot2 tag.
 * The descriptor bytes are not copied; their address remains valid only while
 * the bootloader-provided information storage is retained.
 *
 * @pre abi != NULL && mb2_tag_efimmap != 0
 */
static void mb2_efimmap_to_abi(struct acrn_boot_info *abi,
			const struct multiboot2_tag_efi_mmap *mb2_tag_efimmap)
{
	abi->uefi_info.memdesc_size = mb2_tag_efimmap->descr_size;
	abi->uefi_info.memdesc_version = mb2_tag_efimmap->descr_vers;
	abi->uefi_info.memmap = (uint32_t)(uint64_t)mb2_tag_efimmap->efi_mmap;
	abi->uefi_info.memmap_size = mb2_tag_efimmap->size - 16U;
	/* Per multiboot2 spec, multiboot info is below 4GB space hence memmap_hi must be 0U. */
	abi->uefi_info.memmap_hi = (uint32_t)(((uint64_t)mb2_tag_efimmap->efi_mmap) >> 32U);
}

/**
 * Walk a Multiboot2 information block and translate the subset consumed by
 * BEAU: command line, loader name, memory map, modules, ACPI RSDP, and EFI
 * metadata. Standard tags not consumed by BEAU are skipped; types beyond the
 * known specification range and tags that cannot advance the cursor fail the
 * conversion.
 *
 * @pre abi != NULL
 */
int32_t multiboot2_to_acrn_bi(struct acrn_boot_info *abi, void *mb2_info)
{
	int32_t ret = 0;
	struct multiboot2_tag *mb2_tag, *mb2_tag_end;
	uint32_t mb2_info_size = *(uint32_t *)mb2_info;
	uint32_t mod_idx = 0U;
	void *str;

	/* The fixed prefix is total_size followed by one reserved 32-bit field. */
	mb2_tag = (struct multiboot2_tag *)((uint8_t *)mb2_info + 8U);
	mb2_tag_end = (struct multiboot2_tag *)((uint8_t *)mb2_info + mb2_info_size);

	while ((mb2_tag->type != MULTIBOOT2_TAG_TYPE_END) && (mb2_tag < mb2_tag_end)) {
		/* Translate only fields owned by acrn_boot_info; other standard tags are ignored. */
		switch (mb2_tag->type) {
		case MULTIBOOT2_TAG_TYPE_CMDLINE:
			str = ((struct multiboot2_tag_string *)mb2_tag)->string;
			(void)strncpy_s((void *)(abi->cmdline), MAX_BOOTARGS_SIZE, str,
						strnlen_s(str, (MAX_BOOTARGS_SIZE - 1U)));
			break;
		case MULTIBOOT2_TAG_TYPE_MMAP:
			mb2_mmap_to_abi(abi, (const struct multiboot2_tag_mmap *)mb2_tag);
			break;
		case MULTIBOOT2_TAG_TYPE_MODULE:
			if (mod_idx < MAX_MODULE_NUM) {
				mb2_mods_to_abi(abi, mod_idx, (const struct multiboot2_tag_module *)mb2_tag);
				mod_idx++;
			}
			break;
		case MULTIBOOT2_TAG_TYPE_BOOT_LOADER_NAME:
			str = ((struct multiboot2_tag_string *)mb2_tag)->string;
			(void)strncpy_s((void *)(abi->loader_name), MAX_LOADER_NAME_SIZE, str,
						strnlen_s(str, (MAX_LOADER_NAME_SIZE - 1U)));
			break;
		case MULTIBOOT2_TAG_TYPE_ACPI_NEW:
			abi->acpi_rsdp_va = ((struct multiboot2_tag_new_acpi *)mb2_tag)->rsdp;
			break;
		case MULTIBOOT2_TAG_TYPE_EFI64:
			mb2_efi64_to_abi(abi, (const struct multiboot2_tag_efi64 *)mb2_tag);
			break;
		case MULTIBOOT2_TAG_TYPE_EFI_MMAP:
			mb2_efimmap_to_abi(abi, (const struct multiboot2_tag_efi_mmap *)mb2_tag);
			break;
		default:
			if (mb2_tag->type > MULTIBOOT2_TAG_TYPE_LOAD_BASE_ADDR) {
				ret = -EINVAL;
			}
			break;
		}
		/* A zero-sized tag would leave the cursor unchanged and make the walk unbounded. */
		if (mb2_tag->size == 0U) {
			ret = -EINVAL;
		}

		if (ret != 0) {
			break;
		}
		/*
		 * tag->size includes the tag header but not trailing padding. Round the
		 * cursor up so every next tag starts at the required 8-byte boundary.
		 */
		mb2_tag = (struct multiboot2_tag *)((uint8_t *)mb2_tag
				+ ((mb2_tag->size + (MULTIBOOT2_INFO_ALIGN - 1U)) & ~(MULTIBOOT2_INFO_ALIGN - 1U)));
	}

	/* Record entries converted so far; the caller selects this protocol only when ret is zero. */
	abi->mods_count = mod_idx;

	return ret;
}
