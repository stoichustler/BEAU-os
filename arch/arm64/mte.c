/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <rtl.h>
#include <logmsg.h>
#include <mmu.h>
#include <asm/boot/ld_sym.h>
#include <asm/mte.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>
#include <asm/trap.h>

#define ARM64_MTE_MAX_PAGE_POOLS	2U
#define ARM64_MTE_STATE_ALLOCATED	(1U << 7U)
#define ARM64_MTE_STATE_TAG_MASK	0x0fU
#define ARM64_MTE_MAX_TAG		15U

struct arm64_mte_page_pool {
	const struct page_pool *owner;
	uint64_t start;
	uint64_t end;
	uint64_t page_count;
	uint8_t *tag_states;
	bool valid;
};

static struct arm64_mte_page_pool arm64_mte_page_pools[ARM64_MTE_MAX_PAGE_POOLS];
static spinlock_t arm64_mte_registry_lock;
static uint8_t arm64_mte_bsp_version;
static bool arm64_mte_enabled;
static bool arm64_mte_active;

/* [20260720] BEAU EL2 MTE page ownership
 *
 *   page-pool bitmap reserve
 *            |
 *            v
 *   set 16-byte allocation tags and clear the page
 *            |
 *            v
 *   release-publish tagged pointer state
 *            |
 *            v
 *   descriptor publication / software page-table walk
 *            |
 *            v
 *   unlink descriptor -> clear tags/data -> bitmap release
 *
 * Key rule:
 *   - the page-pool lock owns allocation and tag-state transitions;
 *   - physical descriptors never retain logical pointer tags;
 *   - tag state is published only after every granule has been updated;
 *   - unsupported CPUs never execute an MTE system register or instruction.
 *
 * This is an independent BEAU EL2 design informed by FreeBSD arm64's MTE2
 * capability gate and page-tag synchronization principles. It does not reuse
 * FreeBSD thread, pmap, vm_page, source-code, or comment structure.
 */

static uint8_t arm64_mte_version(void)
{
	uint64_t pfr1 = arm64_sysreg_read(s3_0_c0_c4_1);

	return (uint8_t)((pfr1 & ID_AA64PFR1_MTE_MASK) >>
		ID_AA64PFR1_MTE_SHIFT);
}

uint64_t arm64_mte_untag_address(uint64_t address)
{
	return address & ~ARM64_MTE_TOP_BYTE_MASK;
}

static uint64_t arm64_mte_tag_address(uint64_t address, uint8_t tag)
{
	return arm64_mte_untag_address(address) |
		((uint64_t)(tag & ARM64_MTE_STATE_TAG_MASK) << ARM64_MTE_TAG_SHIFT);
}

void arm64_mte_bootstrap_init(void)
{
	arm64_mte_bsp_version = arm64_mte_version();
	arm64_mte_enabled = arm64_mte_bsp_version >= ID_AA64PFR1_MTE_MTE2;
	arm64_mte_active = false;
	spinlock_init(&arm64_mte_registry_lock);

	if (arm64_mte_enabled) {
		LOG_INF("MTE:    FEAT_MTE%u detected, EL2 synchronous checking selected",
			arm64_mte_bsp_version);
	} else {
		LOG_INF("MTE:    Disabled, FEAT_MTE2 unavailable (version:%u)",
			arm64_mte_bsp_version);
	}
}

bool arm64_mte_prepare_pcpu(uint64_t *mair, uint64_t *tcr,
	uint64_t *sctlr)
{
	uint8_t local_version;

	if ((mair == NULL) || (tcr == NULL) || (sctlr == NULL)) {
		return false;
	}
	if (!arm64_mte_enabled) {
		return true;
	}

	local_version = arm64_mte_version();
	if (local_version < ID_AA64PFR1_MTE_MTE2) {
		LOG_ERR("MTE: pcpu capability mismatch bsp:%u local:%u",
			arm64_mte_bsp_version, local_version);
		return false;
	}

	*mair |= MAIR_EL2_NORMAL_TAGGED;
	*tcr |= TCR_EL2_TBI;
	*sctlr &= ~SCTLR_EL2_TCF_MASK;
	*sctlr |= SCTLR_EL2_ATA | SCTLR_EL2_TCF_SYNC;
	arm64_sysreg_write(s3_4_c5_c6_0, 0UL);
	return true;
}

void arm64_mte_mark_pcpu_enabled(void)
{
	if (arm64_mte_enabled) {
		arm64_isb();
		__atomic_store_n(&arm64_mte_active, true, __ATOMIC_RELEASE);
	}
}

bool arm64_mte_is_enabled(void)
{
	return arm64_mte_enabled;
}

static const struct arm64_mte_page_pool *arm64_mte_pool_by_owner(
	const struct page_pool *pool)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_MTE_MAX_PAGE_POOLS; idx++) {
		if (__atomic_load_n(&arm64_mte_page_pools[idx].valid,
			__ATOMIC_ACQUIRE) &&
			(arm64_mte_page_pools[idx].owner == pool)) {
			return &arm64_mte_page_pools[idx];
		}
	}

	return NULL;
}

static const struct arm64_mte_page_pool *arm64_mte_pool_by_address(
	uint64_t address)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_MTE_MAX_PAGE_POOLS; idx++) {
		if (__atomic_load_n(&arm64_mte_page_pools[idx].valid,
			__ATOMIC_ACQUIRE) &&
			(address >= arm64_mte_page_pools[idx].start) &&
			(address < arm64_mte_page_pools[idx].end)) {
			return &arm64_mte_page_pools[idx];
		}
	}

	return NULL;
}

int32_t arm64_mte_register_page_pool(const struct page_pool *pool,
	void *base, uint64_t page_count, uint8_t *tag_states)
{
	uint64_t start = (uint64_t)base;
	uint64_t end;
	uint64_t section_start = (uint64_t)&_mte_page_pool_start;
	uint64_t section_end = (uint64_t)&_mte_page_pool_end;
	uint32_t idx;
	int32_t status = -ENOMEM;

	if ((pool == NULL) || (base == NULL) || (tag_states == NULL) ||
		(page_count == 0UL) ||
		((start & (PAGE_SIZE - 1UL)) != 0UL) ||
		(page_count > ((UINT64_MAX - start) / PAGE_SIZE))) {
		return -EINVAL;
	}
	end = start + (page_count * PAGE_SIZE);
	if (arm64_mte_enabled &&
		((start < section_start) || (end > section_end))) {
		return -EINVAL;
	}

	spinlock_obtain(&arm64_mte_registry_lock);
	for (idx = 0U; idx < ARM64_MTE_MAX_PAGE_POOLS; idx++) {
		struct arm64_mte_page_pool *entry = &arm64_mte_page_pools[idx];

		if (entry->valid && (entry->owner == pool)) {
			status = ((entry->start == start) && (entry->end == end) &&
				(entry->tag_states == tag_states)) ? 0 : -EINVAL;
			goto out;
		}
		if (entry->valid && (start < entry->end) && (end > entry->start)) {
			status = -EINVAL;
			goto out;
		}
	}

	for (idx = 0U; idx < ARM64_MTE_MAX_PAGE_POOLS; idx++) {
		struct arm64_mte_page_pool *entry = &arm64_mte_page_pools[idx];

		if (!entry->valid) {
			(void)memset(tag_states, 0U, page_count);
			entry->owner = pool;
			entry->start = start;
			entry->end = end;
			entry->page_count = page_count;
			entry->tag_states = tag_states;
			__atomic_store_n(&entry->valid, true, __ATOMIC_RELEASE);
			status = 0;
			break;
		}
	}
out:
	spinlock_release(&arm64_mte_registry_lock);
	return status;
}

static bool arm64_mte_pool_page_index(const struct arm64_mte_page_pool *pool,
	uint64_t address, uint64_t *page_index)
{
	uint64_t raw = arm64_mte_untag_address(address);

	if ((pool == NULL) || (raw < pool->start) || (raw >= pool->end) ||
		((raw & (PAGE_SIZE - 1UL)) != 0UL) || (page_index == NULL)) {
		return false;
	}
	*page_index = (raw - pool->start) / PAGE_SIZE;
	return *page_index < pool->page_count;
}

static void arm64_mte_zero_page_with_tag(uint64_t address, uint8_t tag)
{
	uint64_t tagged = arm64_mte_tag_address(address, tag);
	uint64_t offset;

	for (offset = 0UL; offset < PAGE_SIZE; offset += ARM64_MTE_GRANULE_SIZE) {
		uint64_t granule = tagged + offset;

		asm volatile(
			".arch_extension memtag\n\t"
			"stzg %0, [%0]\n\t"
			".arch_extension nomemtag"
			: "+r" (granule) : : "memory");
	}
	arm64_dsb_ish();
}

void *arm64_mte_page_alloc(const struct page_pool *owner, void *page)
{
	const struct arm64_mte_page_pool *pool;
	uint64_t page_index;
	uint8_t state;
	uint8_t tag;
	uint64_t raw;

	if (!__atomic_load_n(&arm64_mte_active, __ATOMIC_ACQUIRE)) {
		return page;
	}
	pool = arm64_mte_pool_by_owner(owner);
	if (pool == NULL) {
		return page;
	}

	raw = arm64_mte_untag_address((uint64_t)page);
	if (!arm64_mte_pool_page_index(pool, raw, &page_index)) {
		return NULL;
	}
	state = __atomic_load_n(&pool->tag_states[page_index], __ATOMIC_ACQUIRE);
	if ((state & ARM64_MTE_STATE_ALLOCATED) != 0U) {
		return NULL;
	}
	tag = (uint8_t)(((state & ARM64_MTE_STATE_TAG_MASK) %
		ARM64_MTE_MAX_TAG) + 1U);
	arm64_mte_zero_page_with_tag(raw, tag);
	__atomic_store_n(&pool->tag_states[page_index],
		(uint8_t)(ARM64_MTE_STATE_ALLOCATED | tag), __ATOMIC_RELEASE);
	return (void *)arm64_mte_tag_address(raw, tag);
}

bool arm64_mte_page_free(const struct page_pool *owner, const void *page)
{
	const struct arm64_mte_page_pool *pool;
	uint64_t page_index;
	uint64_t address = (uint64_t)page;
	uint8_t pointer_tag;
	uint8_t state;

	if (!__atomic_load_n(&arm64_mte_active, __ATOMIC_ACQUIRE)) {
		return true;
	}
	pool = arm64_mte_pool_by_owner(owner);
	if (pool == NULL) {
		return true;
	}

	if (!arm64_mte_pool_page_index(pool, address, &page_index)) {
		return false;
	}
	pointer_tag = (uint8_t)((address & ARM64_MTE_TAG_MASK) >>
		ARM64_MTE_TAG_SHIFT);
	state = __atomic_load_n(&pool->tag_states[page_index], __ATOMIC_ACQUIRE);
	if (((state & ARM64_MTE_STATE_ALLOCATED) == 0U) ||
		((state & ARM64_MTE_STATE_TAG_MASK) != pointer_tag)) {
		return false;
	}

	arm64_mte_zero_page_with_tag(address, 0U);
	__atomic_store_n(&pool->tag_states[page_index],
		(uint8_t)(state & ARM64_MTE_STATE_TAG_MASK), __ATOMIC_RELEASE);
	return true;
}

void *arm64_mte_hpa_to_hva(uint64_t hpa)
{
	const struct arm64_mte_page_pool *pool;
	uint64_t page_index;
	uint8_t state;

	if (!__atomic_load_n(&arm64_mte_active, __ATOMIC_ACQUIRE)) {
		return (void *)hpa;
	}
	pool = arm64_mte_pool_by_address(hpa);
	if (pool == NULL) {
		return (void *)hpa;
	}

	page_index = (hpa - pool->start) / PAGE_SIZE;
	state = __atomic_load_n(&pool->tag_states[page_index], __ATOMIC_ACQUIRE);
	if ((state & ARM64_MTE_STATE_ALLOCATED) == 0U) {
		return (void *)hpa;
	}
	return (void *)arm64_mte_tag_address(hpa,
		state & ARM64_MTE_STATE_TAG_MASK);
}

uint64_t arm64_mte_hva_to_hpa(const void *hva)
{
	return arm64_mte_untag_address((uint64_t)hva);
}

bool arm64_mte_is_sync_fault(uint64_t esr)
{
	return (ESR_EL2_EC(esr) == ESR_EL2_EC_DABT_CUR) &&
		((esr & ESR_ABORT_FSC_MASK) == ESR_ABORT_FSC_TAG_CHECK);
}

void arm64_mte_report_sync_fault(uint64_t esr, uint64_t far,
	uint64_t elr, uint16_t pcpu_id)
{
	uint8_t logical_tag = (uint8_t)((far & ARM64_MTE_TAG_MASK) >>
		ARM64_MTE_TAG_SHIFT);

	LOG_ERR("MTE: synchronous tag check fault pcpu:%hu elr:0x%lx far:0x%lx tag:%u esr:0x%lx",
		pcpu_id, elr, far, logical_tag, esr);
}
