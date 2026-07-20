/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_MTE_H
#define ARM64_MTE_H

#include <types.h>

#define ARM64_MTE_GRANULE_SIZE	16UL
#define ARM64_MTE_TAG_SHIFT	56U
#define ARM64_MTE_TAG_MASK	(0xfUL << ARM64_MTE_TAG_SHIFT)
#define ARM64_MTE_TOP_BYTE_MASK	(0xffUL << ARM64_MTE_TAG_SHIFT)

struct page_pool;

#ifdef CONFIG_ARM64_MTE

void arm64_mte_bootstrap_init(void);
bool arm64_mte_prepare_pcpu(uint64_t *mair, uint64_t *tcr,
	uint64_t *sctlr);
void arm64_mte_mark_pcpu_enabled(void);
bool arm64_mte_is_enabled(void);
int32_t arm64_mte_register_page_pool(const struct page_pool *pool,
	void *base, uint64_t page_count, uint8_t *tag_states);
void *arm64_mte_page_alloc(const struct page_pool *pool, void *page);
bool arm64_mte_page_free(const struct page_pool *pool, const void *page);
uint64_t arm64_mte_untag_address(uint64_t address);
void *arm64_mte_hpa_to_hva(uint64_t hpa);
uint64_t arm64_mte_hva_to_hpa(const void *hva);
bool arm64_mte_is_sync_fault(uint64_t esr);
void arm64_mte_report_sync_fault(uint64_t esr, uint64_t far,
	uint64_t elr, uint16_t pcpu_id);

#else

static inline void arm64_mte_bootstrap_init(void)
{
}

static inline bool arm64_mte_prepare_pcpu(__unused uint64_t *mair,
	__unused uint64_t *tcr, __unused uint64_t *sctlr)
{
	return true;
}

static inline void arm64_mte_mark_pcpu_enabled(void)
{
}

static inline bool arm64_mte_is_enabled(void)
{
	return false;
}

static inline int32_t arm64_mte_register_page_pool(
	__unused const struct page_pool *pool, __unused void *base,
	__unused uint64_t page_count, __unused uint8_t *tag_states)
{
	return 0;
}

static inline void *arm64_mte_page_alloc(__unused const struct page_pool *pool,
	void *page)
{
	return page;
}

static inline bool arm64_mte_page_free(__unused const struct page_pool *pool,
	__unused const void *page)
{
	return true;
}

static inline uint64_t arm64_mte_untag_address(uint64_t address)
{
	return address;
}

static inline void *arm64_mte_hpa_to_hva(uint64_t hpa)
{
	return (void *)hpa;
}

static inline uint64_t arm64_mte_hva_to_hpa(const void *hva)
{
	return (uint64_t)hva;
}

static inline bool arm64_mte_is_sync_fault(__unused uint64_t esr)
{
	return false;
}

static inline void arm64_mte_report_sync_fault(__unused uint64_t esr,
	__unused uint64_t far, __unused uint64_t elr,
	__unused uint16_t pcpu_id)
{
}

#endif /* CONFIG_ARM64_MTE */

#endif /* ARM64_MTE_H */
