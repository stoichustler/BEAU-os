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
	bool level;
	bool valid;
};

struct bsp_pt_device {
	char name[16];
	uint32_t stream_id;
	uint32_t irq;
	uint16_t owner_vmid;
	enum bsp_pt_owner owner;
	bool valid;
	bool writable;
	struct bsp_pt_mmio_res mmio[BSP_PT_MMIO_RES_NUM];
	struct bsp_pt_irq_res irq_res;
};

/*
 * Device passthrough is a three-part contract:
 *
 *   1. MMIO ownership: only one VM gets the register window.
 *   2. IRQ ownership: host IRQ is translated into that VM's virtual IRQ space.
 *   3. DMA ownership: every StreamID used by the device is bound to that VM's
 *      SMMU domain.
 *
 * MMIO-only assignment is not isolation. A malicious or buggy driver can still
 * program the device to DMA anywhere unless the SMMU stream table points at the
 * VM stage-2 page table. Keep this BSP table as the single ownership ledger so
 * later DT/IORT parsing can populate devices without changing the safety checks.
 */
static spinlock_t bsp_pt_lock = { .head = 0U, .tail = 0U };
static struct bsp_pt_device bsp_pt_devices[BSP_PT_MAX_DEVICES];

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

int32_t passthrough_register_device(uint32_t stream_id, const char *name,
	bool writable)
{
	struct bsp_pt_device *dev;
	uint64_t flags;
	int32_t ret = 0;

	if (!bsp_pt_valid_stream(stream_id)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		dev = bsp_pt_alloc_device(stream_id, name);
	}
	if (dev == NULL) {
		ret = -ENOMEM;
	} else {
		dev->writable = writable;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return ret;
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

	spinlock_irqsave_obtain(&bsp_pt_lock, &flags);
	dev = bsp_pt_find_device(stream_id);
	if (dev == NULL) {
		ret = -ENODEV;
	} else if (dev->owner == BSP_PT_OWNER_VM) {
		ret = -EBUSY;
	} else {
		/*
		 * SPIs are distributor INTIDs. Unlike MSI/MSI-X, a platform SPI
		 * has no ITS DeviceID/EventID; the hypervisor must remember the
		 * physical SPI and inject the configured virtual IRQ when it
		 * fires. The mapping is registered before assignment so a device
		 * cannot be exposed with MMIO/DMA but no interrupt policy.
		 */
		dev->irq_res.phys_spi = mapping->phys_spi;
		dev->irq_res.virt_irq = mapping->virt_irq;
		dev->irq_res.level = mapping->level;
		dev->irq_res.valid = true;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	return ret;
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
	} else if (writable && !dev->writable) {
		ret = -EPERM;
	}
	spinlock_irqrestore_release(&bsp_pt_lock, flags);

	if (ret != 0) {
		return ret;
	}

	/*
	 * Program DMA isolation before publishing VM ownership. The order matters:
	 * once MMIO is visible to a guest, the guest can command DMA immediately.
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
	 * Revoke the stream before returning the device to the host pool. The
	 * full SMMU driver must install an ABORT STE here, so stale DMA is
	 * blocked while ownership changes hands.
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
