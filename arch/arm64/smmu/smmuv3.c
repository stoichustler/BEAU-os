/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <delay.h>
#include <io.h>
#include <irq.h>
#include <logmsg.h>
#include <pgtable.h>
#include <spinlock.h>
#include <util.h>
#include <vm.h>
#include <acrn_hv_defs.h>
#include <asm/mmu.h>
#include <asm/pgtable.h>
#include <asm/vtd.h>
#include <asm/irq.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vgicv3.h>
#include "smmuv3.h"

/* [20260712] SMMUv3 reference and implementation scope
 *
 * Reference document used for terminology:
 *   Arm System Memory Management Unit Architecture Specification,
 *   document IHI 0070C.a, covering SMMU architecture versions 3.0, 3.1
 *   and 3.2. Issue C.a is an amendments/clarifications issue; it does not
 *   imply that this driver implements every optional 3.2 feature.
 *
 * Implemented framework in this file:
 *
 *             PCIe endpoint / platform master
 *                         |
 *                         | StreamID / Requester ID
 *                         v
 *              +-----------------------+
 *              | SMMUv3 stream table   |
 *              | SID -> STE            |
 *              +----+------------+-----+
 *                   |            |
 *                   |            +--> ABORT: default for unassigned streams
 *                   |
 *                   +--> S2_TRANS: use VM stage-2 root
 *                         |
 *                         v
 *                    IPA/IOVA -> PA
 *                         |
 *                         v
 *                    guest-owned memory
 *
 * Control-plane flow:
 *
 *   arm_smmu_probe()
 *       -> read ID registers
 *       -> install zero/abort stream table
 *       -> enable CMDQ + SMMU
 *
 *   arm_smmu_assign_stream(domain, sid)
 *       -> build STE from VM root stage-2 table
 *       -> clean STE to PoC
 *       -> CFGI_STE + CMD_SYNC through CMDQ
 *       -> software owner becomes VM
 *
 *   arm_smmu_unassign_stream(domain, sid)
 *       -> replace STE with ABORT
 *       -> CFGI_STE + CMD_SYNC
 *       -> software owner returns to host/free
 *
 * Out of scope for this minimal EL2 passthrough path:
 *   stage-1 context descriptors, two-level stream tables, ATS/PRI, nested
 *   translation, and event queue decoding. EVTQ storage is allocated for the
 *   architectural shape, but this driver intentionally keeps EVTQ disabled
 *   until the platform exposes the required register window and IRQ policy.
 */

#define ARM_SMMU_MAX_DOMAINS	CONFIG_MAX_VM_NUM
#define ARM_SMMU_PCI_STREAM(bus, devfun)	((((uint32_t)(bus)) << 8U) | (uint32_t)(devfun))
#define ARM_SMMU_PCI_MSI_COMPAT_STREAM	0U
#define ARM64_PTDEV_MSI_COMPAT_DEVID	0U
#define ARM64_PTDEV_MSI_MAX_ALIASES	1U
#define ARM64_PTDEV_MSI_DOORBELL_IOVA_BASE	0x0ff00000UL
#define ARM64_PTDEV_MSI_DOORBELL_IOVA(vm_id) \
	(ARM64_PTDEV_MSI_DOORBELL_IOVA_BASE + ((uint64_t)(vm_id) * PAGE_SIZE))
#define ARM_SMMU_IDR0_ST_LVL_SHIFT	27U
#define ARM_SMMU_IDR0_ST_LVL_MASK	(3U << ARM_SMMU_IDR0_ST_LVL_SHIFT)
#define ARM_SMMU_IDR0_ST_LVL_2LVL	1U
#define ARM_SMMU_IDR0_S2P		(1U << 0U)
#define ARM_SMMU_IDR1_CMDQS_SHIFT	21U
#define ARM_SMMU_IDR1_CMDQS_MASK	(0x1fU << ARM_SMMU_IDR1_CMDQS_SHIFT)
#define ARM_SMMU_IDR1_EVTQS_SHIFT	16U
#define ARM_SMMU_IDR1_EVTQS_MASK	(0x1fU << ARM_SMMU_IDR1_EVTQS_SHIFT)
#define ARM_SMMU_IDR1_SIDSIZE_MASK	0x3fU
#define ARM_SMMU_IDR5_OAS_MASK		0x7U
#define ARM_SMMU_IDR5_OAS_32_BIT	0U
#define ARM_SMMU_IDR5_OAS_36_BIT	1U
#define ARM_SMMU_IDR5_OAS_40_BIT	2U
#define ARM_SMMU_IDR5_OAS_42_BIT	3U
#define ARM_SMMU_IDR5_OAS_44_BIT	4U
#define ARM_SMMU_IDR5_OAS_48_BIT	5U
#define ARM_SMMU_IDR0			0x0000U
#define ARM_SMMU_IDR1			0x0004U
#define ARM_SMMU_IDR5			0x0014U
#define ARM_SMMU_IIDR			0x0018U
#define ARM_SMMU_AIDR			0x001cU
#define ARM_SMMU_CR0			0x0020U
#define ARM_SMMU_CR0ACK		0x0024U
#define ARM_SMMU_CR0_SMMUEN		(1U << 0U)
#define ARM_SMMU_CR0_EVTQEN		(1U << 2U)
#define ARM_SMMU_CR0_CMDQEN		(1U << 3U)
#define ARM_SMMU_GBPA			0x0044U
#define ARM_SMMU_GBPA_UPDATE		(1U << 31U)
#define ARM_SMMU_GBPA_ABORT		(1U << 20U)
#define ARM_SMMU_IRQ_CTRL		0x0050U
#define ARM_SMMU_IRQ_CTRLACK		0x0054U
#define ARM_SMMU_STRTAB_BASE		0x0080U
#define ARM_SMMU_STRTAB_BASE_RA		(1UL << 62U)
#define ARM_SMMU_STRTAB_BASE_ADDR_MASK	0x000ffffffffffffc0UL
#define ARM_SMMU_STRTAB_BASE_CFG	0x0088U
#define ARM_SMMU_STRTAB_BASE_CFG_FMT_LINEAR	0U
#define ARM_SMMU_Q_BASE_RWA		(1UL << 62U)
#define ARM_SMMU_Q_BASE_ADDR_MASK	0x000ffffffffffffe0UL
#define ARM_SMMU_CMDQ_BASE		0x0090U
#define ARM_SMMU_CMDQ_PROD		0x0098U
#define ARM_SMMU_CMDQ_CONS		0x009cU
#define ARM_SMMU_EVTQ_BASE		0x00a0U
#define ARM_SMMU_EVTQ_PROD		0x00a8U
#define ARM_SMMU_EVTQ_CONS		0x00acU
#define ARM_SMMU_QUEUE_LOG2_ENTRIES	4U
#define ARM_SMMU_QUEUE_ENTRIES		(1U << ARM_SMMU_QUEUE_LOG2_ENTRIES)
#define ARM_SMMU_CMD_DWORDS		2U
#define ARM_SMMU_EVT_DWORDS		4U
#define ARM_SMMU_STE_DWORDS		8U
#define ARM_SMMU_STE_SIZE		(ARM_SMMU_STE_DWORDS * sizeof(uint64_t))
#define ARM_SMMU_STRTAB_LOG2_MIN	0U
#define ARM_SMMU_STRTAB_LOG2_MAX	8U
#define ARM_SMMU_POLL_RETRIES		1000U
#define ARM_SMMU_POLL_DELAY_US		1U
#define ARM_SMMU_INIT_UNDISCOVERED	(-ENODEV)
#define ARM_SMMU_Q_PTR_MASK		((ARM_SMMU_QUEUE_ENTRIES << 1U) - 1U)
#define ARM_SMMU_Q_ERR_MASK		(0x7fU << 24U)
#define ARM_SMMU_STE_0_V		(1UL << 0U)
#define ARM_SMMU_STE_0_CFG_SHIFT	1U
#define ARM_SMMU_STE_0_CFG_ABORT	0UL
#define ARM_SMMU_STE_0_CFG_BYPASS	4UL
#define ARM_SMMU_STE_0_CFG_S2_TRANS	6UL
#define ARM_SMMU_STE_1_SHCFG_SHIFT	44U
#define ARM_SMMU_STE_1_SHCFG_INCOMING	1UL
#define ARM_SMMU_STE_2_VTCR_SHIFT	32U
#define ARM_SMMU_STE_2_S2AA64		(1UL << 51U)
#define ARM_SMMU_STE_2_S2PTW		(1UL << 54U)
#define ARM_SMMU_STE_2_S2R		(1UL << 58U)
#define ARM_SMMU_STE_3_S2TTB_MASK	0x000ffffffffffff0UL
#define ARM_SMMU_VTCR_S2T0SZ_SHIFT	0U
#define ARM_SMMU_VTCR_S2SL0_SHIFT	6U
#define ARM_SMMU_VTCR_S2IR0_SHIFT	8U
#define ARM_SMMU_VTCR_S2OR0_SHIFT	10U
#define ARM_SMMU_VTCR_S2SH0_SHIFT	12U
#define ARM_SMMU_VTCR_S2TG_SHIFT	14U
#define ARM_SMMU_VTCR_S2PS_SHIFT	16U
#define ARM_SMMU_CMDQ_OP_CFGI_STE	0x03UL
#define ARM_SMMU_CMDQ_OP_TLBI_S2_IPA	0x2aUL
#define ARM_SMMU_CMDQ_OP_CMD_SYNC	0x46UL
#define ARM_SMMU_CMDQ_0_OP_SHIFT	0U
#define ARM_SMMU_CMDQ_CFGI_0_SID_SHIFT	32U
#define ARM_SMMU_CMDQ_TLBI_0_VMID_SHIFT	32U
#define ARM_SMMU_CMDQ_TLBI_1_IPA_MASK	0x000fffffffff000UL
#define ARM_SMMU_CMDQ_CFGI_1_LEAF	(1UL << 0U)
#define ARM_SMMU_CMDQ_SYNC_0_CS_SHIFT	12U
#define ARM_SMMU_CMDQ_SYNC_0_CS_SEV	2UL
#define ARM_SMMU_CMDQ_SYNC_0_MSH_SHIFT	22U
#define ARM_SMMU_CMDQ_SYNC_0_MSH_ISH	3UL
#define ARM_SMMU_CMDQ_SYNC_0_ATTR_SHIFT	24U
#define ARM_SMMU_CMDQ_SYNC_0_ATTR_OIWB	0xfUL

/* [20260712] BEAU ARM64 passthrough model, first stage.
 *
 * SMMUv3 driver uses the VM P2M/stage-2 table as the SMMU stage-2 table.
 * This file follows that model at the framework boundary:
 *
 *   VM vCPU access:  IPA ---- CPU stage-2 ----> PA
 *   Device DMA:      IPA ---- SMMU stage-2 ---> PA
 *
 * If the two paths diverge, a device may DMA into memory the guest CPU cannot
 * reach, or miss memory the guest owns. Therefore an IOMMU domain stores the
 * VM's stage-2 root HPA and never creates an independent DMA map here.
 *
 * Hardware command queue programming is required for stream assignment. A
 * passthrough stream is admitted only after its STE has been written with the
 * VM stage-2 root and synchronized through CMDQ.
 */
struct iommu_domain {
	uint16_t vm_id;
	uint64_t root_table_hpa;
	uint32_t ipa_width;
	bool used;
	bool hw_bound;
};

struct arm_smmu_stream_state {
	uint32_t stream_id;
	struct iommu_domain *domain;
	bool used;
};

struct arm64_pt_msi_state {
	bool used;
	uint16_t phys_bdf;
	uint16_t entry_nr;
	uint32_t dev_id;
	uint32_t event_id;
	uint32_t lpi;
	uint32_t alias_dev_ids[ARM64_PTDEV_MSI_MAX_ALIASES];
	uint32_t alias_count;
};

static spinlock_t arm_smmu_lock = { .head = 0U, .tail = 0U };
static spinlock_t arm64_pt_msi_lock = { .head = 0U, .tail = 0U };
static struct iommu_domain arm_smmu_domains[ARM_SMMU_MAX_DOMAINS];
static struct arm_smmu_stream_state arm_smmu_streams[ARM_SMMU_MAX_SW_STREAMS];
static struct arm64_pt_msi_state arm64_pt_msi_states[CONFIG_MAX_PT_IRQ_ENTRIES];
static bool arm64_pt_msi_doorbell_mapped[CONFIG_MAX_VM_NUM];
static uint64_t arm64_pt_msi_doorbell_hpa[CONFIG_MAX_VM_NUM];
static struct arm_smmu_hw_info arm_smmu_hw;
static bool arm_smmu_hw_ready;
static bool arm_smmu_assignment_hw_ready;
static uint64_t arm_smmu_strtab[1U << ARM_SMMU_STRTAB_LOG2_MAX][ARM_SMMU_STE_DWORDS]
	__aligned(PAGE_SIZE);
static uint64_t arm_smmu_cmdq[ARM_SMMU_QUEUE_ENTRIES][ARM_SMMU_CMD_DWORDS]
	__aligned(PAGE_SIZE);
static uint64_t arm_smmu_evtq[ARM_SMMU_QUEUE_ENTRIES][ARM_SMMU_EVT_DWORDS]
	__aligned(PAGE_SIZE);
static uint32_t arm_smmu_cmdq_prod;

static inline void *arm_smmu_reg(uint64_t base, uint32_t off)
{
	return (void *)(base + off);
}

static uint32_t arm_smmu_min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static uint32_t arm_smmu_cmdq_next(uint32_t prod)
{
	return (prod + 1U) & ARM_SMMU_Q_PTR_MASK;
}

static uint32_t arm_smmu_cmdq_index(uint32_t prod)
{
	return prod & (ARM_SMMU_QUEUE_ENTRIES - 1U);
}

static bool arm_smmu_stream_in_strtab(uint32_t stream_id)
{
	return arm_smmu_hw.ready &&
		(stream_id < (1U << arm_smmu_hw.strtab_log2_entries));
}

static uint16_t arm_smmu_domain_vmid(const struct iommu_domain *domain)
{
	return (uint16_t)(domain->vm_id + 1U);
}

static uint16_t arm_smmu_vm_vmid(uint16_t vm_id)
{
	return (uint16_t)(vm_id + 1U);
}

static bool arm_smmu_s2_supported_locked(void)
{
	return (arm_smmu_hw.ready && ((arm_smmu_hw.idr0 & ARM_SMMU_IDR0_S2P) != 0U));
}

static bool arm_smmu_s2_supported(void)
{
	bool supported;
	uint64_t flags;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	supported = arm_smmu_s2_supported_locked();
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return supported;
}

static uint64_t arm_smmu_ste_vtcr(uint32_t ipa_width)
{
	uint64_t vtcr = 0UL;
	uint64_t t0sz = 64UL - (uint64_t)ipa_width;

	/*
	 * The STE carries a stage-2 translation control field for device DMA.
	 * It is the SMMU-side description of the same IPA space that the CPU
	 * sees through the VM stage-2 table: input address size, starting level,
	 * cacheability, shareability, granule size, and output PA size must match
	 * the stage-2 tables rooted at S2TTB.
	 */
	vtcr |= t0sz << ARM_SMMU_VTCR_S2T0SZ_SHIFT;
	vtcr |= (VTCR_SL0_LVL0 >> 6U) << ARM_SMMU_VTCR_S2SL0_SHIFT;
	vtcr |= (VTCR_IRGN0_WBWA >> 8U) << ARM_SMMU_VTCR_S2IR0_SHIFT;
	vtcr |= (VTCR_ORGN0_WBWA >> 10U) << ARM_SMMU_VTCR_S2OR0_SHIFT;
	vtcr |= (VTCR_SH0_INNER >> 12U) << ARM_SMMU_VTCR_S2SH0_SHIFT;
	vtcr |= (VTCR_TG0_4K >> 14U) << ARM_SMMU_VTCR_S2TG_SHIFT;
	vtcr |= (VTCR_PS_48BIT >> 16U) << ARM_SMMU_VTCR_S2PS_SHIFT;

	return vtcr;
}

static int32_t arm_smmu_cmdq_issue_locked(uint64_t cmd0, uint64_t cmd1)
{
	uint32_t prod = arm_smmu_cmdq_prod;
	uint32_t next = arm_smmu_cmdq_next(prod);
	uint32_t cons = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_CONS));
	uint32_t idx;

	if ((cons & ARM_SMMU_Q_ERR_MASK) != 0U) {
		LOG_ERR("SMMUv3: CMDQ error cons=0x%x", cons);
		return -EIO;
	}
	if (next == (cons & ARM_SMMU_Q_PTR_MASK)) {
		return -EBUSY;
	}

	idx = arm_smmu_cmdq_index(prod);
	arm_smmu_cmdq[idx][0] = cmd0;
	arm_smmu_cmdq[idx][1] = cmd1;
	/*
	 * CMDQ entries live in normal cacheable memory owned by EL2. Clean the
	 * entry before ringing PROD so the SMMU command fetch observes the final
	 * command words rather than a stale cache line.
	 */
	flush_cache_range(arm_smmu_cmdq[idx], sizeof(arm_smmu_cmdq[idx]));

	arm_smmu_cmdq_prod = next;
	mmio_write32(next, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_PROD));

	return 0;
}

static int32_t arm_smmu_cmdq_wait_locked(uint32_t target)
{
	uint32_t retry;

	for (retry = 0U; retry < ARM_SMMU_POLL_RETRIES; retry++) {
		uint32_t cons = mmio_read32(arm_smmu_reg(arm_smmu_hw.base,
			ARM_SMMU_CMDQ_CONS));

		if ((cons & ARM_SMMU_Q_ERR_MASK) != 0U) {
			LOG_ERR("SMMUv3: CMDQ error cons=0x%x", cons);
			return -EIO;
		}
		if ((cons & ARM_SMMU_Q_PTR_MASK) == target) {
			return 0;
		}
		udelay(ARM_SMMU_POLL_DELAY_US);
	}

	return -ETIMEDOUT;
}

static int32_t arm_smmu_cmdq_sync_locked(void)
{
	uint64_t cmd0 = 0UL;
	int32_t ret;

	/*
	 * CMD_SYNC is the ordering point for prior command queue operations.
	 * Use it after STE invalidation and TLB invalidation so subsequent code
	 * does not publish software ownership before hardware has consumed the
	 * required SMMU-side state changes.
	 */
	cmd0 |= ARM_SMMU_CMDQ_OP_CMD_SYNC << ARM_SMMU_CMDQ_0_OP_SHIFT;
	cmd0 |= ARM_SMMU_CMDQ_SYNC_0_CS_SEV << ARM_SMMU_CMDQ_SYNC_0_CS_SHIFT;
	cmd0 |= ARM_SMMU_CMDQ_SYNC_0_MSH_ISH << ARM_SMMU_CMDQ_SYNC_0_MSH_SHIFT;
	cmd0 |= ARM_SMMU_CMDQ_SYNC_0_ATTR_OIWB << ARM_SMMU_CMDQ_SYNC_0_ATTR_SHIFT;

	ret = arm_smmu_cmdq_issue_locked(cmd0, 0UL);
	if (ret == 0) {
		ret = arm_smmu_cmdq_wait_locked(arm_smmu_cmdq_prod);
	}

	return ret;
}

static int32_t arm_smmu_sync_ste_locked(uint32_t stream_id)
{
	uint64_t cmd0 = 0UL;
	uint64_t cmd1 = ARM_SMMU_CMDQ_CFGI_1_LEAF;
	int32_t ret;

	cmd0 |= ARM_SMMU_CMDQ_OP_CFGI_STE << ARM_SMMU_CMDQ_0_OP_SHIFT;
	cmd0 |= (uint64_t)stream_id << ARM_SMMU_CMDQ_CFGI_0_SID_SHIFT;

	ret = arm_smmu_cmdq_issue_locked(cmd0, cmd1);
	if (ret == 0) {
		ret = arm_smmu_cmdq_sync_locked();
	}

	return ret;
}

static int32_t arm_smmu_tlbi_ipa_locked(uint16_t vm_id, uint64_t iova)
{
	uint64_t cmd0 = 0UL;
	uint64_t cmd1;
	int32_t ret;

	if (!arm_smmu_assignment_hw_ready) {
		return -ENODEV;
	}

	cmd0 |= ARM_SMMU_CMDQ_OP_TLBI_S2_IPA << ARM_SMMU_CMDQ_0_OP_SHIFT;
	cmd0 |= (uint64_t)arm_smmu_vm_vmid(vm_id) << ARM_SMMU_CMDQ_TLBI_0_VMID_SHIFT;
	cmd1 = iova & ARM_SMMU_CMDQ_TLBI_1_IPA_MASK;

	ret = arm_smmu_cmdq_issue_locked(cmd0, cmd1);
	if (ret == 0) {
		ret = arm_smmu_cmdq_sync_locked();
	}

	return ret;
}

static int32_t arm_smmu_flush_vm_iotlb(uint16_t vm_id, uint64_t iova)
{
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	ret = arm_smmu_tlbi_ipa_locked(vm_id, iova);
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

static void arm_smmu_write_abort_ste_locked(uint32_t stream_id)
{
	uint64_t *ste = arm_smmu_strtab[stream_id];

	/*
	 * ABORT is the safe default STE state. A valid abort STE makes the SMMU
	 * terminate transactions for this StreamID instead of letting unknown
	 * DMA bypass translation.
	 */
	ste[0] = ARM_SMMU_STE_0_V |
		(ARM_SMMU_STE_0_CFG_ABORT << ARM_SMMU_STE_0_CFG_SHIFT);
	ste[1] = ARM_SMMU_STE_1_SHCFG_INCOMING << ARM_SMMU_STE_1_SHCFG_SHIFT;
	ste[2] = 0UL;
	ste[3] = 0UL;
	ste[4] = 0UL;
	ste[5] = 0UL;
	ste[6] = 0UL;
	ste[7] = 0UL;
	flush_cache_range(ste, ARM_SMMU_STE_SIZE);
}

static int32_t arm_smmu_write_bypass_ste_locked(uint32_t stream_id)
{
	uint64_t *ste = arm_smmu_strtab[stream_id];

	ste[0] = ARM_SMMU_STE_0_V |
		(ARM_SMMU_STE_0_CFG_BYPASS << ARM_SMMU_STE_0_CFG_SHIFT);
	ste[1] = ARM_SMMU_STE_1_SHCFG_INCOMING << ARM_SMMU_STE_1_SHCFG_SHIFT;
	ste[2] = 0UL;
	ste[3] = 0UL;
	ste[4] = 0UL;
	ste[5] = 0UL;
	ste[6] = 0UL;
	ste[7] = 0UL;
	flush_cache_range(ste, ARM_SMMU_STE_SIZE);

	return arm_smmu_sync_ste_locked(stream_id);
}

static int32_t arm_smmu_write_s2_ste_locked(const struct iommu_domain *domain,
	uint32_t stream_id)
{
	uint64_t *ste = arm_smmu_strtab[stream_id];
	uint64_t vtcr = arm_smmu_ste_vtcr(domain->ipa_width);
	int32_t ret;

	/*
	 * Publish a stage-2 STE in two phases:
	 *
	 *   1. write non-word0 translation state: VMID, VTCR, S2TTB;
	 *   2. clean/sync it, then set word0 valid + S2_TRANS and sync again.
	 *
	 * This prevents hardware table walks from observing a valid STE whose
	 * stage-2 root or control fields are still stale.
	 */
	ste[1] = ARM_SMMU_STE_1_SHCFG_INCOMING << ARM_SMMU_STE_1_SHCFG_SHIFT;
	ste[2] = (uint64_t)arm_smmu_domain_vmid(domain) |
		(vtcr << ARM_SMMU_STE_2_VTCR_SHIFT) |
		ARM_SMMU_STE_2_S2AA64 | ARM_SMMU_STE_2_S2PTW | ARM_SMMU_STE_2_S2R;
	ste[3] = domain->root_table_hpa & ARM_SMMU_STE_3_S2TTB_MASK;
	ste[4] = 0UL;
	ste[5] = 0UL;
	ste[6] = 0UL;
	ste[7] = 0UL;
	flush_cache_range(&ste[1], ARM_SMMU_STE_SIZE - sizeof(uint64_t));

	ret = arm_smmu_sync_ste_locked(stream_id);
	if (ret != 0) {
		return ret;
	}

	ste[0] = ARM_SMMU_STE_0_V |
		(ARM_SMMU_STE_0_CFG_S2_TRANS << ARM_SMMU_STE_0_CFG_SHIFT);
	flush_cache_range(&ste[0], sizeof(ste[0]));

	return arm_smmu_sync_ste_locked(stream_id);
}

static uint32_t arm_smmu_oas_bits(uint32_t idr5)
{
	uint32_t bits;

	switch (idr5 & ARM_SMMU_IDR5_OAS_MASK) {
	case ARM_SMMU_IDR5_OAS_32_BIT:
		bits = 32U;
		break;
	case ARM_SMMU_IDR5_OAS_36_BIT:
		bits = 36U;
		break;
	case ARM_SMMU_IDR5_OAS_40_BIT:
		bits = 40U;
		break;
	case ARM_SMMU_IDR5_OAS_42_BIT:
		bits = 42U;
		break;
	case ARM_SMMU_IDR5_OAS_44_BIT:
		bits = 44U;
		break;
	case ARM_SMMU_IDR5_OAS_48_BIT:
	default:
		bits = 48U;
		break;
	}

	return bits;
}

static uint32_t arm_smmu_cmdq_log2_entries(uint32_t idr1)
{
	return (idr1 & ARM_SMMU_IDR1_CMDQS_MASK) >> ARM_SMMU_IDR1_CMDQS_SHIFT;
}

static uint32_t arm_smmu_evtq_log2_entries(uint32_t idr1)
{
	return (idr1 & ARM_SMMU_IDR1_EVTQS_MASK) >> ARM_SMMU_IDR1_EVTQS_SHIFT;
}

static uint32_t arm_smmu_log2_roundup(uint32_t value)
{
	uint32_t log2 = 0U;
	uint32_t rounded = 1U;

	while (rounded < value) {
		rounded <<= 1U;
		log2++;
	}

	return log2;
}

static uint32_t arm_smmu_strtab_log2_entries(uint32_t sid_bits)
{
	uint32_t policy_log2 = arm_smmu_log2_roundup(ARM_SMMU_MAX_SW_STREAMS);

	return arm_smmu_min_u32(arm_smmu_min_u32(sid_bits, ARM_SMMU_STRTAB_LOG2_MAX),
		policy_log2);
}

static int32_t arm_smmu_wait_reg32(uint32_t reg, uint32_t mask, uint32_t expected)
{
	uint32_t retry;

	for (retry = 0U; retry < ARM_SMMU_POLL_RETRIES; retry++) {
		if ((mmio_read32(arm_smmu_reg(arm_smmu_hw.base, reg)) & mask) == expected) {
			return 0;
		}
		udelay(ARM_SMMU_POLL_DELAY_US);
	}

	return -ETIMEDOUT;
}

static int32_t arm_smmu_update_gbpa(uint32_t set, uint32_t clear)
{
	uint32_t reg;
	int32_t ret;

	ret = arm_smmu_wait_reg32(ARM_SMMU_GBPA, ARM_SMMU_GBPA_UPDATE, 0U);
	if (ret != 0) {
		return ret;
	}

	reg = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_GBPA));
	reg &= ~clear;
	reg |= set;
	mmio_write32(reg | ARM_SMMU_GBPA_UPDATE, arm_smmu_reg(arm_smmu_hw.base,
		ARM_SMMU_GBPA));

	return arm_smmu_wait_reg32(ARM_SMMU_GBPA, ARM_SMMU_GBPA_UPDATE, 0U);
}

static void arm_smmu_zero_abort_tables(uint32_t strtab_log2)
{
	uint32_t strtab_entries = 1U << strtab_log2;

	(void)memset(arm_smmu_strtab, 0U, strtab_entries * ARM_SMMU_STE_SIZE);
	(void)memset(arm_smmu_cmdq, 0U, sizeof(arm_smmu_cmdq));
	(void)memset(arm_smmu_evtq, 0U, sizeof(arm_smmu_evtq));

	/*
	 * SMMUv3 table ownership:
	 *
	 *   EL2 writes STE/CD/CMDQ/EVTQ in normal cacheable RAM
	 *       -> clean to PoC
	 *       -> SMMU table walker reads physical memory
	 *
	 * A zero linear STE is an abort entry during initialization. That is
	 * intentional: a described StreamID faults until assignment replaces only
	 * that STE with stage-2 translation data.
	 */
	flush_cache_range(arm_smmu_strtab, strtab_entries * ARM_SMMU_STE_SIZE);
	flush_cache_range(arm_smmu_cmdq, sizeof(arm_smmu_cmdq));
	flush_cache_range(arm_smmu_evtq, sizeof(arm_smmu_evtq));
}

static int32_t arm_smmu_hw_enable_abort_locked(void)
{
	uint64_t strtab_pa;
	uint64_t cmdq_pa;
	uint64_t evtq_pa;
	uint32_t sid_bits;
	uint32_t strtab_log2;
	uint32_t strtab_cfg;
	uint32_t cr0;
	int32_t ret;

	if (!arm_smmu_hw.discovered) {
		return ARM_SMMU_INIT_UNDISCOVERED;
	}
	sid_bits = arm_smmu_hw.idr1 & ARM_SMMU_IDR1_SIDSIZE_MASK;

	if ((sid_bits > 31U) ||
		(arm_smmu_cmdq_log2_entries(arm_smmu_hw.idr1) < ARM_SMMU_QUEUE_LOG2_ENTRIES)) {
		LOG_ERR("SMMUv3: queue/SID capability too small idr1=0x%x", arm_smmu_hw.idr1);
		return -ENODEV;
	}
	strtab_log2 = arm_smmu_strtab_log2_entries(sid_bits);
	arm_smmu_zero_abort_tables(strtab_log2);

	strtab_pa = hva2hpa(arm_smmu_strtab);
	cmdq_pa = hva2hpa(arm_smmu_cmdq);
	evtq_pa = hva2hpa(arm_smmu_evtq);
	strtab_cfg = ARM_SMMU_STRTAB_BASE_CFG_FMT_LINEAR | strtab_log2;

	/*
	 * Abort-default enable sequence:
	 *
	 *   zero STE table  -> STRTAB_BASE
	 *   zero CMDQ       -> CMDQ_BASE
	 *   GBPA.ABORT      -> unmatched traffic faults instead of bypass
	 *   CR0.*EN         -> stream table + command queue become live
	 *
	 * The order matters. If SMMUEN is set before the stream table points at
	 * known abort entries, an endpoint could DMA through stale reset state.
	 * EVTQ is intentionally left disabled in this stage because its doorbell
	 * registers live in SMMUv3 page1 on many implementations; BEAU DTS must
	 * describe that page before EL2 can program it safely.
	 */
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR0));
	ret = arm_smmu_wait_reg32(ARM_SMMU_CR0ACK, ARM_SMMU_CR0_SMMUEN |
		ARM_SMMU_CR0_EVTQEN | ARM_SMMU_CR0_CMDQEN, 0U);
	if (ret != 0) {
		return ret;
	}

	mmio_write64((strtab_pa & ARM_SMMU_STRTAB_BASE_ADDR_MASK) |
		ARM_SMMU_STRTAB_BASE_RA, arm_smmu_reg(arm_smmu_hw.base,
		ARM_SMMU_STRTAB_BASE));
	mmio_write32(strtab_cfg, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_STRTAB_BASE_CFG));
	mmio_write64((cmdq_pa & ARM_SMMU_Q_BASE_ADDR_MASK) | ARM_SMMU_Q_BASE_RWA |
		ARM_SMMU_QUEUE_LOG2_ENTRIES,
		arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_BASE));
	arm_smmu_cmdq_prod = 0U;
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_PROD));
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_CONS));
	if (arm_smmu_evtq_log2_entries(arm_smmu_hw.idr1) >= ARM_SMMU_QUEUE_LOG2_ENTRIES) {
		mmio_write64((evtq_pa & ARM_SMMU_Q_BASE_ADDR_MASK) | ARM_SMMU_Q_BASE_RWA |
			ARM_SMMU_QUEUE_LOG2_ENTRIES,
			arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_BASE));
	}

	ret = arm_smmu_update_gbpa(ARM_SMMU_GBPA_ABORT, 0U);
	if (ret != 0) {
		return ret;
	}
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_IRQ_CTRL));
	ret = arm_smmu_wait_reg32(ARM_SMMU_IRQ_CTRLACK, UINT32_MAX, 0U);
	if (ret != 0) {
		return ret;
	}

	cr0 = ARM_SMMU_CR0_CMDQEN | ARM_SMMU_CR0_SMMUEN;
	mmio_write32(cr0, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR0));
	ret = arm_smmu_wait_reg32(ARM_SMMU_CR0ACK, cr0, cr0);
	if (ret != 0) {
		return ret;
	}

	arm_smmu_hw.strtab_base = strtab_pa;
	arm_smmu_hw.cmdq_base = cmdq_pa;
	arm_smmu_hw.evtq_base = evtq_pa;
	arm_smmu_hw.sid_bits = sid_bits;
	arm_smmu_hw.oas_bits = arm_smmu_oas_bits(arm_smmu_hw.idr5);
	arm_smmu_hw.strtab_log2_entries = strtab_log2;
	arm_smmu_hw.cmdq_entries = ARM_SMMU_QUEUE_ENTRIES;
	arm_smmu_hw.evtq_entries = (arm_smmu_evtq_log2_entries(arm_smmu_hw.idr1) >=
		ARM_SMMU_QUEUE_LOG2_ENTRIES) ? ARM_SMMU_QUEUE_ENTRIES : 0U;
	arm_smmu_hw.aborted = true;
	arm_smmu_hw.cmdq_enabled = true;
	arm_smmu_hw.evtq_enabled = false;
	arm_smmu_hw.ready = true;
	arm_smmu_hw_ready = true;
	arm_smmu_assignment_hw_ready = true;

	return 0;
}

void arm_smmu_probe(uint64_t base, uint64_t size)
{
	uint64_t flags;
	int32_t ret;

	if ((base == 0UL) || (size < (ARM_SMMU_AIDR + sizeof(uint32_t)))) {
		LOG_ERR("invalid SMMUv3 dts window base=0x%lx size=0x%lx", base, size);
		return;
	}

	/* [20260709] SMMU discovery and abort-default programming:
	 *
	 *   platform.dts -> MMIO ID registers -> zero STEs -> SMMUEN
	 *                                           |
	 *                                           v
	 *                            all described StreamIDs fault by default
	 *
	 * This protects against DMA bypass, but it is not a VM assignment path.
	 * P1 must still replace one STE with a VM stage-2 descriptor and issue
	 * command-queue sync before passthrough can succeed.
	 */
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	(void)memset(&arm_smmu_hw, 0U, sizeof(arm_smmu_hw));
	arm_smmu_hw.base = base;
	arm_smmu_hw.size = size;
	arm_smmu_hw.idr0 = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IDR0));
	arm_smmu_hw.idr1 = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IDR1));
	arm_smmu_hw.idr5 = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IDR5));
	arm_smmu_hw.iidr = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IIDR));
	arm_smmu_hw.aidr = mmio_read32(arm_smmu_reg(base, ARM_SMMU_AIDR));
	arm_smmu_hw.discovered = true;
	arm_smmu_hw.probed = true;
	arm_smmu_hw.ready = false;
	arm_smmu_hw.init_status = ARM_SMMU_INIT_UNDISCOVERED;
	arm_smmu_hw_ready = false;
	arm_smmu_assignment_hw_ready = false;
	ret = arm_smmu_hw_enable_abort_locked();
	arm_smmu_hw.init_status = ret;
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	if (ret != 0) {
		LOG_ERR("SMMUv3: discovered at 0x%lx but abort-default init failed: %d",
			base, ret);
	}
}

void arm_smmu_get_hw_info(struct arm_smmu_hw_info *info)
{
	uint64_t flags;

	if (info == NULL) {
		return;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	*info = arm_smmu_hw;
	info->ready = arm_smmu_hw_ready;
	spinlock_irqrestore_release(&arm_smmu_lock, flags);
}

bool arm_smmu_assignment_ready(void)
{
	bool ready;
	uint64_t flags;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	ready = arm_smmu_assignment_hw_ready;
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ready;
}

uint32_t arm_smmu_get_stream_configs(struct arm_smmu_stream_config *configs,
	uint32_t max_configs)
{
	uint64_t flags;
	uint32_t copied = 0U;
	uint32_t i;

	if ((configs == NULL) || (max_configs == 0U)) {
		return 0U;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	for (i = 0U; (i < ARRAY_SIZE(arm_smmu_streams)) && (copied < max_configs); i++) {
		const struct arm_smmu_stream_state *stream = &arm_smmu_streams[i];

		if (!stream->used) {
			continue;
		}

		configs[copied].stream_id = stream->stream_id;
		configs[copied].assigned = stream->domain != NULL;
		if (stream->domain != NULL) {
			configs[copied].owner_vmid = stream->domain->vm_id;
			configs[copied].ipa_width = stream->domain->ipa_width;
			configs[copied].root_table_hpa = stream->domain->root_table_hpa;
		} else {
			configs[copied].owner_vmid = ACRN_INVALID_VMID;
			configs[copied].ipa_width = 0U;
			configs[copied].root_table_hpa = 0UL;
		}
		copied++;
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return copied;
}

bool arm_smmu_ready(void)
{
	return arm_smmu_hw_ready;
}

bool arm_smmu_domain_valid(const struct iommu_domain *domain)
{
	return (domain != NULL) &&
		(domain >= &arm_smmu_domains[0]) &&
		(domain < &arm_smmu_domains[ARM_SMMU_MAX_DOMAINS]) &&
		domain->used;
}

static struct arm_smmu_stream_state *arm_smmu_find_stream(uint32_t stream_id)
{
	uint32_t i;

	for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
		if (arm_smmu_streams[i].used &&
			(arm_smmu_streams[i].stream_id == stream_id)) {
			return &arm_smmu_streams[i];
		}
	}

	return NULL;
}

bool arm_smmu_stream_assigned_to(uint32_t stream_id, uint16_t vm_id)
{
	const struct arm_smmu_stream_state *stream;
	bool assigned = false;
	uint64_t flags;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	stream = arm_smmu_find_stream(stream_id);
	if ((stream != NULL) && (stream->domain != NULL) &&
		(stream->domain->vm_id == vm_id)) {
		assigned = true;
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return assigned;
}

static struct arm_smmu_stream_state *arm_smmu_alloc_stream(uint32_t stream_id)
{
	uint32_t i;

	for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
		if (!arm_smmu_streams[i].used) {
			arm_smmu_streams[i].used = true;
			arm_smmu_streams[i].stream_id = stream_id;
			arm_smmu_streams[i].domain = NULL;
			return &arm_smmu_streams[i];
		}
	}

	return NULL;
}

struct iommu_domain *arm_smmu_create_domain(uint16_t vm_id,
	uint64_t root_table_hpa, uint32_t ipa_width)
{
	struct iommu_domain *domain = NULL;
	uint64_t flags;

	if ((vm_id >= ARM_SMMU_MAX_DOMAINS) || (root_table_hpa == 0UL)) {
		return NULL;
	}
	if ((ipa_width < 32U) || (ipa_width > 48U)) {
		return NULL;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	domain = &arm_smmu_domains[vm_id];
	if (!domain->used) {
		domain->vm_id = vm_id;
		domain->root_table_hpa = root_table_hpa;
		domain->ipa_width = ipa_width;
		domain->used = true;
		domain->hw_bound = false;
	} else if ((domain->root_table_hpa != root_table_hpa) ||
		(domain->ipa_width != ipa_width)) {
		domain = NULL;
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return domain;
}

void arm_smmu_destroy_domain(struct iommu_domain *domain)
{
	uint32_t i;
	uint64_t flags;

	if (!arm_smmu_domain_valid(domain)) {
		return;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
		if (arm_smmu_streams[i].used &&
			(arm_smmu_streams[i].domain == domain)) {
			if (arm_smmu_assignment_hw_ready &&
				arm_smmu_stream_in_strtab(arm_smmu_streams[i].stream_id)) {
				arm_smmu_write_abort_ste_locked(arm_smmu_streams[i].stream_id);
				(void)arm_smmu_sync_ste_locked(arm_smmu_streams[i].stream_id);
			}
			arm_smmu_streams[i].domain = NULL;
			arm_smmu_streams[i].used = false;
		}
	}
	(void)memset(domain, 0U, sizeof(*domain));
	spinlock_irqrestore_release(&arm_smmu_lock, flags);
}

int32_t arm_smmu_assign_stream(struct iommu_domain *domain, uint32_t stream_id)
{
	struct arm_smmu_stream_state *stream;
	uint64_t flags;
	int32_t ret = 0;

	if (!arm_smmu_domain_valid(domain) ||
		(stream_id == ARM_SMMU_STREAM_ID_INVALID)) {
		return -EINVAL;
	}

	/*
	 * A stream can have exactly one active owner. This is the software
	 * equivalent of "device already assigned" guard and prevents a
	 * device from DMAing with two VMIDs over its lifetime.
	 *
	 * Assignment state machine:
	 *
	 *   FREE/ABORT STE
	 *        |
	 *        v
	 *   S2 STE programmed and synced
	 *        |
	 *        v
	 *   software stream->domain points to the VM
	 *
	 * The software owner is updated only after the STE path succeeds, so
	 * higher layers cannot expose a device as assigned while DMA isolation
	 * is still missing.
	 */
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	stream = arm_smmu_find_stream(stream_id);
	if (stream == NULL) {
		stream = arm_smmu_alloc_stream(stream_id);
	}
	if (stream == NULL) {
		ret = -ENOMEM;
	} else if ((stream->domain != NULL) && (stream->domain != domain)) {
		ret = -EBUSY;
	} else if (!arm_smmu_assignment_hw_ready || !arm_smmu_stream_in_strtab(stream_id)) {
		stream->used = false;
		ret = -ENODEV;
	} else if (stream->domain == domain) {
		ret = 0;
	} else if (!arm_smmu_s2_supported_locked()) {
		ret = arm_smmu_write_bypass_ste_locked(stream_id);
		if (ret == 0) {
			int32_t compat_ret;

			stream->domain = domain;
			domain->hw_bound = true;
			/*
			 * Some platforms tag PCI MSI doorbell writes with the host bridge
			 * stream instead of the endpoint RID. When stage-2 translation is
			 * unavailable, the assigned endpoint is already bypassed for
			 * compatibility; mirror that only for the MSI compatibility stream
			 * so the doorbell write is not dropped by the abort-default table.
			 */
			if ((stream_id != ARM_SMMU_PCI_MSI_COMPAT_STREAM) &&
				arm_smmu_stream_in_strtab(ARM_SMMU_PCI_MSI_COMPAT_STREAM)) {
				compat_ret = arm_smmu_write_bypass_ste_locked(
					ARM_SMMU_PCI_MSI_COMPAT_STREAM);
				if (compat_ret != 0) {
					LOG_ERR("SMMUv3: stream 0x%x bypass for PCI MSI failed: %d",
						ARM_SMMU_PCI_MSI_COMPAT_STREAM, compat_ret);
				}
			}
		} else if (stream->domain == NULL) {
			stream->used = false;
		}
	} else {
		ret = arm_smmu_write_s2_ste_locked(domain, stream_id);
		if (ret == 0) {
			stream->domain = domain;
			domain->hw_bound = true;
		} else if (stream->domain == NULL) {
			stream->used = false;
		}
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	if (ret) {
		LOG_ERR("SMMUv3: assignment rejected (%d): stream 0x%x for vm%u",
			ret, stream_id, domain->vm_id);
	}

	return ret;
}

int32_t arm_smmu_unassign_stream(struct iommu_domain *domain, uint32_t stream_id)
{
	struct arm_smmu_stream_state *stream;
	uint64_t flags;
	int32_t ret = 0;

	if (!arm_smmu_domain_valid(domain) ||
		(stream_id == ARM_SMMU_STREAM_ID_INVALID)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	stream = arm_smmu_find_stream(stream_id);
	if (stream == NULL) {
		ret = -ENODEV;
	} else if (stream->domain != domain) {
		ret = -EPERM;
	} else if (!arm_smmu_assignment_hw_ready || !arm_smmu_stream_in_strtab(stream_id)) {
		ret = -ENODEV;
	} else {
		arm_smmu_write_abort_ste_locked(stream_id);
		ret = arm_smmu_sync_ste_locked(stream_id);
		if (ret == 0) {
			uint32_t i;

			stream->domain = NULL;
			stream->used = false;
			domain->hw_bound = false;
			for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
				if (arm_smmu_streams[i].used &&
					(arm_smmu_streams[i].domain == domain)) {
					domain->hw_bound = true;
					break;
				}
			}
		}
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

int32_t arm_smmu_move_pci_device(struct iommu_domain *src,
	struct iommu_domain *dst, uint8_t bus, uint8_t devfun)
{
	uint32_t stream_id = ARM_SMMU_PCI_STREAM(bus, devfun);
	int32_t ret = 0;

	if (src != NULL) {
		ret = arm_smmu_unassign_stream(src, stream_id);
	}
	if ((ret == 0) && (dst != NULL)) {
		ret = arm_smmu_assign_stream(dst, stream_id);
	}

	return ret;
}

struct iommu_domain *create_iommu_domain(uint16_t vm_id, uint64_t root_table_hpa,
	uint32_t addr_width)
{
	return arm_smmu_create_domain(vm_id, root_table_hpa, addr_width);
}

void destroy_iommu_domain(struct iommu_domain *domain)
{
	arm_smmu_destroy_domain(domain);
}

int32_t move_pt_device(struct iommu_domain *src, struct iommu_domain *dst,
	uint8_t bus, uint8_t devfun)
{
	return arm_smmu_move_pci_device(src, dst, bus, devfun);
}

bool is_pi_capable(__unused const struct acrn_vm *vm)
{
	/*
	 * ARM64 passthrough IRQs are handled through the host GIC/ITS and then
	 * injected as virtual IRQs/LPIs into vGIC, so the common ptdev layer uses
	 * its softirq path here.
	 */
	return false;
}

static uint32_t arm64_ptdev_device_id(uint16_t phys_bdf)
{
	return (uint32_t)phys_bdf;
}

static uint32_t arm64_ptdev_event_id(uint16_t entry_nr)
{
	return (uint32_t)entry_nr;
}

static int32_t arm64_ptdev_map_msi_doorbell(struct acrn_vm *vm,
	struct arm64_gicv3_msi_msg *msg)
{
	uint16_t vm_id;
	uint64_t host_base;
	uint64_t iova_base;
	uint64_t iova;
	uint64_t offset;

	if ((vm == NULL) || (msg == NULL)) {
		return -EINVAL;
	}
	vm_id = vm->vm_id;
	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return -EINVAL;
	}

	host_base = msg->addr & PAGE_MASK;
	offset = msg->addr & (PAGE_SIZE - 1UL);
	iova_base = ARM64_PTDEV_MSI_DOORBELL_IOVA(vm_id);
	iova = iova_base + offset;

	if (!arm_smmu_s2_supported()) {
		return 0;
	}

	/*
	 * The current SMMUv3 model reuses the VM stage-2 root for device DMA.
	 * A physical MSI write therefore needs a guest-IOVA that translates to
	 * the host ITS TRANSLATER page. Use an unadvertised per-VM doorbell IPA
	 * instead of mapping the guest vITS page, so normal vITS MMIO still traps.
	 */
	if (!arm64_pt_msi_doorbell_mapped[vm_id]) {
		int32_t ret;

		arm64_stage2_map_device(vm, host_base, iova_base, PAGE_SIZE, true);
		ret = arm_smmu_flush_vm_iotlb(vm_id, iova_base);
		if (ret != 0) {
			LOG_WRN("vm%u ptdev MSI doorbell iotlb flush failed: %d", vm_id, ret);
			return ret;
		}
		arm64_pt_msi_doorbell_hpa[vm_id] = host_base;
		arm64_pt_msi_doorbell_mapped[vm_id] = true;
	} else if (arm64_pt_msi_doorbell_hpa[vm_id] != host_base) {
		LOG_ERR("vm%u ptdev MSI doorbell hpa mismatch old=0x%lx new=0x%lx",
			vm_id, arm64_pt_msi_doorbell_hpa[vm_id], host_base);
		return -EINVAL;
	}

	msg->addr = iova;
	return 0;
}

static struct arm64_pt_msi_state *arm64_pt_find_msi_state(uint16_t phys_bdf,
	uint16_t entry_nr)
{
	uint32_t i;

	for (i = 0U; i < ARRAY_SIZE(arm64_pt_msi_states); i++) {
		if (arm64_pt_msi_states[i].used &&
			(arm64_pt_msi_states[i].phys_bdf == phys_bdf) &&
			(arm64_pt_msi_states[i].entry_nr == entry_nr)) {
			return &arm64_pt_msi_states[i];
		}
	}

	return NULL;
}

static int32_t arm64_pt_record_msi_state(uint16_t phys_bdf, uint16_t entry_nr,
	uint32_t dev_id, uint32_t event_id, uint32_t lpi, const uint32_t *alias_dev_ids,
	uint32_t alias_count)
{
	uint32_t i;
	uint64_t flags;
	int32_t ret = -ENOMEM;

	if (alias_count > ARM64_PTDEV_MSI_MAX_ALIASES) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&arm64_pt_msi_lock, &flags);
	if (arm64_pt_find_msi_state(phys_bdf, entry_nr) != NULL) {
		ret = -EBUSY;
	} else {
		for (i = 0U; i < ARRAY_SIZE(arm64_pt_msi_states); i++) {
			if (!arm64_pt_msi_states[i].used) {
				uint32_t alias_idx;

				arm64_pt_msi_states[i].used = true;
				arm64_pt_msi_states[i].phys_bdf = phys_bdf;
				arm64_pt_msi_states[i].entry_nr = entry_nr;
				arm64_pt_msi_states[i].dev_id = dev_id;
				arm64_pt_msi_states[i].event_id = event_id;
				arm64_pt_msi_states[i].lpi = lpi;
				arm64_pt_msi_states[i].alias_count = alias_count;
				for (alias_idx = 0U; alias_idx < alias_count; alias_idx++) {
					arm64_pt_msi_states[i].alias_dev_ids[alias_idx] =
						alias_dev_ids[alias_idx];
				}
				ret = 0;
				break;
			}
		}
	}
	spinlock_irqrestore_release(&arm64_pt_msi_lock, flags);

	return ret;
}

static bool arm64_pt_take_msi_state(uint16_t phys_bdf, uint16_t entry_nr,
	struct arm64_pt_msi_state *out_state)
{
	struct arm64_pt_msi_state *state;
	uint64_t flags;
	bool found = false;

	spinlock_irqsave_obtain(&arm64_pt_msi_lock, &flags);
	state = arm64_pt_find_msi_state(phys_bdf, entry_nr);
	if (state != NULL) {
		if (out_state != NULL) {
			*out_state = *state;
		}
		(void)memset(state, 0U, sizeof(*state));
		found = true;
	}
	spinlock_irqrestore_release(&arm64_pt_msi_lock, flags);

	return found;
}

static uint32_t arm64_pt_peek_msi_lpi(uint16_t phys_bdf, uint16_t entry_nr)
{
	struct arm64_pt_msi_state *state;
	uint32_t lpi = IRQ_INVALID;
	uint64_t flags;

	spinlock_irqsave_obtain(&arm64_pt_msi_lock, &flags);
	state = arm64_pt_find_msi_state(phys_bdf, entry_nr);
	if (state != NULL) {
		lpi = state->lpi;
	}
	spinlock_irqrestore_release(&arm64_pt_msi_lock, flags);

	return lpi;
}

static struct ptirq_remapping_info *arm64_pt_find_msi_entry_by_phys(
	const struct acrn_vm *vm, uint16_t phys_bdf, uint16_t entry_nr)
{
	struct ptirq_remapping_info *entry;
	DEFINE_MSI_SID(phys_sid, phys_bdf, entry_nr);

	entry = find_ptirq_entry(PTDEV_INTR_MSI, &phys_sid, NULL);
	if ((entry != NULL) && (entry->vm != vm)) {
		entry = NULL;
	}

	return entry;
}

static void arm64_ptdev_release_msi_vector(const struct ptirq_remapping_info *entry)
{
	uint16_t phys_bdf = entry->phys_sid.msi_id.bdf;
	uint16_t entry_nr = entry->phys_sid.msi_id.entry_nr;
	struct arm64_pt_msi_state state = {};
	uint32_t i;

	if (arm64_pt_take_msi_state(phys_bdf, entry_nr, &state)) {
		for (i = 0U; i < state.alias_count; i++) {
			(void)arm64_gicv3_its_unmap_lpi_event(state.alias_dev_ids[i],
				state.event_id, state.lpi);
		}
		arm64_gicv3_its_release_msix(state.dev_id, state.lpi);
	}
}

static bool arm64_pt_msi_alias_busy(uint32_t alias_dev_id, uint32_t event_id)
{
	uint64_t flags;
	uint32_t i;
	uint32_t alias_idx;
	bool busy = false;

	spinlock_irqsave_obtain(&arm64_pt_msi_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(arm64_pt_msi_states); i++) {
		if (!arm64_pt_msi_states[i].used ||
			(arm64_pt_msi_states[i].event_id != event_id)) {
			continue;
		}
		for (alias_idx = 0U; alias_idx < arm64_pt_msi_states[i].alias_count;
			alias_idx++) {
			if (arm64_pt_msi_states[i].alias_dev_ids[alias_idx] == alias_dev_id) {
				busy = true;
				break;
			}
		}
		if (busy) {
			break;
		}
	}
	spinlock_irqrestore_release(&arm64_pt_msi_lock, flags);

	return busy;
}

static uint32_t arm64_ptdev_map_msi_aliases(struct acrn_vm *vm, uint32_t dev_id,
	uint32_t event_id, uint32_t lpi, uint32_t *alias_dev_ids, uint32_t max_aliases)
{
	uint32_t alias_count = 0U;

	if ((alias_dev_ids == NULL) || (max_aliases == 0U)) {
		return 0U;
	}

	if (!arm_smmu_s2_supported() && (dev_id != ARM64_PTDEV_MSI_COMPAT_DEVID)) {
		int32_t ret;

		/*
		 * Compatibility note:
		 *
		 * Some emulated or simple platforms do not provide SMMU stage-2
		 * translation for the MSI doorbell path. In that mode, the endpoint
		 * may emit MSI writes through a fixed compatibility DeviceID. Mirror
		 * the ITS mapping for that DeviceID only; normal DMA isolation still
		 * depends on each endpoint StreamID assignment.
		 */
		if (arm64_pt_msi_alias_busy(ARM64_PTDEV_MSI_COMPAT_DEVID, event_id)) {
			return 0U;
		}

		ret = arm64_gicv3_its_map_lpi_event(ARM64_PTDEV_MSI_COMPAT_DEVID,
			event_id, lpi, NULL);

		if (ret == 0) {
			alias_dev_ids[alias_count] = ARM64_PTDEV_MSI_COMPAT_DEVID;
			alias_count++;
		} else if (ret != -EBUSY) {
			LOG_WRN("vm%u ptdev msi alias devid 0x%x event %u failed ret:%d",
				vm->vm_id, ARM64_PTDEV_MSI_COMPAT_DEVID, event_id, ret);
		}
	}

	return alias_count;
}

static void arm64_ptdev_unmap_msi_aliases(uint32_t event_id, uint32_t lpi,
	const uint32_t *alias_dev_ids, uint32_t alias_count)
{
	uint32_t i;

	for (i = 0U; i < alias_count; i++) {
		(void)arm64_gicv3_its_unmap_lpi_event(alias_dev_ids[i], event_id, lpi);
	}
}

int32_t ptirq_prepare_msi_remap(struct acrn_vm *vm, uint16_t virt_bdf,
	uint16_t phys_bdf, uint16_t entry_nr, struct msi_info *info,
	__unused uint16_t irte_idx)
{
	struct ptirq_remapping_info *entry;
	struct arm64_gicv3_msi_msg msg;
	uint32_t dev_id = arm64_ptdev_device_id(phys_bdf);
	uint32_t event_id = arm64_ptdev_event_id(entry_nr);
	uint32_t lpi = arm64_pt_peek_msi_lpi(phys_bdf, entry_nr);
	uint32_t acrn_irq;
	uint32_t alias_dev_ids[ARM64_PTDEV_MSI_MAX_ALIASES] = { 0U };
	uint32_t alias_count = 0U;
	int32_t ret;
	bool new_lpi = false;
	DEFINE_MSI_SID(phys_sid, phys_bdf, entry_nr);
	DEFINE_MSI_SID(virt_sid, virt_bdf, entry_nr);

	if ((vm == NULL) || (virt_bdf == 0xffffU) || (info == NULL)) {
		return -EINVAL;
	}

	/*
	 * MSI on GICv3 ITS is a memory write to GITS_TRANSLATER. The SMMU
	 * handles DMA address isolation, while the ITS translates
	 * (DeviceID, EventID) into a guest-visible LPI. This function rewrites
	 * the physical MSI message to target the host ITS and stores the guest
	 * MSI identity for later virtual delivery.
	 *
	 * Boundary with SMMUv3:
	 *   - SMMU validates/translates the MSI write address.
	 *   - ITS consumes DeviceID/EventID and produces an LPI.
	 *   - vGIC injection delivers the virtual interrupt to the VM.
	 */
	if (lpi == IRQ_INVALID) {
		ret = arm64_gicv3_its_alloc_msix(dev_id, event_id, &lpi, &msg);
		if (ret != 0) {
			LOG_WRN("vm%u ptdev msi alloc failed p:%04x v:%04x entry:%u ret:%d",
				vm->vm_id, phys_bdf, virt_bdf, entry_nr, ret);
			return ret;
		}
		new_lpi = true;
		alias_count = arm64_ptdev_map_msi_aliases(vm, dev_id, event_id, lpi,
			alias_dev_ids, ARRAY_SIZE(alias_dev_ids));
	} else {
		ret = arm64_gicv3_its_map_msi(lpi, &msg);
		if (ret != 0) {
			LOG_WRN("vm%u ptdev msi map failed p:%04x v:%04x entry:%u lpi:%u ret:%d",
				vm->vm_id, phys_bdf, virt_bdf, entry_nr, lpi, ret);
			return ret;
		}
	}

	/*
	 * Hardware reports the raw LPI INTID, but common ptdev must request the
	 * dense ACRN IRQ in the LPI domain. Keep the raw LPI in a side table for
	 * ITS release; ptirq_remapping_info::allocated_pirq is owned by common
	 * IRQ code after ptirq_activate_entry().
	 */
	acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC_LPI,
		lpi - ARM64_GIC_FIRST_LPI);
	if (acrn_irq == IRQ_INVALID) {
		ret = -ENODEV;
		LOG_WRN("vm%u ptdev msi lpi domain invalid p:%04x v:%04x lpi:%u",
			vm->vm_id, phys_bdf, virt_bdf, lpi);
		goto fail_release_lpi;
	}

	ret = arm64_ptdev_map_msi_doorbell(vm, &msg);
	if (ret != 0) {
		LOG_WRN("vm%u ptdev msi doorbell map failed p:%04x v:%04x lpi:%u ret:%d",
			vm->vm_id, phys_bdf, virt_bdf, lpi, ret);
		goto fail_release_lpi;
	}

	if (new_lpi) {
		ret = arm64_pt_record_msi_state(phys_bdf, entry_nr, dev_id, event_id, lpi,
			alias_dev_ids, alias_count);
		if (ret != 0) {
			LOG_WRN("vm%u ptdev msi record failed p:%04x v:%04x entry:%u lpi:%u ret:%d",
				vm->vm_id, phys_bdf, virt_bdf, entry_nr, lpi, ret);
			goto fail_release_lpi;
		}
	}

	spinlock_obtain(&ptdev_lock);
	entry = find_ptirq_entry(PTDEV_INTR_MSI, &virt_sid, vm);
	if (entry == NULL) {
		entry = ptirq_alloc_entry(vm, PTDEV_INTR_MSI);
	}
	if (entry == NULL) {
		ret = -ENOMEM;
	} else {
		entry->phys_sid = phys_sid;
		entry->virt_sid = virt_sid;
		entry->vmsi = *info;
		entry->pmsi.addr.full = msg.addr;
		entry->pmsi.data.full = msg.data;
		entry->release_cb = arm64_ptdev_release_msi_vector;
		*info = entry->pmsi;

		if (!is_entry_active(entry)) {
			ret = ptirq_activate_entry(entry, acrn_irq);
			if (ret >= 0) {
				ret = 0;
			}
		}
	}
	spinlock_release(&ptdev_lock);

	if (ret != 0) {
		LOG_WRN("vm%u ptdev msi entry activate failed p:%04x v:%04x entry:%u lpi:%u irq:%u ret:%d",
			vm->vm_id, phys_bdf, virt_bdf, entry_nr, lpi, acrn_irq, ret);
		if (new_lpi) {
			(void)arm64_pt_take_msi_state(phys_bdf, entry_nr, NULL);
		}
		goto fail_release_lpi;
	}

	return 0;

fail_release_lpi:
	if (new_lpi) {
		arm64_ptdev_unmap_msi_aliases(event_id, lpi, alias_dev_ids, alias_count);
		arm64_gicv3_its_release_msix(dev_id, lpi);
	}
	return ret;
}

int32_t ptirq_prepare_msix_remap(struct acrn_vm *vm, uint16_t virt_bdf,
	uint16_t phys_bdf, uint16_t entry_nr, struct msi_info *info,
	uint16_t irte_idx)
{
	return ptirq_prepare_msi_remap(vm, virt_bdf, phys_bdf, entry_nr, info, irte_idx);
}

void ptirq_remove_msi_remapping(const struct acrn_vm *vm, uint16_t phys_bdf,
	uint32_t vector_count)
{
	uint32_t i;

	for (i = 0U; i < vector_count; i++) {
		struct ptirq_remapping_info *entry;

		spinlock_obtain(&ptdev_lock);
		entry = arm64_pt_find_msi_entry_by_phys(vm, phys_bdf, (uint16_t)i);
		if (entry != NULL) {
			if (entry->release_cb != NULL) {
				entry->release_cb(entry);
			}
			ptirq_deactivate_entry(entry);
			ptirq_release_entry(entry);
		}
		spinlock_release(&ptdev_lock);
	}
}

void ptirq_remove_msix_remapping(const struct acrn_vm *vm, uint16_t phys_bdf,
	uint32_t vector_count)
{
	ptirq_remove_msi_remapping(vm, phys_bdf, vector_count);
}

int32_t ptirq_add_intx_remapping(struct acrn_vm *vm,
	uint32_t virt_gsi, uint32_t phys_gsi, bool pic_pin)
{
	struct ptirq_remapping_info *entry;
	uint32_t acrn_irq;
	int32_t ret = 0;
	DEFINE_INTX_SID(phys_sid, phys_gsi, INTX_CTLR_IOAPIC);
	DEFINE_INTX_SID(virt_sid, virt_gsi, INTX_CTLR_IOAPIC);

	if ((vm == NULL) || pic_pin || (phys_gsi < 32U) ||
		(phys_gsi >= ARM64_GIC_SPURIOUS_INTID) ||
		(virt_gsi >= ARM64_VGIC_IRQ_NUM)) {
		return -EINVAL;
	}

	/*
	 * ARM64 does not have PIC/IOAPIC pins. We reuse the common INTx entry
	 * shape only as "platform SPI -> guest virtual IRQ":
	 *
	 *   physical GIC SPI -> ACRN GIC domain IRQ -> ptdev softirq
	 *                    -> vGIC inject virt_gsi
	 *
	 * The caller must provide a platform-vetted SPI mapping. Random SPIs are
	 * not safe to assign because they may be shared with EL2-owned devices.
	 */
	acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, phys_gsi);
	if (acrn_irq == IRQ_INVALID) {
		return -ENODEV;
	}

	spinlock_obtain(&ptdev_lock);
	if (find_ptirq_entry(PTDEV_INTR_INTX, &phys_sid, NULL) != NULL) {
		ret = -EBUSY;
	} else {
		entry = ptirq_alloc_entry(vm, PTDEV_INTR_INTX);
		if (entry == NULL) {
			ret = -ENOMEM;
		} else {
			entry->phys_sid = phys_sid;
			entry->virt_sid = virt_sid;
			entry->polarity = 0U;
			ret = ptirq_activate_entry(entry, acrn_irq);
			if (ret < 0) {
				ptirq_release_entry(entry);
			}
		}
	}
	spinlock_release(&ptdev_lock);

	return ret;
}

void ptirq_remove_intx_remapping(__unused const struct acrn_vm *vm,
	uint32_t virt_gsi, bool pic_pin, bool phys)
{
	struct ptirq_remapping_info *entry;
	DEFINE_INTX_SID(sid, virt_gsi, INTX_CTLR_IOAPIC);

	if ((vm == NULL) || pic_pin) {
		return;
	}

	spinlock_obtain(&ptdev_lock);
	entry = find_ptirq_entry(PTDEV_INTR_INTX, &sid, phys ? NULL : vm);
	if ((entry != NULL) && (phys || (entry->vm == vm))) {
		ptirq_deactivate_entry(entry);
		ptirq_release_entry(entry);
	}
	spinlock_release(&ptdev_lock);
}

void ptirq_softirq(uint16_t pcpu_id)
{
	struct ptirq_remapping_info *entry;

	while ((entry = ptirq_dequeue_softirq(pcpu_id)) != NULL) {
		int32_t ret = 0;

		if (entry->intr_type == PTDEV_INTR_MSI) {
			ret = arm64_vgicv3_inject_msi(entry->vm,
				arm64_ptdev_device_id(entry->virt_sid.msi_id.bdf),
				(uint32_t)entry->vmsi.data.full);
		} else if (entry->intr_type == PTDEV_INTR_INTX) {
			ret = arm64_vgicv3_inject_irq(vcpu_from_vid(entry->vm, 0U),
				entry->virt_sid.intx_id.gsi, true);
		}

		if (ret != 0) {
			LOG_ERR("ptdev softirq inject failed: vm%u type=0x%x ret=%d",
				entry->vm->vm_id, entry->intr_type, ret);
		}
	}
}
