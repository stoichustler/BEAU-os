/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <types.h>
#include <lib/bits.h>
#include <logmsg.h>
#include <util.h>
#include <mmu.h>
#include <pgtable.h>
#include <acrn_hv_defs.h>

/*
 * Keep core page-table walk traces outside the normal LOG_DEBUG stream.
 * Architecture code logs the effective memory maps at boot.
 */
#define DBG_LEVEL_MMU	(LOG_DEBUG + 1U)

/*
 * Generic page-table engine:
 *
 * core/mmu.c is the architecture-neutral walker used by both host MMU maps and
 * VM second-stage maps. It does not know ARM64 descriptor bits or x86/EPT
 * memory-type bits. Instead, each caller supplies a struct pgtable with:
 *
 *   +----------------------+        +-----------------------------+
 *   | common walker        | -----> | architecture pgtable hooks  |
 *   | - allocate pages     |        | - present test              |
 *   | - walk levels 3..0   |        | - set descriptor            |
 *   | - split large pages  |        | - cache/pagewalk flush      |
 *   | - add/modify/delete  |        | - large-page capability     |
 *   +----------------------+        +-----------------------------+
 *
 * The walker treats mappings as:
 *
 *     [vaddr_base, vaddr_base + size)
 *          |
 *          v
 *     [paddr_base, paddr_base + size)
 *
 * For EL2 stage-1, vaddr is a host virtual address. For VM stage-2, vaddr is
 * the guest physical/IPA address. The descriptor format is still supplied by
 * the architecture hook, so the same traversal code can build ARM64 stage-1,
 * ARM64 stage-2, x86 host, or EPT-style page tables.
 *
 * Page-table pages come from a small reserved pool. New tables are sanitized
 * before use, so unused entries are never left with stale descriptors.
 */
void init_page_pool(struct page_pool *pool, uint64_t *page_base, uint64_t *bitmap_base, int page_num)
{
       uint64_t bitmap_size = page_num / 8;
       pool->bitmap = (uint64_t *)bitmap_base;
       pool->start_page = (struct page *)page_base;
       pool->bitmap_size = bitmap_size / sizeof(uint64_t);
       pool->dummy_page = NULL;

       memset(pool->bitmap, 0, bitmap_size);
}

struct page *alloc_page(struct page_pool *pool)
{
	struct page *page = NULL;
	uint64_t loop_idx, idx, bit;

	spinlock_obtain(&pool->lock);
	for (loop_idx = pool->last_hint_id;
		loop_idx < (pool->last_hint_id + pool->bitmap_size); loop_idx++) {
		idx = loop_idx % pool->bitmap_size;
		if (*(pool->bitmap + idx) != ~0UL) {
			bit = ffz64(*(pool->bitmap + idx));
			bitmap_set_non_atomic(bit, pool->bitmap + idx);
			page = pool->start_page + ((idx << 6U) + bit);

			pool->last_hint_id = idx;
			break;
		}
	}
	spinlock_release(&pool->lock);

	ASSERT(page != NULL, "no page aviable!");
	page = (page != NULL) ? page : pool->dummy_page;
	if (page == NULL) {
		/* For HV MMU page-table mapping, we didn't use dummy page when there's no page
		 * available in the page pool. This because we only do MMU page-table mapping on
		 * the early boot time and we reserve enough pages for it. After that, we would
		 * not do any MMU page-table mapping. We would let the system boot fail when page
		 * allocation failed.
		 */
		panic("no dummy aviable!");
	}
	sanitize_pte((uint64_t *)page, NULL);
	return page;
}

void free_page(struct page_pool *pool, struct page *page)
{
	uint64_t idx, bit;

	spinlock_obtain(&pool->lock);
	idx = (page - pool->start_page) >> 6U;
	bit = (page - pool->start_page) & 0x3fUL;
	bitmap_clear_non_atomic(bit, pool->bitmap + idx);
	spinlock_release(&pool->lock);
}

void page_pool_get_stats(struct page_pool *pool, struct page_pool_stats *stats)
{
	uint64_t used_pages = 0UL;
	uint64_t idx;

	if ((pool == NULL) || (stats == NULL)) {
		return;
	}

	spinlock_obtain(&pool->lock);
	for (idx = 0UL; idx < pool->bitmap_size; idx++) {
		used_pages += bitmap_weight(pool->bitmap[idx]);
	}
	stats->total_pages = pool->bitmap_size << 6U;
	stats->used_pages = used_pages;
	stats->free_pages = stats->total_pages - used_pages;
	spinlock_release(&pool->lock);
}

static uint64_t sanitized_page_hpa;

void sanitize_pte_entry(uint64_t *ptep, const struct pgtable *table)
{
	*ptep = sanitized_page_hpa;
	if (table && table->flush_cache_pagewalk)
		table->flush_cache_pagewalk(ptep);
}

void sanitize_pte(uint64_t *pt_page, const struct pgtable *table)
{
	uint64_t i;
	for (i = 0UL; i < PTRS_PER_PGTL0E; i++) {
		sanitize_pte_entry(pt_page + i, table);
	}
}

/**
 * For x86, sanitized_page_hpa need point to one specific page,
 * for other arch,  sanitized_page_hpa is by default 0 without
 * calling this function.
 */
void init_sanitized_page(uint64_t *sanitized_page, uint64_t hpa)
{
	uint64_t i;

	sanitized_page_hpa = hpa;
	/* set ptep in sanitized_page point to itself */
	for (i = 0UL; i < PTRS_PER_PGTL0E; i++) {
		*(sanitized_page + i) = sanitized_page_hpa;
	}
}

static void try_to_free_pgtable_page(const struct pgtable *table,
			uint64_t *pgte, uint64_t *pt_page, uint32_t type)
{
	if (type == MR_DEL) {
		uint64_t index;

		for (index = 0UL; index < PTRS_PER_PGTL0E; index++) {
			uint64_t *pte = pt_page + index;
			if (table->pgentry_present(*pte)) {
				break;
			}
		}

		if (index == PTRS_PER_PGTL0E) {
			free_page(table->pool, (void *)pt_page);
			sanitize_pte_entry(pgte, table);
		}
	}
}

static void split_large_page(uint64_t *pte, enum _page_table_level level,
		__unused uint64_t vaddr, const struct pgtable *table)
{
	uint64_t *pbase;
	uint64_t ref_paddr, paddr, paddrinc;
	uint64_t i, ref_prot;

	if (level == PGT_LVL0)
		LOG_WRN("invalid page level to split huge page \r\n");

	paddrinc = get_level_size(level + 1);
	ref_paddr = (*pte) & PFN_MASK;
	ref_prot = (*pte) & ~PFN_MASK;
	paddr = pfn2paddr(ref_paddr);

	pbase = (uint64_t *)alloc_page(table->pool);
	dev_dbg(DBG_LEVEL_MMU, "%s, paddr: 0x%lx, pbase: 0x%lx\n", __func__, ref_paddr, pbase);

	for (i = 0UL; i < PTRS_PER_PGTL0E; i++) {
		table->set_pgentry(pbase + i, paddr, ref_prot, (level + 1), 1, table);
		paddr += paddrinc;
	}

	table->set_pgentry(pte, hva2hpa((void *)pbase), 0, level, 0, table);
}

static inline void local_modify_or_del_pte(uint64_t *pte,
		uint64_t prot_set, uint64_t prot_clr, uint32_t type, const struct pgtable *table)
{
	if (type == MR_MODIFY) {
		uint64_t new_pte = *pte;
		new_pte &= ~prot_clr;
		new_pte |= prot_set;
		*pte = new_pte;
		table->flush_cache_pagewalk(pte);
	} else {
		sanitize_pte_entry(pte, table);
	}
}

/*
 * In page table level 0,
 * type: MR_MODIFY
 * modify [vaddr_start, vaddr_end) memory type or page access right.
 * type: MR_DEL
 * delete [vaddr_start, vaddr_end) MT PT mapping
 */
static void modify_or_del_pgtl0(uint64_t *pgtl1e, uint64_t vaddr_start, uint64_t vaddr_end,
		uint64_t prot_set, uint64_t prot_clr, const struct pgtable *table, uint32_t type)
{
	uint64_t *pgtl0_page = page_addr(*pgtl1e);
	uint64_t vaddr = vaddr_start;
	uint64_t index = pgtl0e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "%s, vaddr: [0x%lx - 0x%lx]\n", __func__, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL0E; index++) {
		uint64_t *pgtl0e = pgtl0_page + index;

		if (!table->pgentry_present(*pgtl0e)) {
			if (type == MR_MODIFY) {
				LOG_WRN("%s, vaddr: 0x%lx pgtl0e is not present.\n", __func__, vaddr);
			}
		} else {
			local_modify_or_del_pte(pgtl0e, prot_set, prot_clr, type, table);
		}

		vaddr += PGTL0_SIZE;
		if (vaddr >= vaddr_end) {
			break;
		}
	}

	try_to_free_pgtable_page(table, pgtl1e, pgtl0_page, type);
}

/*
 * In page table level 1,
 * type: MR_MODIFY
 * modify [vaddr_start, vaddr_end) memory type or page access right.
 * type: MR_DEL
 * delete [vaddr_start, vaddr_end) MT PT mapping
 */
static void modify_or_del_pgtl1(uint64_t *pgtl2e, uint64_t vaddr_start, uint64_t vaddr_end,
		uint64_t prot_set, uint64_t prot_clr, const struct pgtable *table, uint32_t type)
{
	uint64_t *pgtl1_page = page_addr(*pgtl2e);
	uint64_t vaddr = vaddr_start;
	uint64_t index = pgtl1e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "%s, vaddr: [0x%lx - 0x%lx]\n", __func__, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL1E; index++) {
		uint64_t *pgtl1e = pgtl1_page + index;
		uint64_t vaddr_next = (vaddr & PGTL1_MASK) + PGTL1_SIZE;

		if (!table->pgentry_present(*pgtl1e)) {
			if (type == MR_MODIFY) {
				LOG_WRN("%s, addr: 0x%lx pgtl1e is not present.\n", __func__, vaddr);
			}
		} else {
			if (is_pgtl_large(*pgtl1e) != 0UL) {
				if ((vaddr_next > vaddr_end) || (!mem_aligned_check(vaddr, PGTL1_SIZE))) {
					split_large_page(pgtl1e, PGT_LVL1, vaddr, table);
				} else {
					local_modify_or_del_pte(pgtl1e, prot_set, prot_clr, type, table);
					if (vaddr_next < vaddr_end) {
						vaddr = vaddr_next;
						continue;
					}
					break;	/* done */
				}
			}
			modify_or_del_pgtl0(pgtl1e, vaddr, vaddr_end, prot_set, prot_clr, table, type);
		}
		if (vaddr_next >= vaddr_end) {
			break;	/* done */
		}
		vaddr = vaddr_next;
	}

	try_to_free_pgtable_page(table, pgtl2e, pgtl1_page, type);
}

/*
 * In page table level 2,
 * type: MR_MODIFY
 * modify [vaddr_start, vaddr_end) memory type or page access right.
 * type: MR_DEL
 * delete [vaddr_start, vaddr_end) MT PT mapping
 */
static void modify_or_del_pgtl2(const uint64_t *pgtl3e, uint64_t vaddr_start, uint64_t vaddr_end,
		uint64_t prot_set, uint64_t prot_clr, const struct pgtable *table, uint32_t type)
{
	uint64_t *pgtl2_page = page_addr(*pgtl3e);
	uint64_t vaddr = vaddr_start;
	uint64_t index = pgtl2e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "%s, vaddr: [0x%lx - 0x%lx]\n", __func__, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL2E; index++) {
		uint64_t *pgtl2e = pgtl2_page + index;
		uint64_t vaddr_next = (vaddr & PGTL2_MASK) + PGTL2_SIZE;

		if (!table->pgentry_present(*pgtl2e)) {
			if (type == MR_MODIFY) {
				LOG_WRN("%s, vaddr: 0x%lx pgtl2e is not present.\n", __func__, vaddr);
			}
		} else {
			if (is_pgtl_large(*pgtl2e) != 0UL) {
				if ((vaddr_next > vaddr_end) ||
						(!mem_aligned_check(vaddr, PGTL2_SIZE))) {
					split_large_page(pgtl2e, PGT_LVL2, vaddr, table);
				} else {
					local_modify_or_del_pte(pgtl2e, prot_set, prot_clr, type, table);
					if (vaddr_next < vaddr_end) {
						vaddr = vaddr_next;
						continue;
					}
					break;	/* done */
				}
			}
			modify_or_del_pgtl1(pgtl2e, vaddr, vaddr_end, prot_set, prot_clr, table, type);
		}
		if (vaddr_next >= vaddr_end) {
			break;	/* done */
		}
		vaddr = vaddr_next;
	}
}

/* [20260712] page-table modify/delete ordering
 *
 * Range update flow:
 *
 *   target range
 *        |
 *        v
 *   walk existing page-table hierarchy
 *        |
 *        +-- large leaf fully covers target chunk:
 *        |       modify/delete that leaf directly
 *        |
 *        +-- large leaf partially overlaps target chunk:
 *        |       split large leaf, then continue at lower level
 *        |
 *        +-- type == MR_MODIFY:
 *        |       clear prot_clr bits, set prot_set bits
 *        |
 *        +-- type == MR_DEL:
 *                sanitize leaf entry and free now-empty child tables
 *
 * Key rule:
 *   - callers own range validation and architecture permission encodings;
 *   - this path owns split-before-change ordering and child-table cleanup;
 *   - missing top-level entries are invalid for modify, while lower missing
 *     entries are skipped so sparse mappings can be updated safely.
 */
void pgtable_modify_or_del_map(uint64_t *pgtl3_page, uint64_t vaddr_base, uint64_t size,
		uint64_t prot_set, uint64_t prot_clr, const struct pgtable *table, uint32_t type)
{
	uint64_t vaddr = round_page_up(vaddr_base);
	uint64_t vaddr_next, vaddr_end;
	uint64_t *pgtl3e;

	vaddr_end = vaddr + round_page_down(size);
	dev_dbg(DBG_LEVEL_MMU, "%s, vaddr: 0x%lx, size: 0x%lx\n",
		__func__, vaddr, size);

	while (vaddr < vaddr_end) {
		vaddr_next = (vaddr & PGTL3_MASK) + PGTL3_SIZE;
		pgtl3e = pgtl3e_offset(pgtl3_page, vaddr);
		if ((!table->pgentry_present(*pgtl3e)) && (type == MR_MODIFY)) {
			ASSERT(false, "invalid op, pgtl3e not present");
		} else {
			modify_or_del_pgtl2(pgtl3e, vaddr, vaddr_end, prot_set, prot_clr, table, type);
			vaddr = vaddr_next;
		}
	}
}

/*
 * In page table level 0,
 * add [vaddr_start, vaddr_end) to [paddr_base, ...) MT PT mapping
 */
static void add_pgtl0(const uint64_t *pgtl1e, uint64_t paddr_start, uint64_t vaddr_start, uint64_t vaddr_end,
		uint64_t prot, const struct pgtable *table)
{
	uint64_t *pgtl0_page = page_addr(*pgtl1e);
	uint64_t vaddr = vaddr_start;
	uint64_t paddr = paddr_start;
	uint64_t index = pgtl0e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "%s, paddr: 0x%lx, vaddr: [0x%lx - 0x%lx]\n",
		__func__, paddr, vaddr_start, vaddr_end);
	for (; index < PTRS_PER_PGTL0E; index++) {
		uint64_t *pgtl0e = pgtl0_page + index;

		if (table->pgentry_present(*pgtl0e)) {
			LOG_FTL("%s, pgtl0e 0x%lx is already present!\n", __func__, vaddr);
		} else {
			table->set_pgentry(pgtl0e, paddr, prot, PGT_LVL0, 1, table);
		}
		paddr += PGTL0_SIZE;
		vaddr += PGTL0_SIZE;

		if (vaddr >= vaddr_end) {
			break;	/* done */
		}
	}
}

/*
 * In page table level 1,
 * add [vaddr_start, vaddr_end) to [paddr_base, ...) MT PT mapping
 */
static void add_pgtl1(const uint64_t *pgtl2e, uint64_t paddr_start, uint64_t vaddr_start, uint64_t vaddr_end,
		uint64_t prot, const struct pgtable *table)
{
	uint64_t *pgtl1_page = page_addr(*pgtl2e);
	uint64_t vaddr = vaddr_start;
	uint64_t paddr = paddr_start;
	uint64_t index = pgtl1e_index(vaddr);
	uint64_t local_prot = prot;

	dev_dbg(DBG_LEVEL_MMU, "%s, paddr: 0x%lx, vaddr: [0x%lx - 0x%lx]\n",
		__func__, paddr, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL1E; index++) {
		uint64_t *pgtl1e = pgtl1_page + index;
		uint64_t vaddr_next = (vaddr & PGTL1_MASK) + PGTL1_SIZE;

		if (is_pgtl_large(*pgtl1e) != 0UL) {
			LOG_FTL("%s, pgtl1e 0x%lx is already present!\n", __func__, vaddr);
		} else {
			if (!table->pgentry_present(*pgtl1e)) {
				if (table->large_page_support(PGT_LVL1, prot) &&
					mem_aligned_check(paddr, PGTL1_SIZE) &&
					mem_aligned_check(vaddr, PGTL1_SIZE) &&
					(vaddr_next <= vaddr_end)) {
					table->set_pgentry(pgtl1e, paddr, local_prot, PGT_LVL1, 1, table);
					if (vaddr_next < vaddr_end) {
						paddr += (vaddr_next - vaddr);
						vaddr = vaddr_next;
						continue;
					}
					break;	/* done */
				} else {
					void *pgtl0_page = alloc_page(table->pool);
					table->set_pgentry(pgtl1e, hva2hpa((void *)pgtl0_page), 0, PGT_LVL1, 0, table);
				}
			}
			add_pgtl0(pgtl1e, paddr, vaddr, vaddr_end, prot, table);
		}
		if (vaddr_next >= vaddr_end) {
			break;	/* done */
		}
		paddr += (vaddr_next - vaddr);
		vaddr = vaddr_next;
	}
}

/*
 * In page table level 2,
 * add [vaddr_start, vaddr_end) to [paddr_base, ...) MT PT mapping
 */
static void add_pgtl2(const uint64_t *pgtl3e, uint64_t paddr_start, uint64_t vaddr_start, uint64_t vaddr_end,
		uint64_t prot, const struct pgtable *table)
{
	uint64_t *pgtl2_page = page_addr(*pgtl3e);
	uint64_t vaddr = vaddr_start;
	uint64_t paddr = paddr_start;
	uint64_t index = pgtl2e_index(vaddr);
	uint64_t local_prot = prot;

	dev_dbg(DBG_LEVEL_MMU, "%s, paddr: 0x%lx, vaddr: [0x%lx - 0x%lx]\n", __func__, paddr, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL2E; index++) {
		uint64_t *pgtl2e = pgtl2_page + index;
		uint64_t vaddr_next = (vaddr & PGTL2_MASK) + PGTL2_SIZE;

		if (is_pgtl_large(*pgtl2e) != 0UL) {
			LOG_FTL("%s, pgtl2e 0x%lx is already present!\n", __func__, vaddr);
		} else {
			if (!table->pgentry_present(*pgtl2e)) {
				if (table->large_page_support(PGT_LVL2, prot) &&
					mem_aligned_check(paddr, PGTL2_SIZE) &&
					mem_aligned_check(vaddr, PGTL2_SIZE) &&
					(vaddr_next <= vaddr_end)) {
					table->set_pgentry(pgtl2e, paddr, local_prot, PGT_LVL2, 1, table);
					if (vaddr_next < vaddr_end) {
						paddr += (vaddr_next - vaddr);
						vaddr = vaddr_next;
						continue;
					}
					break;	/* done */
				} else {
					void *pgtl1_page = alloc_page(table->pool);
					table->set_pgentry(pgtl2e, hva2hpa((void *)pgtl1_page), 0, PGT_LVL2, 0, table);
				}
			}
			add_pgtl1(pgtl2e, paddr, vaddr, vaddr_end, prot, table);
		}
		if (vaddr_next >= vaddr_end) {
			break;	/* done */
		}
		paddr += (vaddr_next - vaddr);
		vaddr = vaddr_next;
	}
}

/* [20260712] page-table add-map publication
 *
 * Add-map flow:
 *
 *   pgtable_add_map()
 *          |
 *          v
 *   walk PGTL3 -> PGTL2 -> PGTL1 -> PGTL0
 *          |
 *          +-- if range is aligned and architecture allows it:
 *          |       install a large leaf mapping
 *          |
 *          +-- otherwise:
 *                  allocate the next-level table and continue walking
 *
 * The physical address advances by the same byte distance as the virtual
 * address. Passing identical paddr/vaddr bases therefore builds an identity
 * map; passing different bases builds an offset map.
 *
 * Key rule:
 *   - callers own overlap validation and architecture permission encodings;
 *   - this path owns child-table allocation and increasing-address publication;
 *   - large leaves are used only when address alignment, range size, and the
 *     architecture table callbacks all allow the mapping to stay coarse.
 */
void pgtable_add_map(uint64_t *pgtl3_page, uint64_t paddr_base, uint64_t vaddr_base,
		uint64_t size, uint64_t prot, const struct pgtable *table)
{
	uint64_t vaddr, vaddr_next, vaddr_end;
	uint64_t paddr;
	uint64_t *pgtl3e;

	dev_dbg(DBG_LEVEL_MMU, "%s, paddr 0x%lx, vaddr 0x%lx, size 0x%lx\n", __func__, paddr_base, vaddr_base, size);

	/* align address to page size*/
	vaddr = round_page_up(vaddr_base);
	paddr = round_page_up(paddr_base);
	vaddr_end = vaddr + round_page_down(size);

	while (vaddr < vaddr_end) {
		vaddr_next = (vaddr & PGTL3_MASK) + PGTL3_SIZE;
		pgtl3e = pgtl3e_offset(pgtl3_page, vaddr);
		if (!table->pgentry_present(*pgtl3e)) {
			void *pgtl2_page = alloc_page(table->pool);
			table->set_pgentry(pgtl3e, hva2hpa((void *)pgtl2_page), 0, PGT_LVL3, 0, table);
		}
		add_pgtl2(pgtl3e, paddr, vaddr, vaddr_end, prot, table);

		paddr += (vaddr_next - vaddr);
		vaddr = vaddr_next;
	}
}

void *pgtable_create_root(const struct pgtable *table)
{
	uint64_t *page = (uint64_t *)alloc_page(table->pool);
	return page;
}

const uint64_t *pgtable_lookup_entry(uint64_t *pgtl3_page, uint64_t addr, uint64_t *pg_size, const struct pgtable *table)
{
	const uint64_t *pret = NULL;
	bool present = true;
	uint64_t *pgtl3e, *pgtl2e, *pgtl1e, *pgtl0e;

	pgtl3e = pgtl3e_offset(pgtl3_page, addr);
	present = table->pgentry_present(*pgtl3e);

	if (present) {
		pgtl2e = pgtl2e_offset(pgtl3e, addr);
		present = table->pgentry_present(*pgtl2e);
		if (present) {
                        if (is_pgtl_large(*pgtl2e) != 0UL) {
				*pg_size = PGTL2_SIZE;
				pret = pgtl2e;
			} else {
                                pgtl1e = pgtl1e_offset(pgtl2e, addr);
				present = table->pgentry_present(*pgtl1e);
				if (present) {
                                        if (is_pgtl_large(*pgtl1e) != 0UL) {
						*pg_size = PGTL1_SIZE;
						pret = pgtl1e;
					} else {
                                                pgtl0e = pgtl0e_offset(pgtl1e, addr);
						present = table->pgentry_present(*pgtl0e);
						if (present) {
							*pg_size = PGTL0_SIZE;
                                                        pret = pgtl0e;
						}
					}
				}
			}
		}
	}

	return pret;
}
