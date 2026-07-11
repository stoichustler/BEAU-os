/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PLATFORM_ACPI_INFO_H
#define ARM64_PLATFORM_ACPI_INFO_H

/*
 * Static ARM64 platforms do not consume ACPI, but the shared PCI scanner keeps
 * these names for the physical ECAM window. The QEMU values mirror the `virt`
 * machine device tree and can be overwritten by platform DTS parsing.
 */
#define DEFAULT_PCI_MMCFG_BASE		0x4010000000UL
#define DEFAULT_PCI_MMCFG_START_BUS	0U
#define DEFAULT_PCI_MMCFG_END_BUS	0xffU

#endif /* ARM64_PLATFORM_ACPI_INFO_H */
