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
#include <passthrough.h>
#include <spinlock.h>
#include <util.h>
#include <vm.h>
#include <acrn_hv_defs.h>
#include <asm/mmu.h>
#include <asm/platform.h>
#include <asm/pgtable.h>
#include <asm/vtd.h>
#include <asm/irq.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vgicv3.h>
#include "smmu.h"

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
 *   iommu.c arm_smmu_assign_stream(domain, sid)
 *       -> pass an immutable S2 configuration snapshot
 *       -> arm_smmu_hw_attach_stream(config, sid)
 *       -> build STE from VM root stage-2 table
 *       -> clean STE to PoC
 *       -> CFGI_STE + CMD_SYNC through CMDQ
 *       -> software owner becomes VM
 *
 *   iommu.c arm_smmu_unassign_stream(domain, sid)
 *       -> arm_smmu_hw_detach_stream(config, sid)
 *       -> replace STE with ABORT
 *       -> CFGI_STE + CMD_SYNC
 *       -> software owner returns to host/free
 *
 * EVTQ fault handling:
 *
 *   SMMU fault event
 *       |
 *       v
 *   poll EVTQ from shell / diagnostics path
 *       |
 *       v
 *   decode event id + StreamID + input address
 *       |
 *       +--> mark stream quarantined for diagnostics
 *       |
 *       v
 *   replace STE with ABORT and sync CMDQ
 *
 * Ownership dump model:
 *
 *   software stream state                 hardware stream table
 *   ---------------------                 ---------------------
 *   stream config snapshot -------------> STE word0 CFG
 *        |                                STE word2 S2 VMID
 *        |                                STE word3 S2TTB
 *        v
 *   sw-owner printed by smmustat          ste-vm printed by smmustat
 *
 * The two sides are intentionally printed independently. During passthrough
 * debugging, the important question is often not "did assignment return 0" but
 * whether the SMMU-visible STE really matches the VM that software believes owns
 * the StreamID. A mismatch means DMA may be aborted, bypassed, or translated
 * with the wrong VMID/root even though the high-level device policy looks sane.
 *
 * Out of scope for this minimal EL2 passthrough path:
 *   stage-1 context descriptors, two-level stream tables, ATS/PRI, nested
 *   translation, stall-model recovery, and PRI replay.
 */

#define ARM64_PTDEV_MSI_DOORBELL_IOVA_BASE	0x0ff00000UL
#define ARM64_PTDEV_MSI_DOORBELL_IOVA(vm_id) \
	(ARM64_PTDEV_MSI_DOORBELL_IOVA_BASE + ((uint64_t)(vm_id) * PAGE_SIZE))
#define ARM_SMMU_IDR0_ST_LVL_SHIFT	27U
#define ARM_SMMU_IDR0_ST_LVL_MASK	(3U << ARM_SMMU_IDR0_ST_LVL_SHIFT)
#define ARM_SMMU_IDR0_ST_LVL_2LVL	1U
#define ARM_SMMU_IDR0_STALL_MODEL_SHIFT	24U
#define ARM_SMMU_IDR0_STALL_MODEL_MASK	(3U << ARM_SMMU_IDR0_STALL_MODEL_SHIFT)
#define ARM_SMMU_IDR0_STALL_MODEL_FORCE	2U
#define ARM_SMMU_IDR0_TTENDIAN_SHIFT	21U
#define ARM_SMMU_IDR0_TTENDIAN_MASK	(3U << ARM_SMMU_IDR0_TTENDIAN_SHIFT)
#define ARM_SMMU_IDR0_TTENDIAN_MIXED	0U
#define ARM_SMMU_IDR0_TTENDIAN_LE	2U
#define ARM_SMMU_IDR0_VMID16		(1U << 18U)
#define ARM_SMMU_IDR0_ATS		(1U << 10U)
#define ARM_SMMU_IDR0_COHACC		(1U << 4U)
#define ARM_SMMU_IDR0_TTF_SHIFT		2U
#define ARM_SMMU_IDR0_TTF_MASK		(3U << ARM_SMMU_IDR0_TTF_SHIFT)
#define ARM_SMMU_IDR0_TTF_AARCH64	2U
#define ARM_SMMU_IDR0_S2P		(1U << 0U)
#define ARM_SMMU_IDR1_TABLES_PRESET	(1U << 30U)
#define ARM_SMMU_IDR1_QUEUES_PRESET	(1U << 29U)
#define ARM_SMMU_IDR1_REL		(1U << 28U)
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
#define ARM_SMMU_IDR5_OAS_52_BIT	6U
#define ARM_SMMU_IDR5_OAS_56_BIT	7U
#define ARM_SMMU_IDR5_GRAN4K		(1U << 4U)
#define ARM_SMMU_IDR0			0x0000U
#define ARM_SMMU_IDR1			0x0004U
#define ARM_SMMU_IDR5			0x0014U
#define ARM_SMMU_IIDR			0x0018U
#define ARM_SMMU_AIDR			0x001cU
#define ARM_SMMU_CR0			0x0020U
#define ARM_SMMU_CR0ACK		0x0024U
#define ARM_SMMU_CR0_ATSCHK		(1U << 4U)
#define ARM_SMMU_CR0_SMMUEN		(1U << 0U)
#define ARM_SMMU_CR0_PRIQEN		(1U << 1U)
#define ARM_SMMU_CR0_EVTQEN		(1U << 2U)
#define ARM_SMMU_CR0_CMDQEN		(1U << 3U)
#define ARM_SMMU_CR0_ACK_MASK		(ARM_SMMU_CR0_ATSCHK | ARM_SMMU_CR0_CMDQEN | \
	ARM_SMMU_CR0_EVTQEN | ARM_SMMU_CR0_PRIQEN | ARM_SMMU_CR0_SMMUEN)
#define ARM_SMMU_CR0_ENABLE_MASK	(ARM_SMMU_CR0_CMDQEN | ARM_SMMU_CR0_EVTQEN | \
	ARM_SMMU_CR0_SMMUEN)
#define ARM_SMMU_CR1			0x0028U
#define ARM_SMMU_CR1_TABLE_SH_SHIFT	10U
#define ARM_SMMU_CR1_TABLE_OC_SHIFT	8U
#define ARM_SMMU_CR1_TABLE_IC_SHIFT	6U
#define ARM_SMMU_CR1_QUEUE_SH_SHIFT	4U
#define ARM_SMMU_CR1_QUEUE_OC_SHIFT	2U
#define ARM_SMMU_CR1_QUEUE_IC_SHIFT	0U
#define ARM_SMMU_CR1_SH_INNER		3U
#define ARM_SMMU_CR1_CACHE_WB		1U
#define ARM_SMMU_CR1_STRICT_WB_ISH \
	((ARM_SMMU_CR1_SH_INNER << ARM_SMMU_CR1_TABLE_SH_SHIFT) | \
	 (ARM_SMMU_CR1_CACHE_WB << ARM_SMMU_CR1_TABLE_OC_SHIFT) | \
	 (ARM_SMMU_CR1_CACHE_WB << ARM_SMMU_CR1_TABLE_IC_SHIFT) | \
	 (ARM_SMMU_CR1_SH_INNER << ARM_SMMU_CR1_QUEUE_SH_SHIFT) | \
	 (ARM_SMMU_CR1_CACHE_WB << ARM_SMMU_CR1_QUEUE_OC_SHIFT) | \
	 (ARM_SMMU_CR1_CACHE_WB << ARM_SMMU_CR1_QUEUE_IC_SHIFT))
#define ARM_SMMU_CR2			0x002cU
#define ARM_SMMU_CR2_RECINVSID		(1U << 1U)
#define ARM_SMMU_GBPA			0x0044U
#define ARM_SMMU_GBPA_UPDATE		(1U << 31U)
#define ARM_SMMU_GBPA_ABORT		(1U << 20U)
#define ARM_SMMU_IRQ_CTRL		0x0050U
#define ARM_SMMU_IRQ_CTRLACK		0x0054U
#define ARM_SMMU_STRTAB_BASE		0x0080U
#define ARM_SMMU_STRTAB_BASE_RA		(1UL << 62U)
#define ARM_SMMU_STRTAB_BASE_ADDR_MASK	0x000fffffffffffc0UL
#define ARM_SMMU_STRTAB_BASE_CFG	0x0088U
#define ARM_SMMU_STRTAB_BASE_CFG_FMT_LINEAR	0U
#define ARM_SMMU_Q_BASE_RWA		(1UL << 62U)
#define ARM_SMMU_Q_BASE_ADDR_MASK	0x000fffffffffffe0UL
#define ARM_SMMU_CMDQ_BASE		0x0090U
#define ARM_SMMU_CMDQ_PROD		0x0098U
#define ARM_SMMU_CMDQ_CONS		0x009cU
#define ARM_SMMU_EVTQ_BASE		0x00a0U
#define ARM_SMMU_EVTQ_PROD		0x00a8U
#define ARM_SMMU_EVTQ_CONS		0x00acU
#define ARM_SMMU_PAGE1_OFFSET		0x00010000U
#define ARM_SMMU_MMIO_SIZE		(2UL * ARM_SMMU_PAGE1_OFFSET)
#define ARM_SMMU_MMIO_ALIGNMENT	ARM_SMMU_PAGE1_OFFSET
#define ARM_SMMU_PAGE1_EVTQ_SIZE	(ARM_SMMU_PAGE1_OFFSET + ARM_SMMU_EVTQ_CONS + \
	sizeof(uint32_t))
#define ARM_SMMU_QUEUE_LOG2_ENTRIES	6U
#define ARM_SMMU_QUEUE_ENTRIES		(1U << ARM_SMMU_QUEUE_LOG2_ENTRIES)
#define ARM_SMMU_CMD_DWORDS		2U
#define ARM_SMMU_EVT_DWORDS		4U
#define ARM_SMMU_EVT_0_ID_MASK		0xffUL
#define ARM_SMMU_EVT_0_SID_SHIFT	32U
#define ARM_SMMU_EVT_2_ADDR_MASK	UINT64_MAX
#define ARM_SMMU_STE_DWORDS		8U
#define ARM_SMMU_STE_SIZE		(ARM_SMMU_STE_DWORDS * sizeof(uint64_t))
#define ARM_SMMU_STRTAB_LOG2_MIN	0U
#define ARM_SMMU_STRTAB_LOG2_MAX	8U
#define ARM_SMMU_POLL_RETRIES		1000U
#define ARM_SMMU_POLL_DELAY_US		1U
#define ARM_SMMU_INIT_UNDISCOVERED	(-ENODEV)
#define ARM_SMMU_CPU_STAGE2_OAS_BITS	48U
#define ARM_SMMU_Q_PTR_MASK		((ARM_SMMU_QUEUE_ENTRIES << 1U) - 1U)
#define ARM_SMMU_Q_ERR_MASK		(0x7fU << 24U)
#define ARM_SMMU_STE_0_V		(1UL << 0U)
#define ARM_SMMU_STE_0_CFG_SHIFT	1U
#define ARM_SMMU_STE_0_CFG_MASK	0x7UL
#define ARM_SMMU_STE_0_CFG_ABORT	0UL
#define ARM_SMMU_STE_0_CFG_S2_TRANS	6UL
#define ARM_SMMU_STE_1_SHCFG_SHIFT	44U
#define ARM_SMMU_STE_1_SHCFG_INCOMING	1UL
#define ARM_SMMU_STE_2_VTCR_SHIFT	32U
#define ARM_SMMU_STE_2_S2VMID_MASK	0xffffUL
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

/* [20260716] Physical SMMU ownership snapshot
 *
 *   iommu.c domain/broker
 *       -> immutable arm_smmu_s2_config
 *       -> this file programs and synchronizes the physical STE
 *       -> stream state keeps a value snapshot, never a domain pointer
 *
 *   VM vCPU access:  IPA ---- CPU stage-2 ----> PA
 *   Device DMA:      IPA ---- SMMU stage-2 ---> PA
 *
 * Key rule:
 *   - iommu.c owns domain lifetime and serializes broker calls;
 *   - smmu.c owns physical stream truth, queues, and failure containment;
 *   - no physical stream retains a pointer into the broker's domain pool;
 *   - ownership is visible only after both STE publication phases sync.
 */
struct arm_smmu_stream_state {
	uint32_t stream_id;
	struct arm_smmu_s2_config config;
	uint32_t fault_count;
	uint32_t last_fault_code;
	uint64_t last_fault_iova;
	bool used;
	bool assigned;
	bool quarantined;
};

struct arm64_pt_msi_state {
	bool used;
	uint16_t phys_bdf;
	uint16_t entry_nr;
	uint32_t dev_id;
	uint32_t event_id;
	uint32_t lpi;
};

static spinlock_t arm_smmu_lock = { .head = 0U, .tail = 0U };
static spinlock_t arm64_pt_msi_lock = { .head = 0U, .tail = 0U };
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

struct arm_smmu_pm_state {
	uint64_t suspend_epoch;
	uint64_t strtab_base;
	uint64_t cmdq_base;
	uint64_t evtq_base;
	uint32_t strtab_base_cfg;
	uint32_t cr0;
	uint32_t cr1;
	uint32_t cr2;
	uint32_t gbpa;
	uint32_t irq_ctrl;
	bool hardware_active;
	bool valid;
};

static struct arm_smmu_pm_state arm_smmu_pm_state;

static struct arm_smmu_stream_state *arm_smmu_find_stream(uint32_t stream_id);
static struct arm_smmu_stream_state *arm_smmu_alloc_stream(uint32_t stream_id);
static uint32_t arm_smmu_cmdq_log2_entries(uint32_t idr1);
static uint32_t arm_smmu_evtq_log2_entries(uint32_t idr1);
static int32_t arm_smmu_degrade_locked(const char *reason, int32_t status);

static inline void *arm_smmu_reg(uint64_t base, uint32_t off)
{
	return (void *)(base + off);
}

static inline void *arm_smmu_page1_reg(uint64_t base, uint32_t off)
{
	return (void *)(base + ARM_SMMU_PAGE1_OFFSET + off);
}

static uint32_t arm_smmu_min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static uint32_t arm_smmu_queue_next(uint32_t ptr)
{
	return (ptr + 1U) & ARM_SMMU_Q_PTR_MASK;
}

static uint32_t arm_smmu_cmdq_next(uint32_t prod)
{
	return arm_smmu_queue_next(prod);
}

static uint32_t arm_smmu_cmdq_index(uint32_t prod)
{
	return prod & (ARM_SMMU_QUEUE_ENTRIES - 1U);
}

static uint32_t arm_smmu_evtq_index(uint32_t cons)
{
	return cons & (ARM_SMMU_QUEUE_ENTRIES - 1U);
}

static uint32_t arm_smmu_evt_code(const uint64_t evt[ARM_SMMU_EVT_DWORDS])
{
	return (uint32_t)(evt[0] & ARM_SMMU_EVT_0_ID_MASK);
}

static uint32_t arm_smmu_evt_sid(const uint64_t evt[ARM_SMMU_EVT_DWORDS])
{
	return (uint32_t)(evt[0] >> ARM_SMMU_EVT_0_SID_SHIFT);
}

static uint64_t arm_smmu_evt_iova(const uint64_t evt[ARM_SMMU_EVT_DWORDS])
{
	return evt[2] & ARM_SMMU_EVT_2_ADDR_MASK;
}

static void arm_smmu_record_cmdq_result_locked(int32_t ret, uint32_t cons)
{
	arm_smmu_hw.cmdq_cons = cons;
	arm_smmu_hw.cmdq_last_cons = cons;
	arm_smmu_hw.cmdq_last_ret = ret;

	if (ret == -EIO) {
		arm_smmu_hw.cmdq_errors++;
	} else if (ret == -EBUSY) {
		arm_smmu_hw.cmdq_full++;
	} else if (ret == -ETIMEDOUT) {
		arm_smmu_hw.cmdq_timeouts++;
	}
}

static void arm_smmu_record_stream_result_locked(bool assign, int32_t ret)
{
	if (assign) {
		if (ret == 0) {
			arm_smmu_hw.assign_ok++;
		} else {
			arm_smmu_hw.assign_fail++;
		}
	} else {
		if (ret == 0) {
			arm_smmu_hw.unassign_ok++;
		} else {
			arm_smmu_hw.unassign_fail++;
		}
	}
}

static void arm_smmu_record_stream_result(bool assign, int32_t ret)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	arm_smmu_record_stream_result_locked(assign, ret);
	spinlock_irqrestore_release(&arm_smmu_lock, flags);
}

static bool arm_smmu_stream_in_strtab(uint32_t stream_id)
{
	return arm_smmu_hw.ready &&
		(stream_id < (1U << arm_smmu_hw.strtab_log2_entries));
}

static uint16_t arm_smmu_vm_vmid(uint16_t vm_id)
{
	return (uint16_t)(vm_id + 1U);
}

static uint64_t arm_smmu_ste_vtcr(uint32_t ipa_width)
{
	uint64_t vtcr = 0UL;
	uint64_t t0sz = 64UL - (uint64_t)ipa_width;
	uint32_t s2ps;

	switch (arm_smmu_hw.effective_oas_bits) {
	case 32U:
		s2ps = ARM_SMMU_IDR5_OAS_32_BIT;
		break;
	case 36U:
		s2ps = ARM_SMMU_IDR5_OAS_36_BIT;
		break;
	case 40U:
		s2ps = ARM_SMMU_IDR5_OAS_40_BIT;
		break;
	case 42U:
		s2ps = ARM_SMMU_IDR5_OAS_42_BIT;
		break;
	case 44U:
		s2ps = ARM_SMMU_IDR5_OAS_44_BIT;
		break;
	case 48U:
	default:
		s2ps = ARM_SMMU_IDR5_OAS_48_BIT;
		break;
	}

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
	vtcr |= (uint64_t)s2ps <<
		ARM_SMMU_VTCR_S2PS_SHIFT;

	return vtcr;
}

static int32_t arm_smmu_cmdq_issue_locked(uint64_t cmd0, uint64_t cmd1)
{
	uint32_t prod = arm_smmu_cmdq_prod;
	uint32_t next = arm_smmu_cmdq_next(prod);
	uint32_t cons = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_CONS));
	uint32_t idx;

	arm_smmu_hw.cmdq_prod = prod;
	arm_smmu_hw.cmdq_cons = cons;
	if ((cons & ARM_SMMU_Q_ERR_MASK) != 0U) {
		arm_smmu_record_cmdq_result_locked(-EIO, cons);
		return arm_smmu_degrade_locked("CMDQ CERROR", -EIO);
	}
	if (next == (cons & ARM_SMMU_Q_PTR_MASK)) {
		arm_smmu_record_cmdq_result_locked(-EBUSY, cons);
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
	arm_smmu_hw.cmdq_prod = next;
	arm_smmu_hw.cmdq_issued++;
	arm_smmu_record_cmdq_result_locked(0, cons);
	mmio_write32(next, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_PROD));

	return 0;
}

static int32_t arm_smmu_cmdq_wait_locked(uint32_t target)
{
	uint32_t cons = 0U;
	uint32_t retry;

	for (retry = 0U; retry < ARM_SMMU_POLL_RETRIES; retry++) {
		cons = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_CONS));
		arm_smmu_hw.cmdq_cons = cons;

		if ((cons & ARM_SMMU_Q_ERR_MASK) != 0U) {
			arm_smmu_record_cmdq_result_locked(-EIO, cons);
			return arm_smmu_degrade_locked("CMDQ CERROR", -EIO);
		}
		if ((cons & ARM_SMMU_Q_PTR_MASK) == target) {
			arm_smmu_record_cmdq_result_locked(0, cons);
			return 0;
		}
		udelay(ARM_SMMU_POLL_DELAY_US);
	}

	arm_smmu_record_cmdq_result_locked(-ETIMEDOUT, cons);
	return arm_smmu_degrade_locked("CMDQ timeout", -ETIMEDOUT);
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
	if (ret == 0) {
		arm_smmu_hw.cmdq_syncs++;
	}

	return ret;
}

int32_t arm_smmu_cmdq_sync(void)
{
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	if (!arm_smmu_hw.ready || !arm_smmu_hw.cmdq_enabled ||
		(arm_smmu_hw.base == 0UL)) {
		ret = -ENODEV;
	} else {
		ret = arm_smmu_cmdq_sync_locked();
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

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

static int32_t arm_smmu_write_s2_ste_locked(const struct arm_smmu_s2_config *config,
	uint32_t stream_id)
{
	uint64_t *ste = arm_smmu_strtab[stream_id];
	uint64_t vtcr = arm_smmu_ste_vtcr(config->ipa_width);
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
	ste[2] = (uint64_t)config->hw_vmid |
		(vtcr << ARM_SMMU_STE_2_VTCR_SHIFT) |
		ARM_SMMU_STE_2_S2AA64 | ARM_SMMU_STE_2_S2PTW | ARM_SMMU_STE_2_S2R;
	ste[3] = config->root_table_hpa & ARM_SMMU_STE_3_S2TTB_MASK;
	ste[4] = 0UL;
	ste[5] = 0UL;
	ste[6] = 0UL;
	ste[7] = 0UL;
	flush_cache_range(&ste[1], ARM_SMMU_STE_SIZE - sizeof(uint64_t));

	ret = arm_smmu_sync_ste_locked(stream_id);
	if (ret != 0) {
		goto rollback_abort;
	}

	ste[0] = ARM_SMMU_STE_0_V |
		(ARM_SMMU_STE_0_CFG_S2_TRANS << ARM_SMMU_STE_0_CFG_SHIFT);
	flush_cache_range(&ste[0], sizeof(ste[0]));

	ret = arm_smmu_sync_ste_locked(stream_id);
	if (ret == 0) {
		return 0;
	}

rollback_abort:
	/*
	 * Software ownership is not published until both S2 phases complete. If a
	 * queue error already degraded the instance, global ABORT is the hardware
	 * containment boundary; still replace the memory image so the next verified
	 * rebuild cannot revive a partially published S2 STE.
	 */
	arm_smmu_write_abort_ste_locked(stream_id);
	if (arm_smmu_hw.ready && arm_smmu_hw.cmdq_enabled) {
		int32_t rollback_ret = arm_smmu_sync_ste_locked(stream_id);

		if (rollback_ret != 0) {
			(void)arm_smmu_degrade_locked("STE rollback failure", rollback_ret);
		}
	}

	return ret;
}

static void arm_smmu_clear_stream_fault_locked(struct arm_smmu_stream_state *stream)
{
	if (stream == NULL) {
		return;
	}

	stream->fault_count = 0U;
	stream->last_fault_code = 0U;
	stream->last_fault_iova = 0UL;
	stream->quarantined = false;
}

static void arm_smmu_quarantine_stream_locked(uint32_t stream_id,
	uint32_t event_code, uint64_t iova)
{
	struct arm_smmu_stream_state *stream;

	stream = arm_smmu_find_stream(stream_id);
	if (stream == NULL) {
		stream = arm_smmu_alloc_stream(stream_id);
	}
	if (stream == NULL) {
		arm_smmu_hw.evtq_errors++;
		return;
	}

	if (stream->fault_count != UINT32_MAX) {
		stream->fault_count++;
	}
	stream->last_fault_code = event_code;
	stream->last_fault_iova = iova;
	stream->quarantined = true;
	arm_smmu_hw.evtq_quarantined++;

	/*
	 * Fault containment:
	 *
	 *   EVTQ record identifies SID
	 *       |
	 *       v
	 *   remember owner/fault data for shell diagnostics
	 *       |
	 *       v
	 *   STE := ABORT
	 *       |
	 *       v
	 *   CMDQ CFGI_STE + CMD_SYNC
	 *
	 * The software owner is kept so pcistat can show which VM owned the
	 * device when DMA faulted. The hardware path is closed immediately.
	 */
	if (arm_smmu_stream_in_strtab(stream_id)) {
		arm_smmu_write_abort_ste_locked(stream_id);
		if (arm_smmu_sync_ste_locked(stream_id) != 0) {
			arm_smmu_hw.evtq_errors++;
		}
	}
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
		bits = 48U;
		break;
	case ARM_SMMU_IDR5_OAS_52_BIT:
		bits = 52U;
		break;
	case ARM_SMMU_IDR5_OAS_56_BIT:
		bits = 56U;
		break;
	default:
		bits = 0U;
		break;
	}

	return bits;
}

static bool arm_smmu_range_last(uint64_t base, uint64_t size, uint64_t *last)
{
	if ((last == NULL) || (size == 0UL) || (base > (UINT64_MAX - (size - 1UL)))) {
		return false;
	}

	*last = base + size - 1UL;
	return true;
}

static bool arm_smmu_include_range(uint64_t *maximum, uint64_t base, uint64_t size)
{
	uint64_t last;

	if ((maximum == NULL) || !arm_smmu_range_last(base, size, &last)) {
		return false;
	}
	if (last > *maximum) {
		*maximum = last;
	}

	return true;
}

static uint32_t arm_smmu_address_bits(uint64_t address)
{
	uint32_t bits = 0U;

	do {
		bits++;
		address >>= 1U;
	} while (address != 0UL);

	return bits;
}

static bool arm_smmu_mmio_window_valid(uint64_t base, uint64_t size)
{
	const struct arm64_mem_region *regions;
	uint64_t window_last;
	uint32_t count = 0U;
	uint32_t i;

	if ((base == 0UL) || ((base & (ARM_SMMU_MMIO_ALIGNMENT - 1UL)) != 0UL) ||
		(size < ARM_SMMU_MMIO_SIZE) || !arm_smmu_range_last(base, size,
		&window_last)) {
		return false;
	}

	regions = arm64_get_platform_mmio_regions(&count);
	for (i = 0U; (regions != NULL) && (i < count); i++) {
		uint64_t region_last;

		if (arm_smmu_range_last(regions[i].base, regions[i].size,
			&region_last) && (base >= regions[i].base) &&
			(window_last <= region_last)) {
			return true;
		}
	}

	return false;
}

static uint64_t arm_smmu_validate_caps_locked(uint64_t strtab_pa,
	uint64_t cmdq_pa, uint64_t evtq_pa)
{
	const struct arm64_mem_region *mmio_regions;
	uint32_t idr0 = arm_smmu_hw.idr0;
	uint32_t idr1 = arm_smmu_hw.idr1;
	uint32_t ttendian = (idr0 & ARM_SMMU_IDR0_TTENDIAN_MASK) >>
		ARM_SMMU_IDR0_TTENDIAN_SHIFT;
	uint32_t stall_model = (idr0 & ARM_SMMU_IDR0_STALL_MODEL_MASK) >>
		ARM_SMMU_IDR0_STALL_MODEL_SHIFT;
	uint32_t ttf = (idr0 & ARM_SMMU_IDR0_TTF_MASK) >> ARM_SMMU_IDR0_TTF_SHIFT;
	uint64_t maximum_pa = 0UL;
	uint64_t failures = 0UL;
	uint32_t mmio_count = 0U;
	uint32_t i;
	bool ranges_valid = true;

	arm_smmu_hw.sid_bits = idr1 & ARM_SMMU_IDR1_SIDSIZE_MASK;
	arm_smmu_hw.oas_bits = arm_smmu_oas_bits(arm_smmu_hw.idr5);
	arm_smmu_hw.effective_oas_bits = arm_smmu_min_u32(arm_smmu_hw.oas_bits,
		ARM_SMMU_CPU_STAGE2_OAS_BITS);
	arm_smmu_hw.vmid_bits = ((idr0 & ARM_SMMU_IDR0_VMID16) != 0U) ? 16U : 8U;
	arm_smmu_hw.policy_present = passthrough_get_max_stream_id(
		&arm_smmu_hw.policy_max_sid);

	if ((idr0 & ARM_SMMU_IDR0_S2P) == 0U) {
		failures |= ARM_SMMU_CAP_FAIL_S2P;
	}
	if ((ttf & ARM_SMMU_IDR0_TTF_AARCH64) == 0U) {
		failures |= ARM_SMMU_CAP_FAIL_TTF;
	}
	if ((ttendian != ARM_SMMU_IDR0_TTENDIAN_MIXED) &&
		(ttendian != ARM_SMMU_IDR0_TTENDIAN_LE)) {
		failures |= ARM_SMMU_CAP_FAIL_TTENDIAN;
	}
	if ((arm_smmu_hw.idr5 & ARM_SMMU_IDR5_GRAN4K) == 0U) {
		failures |= ARM_SMMU_CAP_FAIL_GRAN4K;
	}
	if ((idr0 & ARM_SMMU_IDR0_COHACC) == 0U) {
		failures |= ARM_SMMU_CAP_FAIL_COHACC;
	}
	if ((stall_model == ARM_SMMU_IDR0_STALL_MODEL_FORCE) || (stall_model == 3U)) {
		failures |= ARM_SMMU_CAP_FAIL_STALL_MODEL;
	}
	if ((idr1 & ARM_SMMU_IDR1_TABLES_PRESET) != 0U) {
		failures |= ARM_SMMU_CAP_FAIL_TABLES_PRESET;
	}
	if ((idr1 & ARM_SMMU_IDR1_QUEUES_PRESET) != 0U) {
		failures |= ARM_SMMU_CAP_FAIL_QUEUES_PRESET;
	}
	if ((idr1 & ARM_SMMU_IDR1_REL) != 0U) {
		failures |= ARM_SMMU_CAP_FAIL_RELATIVE;
	}
	if (arm_smmu_cmdq_log2_entries(idr1) < ARM_SMMU_QUEUE_LOG2_ENTRIES) {
		failures |= ARM_SMMU_CAP_FAIL_CMDQ;
	}
	if (arm_smmu_evtq_log2_entries(idr1) < ARM_SMMU_QUEUE_LOG2_ENTRIES) {
		failures |= ARM_SMMU_CAP_FAIL_EVTQ;
	}
	if (arm_smmu_hw.sid_bits > 32U) {
		failures |= ARM_SMMU_CAP_FAIL_SID;
	}
	if (arm_smmu_hw.policy_present &&
		((arm_smmu_hw.policy_max_sid >= ARM_SMMU_MAX_SW_STREAMS) ||
		 ((arm_smmu_hw.sid_bits < 32U) &&
		  ((uint64_t)arm_smmu_hw.policy_max_sid >=
		   (1UL << arm_smmu_hw.sid_bits))))) {
		failures |= ARM_SMMU_CAP_FAIL_POLICY_SID;
	}
	if (CONFIG_MAX_VM_NUM > ((1UL << arm_smmu_hw.vmid_bits) - 1UL)) {
		failures |= ARM_SMMU_CAP_FAIL_VMID;
	}

	ranges_valid &= arm_smmu_include_range(&maximum_pa, arm64_get_phys_mem_start(),
		arm64_get_phys_mem_size());
	ranges_valid &= arm_smmu_include_range(&maximum_pa, strtab_pa,
		sizeof(arm_smmu_strtab));
	ranges_valid &= arm_smmu_include_range(&maximum_pa, cmdq_pa,
		sizeof(arm_smmu_cmdq));
	ranges_valid &= arm_smmu_include_range(&maximum_pa, evtq_pa,
		sizeof(arm_smmu_evtq));
	mmio_regions = arm64_get_platform_mmio_regions(&mmio_count);
	if ((mmio_regions == NULL) && (mmio_count != 0U)) {
		ranges_valid = false;
	}
	for (i = 0U; (mmio_regions != NULL) && (i < mmio_count); i++) {
		ranges_valid &= arm_smmu_include_range(&maximum_pa,
			mmio_regions[i].base, mmio_regions[i].size);
	}
	if (beau_config.gits_size != 0UL) {
		ranges_valid &= arm_smmu_include_range(&maximum_pa, beau_config.gits_base,
			beau_config.gits_size);
	}
	arm_smmu_hw.required_oas_bits = ranges_valid ?
		arm_smmu_address_bits(maximum_pa) : 64U;
	if (!ranges_valid || (arm_smmu_hw.effective_oas_bits == 0U) ||
		(arm_smmu_hw.required_oas_bits > arm_smmu_hw.effective_oas_bits) ||
		((strtab_pa & ~ARM_SMMU_STRTAB_BASE_ADDR_MASK) != 0UL) ||
		((cmdq_pa & ~ARM_SMMU_Q_BASE_ADDR_MASK) != 0UL) ||
		((evtq_pa & ~ARM_SMMU_Q_BASE_ADDR_MASK) != 0UL)) {
		failures |= ARM_SMMU_CAP_FAIL_OAS;
	}

	return failures;
}

static uint32_t arm_smmu_cmdq_log2_entries(uint32_t idr1)
{
	return (idr1 & ARM_SMMU_IDR1_CMDQS_MASK) >> ARM_SMMU_IDR1_CMDQS_SHIFT;
}

static uint32_t arm_smmu_evtq_log2_entries(uint32_t idr1)
{
	return (idr1 & ARM_SMMU_IDR1_EVTQS_MASK) >> ARM_SMMU_IDR1_EVTQS_SHIFT;
}

static bool arm_smmu_evtq_supported_locked(void)
{
	return (arm_smmu_evtq_log2_entries(arm_smmu_hw.idr1) >=
		ARM_SMMU_QUEUE_LOG2_ENTRIES) &&
		(arm_smmu_hw.size >= ARM_SMMU_PAGE1_EVTQ_SIZE);
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

	ret = arm_smmu_wait_reg32(ARM_SMMU_GBPA, ARM_SMMU_GBPA_UPDATE, 0U);
	if (ret == 0) {
		reg = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_GBPA));
		arm_smmu_hw.gbpa = reg;
		if (((reg & set) != set) || ((reg & clear) != 0U)) {
			ret = -EIO;
		}
	}

	return ret;
}

static uint32_t arm_smmu_cr0_disabled_value(void)
{
	return ((arm_smmu_hw.idr0 & ARM_SMMU_IDR0_ATS) != 0U) ?
		ARM_SMMU_CR0_ATSCHK : 0U;
}

static int32_t arm_smmu_write_cr0_locked(uint32_t enables)
{
	uint32_t value = (enables & ARM_SMMU_CR0_ENABLE_MASK) |
		arm_smmu_cr0_disabled_value();
	uint32_t readback;
	int32_t ret;

	/* PRI is unsupported here; keep PRIQEN clear in every CR0 state. */
	mmio_write32(value, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR0));
	ret = arm_smmu_wait_reg32(ARM_SMMU_CR0ACK, ARM_SMMU_CR0_ACK_MASK,
		value);
	readback = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR0));
	arm_smmu_hw.cr0 = readback;
	if ((ret == 0) && ((readback & ARM_SMMU_CR0_ACK_MASK) != value)) {
		ret = -EIO;
	}

	return ret;
}

static int32_t arm_smmu_force_containment_locked(bool sticky_failure)
{
	int32_t gbpa_ret;
	int32_t cr0_ret;

	arm_smmu_assignment_hw_ready = false;
	arm_smmu_hw_ready = false;
	arm_smmu_hw.ready = false;
	arm_smmu_hw.cmdq_enabled = false;
	arm_smmu_hw.evtq_enabled = false;

	/* Both operations are best effort: a GBPA failure must not skip CR0 disable. */
	gbpa_ret = arm_smmu_update_gbpa(ARM_SMMU_GBPA_ABORT, 0U);
	cr0_ret = arm_smmu_write_cr0_locked(0U);
	arm_smmu_hw.gbpa = mmio_read32(arm_smmu_reg(arm_smmu_hw.base,
		ARM_SMMU_GBPA));
	arm_smmu_hw.aborted = (gbpa_ret == 0) && (cr0_ret == 0) &&
		((arm_smmu_hw.gbpa & ARM_SMMU_GBPA_ABORT) != 0U);
	arm_smmu_hw.state = sticky_failure ? ARM_SMMU_STATE_FAILED :
		(arm_smmu_hw.aborted ? ARM_SMMU_STATE_ABORT : ARM_SMMU_STATE_FAILED);

	return (gbpa_ret != 0) ? gbpa_ret : cr0_ret;
}

static int32_t arm_smmu_degrade_locked(const char *reason, int32_t status)
{
	int32_t contain_ret;

	/* [20260716] Runtime SMMU degradation
	 *
	 *   CMDQ CERROR / timeout / rollback failure
	 *       -> close assignment gate
	 *       -> GBPA.ABORT
	 *       -> CR0 safe-disabled
	 *       -> DEGRADED only after containment is confirmed
	 *
	 * Key rule:
	 *   - callers already hold arm_smmu_lock and must not publish ownership;
	 *   - containment failure is FAILED because DMA isolation is unproven;
	 *   - the original command status is returned for actionable diagnostics.
	 */
	contain_ret = arm_smmu_force_containment_locked(false);
	if (contain_ret == 0) {
		arm_smmu_hw.state = ARM_SMMU_STATE_DEGRADED;
	} else {
		arm_smmu_hw.state = ARM_SMMU_STATE_FAILED;
	}
	LOG_ERR("SMMUv3: %s, state=%s status=%d containment=%d",
		(reason != NULL) ? reason : "runtime failure",
		(contain_ret == 0) ? "degraded" : "failed", status, contain_ret);

	return status;
}

static void arm_smmu_log_cap_failures(uint64_t failures)
{
	if ((failures & ARM_SMMU_CAP_FAIL_S2P) != 0UL) {
		LOG_ERR("SMMUv3: capability failure S2P (bit 0x2)");
	}
	if ((failures & ARM_SMMU_CAP_FAIL_OAS) != 0UL) {
		LOG_ERR("SMMUv3: capability failure OAS required=%u effective=%u hw=%u",
			arm_smmu_hw.required_oas_bits, arm_smmu_hw.effective_oas_bits,
			arm_smmu_hw.oas_bits);
	}
	if ((failures & ARM_SMMU_CAP_FAIL_POLICY_SID) != 0UL) {
		LOG_ERR("SMMUv3: capability failure policy SID max=0x%x sid-bits=%u table=%u",
			arm_smmu_hw.policy_max_sid, arm_smmu_hw.sid_bits,
			ARM_SMMU_MAX_SW_STREAMS);
	}
}

static int32_t arm_smmu_program_memory_attrs_locked(uint32_t cr1, uint32_t cr2)
{
	uint32_t cr1_read;
	uint32_t cr2_read;

	mmio_write32(cr1, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR1));
	mmio_write32(cr2, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR2));
	cr1_read = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR1));
	cr2_read = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR2));
	arm_smmu_hw.cr1 = cr1_read;
	arm_smmu_hw.cr2 = cr2_read;

	if ((cr1_read != cr1) || ((cr2_read & ARM_SMMU_CR2_RECINVSID) !=
		(cr2 & ARM_SMMU_CR2_RECINVSID))) {
		arm_smmu_hw.cap_fail |= ARM_SMMU_CAP_FAIL_REG_CONFIG;
		arm_smmu_hw.caps_valid = false;
		return -EIO;
	}

	return 0;
}

static bool arm_smmu_pm_snapshot_valid_locked(void)
{
	uint32_t expected_cr0 = ARM_SMMU_CR0_SMMUEN | ARM_SMMU_CR0_CMDQEN |
		arm_smmu_cr0_disabled_value();
	uint32_t cr0ack;
	uint32_t irq_ctrlack;

	if (arm_smmu_hw.evtq_enabled) {
		expected_cr0 |= ARM_SMMU_CR0_EVTQEN;
	}
	cr0ack = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR0ACK));
	irq_ctrlack = mmio_read32(arm_smmu_reg(arm_smmu_hw.base,
		ARM_SMMU_IRQ_CTRLACK));

	return ((arm_smmu_pm_state.cr0 & ARM_SMMU_CR0_ACK_MASK) == expected_cr0) &&
		((cr0ack & ARM_SMMU_CR0_ACK_MASK) == expected_cr0) &&
		(arm_smmu_pm_state.cr1 == arm_smmu_hw.cr1) &&
		((arm_smmu_pm_state.cr2 & ARM_SMMU_CR2_RECINVSID) ==
		 (arm_smmu_hw.cr2 & ARM_SMMU_CR2_RECINVSID)) &&
		((arm_smmu_pm_state.gbpa & ARM_SMMU_GBPA_ABORT) != 0U) &&
		(irq_ctrlack == arm_smmu_pm_state.irq_ctrl);
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
	uint32_t cr0_enables;
	bool evtq_supported;
	int32_t contain_ret;
	int32_t ret;

	if (!arm_smmu_hw.discovered) {
		return ARM_SMMU_INIT_UNDISCOVERED;
	}
	ret = arm_smmu_force_containment_locked(false);
	if (ret != 0) {
		return ret;
	}

	strtab_pa = hva2hpa(arm_smmu_strtab);
	cmdq_pa = hva2hpa(arm_smmu_cmdq);
	evtq_pa = hva2hpa(arm_smmu_evtq);
	arm_smmu_hw.cap_fail = arm_smmu_validate_caps_locked(strtab_pa, cmdq_pa, evtq_pa);
	arm_smmu_hw.caps_valid = arm_smmu_hw.cap_fail == 0UL;
	if (!arm_smmu_hw.caps_valid) {
		LOG_ERR("SMMUv3: strict capability gate failed: 0x%lx", arm_smmu_hw.cap_fail);
		arm_smmu_log_cap_failures(arm_smmu_hw.cap_fail);
		ret = -ENODEV;
		goto fail_contain;
	}

	sid_bits = arm_smmu_hw.sid_bits;
	strtab_log2 = arm_smmu_strtab_log2_entries(sid_bits);
	arm_smmu_zero_abort_tables(strtab_log2);
	strtab_cfg = ARM_SMMU_STRTAB_BASE_CFG_FMT_LINEAR | strtab_log2;
	evtq_supported = arm_smmu_evtq_supported_locked();

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
	 * EVTQ is enabled only when the advertised queue depth can hold the
	 * fixed EL2 buffer and the MMIO window covers page1 producer/consumer
	 * registers. Its IRQ line remains masked; diagnostics poll events and
	 * quarantine the faulting stream.
	 */
	ret = arm_smmu_program_memory_attrs_locked(ARM_SMMU_CR1_STRICT_WB_ISH,
		ARM_SMMU_CR2_RECINVSID);
	if (ret != 0) {
		goto fail_contain;
	}

	mmio_write64((strtab_pa & ARM_SMMU_STRTAB_BASE_ADDR_MASK) |
		ARM_SMMU_STRTAB_BASE_RA, arm_smmu_reg(arm_smmu_hw.base,
		ARM_SMMU_STRTAB_BASE));
	mmio_write32(strtab_cfg, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_STRTAB_BASE_CFG));
	mmio_write64((cmdq_pa & ARM_SMMU_Q_BASE_ADDR_MASK) | ARM_SMMU_Q_BASE_RWA |
		ARM_SMMU_QUEUE_LOG2_ENTRIES,
		arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_BASE));
	arm_smmu_cmdq_prod = 0U;
	arm_smmu_hw.cmdq_prod = 0U;
	arm_smmu_hw.cmdq_cons = 0U;
	arm_smmu_hw.cmdq_last_cons = 0U;
	arm_smmu_hw.cmdq_last_ret = 0;
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_PROD));
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_CONS));
	if (evtq_supported) {
		mmio_write64((evtq_pa & ARM_SMMU_Q_BASE_ADDR_MASK) | ARM_SMMU_Q_BASE_RWA |
			ARM_SMMU_QUEUE_LOG2_ENTRIES,
			arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_BASE));
		arm_smmu_hw.evtq_prod = 0U;
		arm_smmu_hw.evtq_cons = 0U;
		arm_smmu_hw.evtq_last_prod = 0U;
		arm_smmu_hw.evtq_last_cons = 0U;
		mmio_write32(0U, arm_smmu_page1_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_PROD));
		mmio_write32(0U, arm_smmu_page1_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_CONS));
	}

	ret = arm_smmu_update_gbpa(ARM_SMMU_GBPA_ABORT, 0U);
	if (ret != 0) {
		goto fail_contain;
	}
	mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_IRQ_CTRL));
	ret = arm_smmu_wait_reg32(ARM_SMMU_IRQ_CTRLACK, UINT32_MAX, 0U);
	if (ret != 0) {
		goto fail_contain;
	}

	cr0_enables = ARM_SMMU_CR0_CMDQEN | ARM_SMMU_CR0_SMMUEN;
	if (evtq_supported) {
		cr0_enables |= ARM_SMMU_CR0_EVTQEN;
	}
	ret = arm_smmu_write_cr0_locked(cr0_enables);
	if (ret != 0) {
		goto fail_contain;
	}

	arm_smmu_hw.strtab_base = strtab_pa;
	arm_smmu_hw.cmdq_base = cmdq_pa;
	arm_smmu_hw.evtq_base = evtq_supported ? evtq_pa : 0UL;
	arm_smmu_hw.sid_bits = sid_bits;
	arm_smmu_hw.strtab_log2_entries = strtab_log2;
	arm_smmu_hw.cmdq_entries = ARM_SMMU_QUEUE_ENTRIES;
	arm_smmu_hw.evtq_entries = evtq_supported ? ARM_SMMU_QUEUE_ENTRIES : 0U;
	arm_smmu_hw.gbpa = mmio_read32(arm_smmu_reg(arm_smmu_hw.base,
		ARM_SMMU_GBPA));
	arm_smmu_hw.aborted = (arm_smmu_hw.gbpa & ARM_SMMU_GBPA_ABORT) != 0U;
	arm_smmu_hw.cmdq_enabled = true;
	arm_smmu_hw.evtq_enabled = evtq_supported;
	arm_smmu_hw.ready = arm_smmu_hw.caps_valid;
	arm_smmu_hw.state = ARM_SMMU_STATE_READY;
	arm_smmu_hw_ready = arm_smmu_hw.caps_valid;
	arm_smmu_assignment_hw_ready = arm_smmu_hw.caps_valid;

	return 0;

fail_contain:
	contain_ret = arm_smmu_force_containment_locked(false);
	if (contain_ret != 0) {
		LOG_ERR("SMMUv3: containment after init failure also failed: %d",
			contain_ret);
	}
	return ret;
}

void arm_smmu_probe(uint64_t base, uint64_t size)
{
	uint64_t flags;
	int32_t ret;

	/* [20260716] Strict SMMUv3 capability gate
	 *
	 *   platform.dts MMIO
	 *       -> GBPA.ABORT confirmed
	 *       -> CR0 disabled and acknowledged
	 *       -> mandatory capabilities validated
	 *       -> CR1/CR2 + tables/queues programmed
	 *       -> CR0 enabled and READY published
	 *
	 * Key rule:
	 *   - capability failure leaves the instance disabled in global ABORT;
	 *   - assignment readiness is published only after every register readback;
	 *   - SMMU table memory and platform DMA targets must fit the probed OAS.
	 */
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	(void)memset(&arm_smmu_hw, 0U, sizeof(arm_smmu_hw));
	arm_smmu_hw.base = base;
	arm_smmu_hw.size = size;
	arm_smmu_hw.strict = true;
	arm_smmu_hw.probed = true;
	arm_smmu_hw.state = ARM_SMMU_STATE_UNDISCOVERED;
	arm_smmu_hw.init_status = ARM_SMMU_INIT_UNDISCOVERED;
	arm_smmu_hw_ready = false;
	arm_smmu_assignment_hw_ready = false;
	if (!arm_smmu_mmio_window_valid(base, size)) {
		arm_smmu_hw.cap_fail = ARM_SMMU_CAP_FAIL_MMIO;
		arm_smmu_hw.state = ARM_SMMU_STATE_FAILED;
		arm_smmu_hw.init_status = -EINVAL;
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		LOG_ERR("invalid SMMUv3 dts window base=0x%lx size=0x%lx", base, size);
		return;
	}
	arm_smmu_hw.idr0 = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IDR0));
	arm_smmu_hw.idr1 = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IDR1));
	arm_smmu_hw.idr5 = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IDR5));
	arm_smmu_hw.iidr = mmio_read32(arm_smmu_reg(base, ARM_SMMU_IIDR));
	arm_smmu_hw.aidr = mmio_read32(arm_smmu_reg(base, ARM_SMMU_AIDR));
	arm_smmu_hw.discovered = true;
	arm_smmu_hw.ready = false;
	ret = arm_smmu_hw_enable_abort_locked();
	arm_smmu_hw.init_status = ret;
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	if (ret != 0) {
		LOG_ERR("SMMUv3: discovered at 0x%lx but abort-default init failed: %d",
			base, ret);
	}
}

int32_t arm_smmu_pm_suspend(uint64_t epoch)
{
	uint64_t flags;
	int32_t contain_ret;
	int32_t ret;

	if (epoch == 0UL) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	if (arm_smmu_pm_state.valid) {
		ret = (arm_smmu_pm_state.suspend_epoch == epoch) ? 0 : -EBUSY;
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		return ret;
	}
	if (!arm_smmu_hw.discovered || !arm_smmu_hw.ready ||
		(arm_smmu_hw.base == 0UL)) {
		arm_smmu_pm_state.suspend_epoch = epoch;
		arm_smmu_pm_state.hardware_active = false;
		arm_smmu_pm_state.valid = true;
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		return 0;
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	ret = arm_smmu_cmdq_sync();
	if (ret != 0) {
		spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
		contain_ret = arm_smmu_force_containment_locked(true);
		(void)memset(&arm_smmu_pm_state, 0U, sizeof(arm_smmu_pm_state));
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		if (contain_ret != 0) {
			LOG_ERR("SMMUv3: suspend sync and containment failed: %d/%d",
				ret, contain_ret);
		}
		return ret;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	arm_smmu_pm_state.suspend_epoch = epoch;
	arm_smmu_pm_state.strtab_base = mmio_read64(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_STRTAB_BASE));
	arm_smmu_pm_state.strtab_base_cfg = mmio_read32(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_STRTAB_BASE_CFG));
	arm_smmu_pm_state.cmdq_base = mmio_read64(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_CMDQ_BASE));
	arm_smmu_pm_state.evtq_base = arm_smmu_hw.evtq_enabled ?
		mmio_read64(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_BASE)) : 0UL;
	arm_smmu_pm_state.cr0 = mmio_read32(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_CR0));
	arm_smmu_pm_state.cr1 = mmio_read32(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_CR1));
	arm_smmu_pm_state.cr2 = mmio_read32(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_CR2));
	arm_smmu_pm_state.gbpa = mmio_read32(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_GBPA));
	arm_smmu_pm_state.irq_ctrl = mmio_read32(arm_smmu_reg(
		arm_smmu_hw.base, ARM_SMMU_IRQ_CTRL));
	if (!arm_smmu_pm_snapshot_valid_locked()) {
		ret = -EIO;
		contain_ret = arm_smmu_force_containment_locked(true);
		(void)memset(&arm_smmu_pm_state, 0U, sizeof(arm_smmu_pm_state));
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		LOG_ERR("SMMUv3: suspend register snapshot validation failed");
		if (contain_ret != 0) {
			LOG_ERR("SMMUv3: suspend containment failed: %d", contain_ret);
		}
		return ret;
	}
	arm_smmu_pm_state.hardware_active = true;
	arm_smmu_pm_state.valid = true;

	ret = arm_smmu_force_containment_locked(false);
	if (ret != 0) {
		contain_ret = arm_smmu_force_containment_locked(true);
		(void)memset(&arm_smmu_pm_state, 0U, sizeof(arm_smmu_pm_state));
		if (contain_ret != 0) {
			LOG_ERR("SMMUv3: suspend disable and containment failed: %d/%d",
				ret, contain_ret);
		}
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

int32_t arm_smmu_pm_resume(uint64_t epoch)
{
	uint64_t flags;
	uint32_t cr0;
	uint32_t i;
	int32_t contain_ret;
	int32_t ret = 0;

	if (epoch == 0UL) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	if (!arm_smmu_pm_state.valid) {
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		return 0;
	}
	if (arm_smmu_pm_state.suspend_epoch != epoch) {
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		return -EINVAL;
	}
	if ((arm_smmu_hw.state == ARM_SMMU_STATE_FAILED) ||
		!arm_smmu_hw.caps_valid) {
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		return -EIO;
	}
	if (!arm_smmu_pm_state.hardware_active) {
		arm_smmu_pm_state.suspend_epoch = 0UL;
		arm_smmu_pm_state.valid = false;
		spinlock_irqrestore_release(&arm_smmu_lock, flags);
		return 0;
	}

	ret = arm_smmu_update_gbpa(ARM_SMMU_GBPA_ABORT, 0U);
	if (ret == 0) {
		ret = arm_smmu_write_cr0_locked(0U);
	}
	if (ret == 0) {
		ret = arm_smmu_program_memory_attrs_locked(arm_smmu_pm_state.cr1,
			arm_smmu_pm_state.cr2);
	}
	if (ret == 0) {
		/* Resume starts from an ABORT-only table, never retained S2 entries. */
		arm_smmu_zero_abort_tables(arm_smmu_hw.strtab_log2_entries);
		mmio_write64(arm_smmu_pm_state.strtab_base,
			arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_STRTAB_BASE));
		mmio_write32(arm_smmu_pm_state.strtab_base_cfg,
			arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_STRTAB_BASE_CFG));
		mmio_write64(arm_smmu_pm_state.cmdq_base,
			arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CMDQ_BASE));
		arm_smmu_cmdq_prod = 0U;
		mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base,
			ARM_SMMU_CMDQ_PROD));
		mmio_write32(0U, arm_smmu_reg(arm_smmu_hw.base,
			ARM_SMMU_CMDQ_CONS));
		if (arm_smmu_pm_state.evtq_base != 0UL) {
			mmio_write64(arm_smmu_pm_state.evtq_base,
				arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_BASE));
			mmio_write32(0U, arm_smmu_page1_reg(arm_smmu_hw.base,
				ARM_SMMU_EVTQ_PROD));
			mmio_write32(0U, arm_smmu_page1_reg(arm_smmu_hw.base,
				ARM_SMMU_EVTQ_CONS));
		}
		ret = arm_smmu_update_gbpa(ARM_SMMU_GBPA_ABORT, 0U);
	}
	if (ret == 0) {
		mmio_write32(arm_smmu_pm_state.irq_ctrl,
			arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_IRQ_CTRL));
		ret = arm_smmu_wait_reg32(ARM_SMMU_IRQ_CTRLACK, UINT32_MAX,
			arm_smmu_pm_state.irq_ctrl);
	}
	cr0 = arm_smmu_pm_state.cr0 & ARM_SMMU_CR0_ENABLE_MASK;
	if (ret == 0) {
		ret = arm_smmu_write_cr0_locked(cr0);
	}
	if (ret == 0) {
		arm_smmu_hw.cmdq_enabled = (cr0 & ARM_SMMU_CR0_CMDQEN) != 0U;
		arm_smmu_hw.evtq_enabled = (cr0 & ARM_SMMU_CR0_EVTQEN) != 0U;
		for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
			const struct arm_smmu_stream_state *stream = &arm_smmu_streams[i];

			if (stream->used && stream->assigned &&
				(stream->stream_id < (1U << arm_smmu_hw.strtab_log2_entries))) {
				ret = arm_smmu_write_s2_ste_locked(&stream->config,
					stream->stream_id);
				if (ret != 0) {
					break;
				}
			}
		}
	}
	if (ret == 0) {
		arm_smmu_hw.gbpa = mmio_read32(arm_smmu_reg(arm_smmu_hw.base,
			ARM_SMMU_GBPA));
		arm_smmu_hw.aborted = (arm_smmu_hw.gbpa & ARM_SMMU_GBPA_ABORT) != 0U;
		arm_smmu_hw.ready = arm_smmu_hw.caps_valid;
		arm_smmu_hw.state = ARM_SMMU_STATE_READY;
		arm_smmu_hw_ready = arm_smmu_hw.caps_valid;
		arm_smmu_assignment_hw_ready = arm_smmu_hw.caps_valid &&
			((cr0 & (ARM_SMMU_CR0_SMMUEN | ARM_SMMU_CR0_CMDQEN)) ==
			 (ARM_SMMU_CR0_SMMUEN | ARM_SMMU_CR0_CMDQEN));
		arm_smmu_pm_state.suspend_epoch = 0UL;
		arm_smmu_pm_state.hardware_active = false;
		arm_smmu_pm_state.valid = false;
	} else {
		contain_ret = arm_smmu_force_containment_locked(true);
		(void)memset(&arm_smmu_pm_state, 0U, sizeof(arm_smmu_pm_state));
		if (contain_ret != 0) {
			LOG_ERR("SMMUv3: resume failure and containment failed: %d/%d",
				ret, contain_ret);
		}
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

static void arm_smmu_poll_events_locked(void)
{
	uint32_t prod_raw;
	uint32_t cons_raw;
	uint32_t prod;
	uint32_t cons;
	uint32_t budget = 0U;

	if (!arm_smmu_hw.ready || !arm_smmu_hw.evtq_enabled ||
		(arm_smmu_hw.base == 0UL)) {
		return;
	}

	prod_raw = mmio_read32(arm_smmu_page1_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_PROD));
	cons_raw = mmio_read32(arm_smmu_page1_reg(arm_smmu_hw.base, ARM_SMMU_EVTQ_CONS));
	prod = prod_raw & ARM_SMMU_Q_PTR_MASK;
	cons = cons_raw & ARM_SMMU_Q_PTR_MASK;
	arm_smmu_hw.evtq_prod = prod_raw;
	arm_smmu_hw.evtq_cons = cons_raw;
	arm_smmu_hw.evtq_last_prod = prod_raw;
	arm_smmu_hw.evtq_last_cons = cons_raw;
	arm_smmu_hw.evtq_polled++;

	if (((prod_raw | cons_raw) & ARM_SMMU_Q_ERR_MASK) != 0U) {
		arm_smmu_hw.evtq_errors++;
	}

	while ((cons != prod) && (budget < ARM_SMMU_QUEUE_ENTRIES)) {
		uint64_t evt[ARM_SMMU_EVT_DWORDS];
		uint32_t idx = arm_smmu_evtq_index(cons);
		uint32_t i;

		/*
		 * EVTQ entries are written by the SMMU and consumed by EL2. The
		 * available cache helper cleans+invalidates; after initialization
		 * EL2 does not dirty EVTQ entries, so this is used here as the
		 * local stale-line eviction point before reading the event words.
		 */
		flush_cache_range(arm_smmu_evtq[idx], sizeof(arm_smmu_evtq[idx]));
		for (i = 0U; i < ARM_SMMU_EVT_DWORDS; i++) {
			evt[i] = arm_smmu_evtq[idx][i];
		}

		arm_smmu_hw.evtq_last_word0 = evt[0];
		arm_smmu_hw.evtq_last_word1 = evt[1];
		arm_smmu_hw.evtq_last_word2 = evt[2];
		arm_smmu_hw.evtq_last_word3 = evt[3];
		arm_smmu_hw.evtq_events++;
		arm_smmu_quarantine_stream_locked(arm_smmu_evt_sid(evt),
			arm_smmu_evt_code(evt), arm_smmu_evt_iova(evt));

		cons = arm_smmu_queue_next(cons);
		arm_smmu_hw.evtq_cons = cons;
		mmio_write32(cons, arm_smmu_page1_reg(arm_smmu_hw.base,
			ARM_SMMU_EVTQ_CONS));
		budget++;
	}

	if (cons != prod) {
		arm_smmu_hw.evtq_overflow++;
		arm_smmu_hw.evtq_cons = cons;
		mmio_write32(cons, arm_smmu_page1_reg(arm_smmu_hw.base,
			ARM_SMMU_EVTQ_CONS));
	}
}

void arm_smmu_poll_events(void)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	arm_smmu_poll_events_locked();
	spinlock_irqrestore_release(&arm_smmu_lock, flags);
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
	if (arm_smmu_hw.discovered && (arm_smmu_hw.base != 0UL)) {
		info->cr0 = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR0));
		info->cr1 = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR1));
		info->cr2 = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_CR2));
		info->gbpa = mmio_read32(arm_smmu_reg(arm_smmu_hw.base, ARM_SMMU_GBPA));
	}
	if (arm_smmu_hw.cmdq_enabled && (arm_smmu_hw.base != 0UL)) {
		info->cmdq_prod = arm_smmu_cmdq_prod;
		info->cmdq_cons = mmio_read32(arm_smmu_reg(arm_smmu_hw.base,
			ARM_SMMU_CMDQ_CONS));
	}
	if (arm_smmu_hw.evtq_enabled && (arm_smmu_hw.base != 0UL)) {
		info->evtq_prod = mmio_read32(arm_smmu_page1_reg(arm_smmu_hw.base,
			ARM_SMMU_EVTQ_PROD));
		info->evtq_cons = mmio_read32(arm_smmu_page1_reg(arm_smmu_hw.base,
			ARM_SMMU_EVTQ_CONS));
	}
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

		(void)memset(&configs[copied], 0U, sizeof(configs[copied]));
		configs[copied].stream_id = stream->stream_id;
		configs[copied].assigned = stream->assigned;
		configs[copied].quarantined = stream->quarantined;
		configs[copied].fault_count = stream->fault_count;
		configs[copied].last_fault_code = stream->last_fault_code;
		configs[copied].last_fault_iova = stream->last_fault_iova;
		configs[copied].owner_vmid = ACRN_INVALID_VMID;
		configs[copied].domain_vmid = ACRN_INVALID_VMID;
		configs[copied].in_strtab = arm_smmu_stream_in_strtab(stream->stream_id);
		configs[copied].strtab_index = configs[copied].in_strtab ?
			stream->stream_id : ARM_SMMU_STREAM_ID_INVALID;
		if (configs[copied].in_strtab) {
			const uint64_t *ste = arm_smmu_strtab[stream->stream_id];
			uint16_t s2vmid;

			/*
			 * Snapshot the STE words while holding arm_smmu_lock so shell output
			 * can correlate software ownership with the hardware descriptor that
			 * DMA will actually consume. Only word0..word3 are printed because
			 * this implementation uses S2 translation; those words contain
			 * validity/CFG, VMID/VTCR, and S2TTB root state.
			 */
			configs[copied].ste[0] = ste[0];
			configs[copied].ste[1] = ste[1];
			configs[copied].ste[2] = ste[2];
			configs[copied].ste[3] = ste[3];
			configs[copied].ste_valid = (ste[0] & ARM_SMMU_STE_0_V) != 0UL;
			configs[copied].ste_cfg = (uint32_t)((ste[0] >> ARM_SMMU_STE_0_CFG_SHIFT) &
				ARM_SMMU_STE_0_CFG_MASK);
			s2vmid = (uint16_t)(ste[2] & ARM_SMMU_STE_2_S2VMID_MASK);
			if (configs[copied].ste_valid &&
				(configs[copied].ste_cfg == ARM_SMMU_STE_0_CFG_S2_TRANS) &&
				(s2vmid != 0U)) {
				configs[copied].domain_vmid = s2vmid - 1U;
			}
		}
		if (stream->assigned) {
			configs[copied].owner_vmid = stream->config.owner_vmid;
			configs[copied].ipa_width = stream->config.ipa_width;
			configs[copied].root_table_hpa = stream->config.root_table_hpa;
		} else {
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
	if ((stream != NULL) && stream->assigned &&
		(stream->config.owner_vmid == vm_id) && !stream->quarantined) {
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
			(void)memset(&arm_smmu_streams[i].config, 0U,
				sizeof(arm_smmu_streams[i].config));
			arm_smmu_streams[i].assigned = false;
			arm_smmu_clear_stream_fault_locked(&arm_smmu_streams[i]);
			return &arm_smmu_streams[i];
		}
	}

	return NULL;
}

static bool arm_smmu_s2_config_equal(const struct arm_smmu_s2_config *left,
	const struct arm_smmu_s2_config *right)
{
	return (left->owner_vmid == right->owner_vmid) &&
		(left->hw_vmid == right->hw_vmid) &&
		(left->ipa_width == right->ipa_width) &&
		(left->root_table_hpa == right->root_table_hpa);
}

static int32_t arm_smmu_validate_s2_config_locked(
	const struct arm_smmu_s2_config *config)
{
	uint64_t root_last;
	int32_t ret = 0;

	if ((config == NULL) || (config->owner_vmid >= CONFIG_MAX_VM_NUM) ||
		(config->hw_vmid != (uint16_t)(config->owner_vmid + 1U)) ||
		(config->ipa_width == 0U) ||
		(config->ipa_width > ARM_SMMU_CPU_STAGE2_OAS_BITS) ||
		(config->root_table_hpa == 0UL) ||
		((config->root_table_hpa & (PAGE_SIZE - 1UL)) != 0UL)) {
		return -EINVAL;
	}

	if (arm_smmu_hw.caps_valid &&
		((config->ipa_width > arm_smmu_hw.effective_oas_bits) ||
		 !arm_smmu_range_last(config->root_table_hpa, PAGE_SIZE, &root_last) ||
		 (arm_smmu_address_bits(root_last) > arm_smmu_hw.effective_oas_bits))) {
		ret = -EINVAL;
	}

	return ret;
}

int32_t arm_smmu_hw_prepare_s2(struct arm_smmu_s2_config *config)
{
	uint64_t flags;
	int32_t ret;

	/* [20260716] SMMU Stage-2 geometry normalization
	 *
	 *   CPU VM stage-2 request (up to 48-bit IPA)
	 *       -> cap by physical SMMU effective OAS/IAS
	 *       -> encode S2T0SZ from the normalized width
	 *       -> retain the same L0 root; unused high L0 entries stay unreachable
	 *
	 * An IPA width wider than IDR5.OAS produces an invalid STE. Narrowing the
	 * SMMU input geometry leaves the shared table layout unchanged; callers keep
	 * all DMA-visible IPA windows within the normalized width.
	 */
	if (config == NULL) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	if (arm_smmu_hw.caps_valid &&
		(config->ipa_width > arm_smmu_hw.effective_oas_bits)) {
		config->ipa_width = arm_smmu_hw.effective_oas_bits;
	}
	ret = arm_smmu_validate_s2_config_locked(config);
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

int32_t arm_smmu_hw_attach_stream(const struct arm_smmu_s2_config *config,
	uint32_t stream_id)
{
	struct arm_smmu_stream_state *stream;
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	ret = arm_smmu_validate_s2_config_locked(config);
	spinlock_irqrestore_release(&arm_smmu_lock, flags);
	if ((ret != 0) || (stream_id == ARM_SMMU_STREAM_ID_INVALID)) {
		ret = (ret != 0) ? ret : -EINVAL;
		arm_smmu_record_stream_result(true, ret);
		return ret;
	}

	/* [20260716] Physical StreamID attach transaction
	 *
	 *   broker config snapshot
	 *       -> reserve an unowned StreamID
	 *       -> publish S2 STE body + valid word with two CMD_SYNC points
	 *       -> copy config snapshot into physical stream truth
	 *
	 * Key rule:
	 *   - one StreamID has one owner snapshot;
	 *   - a queue/rollback failure closes the global assignment gate;
	 *   - no broker ownership is observable before hardware synchronization.
	 */
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	stream = arm_smmu_find_stream(stream_id);
	if (stream == NULL) {
		stream = arm_smmu_alloc_stream(stream_id);
	}
	if (stream == NULL) {
		ret = -ENOMEM;
	} else if (stream->assigned &&
		!arm_smmu_s2_config_equal(&stream->config, config)) {
		ret = -EBUSY;
	} else if (stream->quarantined) {
		ret = -EIO;
	} else if (!arm_smmu_assignment_hw_ready ||
		!arm_smmu_stream_in_strtab(stream_id)) {
		stream->used = false;
		ret = -ENODEV;
	} else if (stream->assigned) {
		ret = 0;
	} else {
		ret = arm_smmu_write_s2_ste_locked(config, stream_id);
		if (ret == 0) {
			stream->config = *config;
			stream->assigned = true;
		} else {
			stream->used = false;
		}
	}
	arm_smmu_record_stream_result_locked(true, ret);
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	if (ret != 0) {
		LOG_ERR("SMMUv3: assignment rejected (%d): stream 0x%x for vm%u",
			ret, stream_id, config->owner_vmid);
	}
	return ret;
}

int32_t arm_smmu_hw_detach_stream(const struct arm_smmu_s2_config *config,
	uint32_t stream_id)
{
	struct arm_smmu_stream_state *stream;
	uint64_t flags;
	int32_t ret = 0;

	if ((config == NULL) || (stream_id == ARM_SMMU_STREAM_ID_INVALID)) {
		arm_smmu_record_stream_result(false, -EINVAL);
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	stream = arm_smmu_find_stream(stream_id);
	if (stream == NULL) {
		ret = -ENODEV;
	} else if (!stream->assigned ||
		!arm_smmu_s2_config_equal(&stream->config, config)) {
		ret = -EPERM;
	} else if (!arm_smmu_assignment_hw_ready ||
		!arm_smmu_stream_in_strtab(stream_id)) {
		ret = -ENODEV;
	} else {
		arm_smmu_write_abort_ste_locked(stream_id);
		ret = arm_smmu_sync_ste_locked(stream_id);
		if (ret == 0) {
			(void)memset(&stream->config, 0U, sizeof(stream->config));
			stream->assigned = false;
			arm_smmu_clear_stream_fault_locked(stream);
			stream->used = false;
		}
	}
	arm_smmu_record_stream_result_locked(false, ret);
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

int32_t arm_smmu_hw_detach_domain(const struct arm_smmu_s2_config *config)
{
	uint64_t flags;
	uint32_t i;
	int32_t ret = 0;

	if (config == NULL) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
		struct arm_smmu_stream_state *stream = &arm_smmu_streams[i];

		if (!stream->used || !stream->assigned ||
			!arm_smmu_s2_config_equal(&stream->config, config)) {
			continue;
		}
		if (!arm_smmu_assignment_hw_ready ||
			!arm_smmu_stream_in_strtab(stream->stream_id)) {
			ret = -ENODEV;
			break;
		}
		arm_smmu_write_abort_ste_locked(stream->stream_id);
		ret = arm_smmu_sync_ste_locked(stream->stream_id);
		if (ret != 0) {
			break;
		}
	}
	if (ret == 0) {
		for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
			struct arm_smmu_stream_state *stream = &arm_smmu_streams[i];

			if (!stream->used || !stream->assigned ||
				!arm_smmu_s2_config_equal(&stream->config, config)) {
				continue;
			}
			(void)memset(&stream->config, 0U, sizeof(stream->config));
			stream->assigned = false;
			arm_smmu_clear_stream_fault_locked(stream);
			stream->used = false;
		}
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return ret;
}

bool arm_smmu_hw_domain_bound(const struct arm_smmu_s2_config *config)
{
	uint64_t flags;
	uint32_t i;
	bool bound = false;

	if (config == NULL) {
		return false;
	}
	spinlock_irqsave_obtain(&arm_smmu_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(arm_smmu_streams); i++) {
		if (arm_smmu_streams[i].used && arm_smmu_streams[i].assigned &&
			arm_smmu_s2_config_equal(&arm_smmu_streams[i].config, config)) {
			bound = true;
			break;
		}
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	return bound;
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

	if (!arm_smmu_assignment_ready()) {
		return -ENOTSUP;
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
	uint32_t dev_id, uint32_t event_id, uint32_t lpi)
{
	uint32_t i;
	uint64_t flags;
	int32_t ret = -ENOMEM;

	spinlock_irqsave_obtain(&arm64_pt_msi_lock, &flags);
	if (arm64_pt_find_msi_state(phys_bdf, entry_nr) != NULL) {
		ret = -EBUSY;
	} else {
		for (i = 0U; i < ARRAY_SIZE(arm64_pt_msi_states); i++) {
			if (!arm64_pt_msi_states[i].used) {
				arm64_pt_msi_states[i].used = true;
				arm64_pt_msi_states[i].phys_bdf = phys_bdf;
				arm64_pt_msi_states[i].entry_nr = entry_nr;
				arm64_pt_msi_states[i].dev_id = dev_id;
				arm64_pt_msi_states[i].event_id = event_id;
				arm64_pt_msi_states[i].lpi = lpi;
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

	if (arm64_pt_take_msi_state(phys_bdf, entry_nr, &state)) {
		arm64_gicv3_its_release_msix(state.dev_id, state.lpi);
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
	int32_t ret;
	bool new_lpi = false;
	DEFINE_MSI_SID(phys_sid, phys_bdf, entry_nr);
	DEFINE_MSI_SID(virt_sid, virt_bdf, entry_nr);

	if ((vm == NULL) || (virt_bdf == 0xffffU) || (info == NULL)) {
		return -EINVAL;
	}
	if (!arm_smmu_assignment_ready()) {
		return -ENOTSUP;
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
		ret = arm64_pt_record_msi_state(phys_bdf, entry_nr, dev_id, event_id, lpi);
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
			uint16_t affinity_pcpu;

			entry->phys_sid = phys_sid;
			entry->virt_sid = virt_sid;
			entry->polarity = 0U;
			if (passthrough_irq_affinity(vm->vm_id, phys_gsi, &affinity_pcpu)) {
				entry->affinity_pcpu = affinity_pcpu;
				entry->affinity_valid = true;
			}
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
