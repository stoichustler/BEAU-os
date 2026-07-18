/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PCI_DEV_H
#define ARM64_PCI_DEV_H

#include <vconfig.h>

struct pci_pdev;

extern struct acrn_vm_pci_dev_config sos_pci_devs[CONFIG_MAX_PCI_DEV_NUM];

bool allocate_to_prelaunched_vm(struct pci_pdev *pdev);
struct acrn_vm_pci_dev_config *init_one_dev_config(struct pci_pdev *pdev);

#endif /* ARM64_PCI_DEV_H */
