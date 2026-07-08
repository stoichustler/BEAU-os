/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SMMUV3_H
#define ARM64_SMMUV3_H

#include <types.h>

struct acrn_vm;
struct iommu_domain;

/*
 * StreamID is the SMMU-side identity of a DMA master. For PCIe it is normally
 * derived from requester ID, while platform devices get it from firmware
 * description. Keep the width conservative here: SMMUv3 commonly implements a
 * subset of the architected 32-bit space and QEMU/static bring-up does not yet
 * describe real stream tables.
 */
#define ARM_SMMU_MAX_SW_STREAMS		64U

struct arm_smmu_stream_config {
	uint32_t stream_id;
	uint16_t owner_vmid;
	bool assigned;
};

struct iommu_domain *arm_smmu_create_domain(uint16_t vm_id,
	uint64_t root_table_hpa, uint32_t ipa_width);
void arm_smmu_destroy_domain(struct iommu_domain *domain);
int32_t arm_smmu_move_pci_device(struct iommu_domain *src,
	struct iommu_domain *dst, uint8_t bus, uint8_t devfun);
int32_t arm_smmu_assign_stream(struct iommu_domain *domain, uint32_t stream_id);
int32_t arm_smmu_unassign_stream(struct iommu_domain *domain, uint32_t stream_id);
bool arm_smmu_domain_valid(const struct iommu_domain *domain);
bool arm_smmu_ready(void);

#endif /* ARM64_SMMUV3_H */
