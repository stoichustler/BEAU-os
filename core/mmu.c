/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <types.h>
#include <errno.h>
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
	uint64_t bitmap_size;

	if ((pool == NULL) || (page_base == NULL) || (bitmap_base == NULL) ||
		(page_num <= 0) || (((uint32_t)page_num & 0x3fU) != 0U)) {
		panic("invalid page pool configuration");
	}

	bitmap_size = (uint64_t)(uint32_t)page_num / 8UL;
	pool->bitmap = bitmap_base;
	pool->start_page = (struct page *)page_base;
	pool->bitmap_size = bitmap_size / sizeof(uint64_t);
	pool->last_hint_id = 0UL;
	pool->dummy_page = NULL;
	spinlock_init(&pool->lock);
	(void)memset(pool->bitmap, 0U, bitmap_size);
}

struct page *alloc_page(struct page_pool *pool)
{
	struct page *page = NULL;
	uint64_t loop_idx, idx, bit;
	bool tag_error = false;

	spinlock_obtain(&pool->lock);
	for (loop_idx = pool->last_hint_id;
		loop_idx < (pool->last_hint_id + pool->bitmap_size); loop_idx++) {
		idx = loop_idx % pool->bitmap_size;
		if (*(pool->bitmap + idx) != ~0UL) {
			bit = ffz64(*(pool->bitmap + idx));
			bitmap_set_non_atomic(bit, pool->bitmap + idx);
			page = pool->start_page + ((idx << 6U) + bit);
			page = (struct page *)arch_page_pool_alloc(pool, page);
			if (page == NULL) {
				bitmap_clear_non_atomic(bit, pool->bitmap + idx);
				tag_error = true;
			}

			pool->last_hint_id = idx;
			break;
		}
	}
	spinlock_release(&pool->lock);
	if (tag_error) {
		panic("page pool MTE allocation failed");
	}

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
	uint64_t page_address;
	uint64_t pool_start;
	uint64_t pool_size;
	uint64_t page_id;
	uint64_t idx;
	uint64_t bit;

	if ((pool == NULL) || (page == NULL)) {
		panic("invalid page pool free request");
	}
	page_address = (uint64_t)arch_page_pool_untag(page);
	pool_start = (uint64_t)pool->start_page;
	pool_size = (pool->bitmap_size << 6U) * sizeof(struct page);
	if (((page_address & (PAGE_SIZE - 1UL)) != 0UL) ||
		(page_address < pool_start) ||
		((page_address - pool_start) >= pool_size)) {
		panic("page pool free address out of range: 0x%lx", page_address);
	}
	page_id = (page_address - pool_start) / sizeof(struct page);
	idx = page_id >> 6U;
	bit = page_id & 0x3fUL;

	spinlock_obtain(&pool->lock);
	if ((pool->bitmap[idx] & (1UL << bit)) == 0UL) {
		spinlock_release(&pool->lock);
		panic("page pool double free index:%lu", page_id);
	}
	if (!arch_page_pool_free(pool, page)) {
		spinlock_release(&pool->lock);
		panic("page pool MTE free validation failed index:%lu", page_id);
	}
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

void pgtable_update_init(struct pgtable_update *update, uint64_t *retired_bitmap,
	uint64_t retired_bitmap_words, const struct pgtable *table)
{
	if ((update == NULL) || (retired_bitmap == NULL) || (table == NULL) ||
		(table->pool == NULL) ||
		(retired_bitmap_words < table->pool->bitmap_size)) {
		panic("invalid deferred page-table update");
	}

	(void)memset(retired_bitmap, 0U,
		retired_bitmap_words * sizeof(retired_bitmap[0]));
	update->retired_bitmap = retired_bitmap;
	update->retired_bitmap_words = retired_bitmap_words;
	update->retired_pages = 0UL;
	update->range_start = 0UL;
	update->range_end = 0UL;
	update->operations = 0U;
	update->changed = false;
}

static void pgtable_update_record_range(struct pgtable_update *update,
	uint64_t start, uint64_t end, uint32_t operation)
{
	if (update == NULL) {
		return;
	}
	if ((update->operations == 0U) || (start < update->range_start)) {
		update->range_start = start;
	}
	if ((update->operations == 0U) || (end > update->range_end)) {
		update->range_end = end;
	}
	update->operations |= operation;
}

static void pgtable_update_record(struct pgtable_update *update,
	uint64_t start, uint64_t end, uint32_t operation)
{
	pgtable_update_record_range(update, start, end, operation);
	if (update == NULL) {
		return;
	}
	update->changed = true;
}

static void defer_pgtable_page(const struct pgtable *table, uint64_t *pt_page,
	struct pgtable_update *update)
{
	uint64_t page_address;
	uint64_t pool_start;
	uint64_t pool_pages;
	uint64_t page_id;

	if (update == NULL) {
		free_page(table->pool, (void *)pt_page);
		return;
	}

	page_address = (uint64_t)arch_page_pool_untag(pt_page);
	pool_start = (uint64_t)table->pool->start_page;
	pool_pages = table->pool->bitmap_size << 6U;
	if ((page_address < pool_start) ||
		((page_address - pool_start) >= (pool_pages * sizeof(struct page))) ||
		(((page_address - pool_start) % sizeof(struct page)) != 0UL)) {
		panic("deferred page-table page is outside its pool: 0x%lx",
			page_address);
	}
	page_id = (page_address - pool_start) / sizeof(struct page);
	if ((page_id >> 6U) >= update->retired_bitmap_words) {
		panic("deferred page-table bitmap overflow index:%lu", page_id);
	}
	if (bitmap_test(page_id, update->retired_bitmap)) {
		panic("duplicate deferred page-table page index:%lu", page_id);
	}

	bitmap_set_non_atomic(page_id, update->retired_bitmap);
	update->retired_pages++;
}

void pgtable_free_retired_pages(struct pgtable_update *update,
	const struct pgtable *table)
{
	uint64_t page_id;
	uint64_t pool_pages;
	uint64_t freed_pages = 0UL;

	if ((update == NULL) || (table == NULL) || (table->pool == NULL) ||
		(update->retired_bitmap == NULL) ||
		(update->retired_bitmap_words < table->pool->bitmap_size)) {
		panic("invalid retired page-table batch");
	}

	pool_pages = table->pool->bitmap_size << 6U;
	for (page_id = 0UL; page_id < pool_pages; page_id++) {
		if (bitmap_test(page_id, update->retired_bitmap)) {
			struct page *raw_page = table->pool->start_page + page_id;
			struct page *page = hpa2hva(hva2hpa(raw_page));

			free_page(table->pool, page);
			bitmap_clear_non_atomic(page_id, update->retired_bitmap);
			freed_pages++;
		}
	}
	if (freed_pages != update->retired_pages) {
		panic("retired page-table count mismatch expected:%lu freed:%lu",
			update->retired_pages, freed_pages);
	}
	update->retired_pages = 0UL;
}

static void replace_pgtable_entry(uint64_t *pte, uint64_t new_pte,
	uint64_t vaddr, uint64_t size, const struct pgtable *table)
{
	if (table->replace_pgentry != NULL) {
		table->replace_pgentry(pte, new_pte, vaddr, size, table);
	} else {
		*pte = new_pte;
		table->flush_cache_pagewalk(pte);
	}
}

static void try_to_free_pgtable_page(const struct pgtable *table,
	uint64_t *pgte, uint64_t *pt_page, uint32_t type,
	struct pgtable_update *update)
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
			sanitize_pte_entry(pgte, table);
			if (update != NULL) {
				update->changed = true;
			}
			defer_pgtable_page(table, pt_page, update);
		}
	}
}

static void split_large_page(uint64_t *pte, enum _page_table_level level,
	uint64_t vaddr, const struct pgtable *table,
	struct pgtable_update *update)
{
	uint64_t *pbase;
	uint64_t ref_paddr, paddr, paddrinc, new_pte = 0UL;
	uint64_t i, ref_prot;

	if (level == PGT_LVL0)
		LOG_WRN("invalid page level to split huge page \r\n");

	paddrinc = get_level_size(level + 1);
	ref_paddr = (*pte) & PFN_MASK;
	ref_prot = (*pte) & ~PFN_MASK;
	paddr = pfn2paddr(ref_paddr);

	pbase = (uint64_t *)alloc_page(table->pool);
	dev_dbg(DBG_LEVEL_MMU, "MMU:    PA:0x%016lx PB:0x%016lx\n", ref_paddr, pbase);

	for (i = 0UL; i < PTRS_PER_PGTL0E; i++) {
		table->set_pgentry(pbase + i, paddr, ref_prot, (level + 1), 1, table);
		paddr += paddrinc;
	}

	table->set_pgentry(&new_pte, hva2hpa((void *)pbase), 0, level, 0, table);
	replace_pgtable_entry(pte, new_pte, vaddr, get_level_size(level), table);
	if (update != NULL) {
		update->changed = true;
	}
}

static inline void local_modify_or_del_pte(uint64_t *pte,
	uint64_t vaddr, uint64_t size, uint64_t prot_set, uint64_t prot_clr,
	uint32_t type, const struct pgtable *table, struct pgtable_update *update)
{
	if (type == MR_MODIFY) {
		uint64_t new_pte = *pte;
		new_pte &= ~prot_clr;
		new_pte |= prot_set;
		if (new_pte != *pte) {
			replace_pgtable_entry(pte, new_pte, vaddr, size, table);
			if (update != NULL) {
				update->changed = true;
			}
		}
	} else {
		sanitize_pte_entry(pte, table);
		if (update != NULL) {
			update->changed = true;
		}
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
	uint64_t prot_set, uint64_t prot_clr, const struct pgtable *table,
	uint32_t type, struct pgtable_update *update)
{
	uint64_t *pgtl0_page = page_addr(*pgtl1e);
	uint64_t vaddr = vaddr_start;
	uint64_t index = pgtl0e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "MMU:    VA:[0x%016lx - 0x%016lx] PT0\n", vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL0E; index++) {
		uint64_t *pgtl0e = pgtl0_page + index;

		if (!table->pgentry_present(*pgtl0e)) {
			if (type == MR_MODIFY) {
				LOG_WRN("%s, vaddr: 0x%lx pgtl0e is not present.\n", __func__, vaddr);
			}
		} else {
			local_modify_or_del_pte(pgtl0e, vaddr, PGTL0_SIZE,
				prot_set, prot_clr, type, table, update);
		}

		vaddr += PGTL0_SIZE;
		if (vaddr >= vaddr_end) {
			break;
		}
	}

	try_to_free_pgtable_page(table, pgtl1e, pgtl0_page, type, update);
}

/*
 * In page table level 1,
 * type: MR_MODIFY
 * modify [vaddr_start, vaddr_end) memory type or page access right.
 * type: MR_DEL
 * delete [vaddr_start, vaddr_end) MT PT mapping
 */
static void modify_or_del_pgtl1(uint64_t *pgtl2e, uint64_t vaddr_start, uint64_t vaddr_end,
	uint64_t prot_set, uint64_t prot_clr, const struct pgtable *table,
	uint32_t type, struct pgtable_update *update)
{
	uint64_t *pgtl1_page = page_addr(*pgtl2e);
	uint64_t vaddr = vaddr_start;
	uint64_t index = pgtl1e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "MMU:    VA:[0x%016lx - 0x%016lx] PT1\n", vaddr, vaddr_end);
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
					split_large_page(pgtl1e, PGT_LVL1, vaddr, table, update);
				} else {
					local_modify_or_del_pte(pgtl1e, vaddr, PGTL1_SIZE,
						prot_set, prot_clr, type, table, update);
					if (vaddr_next < vaddr_end) {
						vaddr = vaddr_next;
						continue;
					}
					break;	/* done */
				}
			}
			modify_or_del_pgtl0(pgtl1e, vaddr, vaddr_end, prot_set,
				prot_clr, table, type, update);
		}
		if (vaddr_next >= vaddr_end) {
			break;	/* done */
		}
		vaddr = vaddr_next;
	}

	try_to_free_pgtable_page(table, pgtl2e, pgtl1_page, type, update);
}

/*
 * In page table level 2,
 * type: MR_MODIFY
 * modify [vaddr_start, vaddr_end) memory type or page access right.
 * type: MR_DEL
 * delete [vaddr_start, vaddr_end) MT PT mapping
 */
static void modify_or_del_pgtl2(uint64_t *pgtl3e, uint64_t vaddr_start,
	uint64_t vaddr_end, uint64_t prot_set, uint64_t prot_clr,
	const struct pgtable *table, uint32_t type, struct pgtable_update *update)
{
	uint64_t *pgtl2_page = page_addr(*pgtl3e);
	uint64_t vaddr = vaddr_start;
	uint64_t index = pgtl2e_index(vaddr);

	dev_dbg(DBG_LEVEL_MMU, "MMU:    VA:[0x%016lx - 0x%016lx] PT2\n", vaddr, vaddr_end);
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
					split_large_page(pgtl2e, PGT_LVL2, vaddr, table, update);
				} else {
					local_modify_or_del_pte(pgtl2e, vaddr, PGTL2_SIZE,
						prot_set, prot_clr, type, table, update);
					if (vaddr_next < vaddr_end) {
						vaddr = vaddr_next;
						continue;
					}
					break;	/* done */
				}
			}
			modify_or_del_pgtl1(pgtl2e, vaddr, vaddr_end, prot_set,
				prot_clr, table, type, update);
		}
		if (vaddr_next >= vaddr_end) {
			break;	/* done */
		}
		vaddr = vaddr_next;
	}

	try_to_free_pgtable_page(table, pgtl3e, pgtl2_page, type, update);
}

/* [20260720] Page-table replacement and retirement ordering
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
 *        |       populate child table, replace through architecture BBM hook,
 *        |       then continue at lower level
 *        |
 *        +-- type == MR_MODIFY:
 *        |       clear prot_clr bits, set prot_set bits
 *        |
 *        +-- type == MR_DEL:
 *                sanitize leaf entry, detach empty child tables, and either
 *                free immediately or record them in the caller's retire batch
 *
 * Key rule:
 *   - callers own range validation and architecture permission encodings;
 *   - this path owns split-before-change ordering and table detachment;
 *   - a deferred caller must synchronize every hardware walker before calling
 *     pgtable_free_retired_pages(), which may retag and reuse those pages;
 *   - missing top-level entries are invalid for modify, while lower missing
 *     entries are skipped so sparse mappings can be updated safely.
 */
void pgtable_modify_or_del_map_deferred(uint64_t *pgtl3_page,
	uint64_t vaddr_base, uint64_t size, uint64_t prot_set,
	uint64_t prot_clr, const struct pgtable *table, uint32_t type,
	struct pgtable_update *update)
{
	uint64_t vaddr = round_page_up(vaddr_base);
	uint64_t vaddr_next, vaddr_end;
	uint64_t *pgtl3e;

	vaddr_end = vaddr + round_page_down(size);
	pgtable_update_record_range(update, vaddr, vaddr_end,
		(type == MR_MODIFY) ? PGTABLE_UPDATE_MODIFY : PGTABLE_UPDATE_DELETE);
	dev_dbg(DBG_LEVEL_MMU, "MMU:    VA:[0x%016lx - 0x%016lx]\n",
		vaddr, vaddr + size);

	while (vaddr < vaddr_end) {
		vaddr_next = (vaddr & PGTL3_MASK) + PGTL3_SIZE;
		pgtl3e = pgtl3e_offset(pgtl3_page, vaddr);
		if (!table->pgentry_present(*pgtl3e)) {
			if (type == MR_MODIFY) {
				ASSERT(false, "invalid op, pgtl3e not present");
			}
		} else {
			modify_or_del_pgtl2(pgtl3e, vaddr, vaddr_end,
				prot_set, prot_clr, table, type, update);
		}
		vaddr = vaddr_next;
	}
}

void pgtable_modify_or_del_map(uint64_t *pgtl3_page, uint64_t vaddr_base,
	uint64_t size, uint64_t prot_set, uint64_t prot_clr,
	const struct pgtable *table, uint32_t type)
{
	pgtable_modify_or_del_map_deferred(pgtl3_page, vaddr_base, size,
		prot_set, prot_clr, table, type, NULL);
}

static bool pgtable_map_allows_leaf(const struct pgtable_map_request *request,
	enum _page_table_level level, uint64_t paddr, uint64_t vaddr,
	uint64_t vaddr_next, uint64_t vaddr_end, const struct pgtable *table)
{
	return ((request->allowed_leaf_levels & (1U << level)) != 0U) &&
		table->large_page_support(level, request->prot) &&
		mem_aligned_check(paddr, get_level_size(level)) &&
		mem_aligned_check(vaddr, get_level_size(level)) &&
		(vaddr_next <= vaddr_end);
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

	dev_dbg(DBG_LEVEL_MMU, "PT0:    PA:0x%016lx VA:[0x%016lx - 0x%016lx]\n",
		paddr, vaddr_start, vaddr_end);
	for (; index < PTRS_PER_PGTL0E; index++) {
		uint64_t *pgtl0e = pgtl0_page + index;

		if (table->pgentry_present(*pgtl0e)) {
			if (table->replace_pgentry != NULL) {
				panic("%s stage-2 mapping overlap at 0x%lx", __func__, vaddr);
			}
			LOG_ERR("%s, pgtl0e 0x%lx is already present!\n",
				__func__, vaddr);
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
		uint64_t prot, const struct pgtable *table,
		const struct pgtable_map_request *request)
{
	uint64_t *pgtl1_page = page_addr(*pgtl2e);
	uint64_t vaddr = vaddr_start;
	uint64_t paddr = paddr_start;
	uint64_t index = pgtl1e_index(vaddr);
		uint64_t local_prot = prot;

	dev_dbg(DBG_LEVEL_MMU, "PT1:    PA:0x%016lx VA:[0x%016lx - 0x%016lx]\n",
		paddr, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL1E; index++) {
		uint64_t *pgtl1e = pgtl1_page + index;
		uint64_t vaddr_next = (vaddr & PGTL1_MASK) + PGTL1_SIZE;

		if (is_pgtl_large(*pgtl1e) != 0UL) {
			if (table->replace_pgentry != NULL) {
				panic("%s stage-2 mapping overlap at 0x%lx", __func__, vaddr);
			}
			LOG_ERR("%s, pgtl1e 0x%lx is already present!\n",
				__func__, vaddr);
		} else {
			if (!table->pgentry_present(*pgtl1e)) {
				if (pgtable_map_allows_leaf(request, PGT_LVL1, paddr, vaddr,
					vaddr_next, vaddr_end, table)) {
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
		uint64_t prot, const struct pgtable *table,
		const struct pgtable_map_request *request)
{
	uint64_t *pgtl2_page = page_addr(*pgtl3e);
	uint64_t vaddr = vaddr_start;
	uint64_t paddr = paddr_start;
	uint64_t index = pgtl2e_index(vaddr);
	uint64_t local_prot = prot;

	dev_dbg(DBG_LEVEL_MMU, "PT2:    PA:0x%016lx VA:[0x%016lx - 0x%016lx]\n", paddr, vaddr, vaddr_end);
	for (; index < PTRS_PER_PGTL2E; index++) {
		uint64_t *pgtl2e = pgtl2_page + index;
		uint64_t vaddr_next = (vaddr & PGTL2_MASK) + PGTL2_SIZE;

		if (is_pgtl_large(*pgtl2e) != 0UL) {
			if (table->replace_pgentry != NULL) {
				panic("%s stage-2 mapping overlap at 0x%lx", __func__, vaddr);
			}
			LOG_ERR("%s, pgtl2e 0x%lx is already present!\n",
				__func__, vaddr);
		} else {
			if (!table->pgentry_present(*pgtl2e)) {
				if (pgtable_map_allows_leaf(request, PGT_LVL2, paddr, vaddr,
					vaddr_next, vaddr_end, table)) {
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
				add_pgtl1(pgtl2e, paddr, vaddr, vaddr_end, prot, table, request);
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
int32_t pgtable_add_map_deferred(uint64_t *pgtl3_page,
	const struct pgtable_map_request *request, const struct pgtable *table,
	struct pgtable_update *update)
{
	uint64_t vaddr, vaddr_next, vaddr_end;
	uint64_t paddr;
	uint64_t map_size;
	uint64_t *pgtl3e;

	if ((pgtl3_page == NULL) || (request == NULL) || (table == NULL) ||
		(table->pool == NULL) || (table->pgentry_present == NULL) ||
		(table->large_page_support == NULL) || (table->set_pgentry == NULL) ||
		(table->flush_cache_pagewalk == NULL) || (request->size == 0UL) ||
		((request->allowed_leaf_levels & ~PGTABLE_LEAF_LEVEL_MASK) != 0U) ||
		((request->allowed_leaf_levels & PGTABLE_LEAF_LVL0) == 0U)) {
		return -EINVAL;
	}
	if ((request->vaddr_base > (UINT64_MAX - (PAGE_SIZE - 1UL))) ||
		(request->paddr_base > (UINT64_MAX - (PAGE_SIZE - 1UL)))) {
		return -EINVAL;
	}

	map_size = round_page_down(request->size);
	vaddr = round_page_up(request->vaddr_base);
	paddr = round_page_up(request->paddr_base);
	if ((map_size == 0UL) || (map_size > (UINT64_MAX - vaddr)) ||
		(map_size > (UINT64_MAX - paddr))) {
		return -EINVAL;
	}
	vaddr_end = vaddr + map_size;

	dev_dbg(DBG_LEVEL_MMU, "MAP:    PA:0x%016lx VA:0x%016lx (size:0x%08lx)\n",
		request->paddr_base, request->vaddr_base, request->size);

	while (vaddr < vaddr_end) {
		vaddr_next = (vaddr & PGTL3_MASK) + PGTL3_SIZE;
		pgtl3e = pgtl3e_offset(pgtl3_page, vaddr);
		if (!table->pgentry_present(*pgtl3e)) {
			void *pgtl2_page = alloc_page(table->pool);
			table->set_pgentry(pgtl3e, hva2hpa((void *)pgtl2_page), 0, PGT_LVL3, 0, table);
		}
		add_pgtl2(pgtl3e, paddr, vaddr, vaddr_end, request->prot, table,
			request);

		paddr += (vaddr_next - vaddr);
		vaddr = vaddr_next;
	}
	pgtable_update_record(update, round_page_up(request->vaddr_base), vaddr_end,
		PGTABLE_UPDATE_MAP);

	return 0;
}

void pgtable_add_map(uint64_t *pgtl3_page, uint64_t paddr_base, uint64_t vaddr_base,
	uint64_t size, uint64_t prot, const struct pgtable *table)
{
	const struct pgtable_map_request request = {
		.paddr_base = paddr_base,
		.vaddr_base = vaddr_base,
		.size = size,
		.prot = prot,
		.allowed_leaf_levels = PGTABLE_LEAF_LEVEL_MASK,
	};
	int32_t status;

	status = pgtable_add_map_deferred(pgtl3_page, &request, table, NULL);
	if (status != 0) {
		panic("invalid page-table map status:%d va=0x%lx size=0x%lx", status,
			vaddr_base, size);
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
