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
#include <asm/boot/ld_sym.h>
#include <asm/mte.h>
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
 * [20260630] ARM64 MMU principle:
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

void arm64_get_hv_s1_page_pool_stats(struct page_pool_stats *stats)
{
	page_pool_get_stats(&ppt_page_pool, stats);
}

#define PPT_PGTL3_PAGE_NUM	1UL
#define PPT_PGTL2_PAGE_NUM	1UL
#define PPT_PGTL1_PAGE_NUM	8UL
#define PPT_PGTL0_PAGE_NUM	4UL
#define PPT_PAGE_NUM_SUM	(PPT_PGTL3_PAGE_NUM + PPT_PGTL2_PAGE_NUM + PPT_PGTL1_PAGE_NUM + PPT_PGTL0_PAGE_NUM)
#define PPT_PAGE_NUM		roundup(PPT_PAGE_NUM_SUM, 64U)

DEFINE_MTE_PAGE_TABLES(ppt_pages, PPT_PAGE_NUM);
DEFINE_PAGE_TABLE(ppt_pages_bitmap);
static uint8_t ppt_page_tag_states[PPT_PAGE_NUM];

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

static enum arm64_memory_type arm64_s1_memory_type(uint8_t mair_attr)
{
	enum arm64_memory_type type;

	switch (mair_attr) {
	case MAIR_ATTR_DEVICE_GRE:
		type = ARM64_MEMORY_DEVICE_GRE;
		break;
	case MAIR_ATTR_DEVICE_nGRE:
		type = ARM64_MEMORY_DEVICE_nGRE;
		break;
	case MAIR_ATTR_DEVICE_nGnRE:
		type = ARM64_MEMORY_DEVICE_nGnRE;
		break;
	case MAIR_ATTR_DEVICE_nGnRnE:
		type = ARM64_MEMORY_DEVICE_nGnRnE;
		break;
	default:
		type = ((mair_attr & 0xf0U) != 0U) ?
			ARM64_MEMORY_NORMAL : ARM64_MEMORY_UNKNOWN;
		break;
	}

	return type;
}

/* [20260717] Stage-1 memory-type query
 *
 *   EL2 VA -> leaf AttrIdx -> current MAIR_EL2 byte -> memory type
 *
 * Key rule:
 *   - the host stage-1 table is immutable after boot;
 *   - the type is derived from the active MAIR_EL2, not a display-time guess;
 *   - an absent leaf is reported as Unmapped, while a reserved MAIR byte is
 *     reported as Unknown.
 */
bool arm64_get_hv_s1_memory_attr(uint64_t addr,
	struct arm64_memory_attr *attr)
{
	const uint64_t *entry;
	uint64_t pg_size = 0UL;
	uint64_t mair;
	uint32_t attr_idx;
	uint8_t mair_attr;

	if (attr == NULL) {
		return false;
	}
	attr->type = ARM64_MEMORY_UNKNOWN;
	attr->encoding = 0U;
	if (ppt_mmu_top_addr == NULL) {
		return false;
	}

	entry = pgtable_lookup_entry((uint64_t *)ppt_mmu_top_addr, addr,
		&pg_size, &ppt_pgtable);
	if (entry == NULL) {
		attr->type = ARM64_MEMORY_UNMAPPED;
		return true;
	}

	attr_idx = (uint32_t)((*entry & PAGE_ATTR_IDX_MASK) >>
		PAGE_ATTR_IDX_SHIFT);
	mair = arm64_sysreg_read(mair_el2);
	mair_attr = (uint8_t)((mair >> (attr_idx * 8U)) & 0xffUL);
	attr->type = arm64_s1_memory_type(mair_attr);
	attr->encoding = mair_attr & 0x0fU;

	return true;
}

bool arm64_get_hv_s1_memory_access(uint64_t addr, uint8_t *access)
{
	const uint64_t *entry;
	uint64_t pg_size = 0UL;
	uint64_t descriptor;
	bool writable;

	if (access == NULL) {
		return false;
	}
	*access = 0U;
	if (ppt_mmu_top_addr == NULL) {
		return false;
	}

	entry = pgtable_lookup_entry((uint64_t *)ppt_mmu_top_addr, addr,
		&pg_size, &ppt_pgtable);
	if (entry == NULL) {
		return true;
	}

	descriptor = *entry;
	*access = ARM64_S1_ACCESS_READ;
	writable = (descriptor & PAGE_AP_RO_EL2) == 0UL;
	if (writable) {
		*access |= ARM64_S1_ACCESS_WRITE;
	}
	if (((descriptor & (PAGE_PXN | PAGE_UXN)) == 0UL) &&
		(((read_sctlr_el2() & SCTLR_EL2_WXN) == 0UL) || !writable)) {
		*access |= ARM64_S1_ACCESS_EXECUTE;
	}

	return true;
}

static void arm64_set_hv_section_permissions(const char *name,
	uint64_t start, uint64_t end, uint64_t prot_set, uint64_t prot_clr)
{
	uint64_t size;

	if ((end <= start) || !mem_aligned_check(start, PAGE_SIZE)) {
		panic("arm64 mmu invalid %s range [0x%lx,0x%lx)", name, start, end);
	}

	size = round_page_up(end - start);
	pgtable_modify_or_del_map((uint64_t *)ppt_mmu_top_addr, start, size,
		prot_set, prot_clr, &ppt_pgtable, MR_MODIFY);
}

static void arm64_protect_hv_sections(void)
{
	arm64_set_hv_section_permissions("text", (uint64_t)&_text_start,
		(uint64_t)&_text_end, PAGE_AP_RO_EL2, PAGE_PXN | PAGE_UXN);
	arm64_set_hv_section_permissions("rodata", (uint64_t)&_rodata_start,
		(uint64_t)&_rodata_end, PAGE_AP_RO_EL2 | PAGE_PXN | PAGE_UXN, 0UL);
}

static void arm64_map_mte_page_pools(void)
{
	uint64_t start;
	uint64_t end;

	if (!arm64_mte_is_enabled()) {
		return;
	}
	start = (uint64_t)&_mte_page_pool_start;
	end = (uint64_t)&_mte_page_pool_end;
	if ((end <= start) || ((start & (PAGE_SIZE - 1UL)) != 0UL) ||
		((end & (PAGE_SIZE - 1UL)) != 0UL)) {
		panic("MTE: invalid page-pool section [0x%lx,0x%lx)", start, end);
	}

	/* [20260720] EL2 MTE page-pool mapping
	 *
	 *   RAM block map -> split dedicated pool range -> Normal-Tagged leaves
	 *
	 * Key rule:
	 *   - the linker owns a single page-aligned physical range;
	 *   - the range is converted before the MMU and tag checking are enabled;
	 *   - no second virtual alias retains a conflicting Normal-WB attribute.
	 */
	pgtable_modify_or_del_map((uint64_t *)ppt_mmu_top_addr, start, end - start,
		PAGE_ATTR_IDX_NORMAL_TAGGED, PAGE_ATTR_IDX_MASK,
		&ppt_pgtable, MR_MODIFY);
}

static void init_hv_mapping(void)
{
	const struct arm64_mem_region *mmio_regions;
	uint32_t mmio_region_count;
	uint32_t idx;
	int i;

	ppt_mmu_top_addr = (uint64_t *)alloc_page(&ppt_page_pool);

	/* [20260719] EL2 stage-1 identity map and W^X
	 *
	 *   platform MMIO -> Device, RW-
	 *   platform RAM  -> Normal, RW-
	 *          |
	 *          +--> remove reserved ranges
	 *          |
	 *          +--> .text   -> R-X
	 *          +--> .rodata -> R--
	 *          `--> writable sections remain RW-
	 *
	 * Key rule:
	 *   - EL2 virtual addresses remain identical to host physical addresses;
	 *   - RAM starts execute-never and only the page-aligned text range gains
	 *     execute permission after becoming read-only;
	 *   - SCTLR_EL2.WXN provides a second barrier against writable code pages.
	 */
	mmio_regions = arm64_get_platform_mmio_regions(&mmio_region_count);
	for (idx = 0U; idx < mmio_region_count; idx++) {
		pgtable_add_map((uint64_t *)ppt_mmu_top_addr, mmio_regions[idx].base,
			mmio_regions[idx].base, mmio_regions[idx].size,
			PAGE_ATTR_DEVICE | PAGE_BLOCK_DESC, &ppt_pgtable);
	}

	pgtable_add_map((uint64_t *)ppt_mmu_top_addr, phys_mem_start,
		phys_mem_start, phys_mem_size,
		PAGE_ATTR_NORMAL | PAGE_PXN | PAGE_UXN | PAGE_BLOCK_DESC,
		&ppt_pgtable);

	for (i = 0; i < nr_rsvd_regions; i++) {
		pgtable_modify_or_del_map((uint64_t *)ppt_mmu_top_addr, rsvd_regions[i].addr,
			rsvd_regions[i].size, 0UL, 0UL, &ppt_pgtable, MR_DEL);
	}

	arm64_map_mte_page_pools();
	arm64_protect_hv_sections();

	init_ttbr0_el2 = (uint64_t)ppt_mmu_top_addr;
	if (!enable_paging()) {
		panic("MTE: failed to enable EL2 paging controls on bsp");
	}
}

void init_paging(void)
{
	int32_t status;

	init_phys_mem_range();
	arm64_mte_bootstrap_init();
	init_page_pool(&ppt_page_pool, (uint64_t *)ppt_pages,
		(uint64_t *)ppt_pages_bitmap, PPT_PAGE_NUM);
	status = arm64_mte_register_page_pool(&ppt_page_pool, ppt_pages,
		PPT_PAGE_NUM, ppt_page_tag_states);
	if (status != 0) {
		panic("MTE: failed to register stage-1 page pool status:%d", status);
	}

	init_hv_mapping();
}

bool enable_paging(void)
{
	uint64_t mair = MAIR_EL2_VALUE;
	uint64_t tcr = TCR_EL2_VALUE;
	uint64_t sctlr;

	/*
	 * Program EL2 translation controls before setting SCTLR.M:
	 *
	 *   MAIR_EL2  names AttrIndx encodings used by descriptors.
	 *   TCR_EL2   selects the VA size, granule, cacheability, and PA size.
	 *   TTBR0_EL2 points the table-walk unit at BEAU's root table.
	 *   SCTLR_EL2.M enables translation; C/I enable caches; WXN enforces W^X.
	 *
	 * The local TLB flush ensures no stale translation survives a rebuild of
	 * the bootstrap page table before the MMU begins using it.
	 */
	sctlr = read_sctlr_el2() | SCTLR_EL2_VALUE;
	if (!arm64_mte_prepare_pcpu(&mair, &tcr, &sctlr)) {
		return false;
	}
	write_mair_el2(mair);
	write_tcr_el2(tcr);
	write_ttbr0_el2(init_ttbr0_el2);
	flush_tlb_local();

	write_sctlr_el2(sctlr);
	arm64_mte_mark_pcpu_enabled();
	return true;
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
	arm64_dsb_sy();
	arm64_isb();
}

void flush_cacheline(const volatile void *p)
{
	arm64_dc(civac, p);
	arm64_dsb_ish();
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
