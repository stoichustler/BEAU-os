/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <rtl.h>
#include <mmu.h>
#include <logmsg.h>
#include <reloc.h>
#include <fdt_api.h>
#include <acrn_hv_defs.h>
#include <asm/platform.h>
#include <asm/sysreg.h>

void set_paging_supervisor(__unused uint64_t base, __unused uint64_t size)
{
}

static struct page_pool ppt_page_pool;
static void *ppt_mmu_top_addr;
static uint64_t init_ttbr0_el2;

static uint64_t phys_mem_start;
static uint64_t phys_mem_size;
static struct mem_region rsvd_regions[MAX_FDT_RSVD_REGIONS];
static int nr_rsvd_regions;

/*
 * ARM64 uses separate translation regimes for the host and guests:
 * - EL2 stage-1 maps hypervisor virtual addresses to host physical addresses.
 * - VM stage-2 maps guest IPAs to host physical addresses.
 *
 * This file owns only the EL2 stage-1 map. Guest memory isolation is built in
 * arch/arm64/guest/vm.c so the two regimes stay independently auditable.
 *
 * 2026-06-30 ARM64 MMU principle:
 *
 * Software issues virtual addresses. The MMU first looks for a cached
 * translation in the TLB; on a miss, its table-walk unit reads translation
 * table descriptors from memory. Even BEAU's identity map must go through this
 * machinery after SCTLR_EL2.M is set:
 *
 *   EL2 VA/HVA
 *      |
 *      v
 *   +-----+     hit      +----------------+
 *   | TLB | -----------> | HPA + attrs    |
 *   +-----+              +----------------+
 *      |
 *      | miss
 *      v
 *   +------------+       +----------------+
 *   | table walk | ----> | page tables    |
 *   +------------+       +----------------+
 *
 * Identity mapping keeps the address numbers equal; the descriptors still
 * provide memory type, shareability, access flag, and execute permissions.
 */
void init_phys_mem_range(void)
{
#ifdef CONFIG_FDT_PARSE_ENABLED
	int ret;

	ret = fdt_get_phys_mem_region(get_host_fdt(), &phys_mem_start, &phys_mem_size);
	if (ret < 0) {
		panic("failed to find memory information from fdt");
	}

	fdt_get_rsvd_mem_regions(get_host_fdt(), rsvd_regions, &nr_rsvd_regions);
#else
	phys_mem_start = beau_config.ram_start;
	phys_mem_size = beau_config.ram_size;
#endif
}

uint64_t arm64_get_phys_mem_start(void)
{
	return phys_mem_start;
}

uint64_t arm64_get_phys_mem_size(void)
{
	return phys_mem_size;
}

const struct mem_region *arm64_get_reserved_mem_regions(uint32_t *count)
{
	*count = (uint32_t)nr_rsvd_regions;
	return rsvd_regions;
}

#define PPT_PGTL3_PAGE_NUM	1UL
#define PPT_PGTL2_PAGE_NUM	1UL
#define PPT_PGTL1_PAGE_NUM	8UL
#define PPT_PGTL0_PAGE_NUM	4UL
#define PPT_PAGE_NUM_SUM	(PPT_PGTL3_PAGE_NUM + PPT_PGTL2_PAGE_NUM + PPT_PGTL1_PAGE_NUM + PPT_PGTL0_PAGE_NUM)
#define PPT_PAGE_NUM		roundup(PPT_PAGE_NUM_SUM, 64U)

DEFINE_PAGE_TABLES(ppt_pages, PPT_PAGE_NUM);
DEFINE_PAGE_TABLE(ppt_pages_bitmap);

static bool large_page_support(enum _page_table_level level, __unused uint64_t prot)
{
	return (level == PGT_LVL1) || (level == PGT_LVL2);
}

static void ppt_flush_cache_pagewalk(__unused const void *entry)
{
}

static uint64_t ppt_pgentry_present(uint64_t pte)
{
	return pte & PAGE_DESC_VALID;
}

static inline uint64_t arm64_leaf_desc_type(enum _page_table_level level)
{
	return (level == PGT_LVL0) ? PAGE_PAGE_DESC : PAGE_BLOCK_DESC;
}

/*
 * The generic page-table walker supplies the physical page and requested
 * attributes; ARM64 supplies the descriptor type and access flag required by
 * the architecture. A missing memory type defaults to normal memory, because
 * the host stage-1 map is primarily RAM and MMIO callers pass DEVICE
 * explicitly.
 */
static inline void ppt_set_pgentry(uint64_t *pte, uint64_t page, uint64_t prot,
	enum _page_table_level level, bool is_leaf, const struct pgtable *table)
{
	uint64_t prot_tmp;

	if (!is_leaf) {
		/*
		 * Non-leaf descriptors only point to the next table level. They
		 * do not describe RAM/MMIO permissions for a final translation.
		 */
		prot_tmp = PAGE_TABLE_DESC;
	} else {
		/*
		 * Leaf descriptors terminate the walk. Level 1/2 can use block
		 * mappings; level 0 must be a page descriptor in this walker.
		 */
		prot_tmp = (prot & ~PAGE_DESC_TYPE_MASK) | arm64_leaf_desc_type(level) | PAGE_AF;
		if ((prot_tmp & (PAGE_ATTR_IDX_DEVICE | PAGE_ATTR_NORMAL)) == 0UL) {
			prot_tmp |= PAGE_ATTR_NORMAL;
		}
	}

	make_pgentry(pte, page, prot_tmp, table);
}

static const struct pgtable ppt_pgtable = {
	.pool = &ppt_page_pool,
	.large_page_support = large_page_support,
	.pgentry_present = ppt_pgentry_present,
	.flush_cache_pagewalk = ppt_flush_cache_pagewalk,
	.set_pgentry = ppt_set_pgentry,
};

static void init_hv_mapping(void)
{
	uint64_t hva_base;
	const struct arm64_mem_region *mmio_regions;
	uint32_t mmio_region_count;
	uint32_t idx;
	int i;

	ppt_mmu_top_addr = (uint64_t *)alloc_page(&ppt_page_pool);

	/*
	 * EL2 stage-1 identity-map principle:
	 *
	 * BEAU keeps the host bootstrap address model simple: the virtual address
	 * used by EL2 is the same numeric value as the host physical address placed
	 * in the descriptor. Translation is still enabled, but it preserves the
	 * address number while attaching ARM64 memory attributes and permissions.
	 *
	 *      EL2 VA / HVA                 stage-1 page table                 HPA
	 *   +--------------+        +-----------------------------+       +--------------+
	 *   | 0x48000000   | -----> | output address: 0x48000000  | ----> | 0x48000000   |
	 *   +--------------+        | attr: normal/device, XN/RW  |       +--------------+
	 *                           +-----------------------------+
	 *
	 * pgtable_add_map(root, paddr, vaddr, size, ...) receives the physical base
	 * first and the virtual base second. Passing the same value for both bases
	 * builds the 1:1 relationship:
	 *
	 *      paddr_base == vaddr_base
	 *              |
	 *              v
	 *      [VA start, VA end) -> [same-number PA start, same-number PA end)
	 *
	 * 1:1 does not mean all ranges are equivalent. The address relation stays
	 * identity, while descriptor attributes remain range-specific:
	 *
	 *   +---------+  device memory, no normal caching    +----------------+
	 *   |  MMIO   | -----------------------------------> | device window  |
	 *   +---------+                                      +----------------+
	 *
	 *   +---------+  normal cacheable RAM, mostly NX     +----------------+
	 *   |  RAM    | -----------------------------------> | physical RAM   |
	 *   +---------+                                      +----------------+
	 *
	 *   +---------+  same VA==PA, executable code/data   +----------------+
	 *   | HV image| -----------------------------------> | BEAU image     |
	 *   +---------+                                      +----------------+
	 *
	 * The map is built broad-to-narrow: map platform MMIO, map RAM, delete FDT
	 * reserved ranges from the RAM map, then relax XN only for the hypervisor
	 * image. This keeps early boot, exception vectors, memcpy, and MMIO helpers
	 * using direct physical-looking addresses without giving reserved firmware
	 * memory or ordinary RAM unintended execute permission.
	 *
	 * Attribute ownership:
	 *
	 *   MMIO      -> Device memory: ordered enough for registers, never execute.
	 *   RAM       -> Normal cacheable memory: fast data access, initially XN.
	 *   HV image  -> Same VA==PA range, but code must be executable.
	 *
	 * The page-table output address can be the same for all three cases only
	 * because the descriptor attributes carry the real memory-management policy.
	 */
	mmio_regions = arm64_get_platform_mmio_regions(&mmio_region_count);
	for (idx = 0U; idx < mmio_region_count; idx++) {
		pgtable_add_map((uint64_t *)ppt_mmu_top_addr, mmio_regions[idx].base,
			mmio_regions[idx].base, mmio_regions[idx].size,
			PAGE_ATTR_DEVICE | PAGE_BLOCK_DESC, &ppt_pgtable);
	}

	pgtable_add_map((uint64_t *)ppt_mmu_top_addr, phys_mem_start,
		phys_mem_start, phys_mem_size,
		PAGE_ATTR_NORMAL | PAGE_BLOCK_DESC, &ppt_pgtable);

	for (i = 0; i < nr_rsvd_regions; i++) {
		pgtable_modify_or_del_map((uint64_t *)ppt_mmu_top_addr, rsvd_regions[i].addr,
			rsvd_regions[i].size, 0UL, 0UL, &ppt_pgtable, MR_DEL);
	}

	hva_base = get_hv_image_base();
	pgtable_modify_or_del_map((uint64_t *)ppt_mmu_top_addr, hva_base,
		get_hv_image_size(), 0UL, PAGE_PXN | PAGE_UXN, &ppt_pgtable, MR_MODIFY);

	init_ttbr0_el2 = (uint64_t)ppt_mmu_top_addr;
	enable_paging();
}

void init_paging(void)
{
	init_phys_mem_range();
	init_page_pool(&ppt_page_pool, (uint64_t *)ppt_pages,
		(uint64_t *)ppt_pages_bitmap, PPT_PAGE_NUM);

	init_hv_mapping();
}

void enable_paging(void)
{
	uint64_t sctlr;

	/*
	 * Program EL2 translation controls before setting SCTLR.M:
	 *
	 *   MAIR_EL2  names AttrIndx encodings used by descriptors.
	 *   TCR_EL2   selects the VA size, granule, cacheability, and PA size.
	 *   TTBR0_EL2 points the table-walk unit at BEAU's root table.
	 *   SCTLR_EL2.M enables translation; C/I enable data and instruction cache.
	 *
	 * The local TLB flush ensures no stale translation survives a rebuild of
	 * the bootstrap page table before the MMU begins using it.
	 */
	write_mair_el2(MAIR_EL2_VALUE);
	write_tcr_el2(TCR_EL2_VALUE);
	write_ttbr0_el2(init_ttbr0_el2);
	flush_tlb_local();

	sctlr = read_sctlr_el2();
	sctlr |= SCTLR_EL2_VALUE;
	write_sctlr_el2(sctlr);
}

bool arm64_mmu_is_enabled(void)
{
	return (read_sctlr_el2() & SCTLR_EL2_M) != 0UL;
}

void dummy_pgtable_add_map(uint64_t paddr_base, uint64_t vaddr_base, uint64_t size, uint64_t prot)
{
	pgtable_add_map((uint64_t *)ppt_mmu_top_addr, paddr_base, vaddr_base, size, prot, &ppt_pgtable);
}

void flush_tlb(__unused uint64_t addr)
{
	flush_tlb_local();
}

void flush_tlb_range(__unused uint64_t addr, __unused uint64_t size)
{
	flush_tlb_local();
}

void flush_invalidate_all_cache(void)
{
	asm volatile ("dsb sy; isb" ::: "memory");
}

void flush_cacheline(const volatile void *p)
{
	asm volatile ("dc civac, %0; dsb ish" : : "r" (p) : "memory");
}

void flush_cache_range(const volatile void *p, uint64_t size)
{
	uint64_t addr = (uint64_t)p;
	uint64_t end = addr + size;

	addr &= ~(CACHE_LINE_SIZE - 1UL);
	while (addr < end) {
		flush_cacheline((const volatile void *)addr);
		addr += CACHE_LINE_SIZE;
	}
}
