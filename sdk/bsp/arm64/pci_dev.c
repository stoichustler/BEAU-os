/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vconfig.h>
#include <bsp/pci.h>
#include <bsp/vpci.h>
#include <asm/board.h>
#include <asm/pci_dev.h>

struct dmar_info plat_dmar_info;
const union pci_bdf plat_hidden_pdevs[1];
const struct vmsix_on_msi_info vmsix_on_msi_devs[1];
struct acrn_vm_pci_dev_config sos_pci_devs[CONFIG_MAX_PCI_DEV_NUM];

bool allocate_to_prelaunched_vm(struct pci_pdev *pdev)
{
	uint16_t vmid;
	bool found = false;

	if (pdev == NULL) {
		return false;
	}

	for (vmid = 0U; (vmid < CONFIG_MAX_VM_NUM) && !found; vmid++) {
		struct acrn_vm_config *vm_config = get_vm_config(vmid);
		uint16_t pci_idx;

		if ((vm_config->load_order != PRE_LAUNCHED_VM) ||
			(vm_config->pci_devs == NULL)) {
			continue;
		}

		for (pci_idx = 0U; pci_idx < vm_config->pci_dev_num; pci_idx++) {
			struct acrn_vm_pci_dev_config *dev_config =
				&vm_config->pci_devs[pci_idx];

			if ((dev_config->emu_type == PCI_DEV_TYPE_PTDEV) &&
				bdf_is_equal(dev_config->pbdf, pdev->bdf)) {
				dev_config->pdev = pdev;
				found = true;
				break;
			}
		}
	}

	return found;
}

struct acrn_vm_pci_dev_config *init_one_dev_config(struct pci_pdev *pdev)
{
	struct acrn_vm_pci_dev_config *dev_config = NULL;

	if (pdev == NULL) {
		return NULL;
	}

	if (allocate_to_prelaunched_vm(pdev)) {
		return NULL;
	}

	/*
	 * The current ARM64 4OS QEMU service VM is Zephyr, not a PCI-owning
	 * management VM. Do not expose unassigned host PCI devices to it.
	 */
	if ((service_vm_config != NULL) &&
		(service_vm_config->os_config.os_family == VM_OS_LINUX) &&
		(service_vm_config->pci_devs != NULL) &&
		(service_vm_config->pci_dev_num < CONFIG_MAX_PCI_DEV_NUM) &&
		!is_hv_owned_pdev(pdev->bdf)) {
		dev_config = &service_vm_config->pci_devs[service_vm_config->pci_dev_num];
		dev_config->emu_type = PCI_DEV_TYPE_PTDEV;
		dev_config->vbdf.value = pdev->bdf.value;
		dev_config->pbdf.value = pdev->bdf.value;
		dev_config->pdev = pdev;
		service_vm_config->pci_dev_num++;
	}

	return dev_config;
}
