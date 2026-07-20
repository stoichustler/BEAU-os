/*
 * Copyright (C) 2020-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef MULTIBOOT_PRIV_H
#define MULTIBOOT_PRIV_H

#include <multiboot_std.h>

/*
 * Private protocol-selection boundary shared by the Multiboot1 dispatcher and
 * the optional Multiboot2 tag parser. These helpers recognize the entry
 * register contract; they do not validate or take ownership of the information
 * block referenced by the companion register.
 */

#ifdef CONFIG_MULTIBOOT2
/*
 * Multiboot2 selection follows the protocol magic alone. Unlike the
 * Multiboot1 path below, a zero information address is not rejected here for
 * compatibility with older GRUB versions that could place the information
 * block at physical address zero.
 */
static inline bool boot_from_multiboot2(uint32_t magic)
{
	/*
	 * Multiboot spec states that the Multiboot information structure may be placed
	 * anywhere in memory by the boot loader.
	 *
	 * Seems both SBL and GRUB won't place multiboot1 MBI structure at 0 address,
	 * but GRUB could place Multiboot2 MBI structure at 0 address until commit
	 * 0f3f5b7c13fa9b67 ("multiboot2: Set min address for mbi allocation to 0x1000")
	 * which dates on Dec 26 2019.
	 */
	return (magic == MULTIBOOT2_INFO_MAGIC);
}

int32_t multiboot2_to_acrn_bi(struct acrn_boot_info *abi, void *mb2_info);
#endif

/* Multiboot1 requires both its entry magic and a nonzero information address. */
static inline bool boot_from_multiboot(uint32_t magic, uint32_t info)
{
	return ((magic == MULTIBOOT_INFO_MAGIC) && (info != 0U));
}

#endif /* MULTIBOOT_PRIV_H */
