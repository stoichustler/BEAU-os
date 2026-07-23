/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <cpu.h>
#include <irq.h>
#include <logmsg.h>
#include <spinlock.h>
#include <vm.h>
#include <passthrough.h>
#include <asm/vtd.h>

#define BSP_PT_MAX_DEVICES	16U
#define BSP_PT_MMIO_RES_NUM	4U

enum bsp_pt_owner {
	BSP_PT_OWNER_FREE = 0U,
	BSP_PT_OWNER_HOST,
	BSP_PT_OWNER_VM,
};

struct bsp_pt_mmio_res {
	uint64_t hpa;
	uint64_t size;
};

struct bsp_pt_irq_res {
	uint32_t phys_spi;
	uint32_t virt_irq;
	uint16_t affinity_pcpu;
	enum passthrough_irq_affinity_policy affinity_policy;
	bool level;
	bool valid;
};

struct bsp_pt_device {
	char name[16];
	uint32_t stream_id;
	uint32_t irq;
	uint16_t owner_vmid;
	uint16_t policy_owner_vmid;
	enum bsp_pt_owner owner;
	bool valid;
	bool writable;
	bool pm_irq_masked;
	struct bsp_pt_mmio_res mmio[BSP_PT_MMIO_RES_NUM];
	struct bsp_pt_irq_res irq_res;
};

/* [20260712] PCI/device passthrough ownership model
 *
 *           platform policy / device tree
 *                    |
 *                    v
 *        passthrough_register_device_owner()
 *        passthrough_register_spi()
 *                    |
 *                    v
 *      +------------------------------------+
 *      |  BSP passthrough ownership table   |
 *      |  - StreamID -> allowed VM          |
 *      |  - writable policy                 |
 *      |  - optional SPI mapping            |
 *      |  - optional SPI affinity policy    |
 *      +----------------+-------------------+
 *                       |
 *                       v
 *      passthrough_assign_device(vm, StreamID)
 *                       |
 *       +---------------+----------------+
 *       |                                |
 *       v                                v
 *  SMMUv3 stream assignment         vPCI / MMIO side
 *  StreamID -> VM domain            exposes cfg/BAR to VM
 *  DMA observes VM Stage-2          and handles MSI/MSI-X
 *
 * Device passthrough is a three-part contract:
 *
 *   1. MMIO/config ownership: only one VM may see the register window or PCI
 *      config endpoint for a physical device at a time.
 *   2. IRQ ownership: host SPI or PCI MSI/MSI-X routing must resolve into that
 *      VM's virtual interrupt space.
 *   3. DMA ownership: every StreamID used by the device must point at that VM's
 *      SMMU domain before the guest can program the device.
 *
 * This file is the BSP policy and ownership gate. It does not map PCI BARs or
 * rewrite MSI/MSI-X messages directly; those are handled by the vPCI/PT paths.
 * Its job is to make sure those paths never expose a device unless the platform
 * policy allows it and DMA isolation can be established first.
 *
 * MMIO-only assignment is not isolation. A malicious or buggy driver can still
 * program the device to DMA anywhere unless the SMMU stream table points at the
 * VM stage-2 page table. Keep this BSP table as the single ownership ledger so
 * later DT/IORT parsing can populate devices without changing the safety checks.
 *
 * IRQ affinity extension:
 *
 *   platform.dts
 *       beau,irq-affinity = "owner" | "pcpu"
 *                    |
 *                    v
 *   passthrough_register_spi()
 *       stores policy beside the physical SPI and owner policy
 *                    |
 *                    v
 *   passthrough_irq_affinity(vm, phys_spi)
 *       resolves to a concrete pCPU only if the caller's VM matches the
 *       boot-time owner policy
 *                    |
 *                    v
 *   ptirq_activate_entry()
 *       programs the physical GIC route before enabling the interrupt
 *
 * This intentionally binds a host SPI to the VM owner pCPU rather than to the
 * currently running pCPU. For RTOS/ARINC653-style partitioning the device
 * interrupt should enter the same CPU pool as the guest vCPU that will consume
 * it; otherwise Linux or another bursty VM can keep moving the physical IRQ
 * across cores and add cross-core wakeup/IPI noise to the RTOS path.
 */
static spinlock_t bsp_pt_lock = { .head = 0U, .tail = 0U };
static struct bsp_pt_device bsp_pt_devices[BSP_PT_MAX_DEVICES];
static uint64_t bsp_pt_suspend_epoch;
static bool bsp_pt_suspended;
static bool bsp_pt_pm_dma_required;

static bool bsp_pt_valid_stream(uint32_t stream_id)
{
	return stream_id != ARM_SMMU_STREAM_ID_INVALID;
}

static struct bsp_pt_device *bsp_pt_find_device(uint32_t stream_id)
{
	uint32_t i;

	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		if (bsp_pt_devices[i].valid &&
			(bsp_pt_devices[i].stream_id == stream_id)) {
			return &bsp_pt_devices[i];
		}
	}

	return NULL;
}

static uint16_t bsp_pt_vm_owner_pcpu(uint16_t vm_id)
{
	const struct acrn_vm_config *vm_config;
	uint16_t pcpu_id = INVALID_CPU_ID;

	if (vm_id < CONFIG_MAX_VM_NUM) {
		vm_config = get_vm_config(vm_id);
		if (vm_config->cpu_affinity_num != 0U) {
			pcpu_id = vm_config->cpu_affinity_order[0U];
		} else if (vm_config->cpu_affinity != 0UL) {
			pcpu_id = ffs64(vm_config->cpu_affinity);
		}
	}

	return pcpu_id;
}

static struct bsp_pt_device *bsp_pt_alloc_device(uint32_t stream_id,
	const char *name)
{
	struct bsp_pt_device *dev = NULL;
	uint32_t i;

	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		if (!bsp_pt_devices[i].valid) {
			dev = &bsp_pt_devices[i];
			(void)memset(dev, 0U, sizeof(*dev));
			dev->valid = true;
			dev->stream_id = stream_id;
			dev->irq = IRQ_INVALID;
			dev->owner = BSP_PT_OWNER_HOST;
			dev->owner_vmid = ACRN_INVALID_VMID;
			dev->policy_owner_vmid = ACRN_INVALID_VMID;
			if (name != NULL) {
				uint32_t j;

				for (j = 0U; (j < (sizeof(dev->name) - 1U)) &&
					(name[j] != '\0'); j++) {
					dev->name[j] = name[j];
				}
			}
			break;
		}
	}

	return dev;
}

int32_t passthrough_register_device_owner(uint32_t stream_id, const char *name,
	bool writable, uint16_t owner_vmid)
{
	struct bsp_pt_device *dev;
	uint64_t flags;
	int32_t ret = 0;

	if (!bsp_pt_valid_stream(stream_id) ||
		((owner_vmid != ACRN_INVALID_VMID) && (owner_vmid >= CONFIG_MAX_VM_NUM))) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		dev = bsp_pt_alloc_device(stream_id, name);
	}
	if (dev == NULL) {
		ret = -ENOMEM;
	} else if ((dev->policy_owner_vmid != ACRN_INVALID_VMID) &&
		(dev->policy_owner_vmid != owner_vmid)) {
		ret = -EBUSY;
	} else {
		/*
		 * A valid StreamID can be claimed only once by policy. Runtime
		 * assignment may move host ownership to a VM, but it must not
		 * override the boot-time owner selected by the platform.
		 */
		dev->writable = writable;
		dev->policy_owner_vmid = owner_vmid;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return ret;
}

int32_t passthrough_register_device(uint32_t stream_id, const char *name,
	bool writable)
{
	return passthrough_register_device_owner(stream_id, name, writable,
		ACRN_INVALID_VMID);
}

int32_t passthrough_register_spi(uint32_t stream_id,
	const struct passthrough_spi_mapping *mapping)
{
	struct bsp_pt_device *dev;
	uint64_t flags;
	int32_t ret = 0;

	if (!bsp_pt_valid_stream(stream_id) || (mapping == NULL)) {
		return -EINVAL;
	}
	if ((mapping->phys_spi < 32U) ||
		(mapping->phys_spi >= ARM64_GIC_SPURIOUS_INTID)) {
		return -EINVAL;
	}
	if ((mapping->affinity_policy != PASSTHROUGH_IRQ_AFFINITY_NONE) &&
		(mapping->affinity_policy != PASSTHROUGH_IRQ_AFFINITY_OWNER) &&
		(mapping->affinity_policy != PASSTHROUGH_IRQ_AFFINITY_PCPU)) {
		return -EINVAL;
	}
	if ((mapping->affinity_policy == PASSTHROUGH_IRQ_AFFINITY_PCPU) &&
		(mapping->affinity_pcpu >= get_pcpu_nums())) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		ret = -ENODEV;
	} else if (dev->owner == BSP_PT_OWNER_VM) {
		ret = -EBUSY;
	} else {
		uint32_t i;

		for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
			const struct bsp_pt_device *other = &bsp_pt_devices[i];

			if ((other != dev) && other->valid && other->irq_res.valid &&
				(other->irq_res.phys_spi == mapping->phys_spi)) {
				ret = -EBUSY;
				break;
			}
		}
	}
	if (ret == 0) {
		/*
		 * SPIs are distributor INTIDs. Unlike MSI/MSI-X, a platform SPI
		 * has no ITS DeviceID/EventID; the hypervisor must remember the
		 * physical SPI and inject the configured virtual IRQ when it
		 * fires. The mapping is registered before assignment so a device
		 * cannot be exposed with MMIO/DMA but no interrupt policy.
		 */
		dev->irq_res.phys_spi = mapping->phys_spi;
		dev->irq_res.virt_irq = mapping->virt_irq;
		dev->irq_res.affinity_pcpu = mapping->affinity_pcpu;
		dev->irq_res.affinity_policy = mapping->affinity_policy;
		dev->irq_res.level = mapping->level;
		dev->irq_res.valid = true;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return ret;
}

/* Resolve a DTS-authored IRQ affinity policy into a concrete pCPU.
 *
 * Flow:
 *
 *   phys_spi lookup
 *      -> reject if no passthrough SPI policy owns this interrupt
 *      -> reject if the policy owner is another VM
 *      -> "owner": use VM cpu_affinity_order[0], falling back to bitmap ffs
 *      -> "pcpu" : use the explicit DTS pCPU
 *
 * Returning false is not an error for devices without affinity policy; it means
 * the legacy routing selected during GIC distributor init should remain in
 * effect. Returning true is a hard contract: ptdev will fail activation if the
 * architecture cannot program that route.
 */
bool passthrough_irq_affinity(uint16_t vm_id, uint32_t phys_spi,
	uint16_t *pcpu_id)
{
	uint64_t flags;
	uint16_t resolved_pcpu = INVALID_CPU_ID;
	bool ret = false;
	uint32_t i;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || (pcpu_id == NULL)) {
		return false;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		const struct bsp_pt_device *dev = &bsp_pt_devices[i];

		if (!dev->valid || !dev->irq_res.valid ||
			(dev->irq_res.phys_spi != phys_spi)) {
			continue;
		}
		if ((dev->policy_owner_vmid != ACRN_INVALID_VMID) &&
			(dev->policy_owner_vmid != vm_id)) {
			continue;
		}

		switch (dev->irq_res.affinity_policy) {
		case PASSTHROUGH_IRQ_AFFINITY_OWNER:
			resolved_pcpu = bsp_pt_vm_owner_pcpu(vm_id);
			ret = resolved_pcpu < get_pcpu_nums();
			break;
		case PASSTHROUGH_IRQ_AFFINITY_PCPU:
			resolved_pcpu = dev->irq_res.affinity_pcpu;
			ret = resolved_pcpu < get_pcpu_nums();
			break;
		default:
			ret = false;
			break;
		}
		break;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	if (ret) {
		*pcpu_id = resolved_pcpu;
	}

	return ret;
}

bool passthrough_get_max_stream_id(uint32_t *max_stream_id)
{
	uint64_t flags;
	uint32_t maximum = 0U;
	uint32_t i;
	bool found = false;

	if (max_stream_id == NULL) {
		return false;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		if (bsp_pt_devices[i].valid) {
			if (!found || (bsp_pt_devices[i].stream_id > maximum)) {
				maximum = bsp_pt_devices[i].stream_id;
			}
			found = true;
		}
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	*max_stream_id = maximum;
	return found;
}

int32_t passthrough_assign_device(struct acrn_vm *vm, uint32_t stream_id,
	bool writable)
{
	struct bsp_pt_device *dev;
	uint64_t flags;
	int32_t ret = 0;

	if ((vm == NULL) || (vm->iommu == NULL) || !bsp_pt_valid_stream(stream_id)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		ret = -ENODEV;
	} else if ((dev->owner == BSP_PT_OWNER_VM) &&
		(dev->owner_vmid != vm->vm_id)) {
		ret = -EBUSY;
	} else if ((dev->policy_owner_vmid != ACRN_INVALID_VMID) &&
		(dev->policy_owner_vmid != vm->vm_id)) {
		ret = -EPERM;
	} else if (writable && !dev->writable) {
		ret = -EPERM;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	if (ret != 0) {
		return ret;
	}

	/*
	 * Assignment order:
	 *
	 *   validate policy -> bind StreamID to VM SMMU domain -> publish VM owner
	 *
	 * Program DMA isolation before publishing VM ownership. The order matters:
	 * once config space or BAR MMIO is visible to a guest, the guest can command
	 * DMA immediately. If the ownership table is updated first, other paths may
	 * treat the device as guest-owned while the SMMU stream is still host-owned
	 * or blocked.
	 */
	ret = arm_smmu_assign_stream(vm->iommu, stream_id);
	if (ret != 0) {
		return ret;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		ret = -ENODEV;
	} else {
		dev->owner = BSP_PT_OWNER_VM;
		dev->owner_vmid = vm->vm_id;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	if (ret != 0) {
		(void)arm_smmu_unassign_stream(vm->iommu, stream_id);
	}

	return ret;
}

bool passthrough_policy_allows_device(uint16_t vm_id, uint32_t stream_id,
	bool writable)
{
	struct bsp_pt_device *dev;
	uint64_t flags;
	bool allowed = false;

	if ((vm_id >= CONFIG_MAX_VM_NUM) || !bsp_pt_valid_stream(stream_id)) {
		return false;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if ((dev != NULL) &&
		(dev->policy_owner_vmid == vm_id) &&
		(!writable || dev->writable)) {
		allowed = true;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return allowed;
}

int32_t passthrough_deassign_device(struct acrn_vm *vm, uint32_t stream_id)
{
	struct bsp_pt_device *dev;
	uint64_t flags;
	int32_t ret = 0;

	if ((vm == NULL) || (vm->iommu == NULL) || !bsp_pt_valid_stream(stream_id)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		ret = -ENODEV;
	} else if ((dev->owner != BSP_PT_OWNER_VM) ||
		(dev->owner_vmid != vm->vm_id)) {
		ret = -EPERM;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	if (ret != 0) {
		return ret;
	}

	/*
	 * Deassignment order is the inverse of assignment:
	 *
	 *   verify current owner -> revoke StreamID from VM domain -> publish host owner
	 *
	 * Revoke the stream before returning the device to the host pool. The full
	 * SMMU driver must install an ABORT STE here, so stale DMA is blocked while
	 * ownership changes hands.
	 */
	ret = arm_smmu_unassign_stream(vm->iommu, stream_id);
	if (ret != 0) {
		return ret;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev != NULL) {
		dev->owner = BSP_PT_OWNER_HOST;
		dev->owner_vmid = ACRN_INVALID_VMID;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return 0;
}

void passthrough_deassign_vm(struct acrn_vm *vm)
{
	uint32_t i;

	if ((vm == NULL) || (vm->iommu == NULL)) {
		return;
	}

	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		uint32_t stream_id;
		bool owned = false;
		uint64_t flags;

		/*
		 * Snapshot one StreamID under the lock, then drop the lock before
		 * calling the full deassign path. That path may touch the SMMU and
		 * reacquire this table lock to publish final ownership.
		 */
		spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
		if (bsp_pt_devices[i].valid &&
			(bsp_pt_devices[i].owner == BSP_PT_OWNER_VM) &&
			(bsp_pt_devices[i].owner_vmid == vm->vm_id)) {
			stream_id = bsp_pt_devices[i].stream_id;
			owned = true;
		}
		spinlock_irqrestore_release(&bsp_pt_lock, flags);

		if (owned) {
			(void)passthrough_deassign_device(vm, stream_id);
		}
	}
}

bool passthrough_vm_has_owned_devices(uint16_t vm_id)
{
	uint64_t flags;
	uint32_t i;
	bool owned = false;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return false;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		const struct bsp_pt_device *dev = &bsp_pt_devices[i];

		if (dev->valid && (dev->owner == BSP_PT_OWNER_VM) &&
			(dev->owner_vmid == vm_id)) {
			owned = true;
			break;
		}
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return owned;
}

int32_t passthrough_pm_suspend(uint64_t epoch, uint64_t required_vm_mask)
{
	uint64_t flags;
	uint32_t i;

	if ((epoch == 0UL) || (required_vm_mask == 0UL)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	if (bsp_pt_suspended) {
		int32_t status = (bsp_pt_suspend_epoch == epoch) ? 0 : -EBUSY;

		spinlock_irqrestore_release(&bsp_pt_lock, flags);
		return status;
	}
	bsp_pt_pm_dma_required = false;
	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		struct bsp_pt_device *dev = &bsp_pt_devices[i];

		if (!dev->valid || (dev->owner != BSP_PT_OWNER_VM) ||
			(dev->owner_vmid >= 64U) ||
			((required_vm_mask & (1UL << dev->owner_vmid)) == 0UL) ||
			!dev->irq_res.valid) {
			continue;
		}
		arm64_gicv3_disable_irq(dev->irq_res.phys_spi);
		dev->pm_irq_masked = true;
		bsp_pt_pm_dma_required = true;
	}
	bsp_pt_suspend_epoch = epoch;
	bsp_pt_suspended = true;
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return 0;
}

int32_t passthrough_pm_resume(uint64_t epoch)
{
	uint64_t flags;
	uint32_t i;

	if (epoch == 0UL) {
		return -EINVAL;
	}
	if (bsp_pt_suspended && bsp_pt_pm_dma_required &&
		!arm_smmu_assignment_ready()) {
		return -EACCES;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	if (!bsp_pt_suspended) {
		spinlock_irqrestore_release(&bsp_pt_lock, flags);
		return 0;
	}
	if (bsp_pt_suspend_epoch != epoch) {
		spinlock_irqrestore_release(&bsp_pt_lock, flags);
		return -EINVAL;
	}
	for (i = 0U; i < ARRAY_SIZE(bsp_pt_devices); i++) {
		struct bsp_pt_device *dev = &bsp_pt_devices[i];

		if (dev->pm_irq_masked) {
			arm64_gicv3_unmask_irq(dev->irq_res.phys_spi);
			dev->pm_irq_masked = false;
		}
	}
	bsp_pt_suspend_epoch = 0UL;
	bsp_pt_suspended = false;
	bsp_pt_pm_dma_required = false;
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return 0;
}
