/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_VTD_H
#define ARM64_VTD_H

#include <types.h>
#include <ptdev.h>

struct acrn_vm;
struct iommu_domain;

#define INVALID_IRTE_ID		0xffffU
#define ARM_SMMU_STREAM_ID_INVALID	0xffffffffU
#define INVALID_DRHD_INDEX		0xffffffffU
#define DRHD_FLAG_INCLUDE_PCI_ALL_MASK	1U
#define DEVFUN(dev, fun)		((((dev) & 0x1FU) << 3U) | ((fun) & 0x7U))

enum acpi_dmar_scope_type {
	ACPI_DMAR_SCOPE_TYPE_ENDPOINT = 1U,
	ACPI_DMAR_SCOPE_TYPE_BRIDGE = 2U,
};

struct dmar_dev_scope {
	enum acpi_dmar_scope_type type;
	uint8_t id;
	uint8_t bus;
	uint8_t devfun;
};

struct dmar_drhd {
	uint32_t dev_cnt;
	uint16_t segment;
	uint8_t flags;
	bool ignore;
	uint64_t reg_base_addr;
	struct dmar_dev_scope *devices;
};

struct dmar_info {
	uint32_t drhd_count;
	struct dmar_drhd *drhd_units;
};

struct arm_smmu_hw_info {
	uint64_t base;
	uint64_t size;
	uint64_t strtab_base;
	uint64_t cmdq_base;
	uint64_t evtq_base;
	uint32_t idr0;
	uint32_t idr1;
	uint32_t idr5;
	uint32_t iidr;
	uint32_t aidr;
	uint32_t sid_bits;
	uint32_t oas_bits;
	uint32_t strtab_log2_entries;
	uint32_t cmdq_entries;
	uint32_t evtq_entries;
	uint32_t cmdq_prod;
	uint32_t cmdq_cons;
	uint32_t cmdq_last_cons;
	uint32_t cmdq_issued;
	uint32_t cmdq_syncs;
	uint32_t cmdq_errors;
	uint32_t cmdq_full;
	uint32_t cmdq_timeouts;
	uint32_t evtq_prod;
	uint32_t evtq_cons;
	uint32_t evtq_last_prod;
	uint32_t evtq_last_cons;
	uint32_t evtq_polled;
	uint32_t evtq_events;
	uint32_t evtq_errors;
	uint32_t evtq_overflow;
	uint32_t evtq_quarantined;
	uint64_t evtq_last_word0;
	uint64_t evtq_last_word1;
	uint64_t evtq_last_word2;
	uint64_t evtq_last_word3;
	uint32_t assign_ok;
	uint32_t assign_fail;
	uint32_t unassign_ok;
	uint32_t unassign_fail;
	int32_t init_status;
	int32_t cmdq_last_ret;
	bool discovered;
	bool probed;
	bool aborted;
	bool cmdq_enabled;
	bool evtq_enabled;
	bool ready;
};

struct arm_smmu_stream_config {
	uint32_t stream_id;
	uint16_t owner_vmid;
	uint32_t ipa_width;
	uint64_t root_table_hpa;
	bool assigned;
	bool quarantined;
	uint32_t fault_count;
	uint32_t last_fault_code;
	uint64_t last_fault_iova;
};

/*
 * ARM64 PCI passthrough is backed by SMMUv3 concepts:
 *
 *   PCI RID / platform StreamID
 *             |
 *             v
 *   SMMU stream table entry
 *             |
 *             v
 *   VM stage-2 translation table
 *
 * The stage-2 root is shared with the CPU-side guest page table. That is the
 * important security property: a DMA issued by a passed-through device must see
 * the same IPA->PA permissions that the VM's vCPUs see.
 */
struct iommu_domain *create_iommu_domain(uint16_t vm_id, uint64_t root_table_hpa,
	uint32_t addr_width);
void destroy_iommu_domain(struct iommu_domain *domain);
int32_t move_pt_device(struct iommu_domain *src, struct iommu_domain *dst,
	uint8_t bus, uint8_t devfun);
void arm_smmu_probe(uint64_t base, uint64_t size);
void arm_smmu_get_hw_info(struct arm_smmu_hw_info *info);
void arm_smmu_poll_events(void);
bool arm_smmu_assignment_ready(void);
uint32_t arm_smmu_get_stream_configs(struct arm_smmu_stream_config *configs,
	uint32_t max_configs);
bool arm_smmu_stream_assigned_to(uint32_t stream_id, uint16_t vm_id);
int32_t arm_smmu_assign_stream(struct iommu_domain *domain, uint32_t stream_id);
int32_t arm_smmu_unassign_stream(struct iommu_domain *domain, uint32_t stream_id);
bool arm_smmu_domain_valid(const struct iommu_domain *domain);
bool arm_smmu_ready(void);

bool is_pi_capable(const struct acrn_vm *vm);
int32_t ptirq_prepare_msi_remap(struct acrn_vm *vm, uint16_t virt_bdf,
	uint16_t phys_bdf, uint16_t entry_nr, struct msi_info *info,
	uint16_t irte_idx);
int32_t ptirq_prepare_msix_remap(struct acrn_vm *vm, uint16_t virt_bdf,
	uint16_t phys_bdf, uint16_t entry_nr, struct msi_info *info,
	uint16_t irte_idx);
void ptirq_remove_msix_remapping(const struct acrn_vm *vm, uint16_t phys_bdf,
	uint32_t vector_count);
void ptirq_remove_msi_remapping(const struct acrn_vm *vm, uint16_t phys_bdf,
	uint32_t vector_count);
int32_t ptirq_add_intx_remapping(struct acrn_vm *vm, uint32_t virt_gsi,
	uint32_t phys_gsi, bool pic_pin);
void ptirq_remove_intx_remapping(const struct acrn_vm *vm, uint32_t virt_gsi,
	bool pic_pin, bool phys);

#endif /* ARM64_VTD_H */
