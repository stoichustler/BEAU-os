/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <guest_memory.h>
#include <logmsg.h>
#include <softirq.h>
#include <spinlock.h>
#include <vm.h>
#include <vm_config.h>
#include <bsp/io_req.h>
#include <asm/guest/vsmmu.h>

#define VSMMU_MMIO_SIZE			0x20000UL
#define VSMMU_CMDQ_ENTRY_SIZE		16UL
#define VSMMU_EVTQ_ENTRY_SIZE		32UL
#define VSMMU_STE_SIZE			64UL
#define VSMMU_CMD_BUDGET			32U

#define VSMMU_IDR0			0x00000U
#define VSMMU_IDR1			0x00004U
#define VSMMU_IDR2			0x00008U
#define VSMMU_IDR3			0x0000cU
#define VSMMU_IDR4			0x00010U
#define VSMMU_IDR5			0x00014U
#define VSMMU_IIDR			0x00018U
#define VSMMU_AIDR			0x0001cU
#define VSMMU_CR0			0x00020U
#define VSMMU_CR0ACK			0x00024U
#define VSMMU_CR1			0x00028U
#define VSMMU_CR2			0x0002cU
#define VSMMU_STATUSR			0x00040U
#define VSMMU_GBPA			0x00044U
#define VSMMU_IRQ_CTRL			0x00050U
#define VSMMU_IRQ_CTRLACK		0x00054U
#define VSMMU_GERROR			0x00060U
#define VSMMU_GERRORN			0x00064U
#define VSMMU_STRTAB_BASE		0x00080U
#define VSMMU_STRTAB_BASE_CFG		0x00088U
#define VSMMU_CMDQ_BASE			0x00090U
#define VSMMU_CMDQ_PROD			0x00098U
#define VSMMU_CMDQ_CONS			0x0009cU
#define VSMMU_EVTQ_BASE			0x000a0U
#define VSMMU_EVTQ_PROD			0x100a8U
#define VSMMU_EVTQ_CONS			0x100acU

#define VSMMU_IDR0_STALL_TERMINATE	(1U << 24U)
#define VSMMU_IDR0_TTENDIAN_LE		(2U << 21U)
#define VSMMU_IDR0_ASID16		(1U << 12U)
#define VSMMU_IDR0_COHACC		(1U << 4U)
#define VSMMU_IDR0_TTF_AARCH64		(2U << 2U)
#define VSMMU_IDR0_S1P			(1U << 1U)
#define VSMMU_IDR0_VALUE			(VSMMU_IDR0_STALL_TERMINATE | \
	VSMMU_IDR0_TTENDIAN_LE | VSMMU_IDR0_ASID16 | VSMMU_IDR0_COHACC | \
	VSMMU_IDR0_TTF_AARCH64 | VSMMU_IDR0_S1P)

#define VSMMU_IDR1_CMDQ_SHIFT		21U
#define VSMMU_IDR1_EVTQ_SHIFT		16U
#define VSMMU_IDR1_SID_BITS		6U
#define VSMMU_IDR5_GRAN4K		(1U << 4U)
#define VSMMU_IDR5_OAS48			5U

#define VSMMU_CR0_SMMUEN			(1U << 0U)
#define VSMMU_CR0_EVTQEN			(1U << 2U)
#define VSMMU_CR0_CMDQEN			(1U << 3U)
#define VSMMU_CR0_ALLOWED		(VSMMU_CR0_SMMUEN | \
	VSMMU_CR0_EVTQEN | VSMMU_CR0_CMDQEN)
#define VSMMU_CR1_ALLOWED		0x00000fffU
#define VSMMU_CR2_ALLOWED		0x00000007U

#define VSMMU_GBPA_ABORT			(1U << 20U)
#define VSMMU_GBPA_UPDATE		(1U << 31U)
#define VSMMU_IRQ_GERROR			(1U << 0U)
#define VSMMU_IRQ_EVTQ			(1U << 2U)
#define VSMMU_IRQ_ALLOWED		(VSMMU_IRQ_GERROR | VSMMU_IRQ_EVTQ)
#define VSMMU_GERROR_CMDQ_ERR		(1U << 0U)
#define VSMMU_GERROR_SFM_ERR		(1U << 8U)
#define VSMMU_GERROR_ALLOWED		(VSMMU_GERROR_CMDQ_ERR | \
	VSMMU_GERROR_SFM_ERR)

#define VSMMU_Q_BASE_RWA			(1UL << 62U)
#define VSMMU_Q_BASE_ADDR_MASK		0x000fffffffffffe0UL
#define VSMMU_Q_BASE_LOG2_MASK		0x1fUL
#define VSMMU_Q_BASE_ALLOWED		(VSMMU_Q_BASE_RWA | \
	VSMMU_Q_BASE_ADDR_MASK | VSMMU_Q_BASE_LOG2_MASK)
#define VSMMU_STRTAB_BASE_RA		(1UL << 62U)
#define VSMMU_STRTAB_ADDR_MASK		0x000fffffffffffc0UL
#define VSMMU_STRTAB_BASE_ALLOWED	(VSMMU_STRTAB_BASE_RA | \
	VSMMU_STRTAB_ADDR_MASK)
#define VSMMU_STRTAB_CFG_LOG2_MASK	0x3fU
#define VSMMU_STRTAB_CFG_ALLOWED	VSMMU_STRTAB_CFG_LOG2_MASK

#define VSMMU_CMDQ_CONS_ERR_SHIFT	24U
#define VSMMU_CMDQ_CONS_ERR_MASK		(0x7fU << VSMMU_CMDQ_CONS_ERR_SHIFT)
#define VSMMU_CERROR_NONE		0U
#define VSMMU_CERROR_ILL			1U
#define VSMMU_CERROR_ABT			2U

#define VSMMU_CMD_CFGI_STE		0x03U
#define VSMMU_CMD_CFGI_ALL		0x04U
#define VSMMU_CMD_CFGI_CD		0x05U
#define VSMMU_CMD_CFGI_CD_ALL		0x06U
#define VSMMU_CMD_TLBI_NH_ALL		0x10U
#define VSMMU_CMD_TLBI_NH_ASID		0x11U
#define VSMMU_CMD_TLBI_NH_VA		0x12U
#define VSMMU_CMD_TLBI_NH_VAA		0x13U
#define VSMMU_CMD_TLBI_NSNH_ALL		0x30U
#define VSMMU_CMD_SYNC			0x46U

#define VSMMU_CMD_OP_MASK		0xffUL
#define VSMMU_CMD_CFGI_RANGE_MASK	0x1fUL
#define VSMMU_CMD_TLBI_NUM_MASK		(0x1fUL << 12U)
#define VSMMU_CMD_TLBI_SCALE_MASK	(0x1fUL << 20U)
#define VSMMU_CMD_TLBI_VMID_MASK	(0xffffUL << 32U)
#define VSMMU_CMD_TLBI_ASID_MASK	(0xffffUL << 48U)
#define VSMMU_CMD_TLBI_LEAF_MASK	(1UL << 0U)
#define VSMMU_CMD_TLBI_TTL_MASK		(0x3UL << 8U)
#define VSMMU_CMD_TLBI_TG_MASK		(0x3UL << 10U)
#define VSMMU_CMD_TLBI_VA_MASK		0xfffffffffffff000UL
#define VSMMU_CMD_SYNC_CS_MASK		(0x3UL << 12U)
#define VSMMU_CMD_SYNC_CS_IRQ		(1UL << 12U)
#define VSMMU_CMD_SYNC_MSH_MASK		(0x3UL << 22U)
#define VSMMU_CMD_SYNC_ATTR_MASK	(0xfUL << 24U)

struct arm64_vsmmu {
	spinlock_t lock;
	struct acrn_vm *vm;
	uint64_t strtab_base;
	uint64_t cmdq_base;
	uint64_t evtq_base;
	uint64_t generation;
	uint64_t commands_processed;
	uint64_t commands_rejected;
	uint64_t budget_exhausted;
	uint32_t strtab_cfg;
	uint32_t cmdq_prod;
	uint32_t cmdq_cons;
	uint32_t evtq_prod;
	uint32_t evtq_cons;
	uint32_t cr0;
	uint32_t cr1;
	uint32_t cr2;
	uint32_t gbpa;
	uint32_t irq_ctrl;
	uint32_t gerror;
	uint32_t gerrorn;
	uint16_t worker_pcpu;
	uint8_t max_cmdq_log2;
	uint8_t max_evtq_log2;
	bool worker_pending;
	bool irq_asserted;
	bool initialized;
};

struct vsmmu_cmd_snapshot {
	struct acrn_vm *vm;
	uint64_t generation;
	uint64_t gpa;
	uint32_t cons;
};

static struct arm64_vsmmu vsmmu_devs[CONFIG_MAX_VM_NUM];
static bool vsmmu_softirq_registered;

/* [20260716] Guest-visible SMMUv3 trust boundary
 *
 *   guest SMMU MMIO
 *       -> synthetic register state under vsmmu.lock
 *       -> PROD write schedules a configured-pCPU softirq
 *       -> one command copied once through copy_from_gpa()
 *       -> bounded command whitelist or virtual CERROR/GERROR
 *
 * Key rules:
 *   - this file never reads physical SMMU registers or physical capabilities;
 *   - guest queue/table addresses are GPAs and are validated as complete ranges;
 *   - no guest value becomes a physical SID, VMID, S2TTB, or raw CMDQ entry;
 *   - per-stream commands stay illegal until iommu.c provides the S1+S2 broker;
 *   - no guest PCI iommu-map is published by this skeleton.
 */

static uint32_t vsmmu_idr1(const struct arm64_vsmmu *vsmmu)
{
	return ((uint32_t)vsmmu->max_cmdq_log2 << VSMMU_IDR1_CMDQ_SHIFT) |
		((uint32_t)vsmmu->max_evtq_log2 << VSMMU_IDR1_EVTQ_SHIFT) |
		VSMMU_IDR1_SID_BITS;
}

static uint32_t vsmmu_ring_mask(uint8_t log2_entries)
{
	return (1U << ((uint32_t)log2_entries + 1U)) - 1U;
}

static uint32_t vsmmu_cmdq_log2(const struct arm64_vsmmu *vsmmu)
{
	return (uint32_t)(vsmmu->cmdq_base & VSMMU_Q_BASE_LOG2_MASK);
}

static uint32_t vsmmu_evtq_log2(const struct arm64_vsmmu *vsmmu)
{
	return (uint32_t)(vsmmu->evtq_base & VSMMU_Q_BASE_LOG2_MASK);
}

static bool vsmmu_queue_valid(const struct arm64_vsmmu *vsmmu,
	uint64_t value, uint8_t max_log2, uint64_t entry_size)
{
	uint64_t base = value & VSMMU_Q_BASE_ADDR_MASK;
	uint32_t log2_entries = (uint32_t)(value & VSMMU_Q_BASE_LOG2_MASK);
	uint64_t bytes;

	if (((value & ~VSMMU_Q_BASE_ALLOWED) != 0UL) || (log2_entries == 0U) ||
		(log2_entries > max_log2)) {
		return false;
	}
	bytes = entry_size << log2_entries;
	return ((base & (bytes - 1UL)) == 0UL) &&
		arm64_guest_gpa_range_valid(vsmmu->vm, base, bytes);
}

static bool vsmmu_strtab_valid(const struct arm64_vsmmu *vsmmu)
{
	uint64_t base = vsmmu->strtab_base & VSMMU_STRTAB_ADDR_MASK;
	uint32_t log2_entries = vsmmu->strtab_cfg & VSMMU_STRTAB_CFG_LOG2_MASK;
	uint64_t bytes;

	if (((vsmmu->strtab_base & ~VSMMU_STRTAB_BASE_ALLOWED) != 0UL) ||
		((vsmmu->strtab_cfg & ~VSMMU_STRTAB_CFG_ALLOWED) != 0U) ||
		(log2_entries == 0U) || (log2_entries > VSMMU_IDR1_SID_BITS)) {
		return false;
	}
	bytes = VSMMU_STE_SIZE << log2_entries;
	return ((base & (bytes - 1UL)) == 0UL) &&
		arm64_guest_gpa_range_valid(vsmmu->vm, base, bytes);
}

static bool vsmmu_cmdq_valid(const struct arm64_vsmmu *vsmmu)
{
	return vsmmu_queue_valid(vsmmu, vsmmu->cmdq_base,
		vsmmu->max_cmdq_log2, VSMMU_CMDQ_ENTRY_SIZE);
}

static bool vsmmu_evtq_valid(const struct arm64_vsmmu *vsmmu)
{
	return vsmmu_queue_valid(vsmmu, vsmmu->evtq_base,
		vsmmu->max_evtq_log2, VSMMU_EVTQ_ENTRY_SIZE);
}

static uint32_t vsmmu_active_errors(const struct arm64_vsmmu *vsmmu)
{
	return (vsmmu->gerror ^ vsmmu->gerrorn) & VSMMU_GERROR_ALLOWED;
}

static void vsmmu_raise_error_locked(struct arm64_vsmmu *vsmmu, uint32_t error)
{
	if ((vsmmu_active_errors(vsmmu) & error) == 0U) {
		vsmmu->gerror ^= error;
	}
}

static void vsmmu_fail_control_locked(struct arm64_vsmmu *vsmmu)
{
	vsmmu->cr0 = 0U;
	vsmmu->gbpa = VSMMU_GBPA_ABORT;
	vsmmu->worker_pending = false;
	vsmmu_raise_error_locked(vsmmu, VSMMU_GERROR_SFM_ERR);
}

static void vsmmu_set_cerror_locked(struct arm64_vsmmu *vsmmu, uint32_t cerror)
{
	vsmmu->cmdq_cons &= ~VSMMU_CMDQ_CONS_ERR_MASK;
	vsmmu->cmdq_cons |= cerror << VSMMU_CMDQ_CONS_ERR_SHIFT;
	if (cerror != VSMMU_CERROR_NONE) {
		vsmmu->commands_rejected++;
		vsmmu->worker_pending = false;
		vsmmu_raise_error_locked(vsmmu, VSMMU_GERROR_CMDQ_ERR);
	}
}

static void vsmmu_refresh_irq(struct arm64_vsmmu *vsmmu)
{
	struct acrn_vm *vm;
	uint32_t irq;
	uint64_t flags;
	bool old_asserted;
	bool new_asserted;

	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	vm = vsmmu->vm;
	irq = (vm == NULL) ? 0U :
		get_vm_config(vm->vm_id)->arch.guest_smmu_irq;
	old_asserted = vsmmu->irq_asserted;
	new_asserted = vsmmu->initialized &&
		(((vsmmu_active_errors(vsmmu) != 0U) &&
		 ((vsmmu->irq_ctrl & VSMMU_IRQ_GERROR) != 0U)) ||
		 ((vsmmu->evtq_prod != vsmmu->evtq_cons) &&
		 ((vsmmu->irq_ctrl & VSMMU_IRQ_EVTQ) != 0U)));
	vsmmu->irq_asserted = new_asserted;
	spinlock_irqrestore_release(&vsmmu->lock, flags);

	if ((vm != NULL) && (old_asserted != new_asserted)) {
		arch_trigger_level_intr(vm, irq, new_asserted);
	}
}

static bool vsmmu_command_supported(const uint64_t command[2])
{
	uint8_t opcode = (uint8_t)(command[0] & 0xffUL);
	uint64_t word0_allowed;
	uint64_t word1_allowed;
	uint32_t scale;

	switch (opcode) {
	case VSMMU_CMD_CFGI_ALL:
		return (command[0] == VSMMU_CMD_CFGI_ALL) &&
			(command[1] == VSMMU_CMD_CFGI_RANGE_MASK);
	case VSMMU_CMD_TLBI_NH_ALL:
	case VSMMU_CMD_TLBI_NSNH_ALL:
		return (command[0] == opcode) && (command[1] == 0UL);
	case VSMMU_CMD_TLBI_NH_ASID:
		word0_allowed = VSMMU_CMD_OP_MASK | VSMMU_CMD_TLBI_ASID_MASK;
		return ((command[0] & ~word0_allowed) == 0UL) &&
			((command[0] & VSMMU_CMD_TLBI_VMID_MASK) == 0UL) &&
			(command[1] == 0UL);
	case VSMMU_CMD_TLBI_NH_VA:
	case VSMMU_CMD_TLBI_NH_VAA:
		word0_allowed = VSMMU_CMD_OP_MASK | VSMMU_CMD_TLBI_NUM_MASK |
			VSMMU_CMD_TLBI_SCALE_MASK | VSMMU_CMD_TLBI_ASID_MASK;
		word1_allowed = VSMMU_CMD_TLBI_LEAF_MASK |
			VSMMU_CMD_TLBI_TTL_MASK | VSMMU_CMD_TLBI_TG_MASK |
			VSMMU_CMD_TLBI_VA_MASK;
		scale = (uint32_t)((command[0] & VSMMU_CMD_TLBI_SCALE_MASK) >> 20U);
		return ((command[0] & ~word0_allowed) == 0UL) &&
			((command[0] & VSMMU_CMD_TLBI_VMID_MASK) == 0UL) &&
			((command[1] & ~word1_allowed) == 0UL) && (scale <= 3U);
	case VSMMU_CMD_SYNC:
		word0_allowed = VSMMU_CMD_OP_MASK | VSMMU_CMD_SYNC_CS_MASK |
			VSMMU_CMD_SYNC_MSH_MASK | VSMMU_CMD_SYNC_ATTR_MASK;
		return ((command[0] & ~word0_allowed) == 0UL) &&
			((command[0] & VSMMU_CMD_SYNC_CS_MASK) !=
			 VSMMU_CMD_SYNC_CS_IRQ) && (command[1] == 0UL);
	case VSMMU_CMD_CFGI_STE:
	case VSMMU_CMD_CFGI_CD:
	case VSMMU_CMD_CFGI_CD_ALL:
	default:
		return false;
	}
}

static bool vsmmu_snapshot_command(struct arm64_vsmmu *vsmmu,
	struct vsmmu_cmd_snapshot *snapshot)
{
	uint64_t flags;
	uint32_t log2_entries;
	uint32_t pointer_mask;
	uint32_t cons;
	uint32_t prod;
	bool available = false;

	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	if (vsmmu->initialized &&
		((vsmmu->cr0 & VSMMU_CR0_CMDQEN) != 0U) &&
		(vsmmu_active_errors(vsmmu) == 0U) && vsmmu_cmdq_valid(vsmmu)) {
		log2_entries = vsmmu_cmdq_log2(vsmmu);
		pointer_mask = vsmmu_ring_mask((uint8_t)log2_entries);
		cons = vsmmu->cmdq_cons & pointer_mask;
		prod = vsmmu->cmdq_prod & pointer_mask;
		if (cons != prod) {
			snapshot->vm = vsmmu->vm;
			snapshot->generation = vsmmu->generation;
			snapshot->cons = cons;
			snapshot->gpa =
				(vsmmu->cmdq_base & VSMMU_Q_BASE_ADDR_MASK) +
				((uint64_t)(cons & ((1U << log2_entries) - 1U)) *
				 VSMMU_CMDQ_ENTRY_SIZE);
			available = true;
		}
	}
	spinlock_irqrestore_release(&vsmmu->lock, flags);

	return available;
}

static bool vsmmu_commit_command(struct arm64_vsmmu *vsmmu,
	const struct vsmmu_cmd_snapshot *snapshot, int32_t fetch_ret,
	const uint64_t command[2])
{
	uint64_t flags;
	uint32_t log2_entries;
	uint32_t pointer_mask;
	uint32_t current_cons;
	bool committed = false;

	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	log2_entries = vsmmu_cmdq_log2(vsmmu);
	pointer_mask = vsmmu_ring_mask((uint8_t)log2_entries);
	current_cons = vsmmu->cmdq_cons & pointer_mask;
	if (vsmmu->initialized && (vsmmu->generation == snapshot->generation) &&
		(current_cons == snapshot->cons)) {
		if (fetch_ret != 0) {
			vsmmu_set_cerror_locked(vsmmu, VSMMU_CERROR_ABT);
		} else if (!vsmmu_command_supported(command)) {
			vsmmu_set_cerror_locked(vsmmu, VSMMU_CERROR_ILL);
		} else {
			current_cons = (current_cons + 1U) & pointer_mask;
			vsmmu->cmdq_cons = current_cons;
			vsmmu->commands_processed++;
			vsmmu->worker_pending =
				(current_cons != (vsmmu->cmdq_prod & pointer_mask));
		}
		committed = true;
	}
	spinlock_irqrestore_release(&vsmmu->lock, flags);

	return committed;
}

static void vsmmu_process_batch(struct arm64_vsmmu *vsmmu)
{
	struct vsmmu_cmd_snapshot snapshot;
	uint64_t command[2];
	uint64_t flags;
	uint32_t count;
	int32_t ret;

	for (count = 0U; count < VSMMU_CMD_BUDGET; count++) {
		if (!vsmmu_snapshot_command(vsmmu, &snapshot)) {
			break;
		}
		ret = copy_from_gpa(snapshot.vm, command, snapshot.gpa,
			sizeof(command));
		if (!vsmmu_commit_command(vsmmu, &snapshot, ret, command)) {
			continue;
		}
		if (ret != 0) {
			break;
		}
	}

	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	if ((count == VSMMU_CMD_BUDGET) && vsmmu->worker_pending) {
		/* A later CONS poll schedules the next bounded batch. */
		vsmmu->budget_exhausted++;
	}
	spinlock_irqrestore_release(&vsmmu->lock, flags);
	vsmmu_refresh_irq(vsmmu);
}

static void vsmmu_softirq(uint16_t pcpu_id)
{
	uint16_t vm_id;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct arm64_vsmmu *vsmmu = &vsmmu_devs[vm_id];

		if (vsmmu->initialized && (vsmmu->worker_pcpu == pcpu_id)) {
			vsmmu_process_batch(vsmmu);
		}
	}
}

static bool vsmmu_reg_is_64(uint32_t offset)
{
	return (offset == VSMMU_STRTAB_BASE) || (offset == VSMMU_CMDQ_BASE) ||
		(offset == VSMMU_EVTQ_BASE);
}

static bool vsmmu_access_width_valid(uint32_t offset, uint64_t size)
{
	return vsmmu_reg_is_64(offset) ? (size == 8UL) : (size == 4UL);
}

static int32_t vsmmu_read_locked(const struct arm64_vsmmu *vsmmu,
	uint32_t offset, uint64_t *value)
{
	switch (offset) {
	case VSMMU_IDR0:
		*value = VSMMU_IDR0_VALUE;
		break;
	case VSMMU_IDR1:
		*value = vsmmu_idr1(vsmmu);
		break;
	case VSMMU_IDR2:
	case VSMMU_IDR3:
	case VSMMU_IDR4:
	case VSMMU_STATUSR:
		*value = 0UL;
		break;
	case VSMMU_IDR5:
		*value = VSMMU_IDR5_GRAN4K | VSMMU_IDR5_OAS48;
		break;
	case VSMMU_IIDR:
		*value = 1UL;
		break;
	case VSMMU_AIDR:
		*value = 1UL;
		break;
	case VSMMU_CR0:
	case VSMMU_CR0ACK:
		*value = vsmmu->cr0;
		break;
	case VSMMU_CR1:
		*value = vsmmu->cr1;
		break;
	case VSMMU_CR2:
		*value = vsmmu->cr2;
		break;
	case VSMMU_GBPA:
		*value = vsmmu->gbpa;
		break;
	case VSMMU_IRQ_CTRL:
	case VSMMU_IRQ_CTRLACK:
		*value = vsmmu->irq_ctrl;
		break;
	case VSMMU_GERROR:
		*value = vsmmu->gerror;
		break;
	case VSMMU_GERRORN:
		*value = vsmmu->gerrorn;
		break;
	case VSMMU_STRTAB_BASE:
		*value = vsmmu->strtab_base;
		break;
	case VSMMU_STRTAB_BASE_CFG:
		*value = vsmmu->strtab_cfg;
		break;
	case VSMMU_CMDQ_BASE:
		*value = vsmmu->cmdq_base;
		break;
	case VSMMU_CMDQ_PROD:
		*value = vsmmu->cmdq_prod;
		break;
	case VSMMU_CMDQ_CONS:
		*value = vsmmu->cmdq_cons;
		break;
	case VSMMU_EVTQ_BASE:
		*value = vsmmu->evtq_base;
		break;
	case VSMMU_EVTQ_PROD:
		*value = vsmmu->evtq_prod;
		break;
	case VSMMU_EVTQ_CONS:
		*value = vsmmu->evtq_cons;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int32_t vsmmu_write_locked(struct arm64_vsmmu *vsmmu,
	uint32_t offset, uint64_t value, bool *kick_worker)
{
	uint32_t pointer_mask;
	uint32_t log2_entries;

	switch (offset) {
	case VSMMU_CR0:
		if ((value & ~VSMMU_CR0_ALLOWED) != 0UL) {
			return -EINVAL;
		}
		if ((((value & VSMMU_CR0_CMDQEN) != 0UL) && !vsmmu_cmdq_valid(vsmmu)) ||
			(((value & VSMMU_CR0_EVTQEN) != 0UL) && !vsmmu_evtq_valid(vsmmu)) ||
			(((value & VSMMU_CR0_SMMUEN) != 0UL) && !vsmmu_strtab_valid(vsmmu))) {
			return -EINVAL;
		}
		vsmmu->cr0 = (uint32_t)value;
		vsmmu->generation++;
		vsmmu->worker_pending =
			((vsmmu->cr0 & VSMMU_CR0_CMDQEN) != 0U) &&
			(vsmmu->cmdq_prod != vsmmu->cmdq_cons);
		*kick_worker = vsmmu->worker_pending;
		break;
	case VSMMU_CR1:
		if (((vsmmu->cr0 & VSMMU_CR0_SMMUEN) != 0U) ||
			((value & ~VSMMU_CR1_ALLOWED) != 0UL)) {
			return -EINVAL;
		}
		vsmmu->cr1 = (uint32_t)value;
		break;
	case VSMMU_CR2:
		if (((vsmmu->cr0 & VSMMU_CR0_SMMUEN) != 0U) ||
			((value & ~VSMMU_CR2_ALLOWED) != 0UL)) {
			return -EINVAL;
		}
		vsmmu->cr2 = (uint32_t)value;
		break;
	case VSMMU_GBPA:
		if (((value & VSMMU_GBPA_UPDATE) == 0UL) ||
			((value & ~(VSMMU_GBPA_UPDATE | VSMMU_GBPA_ABORT)) != 0UL)) {
			return -EINVAL;
		}
		vsmmu->gbpa = (uint32_t)value & VSMMU_GBPA_ABORT;
		break;
	case VSMMU_IRQ_CTRL:
		if ((value & ~VSMMU_IRQ_ALLOWED) != 0UL) {
			return -EINVAL;
		}
		vsmmu->irq_ctrl = (uint32_t)value;
		break;
	case VSMMU_GERRORN:
		if ((value & ~VSMMU_GERROR_ALLOWED) != 0UL) {
			return -EINVAL;
		}
		vsmmu->gerrorn = (uint32_t)value;
		if (((vsmmu->cmdq_cons & VSMMU_CMDQ_CONS_ERR_MASK) != 0U) &&
			((vsmmu_active_errors(vsmmu) & VSMMU_GERROR_CMDQ_ERR) == 0U)) {
			vsmmu->worker_pending = true;
			*kick_worker = true;
		}
		break;
	case VSMMU_STRTAB_BASE:
		if ((vsmmu->cr0 & VSMMU_CR0_SMMUEN) != 0U) {
			return -EINVAL;
		}
		vsmmu->strtab_base = value;
		vsmmu->generation++;
		break;
	case VSMMU_STRTAB_BASE_CFG:
		if ((vsmmu->cr0 & VSMMU_CR0_SMMUEN) != 0U) {
			return -EINVAL;
		}
		vsmmu->strtab_cfg = (uint32_t)value;
		vsmmu->generation++;
		break;
	case VSMMU_CMDQ_BASE:
		if ((vsmmu->cr0 & VSMMU_CR0_CMDQEN) != 0U) {
			return -EINVAL;
		}
		vsmmu->cmdq_base = value;
		vsmmu->generation++;
		break;
	case VSMMU_CMDQ_PROD:
		log2_entries = vsmmu_cmdq_log2(vsmmu);
		if ((log2_entries == 0U) || (log2_entries > vsmmu->max_cmdq_log2)) {
			return -EINVAL;
		}
		pointer_mask = vsmmu_ring_mask((uint8_t)log2_entries);
		if ((value & ~pointer_mask) != 0UL) {
			return -EINVAL;
		}
		vsmmu->cmdq_prod = (uint32_t)value;
		vsmmu->worker_pending = true;
		*kick_worker = true;
		break;
	case VSMMU_CMDQ_CONS:
		if ((vsmmu->cr0 & VSMMU_CR0_CMDQEN) != 0U) {
			return -EINVAL;
		}
		log2_entries = vsmmu_cmdq_log2(vsmmu);
		if ((log2_entries == 0U) || (log2_entries > vsmmu->max_cmdq_log2)) {
			return -EINVAL;
		}
		pointer_mask = vsmmu_ring_mask((uint8_t)log2_entries);
		if ((value & ~pointer_mask) != 0UL) {
			return -EINVAL;
		}
		vsmmu->cmdq_cons = (uint32_t)value;
		break;
	case VSMMU_EVTQ_BASE:
		if ((vsmmu->cr0 & VSMMU_CR0_EVTQEN) != 0U) {
			return -EINVAL;
		}
		vsmmu->evtq_base = value;
		vsmmu->generation++;
		break;
	case VSMMU_EVTQ_PROD:
		if ((vsmmu->cr0 & VSMMU_CR0_EVTQEN) != 0U) {
			return -EINVAL;
		}
		vsmmu->evtq_prod = (uint32_t)value;
		break;
	case VSMMU_EVTQ_CONS:
		log2_entries = vsmmu_evtq_log2(vsmmu);
		if ((log2_entries == 0U) || (log2_entries > vsmmu->max_evtq_log2)) {
			return -EINVAL;
		}
		pointer_mask = vsmmu_ring_mask((uint8_t)log2_entries);
		if ((value & ~((uint64_t)pointer_mask | (1UL << 31U))) != 0UL) {
			return -EINVAL;
		}
		vsmmu->evtq_cons = (uint32_t)value;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

void arm64_vsmmu_init_vm(struct acrn_vm *vm)
{
	const struct arch_vm_config *config;
	struct arm64_vsmmu *vsmmu;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}
	config = &get_vm_config(vm->vm_id)->arch;
	if (config->guest_smmu_size == 0UL) {
		return;
	}
	if ((config->guest_smmu_size != VSMMU_MMIO_SIZE) ||
		(config->guest_smmu_cmdq_log2 == 0U) ||
		(config->guest_smmu_evtq_log2 == 0U)) {
		LOG_ERR("vSMMUv3: vm%u rejected invalid static configuration",
			vm->vm_id);
		return;
	}
	vsmmu = &vsmmu_devs[vm->vm_id];
	(void)memset(vsmmu, 0U, sizeof(*vsmmu));
	vsmmu->vm = vm;
	vsmmu->gbpa = VSMMU_GBPA_ABORT;
	vsmmu->worker_pcpu = config->guest_smmu_worker_pcpu;
	vsmmu->max_cmdq_log2 = config->guest_smmu_cmdq_log2;
	vsmmu->max_evtq_log2 = config->guest_smmu_evtq_log2;
	vsmmu->generation = 1UL;
	vsmmu->initialized = true;
	if (!vsmmu_softirq_registered) {
		register_softirq(SOFTIRQ_VSMMU, vsmmu_softirq);
		vsmmu_softirq_registered = true;
	}
	LOG_INF("vSMMUv3: vm%u [0x%lx-0x%lx] irq:%u worker:cpu%u",
		vm->vm_id, config->guest_smmu_base,
		config->guest_smmu_base + config->guest_smmu_size - 1UL,
		config->guest_smmu_irq, config->guest_smmu_worker_pcpu);
}

void arm64_vsmmu_reset_vm(struct acrn_vm *vm)
{
	arm64_vsmmu_deinit_vm(vm);
	arm64_vsmmu_init_vm(vm);
}

void arm64_vsmmu_deinit_vm(struct acrn_vm *vm)
{
	struct arm64_vsmmu *vsmmu;
	uint64_t flags;
	uint32_t irq;
	bool asserted;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}
	vsmmu = &vsmmu_devs[vm->vm_id];
	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	asserted = vsmmu->irq_asserted;
	irq = get_vm_config(vm->vm_id)->arch.guest_smmu_irq;
	vsmmu->initialized = false;
	vsmmu->worker_pending = false;
	vsmmu->irq_asserted = false;
	vsmmu->generation++;
	spinlock_irqrestore_release(&vsmmu->lock, flags);
	if (asserted) {
		arch_trigger_level_intr(vm, irq, false);
	}
}

bool arm64_vsmmu_available(const struct acrn_vm *vm)
{
	return (vm != NULL) && (vm->vm_id < CONFIG_MAX_VM_NUM) &&
		vsmmu_devs[vm->vm_id].initialized;
}

bool arm64_vsmmu_get_debug(uint16_t vm_id, struct arm64_vsmmu_debug *debug)
{
	const struct arch_vm_config *config;
	struct arm64_vsmmu *vsmmu;
	uint64_t flags;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (debug == NULL)) {
		return false;
	}
	config = &get_vm_config(vm_id)->arch;
	vsmmu = &vsmmu_devs[vm_id];
	(void)memset(debug, 0U, sizeof(*debug));
	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	debug->base = config->guest_smmu_base;
	debug->size = config->guest_smmu_size;
	debug->strtab_base = vsmmu->strtab_base;
	debug->cmdq_base = vsmmu->cmdq_base;
	debug->evtq_base = vsmmu->evtq_base;
	debug->generation = vsmmu->generation;
	debug->commands_processed = vsmmu->commands_processed;
	debug->commands_rejected = vsmmu->commands_rejected;
	debug->budget_exhausted = vsmmu->budget_exhausted;
	debug->cr0 = vsmmu->cr0;
	debug->irq_ctrl = vsmmu->irq_ctrl;
	debug->gerror = vsmmu->gerror;
	debug->gerrorn = vsmmu->gerrorn;
	debug->cmdq_prod = vsmmu->cmdq_prod;
	debug->cmdq_cons = vsmmu->cmdq_cons;
	debug->evtq_prod = vsmmu->evtq_prod;
	debug->evtq_cons = vsmmu->evtq_cons;
	debug->worker_pcpu = vsmmu->worker_pcpu;
	debug->worker_pending = vsmmu->worker_pending;
	debug->irq_asserted = vsmmu->irq_asserted;
	debug->available = vsmmu->initialized;
	spinlock_irqrestore_release(&vsmmu->lock, flags);

	return debug->available || (debug->size != 0UL);
}

int32_t arm64_vsmmu_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct acrn_mmio_request *mmio;
	struct arm64_vsmmu *vsmmu;
	const struct arch_vm_config *config;
	uint64_t flags;
	uint64_t value = 0UL;
	uint32_t offset;
	int32_t ret;
	bool kick_worker = false;

	if ((io_req == NULL) || (vm == NULL) ||
		(vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return -EINVAL;
	}
	mmio = &io_req->reqs.mmio_request;
	config = &get_vm_config(vm->vm_id)->arch;
	vsmmu = &vsmmu_devs[vm->vm_id];
	if (!vsmmu->initialized || (mmio->address < config->guest_smmu_base) ||
		(mmio->address >= (config->guest_smmu_base + config->guest_smmu_size))) {
		return -EINVAL;
	}
	offset = (uint32_t)(mmio->address - config->guest_smmu_base);

	spinlock_irqsave_obtain(&vsmmu->lock, &flags);
	if (!vsmmu_access_width_valid(offset, mmio->size)) {
		ret = -EINVAL;
	} else if (mmio->direction == ACRN_IOREQ_DIR_READ) {
		ret = vsmmu_read_locked(vsmmu, offset, &value);
		if (ret == 0) {
			mmio->value = value;
			kick_worker = (offset == VSMMU_CMDQ_CONS) &&
				vsmmu->worker_pending;
		}
	} else {
		ret = vsmmu_write_locked(vsmmu, offset, mmio->value,
			&kick_worker);
	}
	if (ret != 0) {
		vsmmu_fail_control_locked(vsmmu);
	}
	spinlock_irqrestore_release(&vsmmu->lock, flags);

	vsmmu_refresh_irq(vsmmu);
	if (kick_worker) {
		fire_softirq_on(SOFTIRQ_VSMMU, vsmmu->worker_pcpu);
	}

	/* Invalid accesses are reported in virtual GERROR and consumed by vSMMU. */
	return 0;
}
