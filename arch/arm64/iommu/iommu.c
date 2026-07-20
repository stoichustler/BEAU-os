/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <logmsg.h>
#include <spinlock.h>
#include <vconfig.h>
#include <asm/page.h>
#include <asm/vtd.h>
#include "smmu.h"

#define ARM64_IOMMU_MAX_DOMAINS	CONFIG_MAX_VM_NUM
#define ARM64_IOMMU_PCI_STREAM(bus, devfun) \
	((((uint32_t)(bus)) << 8U) | (uint32_t)(devfun))

struct iommu_domain {
	struct arm_smmu_s2_config s2;
	bool used;
	bool hw_bound;
};

static spinlock_t iommu_lock = { .head = 0U, .tail = 0U };
static struct iommu_domain iommu_domains[ARM64_IOMMU_MAX_DOMAINS];

/* [20260716] ARM64 IOMMU broker boundary
 *
 *   VM/vPCI policy
 *       -> iommu_domain lifetime and immutable S2 configuration
 *       -> structured attach/detach request
 *       -> smmu.c physical STE/CMDQ transaction
 *
 * Key rule:
 *   - this file owns domain allocation and lifetime;
 *   - smmu.c receives value snapshots and never stores a domain pointer;
 *   - iommu_lock serializes domain destruction against stream transactions;
 *   - domain memory is released only after physical ABORT synchronization.
 */
static bool iommu_domain_valid_locked(const struct iommu_domain *domain)
{
	uint64_t address = (uint64_t)domain;
	uint64_t start = (uint64_t)&iommu_domains[0];
	uint64_t end = (uint64_t)&iommu_domains[ARM64_IOMMU_MAX_DOMAINS];
	bool in_pool;

	in_pool = (domain != NULL) && (address >= start) && (address < end) &&
		(((address - start) % sizeof(struct iommu_domain)) == 0UL);

	return in_pool && domain->used;
}

bool arm_smmu_domain_valid(const struct iommu_domain *domain)
{
	uint64_t flags;
	bool valid;

	spinlock_irqsave_obtain(&iommu_lock, &flags);
	valid = iommu_domain_valid_locked(domain);
	spinlock_irqrestore_release(&iommu_lock, flags);

	return valid;
}

int32_t arm_smmu_sync_vm_stage2(uint16_t vm_id, uint64_t root_table_hpa)
{
	struct iommu_domain *domain;
	uint64_t flags;
	int32_t ret = 0;

	if ((vm_id >= ARM64_IOMMU_MAX_DOMAINS) || (root_table_hpa == 0UL) ||
		((root_table_hpa & (PAGE_SIZE - 1UL)) != 0UL)) {
		return -EINVAL;
	}

	/* [20260720] CPU and DMA Stage-2 synchronization
	 *
	 *   VM page-table update
	 *       -> validate immutable domain root under iommu_lock
	 *       -> no bound stream: no DMA translation can be stale
	 *       -> bound stream: SMMU VMID invalidation + CMD_SYNC
	 *
	 * Key rule:
	 *   - iommu_lock serializes this transaction with attach and detach;
	 *   - the domain root must still match the CPU Stage-2 root;
	 *   - a bound domain is synchronized before detached table pages are reused.
	 */
	spinlock_irqsave_obtain(&iommu_lock, &flags);
	domain = &iommu_domains[vm_id];
	if (!domain->used) {
		ret = 0;
	} else if ((domain->s2.owner_vmid != vm_id) ||
		(domain->s2.root_table_hpa != root_table_hpa)) {
		ret = -EPERM;
	} else if (domain->hw_bound) {
		ret = arm_smmu_hw_sync_s2(&domain->s2);
	}
	spinlock_irqrestore_release(&iommu_lock, flags);

	return ret;
}

struct iommu_domain *create_iommu_domain(uint16_t vm_id, uint64_t root_table_hpa,
	uint32_t addr_width)
{
	struct arm_smmu_s2_config config;
	struct iommu_domain *domain = NULL;
	uint64_t flags;

	if (vm_id >= ARM64_IOMMU_MAX_DOMAINS) {
		return NULL;
	}
	config.root_table_hpa = root_table_hpa;
	config.ipa_width = addr_width;
	config.owner_vmid = vm_id;
	config.hw_vmid = (uint16_t)(vm_id + 1U);
	if (arm_smmu_hw_prepare_s2(&config) != 0) {
		return NULL;
	}

	spinlock_irqsave_obtain(&iommu_lock, &flags);
	domain = &iommu_domains[vm_id];
	if (!domain->used) {
		domain->s2 = config;
		domain->used = true;
		domain->hw_bound = false;
	} else if ((domain->s2.root_table_hpa != config.root_table_hpa) ||
		(domain->s2.ipa_width != config.ipa_width) ||
		(domain->s2.hw_vmid != config.hw_vmid)) {
		domain = NULL;
	}
	spinlock_irqrestore_release(&iommu_lock, flags);

	return domain;
}

void destroy_iommu_domain(struct iommu_domain *domain)
{
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&iommu_lock, &flags);
	if (!iommu_domain_valid_locked(domain)) {
		spinlock_irqrestore_release(&iommu_lock, flags);
		return;
	}
	ret = arm_smmu_hw_detach_domain(&domain->s2);
	if (ret == 0) {
		(void)memset(domain, 0U, sizeof(*domain));
	} else {
		LOG_ERR("IOMMU: keep vm%u domain after physical detach failure: %d",
			domain->s2.owner_vmid, ret);
	}
	spinlock_irqrestore_release(&iommu_lock, flags);
}

int32_t arm_smmu_assign_stream(struct iommu_domain *domain, uint32_t stream_id)
{
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&iommu_lock, &flags);
	if (!iommu_domain_valid_locked(domain)) {
		ret = -EINVAL;
	} else {
		ret = arm_smmu_hw_attach_stream(&domain->s2, stream_id);
		if (ret == 0) {
			domain->hw_bound = true;
		}
	}
	spinlock_irqrestore_release(&iommu_lock, flags);

	return ret;
}

int32_t arm_smmu_unassign_stream(struct iommu_domain *domain, uint32_t stream_id)
{
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&iommu_lock, &flags);
	if (!iommu_domain_valid_locked(domain)) {
		ret = -EINVAL;
	} else {
		ret = arm_smmu_hw_detach_stream(&domain->s2, stream_id);
		if (ret == 0) {
			domain->hw_bound = arm_smmu_hw_domain_bound(&domain->s2);
		}
	}
	spinlock_irqrestore_release(&iommu_lock, flags);

	return ret;
}

int32_t move_pt_device(struct iommu_domain *src, struct iommu_domain *dst,
	uint8_t bus, uint8_t devfun)
{
	uint32_t stream_id = ARM64_IOMMU_PCI_STREAM(bus, devfun);
	int32_t ret = 0;

	if (src != NULL) {
		ret = arm_smmu_unassign_stream(src, stream_id);
	}
	if ((ret == 0) && (dst != NULL)) {
		ret = arm_smmu_assign_stream(dst, stream_id);
	}

	return ret;
}
