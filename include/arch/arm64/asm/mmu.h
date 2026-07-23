/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_MMU_H
#define ARM64_MMU_H

#include <types.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/mte.h>
#include <asm/sysreg.h>

#define MAX_FDT_RSVD_REGIONS	16
#define CACHE_LINE_SIZE		64U

#define ARM64_S1_ACCESS_READ	(1U << 0U)
#define ARM64_S1_ACCESS_WRITE	(1U << 1U)
#define ARM64_S1_ACCESS_EXECUTE	(1U << 2U)

struct page_pool_stats;
struct page_pool;

static inline void *arch_page_pool_alloc(const struct page_pool *pool,
	void *page)
{
	return arm64_mte_page_alloc(pool, page);
}

static inline bool arch_page_pool_free(const struct page_pool *pool,
	const void *page)
{
	return arm64_mte_page_free(pool, page);
}

static inline void *arch_page_pool_untag(const void *page)
{
	return (void *)arm64_mte_untag_address((uint64_t)page);
}

static inline void arm64_set_ttbr0_el2(uint64_t ttbr)
{
	write_ttbr0_el2(ttbr);
	flush_tlb_local();
}

void init_paging(void);
bool enable_paging(void);
bool arm64_mmu_is_enabled(void);
void flush_tlb(uint64_t addr);
void flush_tlb_range(uint64_t addr, uint64_t size);
void flush_invalidate_all_cache(void);
void clean_cacheline(const volatile void *p);
void clean_cache_range(const volatile void *p, uint64_t size);
void invalidate_cache_range(const volatile void *p, uint64_t size);
void arm64_nullop(void);
void arm64_dcache_wb_range(const volatile void *p, uint64_t size);
void arm64_dcache_wbinv_range(const volatile void *p, uint64_t size);
void arm64_dcache_inv_range(const volatile void *p, uint64_t size);
void arm64_dic_idc_icache_sync_range(const volatile void *p, uint64_t size);
void arm64_idc_aliasing_icache_sync_range(const volatile void *p, uint64_t size);
void arm64_aliasing_icache_sync_range(const volatile void *p, uint64_t size);
uint64_t arm64_get_phys_mem_start(void);
uint64_t arm64_get_phys_mem_size(void);
const struct mem_region *arm64_get_reserved_mem_regions(uint32_t *count);
void arm64_get_hv_s1_page_pool_stats(struct page_pool_stats *stats);
bool arm64_get_hv_s1_memory_attr(uint64_t addr,
	struct arm64_memory_attr *attr);
bool arm64_get_hv_s1_memory_access(uint64_t addr, uint8_t *access);

#endif /* ARM64_MMU_H */
