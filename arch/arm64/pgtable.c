/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pgtable.h>

/* [20260725] ARM64 page-table mapping granularity
 *
 *  IPA/VA [47:0]
 *  +-------------------+-------------------+-------------------+-----------+
 *  | [47:39] PGT_LVL3  | [38:30] PGT_LVL2  | [29:21] PGT_LVL1  | [20:12]   |
 *  +-------------------+-------------------+-------------------+-----------+
 *  | PGD table index   | PUD index         | PMD index         | PTE index |
 *  +-------------------+-------------------+-------------------+-----------+
 *                                                               [11:0] offset
 *
 * pgtable_add_map()
 *        |
 *        v
 * PGT_LVL3 / PGD table
 *        |
 *        v
 * PGT_LVL2 / PUD: 1 GiB block or next-level table
 *        |
 *        v
 * PGT_LVL1 / PMD: 2 MiB block or next-level table
 *        |
 *        v
 * PGT_LVL0 / PTE: 4 KiB page
 *
 * A future Stage-2 1 GiB block is limited to static guest RAM whose IPA, HPA,
 * and length are all 1 GiB aligned and physically contiguous. The common
 * walker selects a block only when the architecture callback permits its level
 * and the PA, IPA/VA, and remaining range meet that granule. Device windows,
 * shared pages, and ranges needing finer permissions must split into PMD or
 * PTE mappings. Host Stage-1 already accepts PUD and PMD blocks; current
 * Stage-2 intentionally accepts only PMD blocks.
 *
 * Key rule:
 *   - the VM owns its Stage-2 descriptor state and the SMMU shares its root;
 *   - validate and split a block before changing a partial range;
 *   - complete break-before-make, CPU TLB, and bound-SMMU synchronization
 *     before publishing new ownership or returning retired table pages.
 */

uint64_t arch_pgtl_large(uint64_t pgtle)
{
	return ((pgtle & PAGE_DESC_VALID) != 0UL) && ((pgtle & PAGE_DESC_TYPE_MASK) == PAGE_BLOCK_DESC);
}

uint64_t arch_pgtl_page_paddr(uint64_t pgtle)
{
	return pgtle & PTE_PFN_MASK;
}

void *arch_hpa2hva_early(uint64_t x)
{
	return (void *)x;
}

uint64_t arch_hva2hpa_early(void *x)
{
	return (uint64_t)x;
}
