/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <irq.h>
#include <logmsg.h>
#include <spinlock.h>
#include <vm.h>
#include <asm/vtd.h>
#include <asm/irq.h>
#include <asm/guest/vgicv3.h>
#include "smmuv3.h"

#define ARM_SMMU_MAX_DOMAINS	CONFIG_MAX_VM_NUM
#define ARM_SMMU_PCI_STREAM(bus, devfun)	((((uint32_t)(bus)) << 8U) | (uint32_t)(devfun))

/*
 * BEAU ARM64 passthrough model, first stage.
 *
 * Xen's SMMUv3 driver uses the VM P2M/stage-2 table as the SMMU stage-2 table.
 * This file follows that model at the framework boundary:
 *
 *   VM vCPU access:  IPA ---- CPU stage-2 ----> PA
 *   Device DMA:      IPA ---- SMMU stage-2 ---> PA
 *
 * If the two paths diverge, a device may DMA into memory the guest CPU cannot
 * reach, or miss memory the guest owns. Therefore an IOMMU domain stores the
 * VM's stage-2 root HPA and never creates an independent DMA map here.
 *
 * Hardware command queue programming is intentionally not faked. Until a real
 * SMMUv3 instance and stream table are registered by platform code, stream
 * assignment returns -ENODEV. Failing closed is the only safe default for DMA
 * isolation: bypass would make MMIO filtering meaningless because DMA can still
 * touch RAM directly.
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

struct arm64_pt_msix_state {
	bool used;
	uint16_t phys_bdf;
	uint16_t entry_nr;
	uint32_t lpi;
};

static spinlock_t arm_smmu_lock = { .head = 0U, .tail = 0U };
static spinlock_t arm64_pt_msix_lock = { .head = 0U, .tail = 0U };
static struct iommu_domain arm_smmu_domains[ARM_SMMU_MAX_DOMAINS];
static struct arm_smmu_stream_state arm_smmu_streams[ARM_SMMU_MAX_SW_STREAMS];
static struct arm64_pt_msix_state arm64_pt_msix_states[CONFIG_MAX_PT_IRQ_ENTRIES];
static bool arm_smmu_hw_ready;

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
	 * equivalent of Xen's "device already assigned" guard and prevents a
	 * device from DMAing with two VMIDs over its lifetime.
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
	} else if (!arm_smmu_hw_ready) {
		/*
		 * Do not remember the assignment on -ENODEV. A later platform
		 * SMMU registration can retry from a clean state, and current
		 * callers know the device was not made DMA-capable for the VM.
		 */
		stream->used = false;
		ret = -ENODEV;
	} else {
		stream->domain = domain;
		domain->hw_bound = true;
		/* Future hook: write STE.S2VMID/S2TTB and issue CMD_SYNC. */
	}
	spinlock_irqrestore_release(&arm_smmu_lock, flags);

	if (ret == -ENODEV) {
		LOG_ERR("SMMUv3 not ready: reject stream 0x%x for vm%u",
			stream_id, domain->vm_id);
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
	} else if (!arm_smmu_hw_ready) {
		ret = -ENODEV;
	} else {
		/*
		 * Hardware sequence for the full driver:
		 *   1. Replace the STE with ABORT, not BYPASS.
		 *   2. Invalidate cached STE/context entries.
		 *   3. CMD_SYNC before ownership state is released.
		 */
		stream->domain = NULL;
		stream->used = false;
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
	 * Posted interrupts are an x86 VT-d/APIC optimization. ARM64 delivers
	 * physical pass-through IRQs through the host GIC/ITS and then injects a
	 * virtual IRQ/LPI into vGIC, so the common ptdev layer must use its
	 * softirq path.
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

static struct arm64_pt_msix_state *arm64_pt_find_msix_state(uint16_t phys_bdf,
	uint16_t entry_nr)
{
	uint32_t i;

	for (i = 0U; i < ARRAY_SIZE(arm64_pt_msix_states); i++) {
		if (arm64_pt_msix_states[i].used &&
			(arm64_pt_msix_states[i].phys_bdf == phys_bdf) &&
			(arm64_pt_msix_states[i].entry_nr == entry_nr)) {
			return &arm64_pt_msix_states[i];
		}
	}

	return NULL;
}

static int32_t arm64_pt_record_msix_state(uint16_t phys_bdf, uint16_t entry_nr,
	uint32_t lpi)
{
	uint32_t i;
	uint64_t flags;
	int32_t ret = -ENOMEM;

	spinlock_irqsave_obtain(&arm64_pt_msix_lock, &flags);
	if (arm64_pt_find_msix_state(phys_bdf, entry_nr) != NULL) {
		ret = -EBUSY;
	} else {
		for (i = 0U; i < ARRAY_SIZE(arm64_pt_msix_states); i++) {
			if (!arm64_pt_msix_states[i].used) {
				arm64_pt_msix_states[i].used = true;
				arm64_pt_msix_states[i].phys_bdf = phys_bdf;
				arm64_pt_msix_states[i].entry_nr = entry_nr;
				arm64_pt_msix_states[i].lpi = lpi;
				ret = 0;
				break;
			}
		}
	}
	spinlock_irqrestore_release(&arm64_pt_msix_lock, flags);

	return ret;
}

static uint32_t arm64_pt_take_msix_lpi(uint16_t phys_bdf, uint16_t entry_nr)
{
	struct arm64_pt_msix_state *state;
	uint32_t lpi = IRQ_INVALID;
	uint64_t flags;

	spinlock_irqsave_obtain(&arm64_pt_msix_lock, &flags);
	state = arm64_pt_find_msix_state(phys_bdf, entry_nr);
	if (state != NULL) {
		lpi = state->lpi;
		(void)memset(state, 0U, sizeof(*state));
	}
	spinlock_irqrestore_release(&arm64_pt_msix_lock, flags);

	return lpi;
}

static uint32_t arm64_pt_peek_msix_lpi(uint16_t phys_bdf, uint16_t entry_nr)
{
	struct arm64_pt_msix_state *state;
	uint32_t lpi = IRQ_INVALID;
	uint64_t flags;

	spinlock_irqsave_obtain(&arm64_pt_msix_lock, &flags);
	state = arm64_pt_find_msix_state(phys_bdf, entry_nr);
	if (state != NULL) {
		lpi = state->lpi;
	}
	spinlock_irqrestore_release(&arm64_pt_msix_lock, flags);

	return lpi;
}

static void arm64_ptdev_release_msix(const struct ptirq_remapping_info *entry)
{
	uint16_t phys_bdf = entry->phys_sid.msi_id.bdf;
	uint16_t entry_nr = entry->phys_sid.msi_id.entry_nr;
	uint32_t lpi = arm64_pt_take_msix_lpi(phys_bdf, entry_nr);

	if (lpi != IRQ_INVALID) {
		arm64_gicv3_its_release_msix(arm64_ptdev_device_id(phys_bdf), lpi);
	}
}

int32_t ptirq_prepare_msix_remap(struct acrn_vm *vm, uint16_t virt_bdf,
	uint16_t phys_bdf, uint16_t entry_nr, struct msi_info *info,
	__unused uint16_t irte_idx)
{
	struct ptirq_remapping_info *entry;
	struct arm64_gicv3_msi_msg msg;
	uint32_t lpi = arm64_pt_peek_msix_lpi(phys_bdf, entry_nr);
	uint32_t acrn_irq;
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
	 */
	if (lpi == IRQ_INVALID) {
		ret = arm64_gicv3_its_alloc_msix(arm64_ptdev_device_id(phys_bdf),
			arm64_ptdev_event_id(entry_nr), &lpi, &msg);
		if (ret != 0) {
			return ret;
		}
		new_lpi = true;
	} else {
		ret = arm64_gicv3_its_map_msi(lpi, &msg);
		if (ret != 0) {
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
	if (!arm64_is_valid_acrn_irq(acrn_irq)) {
		ret = -ENODEV;
		goto fail_release_lpi;
	}

	if (new_lpi) {
		ret = arm64_pt_record_msix_state(phys_bdf, entry_nr, lpi);
		if (ret != 0) {
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
		entry->release_cb = arm64_ptdev_release_msix;
		*info = entry->pmsi;

		if (!is_entry_active(entry)) {
			ret = ptirq_activate_entry(entry, acrn_irq);
		}
	}
	spinlock_release(&ptdev_lock);

	if (ret != 0) {
		if (new_lpi) {
			(void)arm64_pt_take_msix_lpi(phys_bdf, entry_nr);
		}
		goto fail_release_lpi;
	}

	return 0;

fail_release_lpi:
	if (new_lpi) {
		arm64_gicv3_its_release_msix(arm64_ptdev_device_id(phys_bdf), lpi);
	}
	return ret;
}

void ptirq_remove_msix_remapping(const struct acrn_vm *vm, uint16_t phys_bdf,
	uint32_t vector_count)
{
	uint32_t i;

	for (i = 0U; i < vector_count; i++) {
		struct ptirq_remapping_info *entry;
		DEFINE_MSI_SID(phys_sid, phys_bdf, (uint16_t)i);

		spinlock_obtain(&ptdev_lock);
		entry = find_ptirq_entry(PTDEV_INTR_MSI, &phys_sid, NULL);
		if ((entry != NULL) && (entry->vm == vm)) {
			if (entry->release_cb != NULL) {
				entry->release_cb(entry);
			}
			ptirq_deactivate_entry(entry);
			ptirq_release_entry(entry);
		}
		spinlock_release(&ptdev_lock);
	}
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
	if (!arm64_is_valid_acrn_irq(acrn_irq)) {
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
				arm64_ptdev_event_id(entry->virt_sid.msi_id.entry_nr));
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
