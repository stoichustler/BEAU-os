/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SMMU_H
#define ARM64_SMMU_H

#include <types.h>

struct acrn_vm;

/*
 * StreamID is the SMMU-side identity of a DMA master. For PCIe it is normally
 * derived from requester ID, while platform devices get it from firmware
 * description. Keep the software tracking pool bounded independently from the
 * architected StreamID width; the physical table is sized from IDR1.SIDSIZE.
 */
#define ARM_SMMU_MAX_SW_STREAMS		64U

struct arm_smmu_s2_config {
	uint64_t root_table_hpa;
	uint32_t ipa_width;
	uint16_t owner_vmid;
	uint16_t hw_vmid;
};

int32_t arm_smmu_hw_prepare_s2(struct arm_smmu_s2_config *config);
int32_t arm_smmu_hw_attach_stream(const struct arm_smmu_s2_config *config,
	uint32_t stream_id);
int32_t arm_smmu_hw_detach_stream(const struct arm_smmu_s2_config *config,
	uint32_t stream_id);
int32_t arm_smmu_hw_detach_domain(const struct arm_smmu_s2_config *config);
bool arm_smmu_hw_domain_bound(const struct arm_smmu_s2_config *config);
bool arm_smmu_ready(void);

#endif /* ARM64_SMMU_H */
