/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_BOARD_H
#define ARM64_BOARD_H

#include <types.h>
#include <board_info.h>
#include <bsp/pci.h>
#include <asm/vtd.h>

extern struct dmar_info plat_dmar_info;
extern const union pci_bdf plat_hidden_pdevs[];

struct vmsix_on_msi_info {
	union pci_bdf bdf;
	uint64_t mmio_base;
};

extern const struct vmsix_on_msi_info vmsix_on_msi_devs[];

#endif /* ARM64_BOARD_H */
