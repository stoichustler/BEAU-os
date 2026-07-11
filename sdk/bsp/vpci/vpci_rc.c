/*-
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2018-2024 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <acrn_common.h>
#include <errno.h>
#include <vm.h>
#include <bsp/pci.h>
#include <logmsg.h>
#include <bsp/vacpi.h>
#include <bsp/vroot_port.h>
#include "vpci_internal.h"

/* Host bridge device IDs whose PCIEXBAR is at config offset 0x60. */
static const uint32_t hostbridge_did_highbytes[] = {0x19U, 0x5aU, 0x59U, 0x3eU, 0x9aU, 0x45U, 0x9bU};

/**
 * @brief Initialize the virtual host bridge.
 *
 * A host bridge is a PCI device that is used to support the pci devices under it. This function initializes the
 * specified virtual PCI device as a host bridge. It's usually called during the initialization of a VM.
 *
 * This function emulates the virtual host bridge as a "Celeron N3350/Pentium N4200/Atom E3900 Series Host Bridge",
 * which belongs to Intel Apollo Lake processors family, and the device id is 0x5af0. Per "Section 9 C-Unit in Intel®
 * Pentium® and Celeron® Processor N- and J- Series, Datasheet Volume 2", it initializes related type info registers in
 * configuration space.
 * PCI Express Enhanced Configuration Range Base Address Register (PCIEXBAR) is emulated differently for pre-launched
 * VMs and Service VM, to support PCI Express Enhanced Configuration Access Mechanism (ECAM).
 * - For a pre-launched VM, it is emulated as 'USER_VM_VIRT_PCI_MMCFG_BASE | 0x1'. USER_VM_VIRT_PCI_MMCFG_BASE
 *   (0xE0000000) is the hard-coded virtual PCI MMCFG address base for pre/post-launched VMs. Bit 0 is set to 1 to
 *   indicate that the base address defined in the PCIEXBAR register is active.
 * - For a Service VM, it is emulated to be the same value as the physical PCIEXBAR. It is not used for now, mainly for
 *   feature extension in the future.
 * Finally, it sets the field parent_user to NULL and the field user to vdev, indicating that this vPCI bridge is used
 * by a VM.
 *
 * @param[inout] vdev Pointer to the virtual PCI device to be initialized.
 *
 * @return None
 *
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 *
 * @post N/A
 */
static void init_vhostbridge(struct pci_vdev *vdev)
{
	union pci_bdf hostbridge_bdf = {.value = 0x0U};
	uint32_t pciexbar_low = 0x0U, pciexbar_high = 0x0U, phys_did, i;
	/* Refer to Section 9 C-Unit in Intel® Pentium® and Celeron® Processor N- and J- Series, Datasheet Volume 2 */
	/* PCI config space */
	pci_vdev_write_vcfg(vdev, PCIR_VENDOR, 2U, 0x8086U);
	pci_vdev_write_vcfg(vdev, PCIR_DEVICE, 2U, 0x5af0U);
	pci_vdev_write_vcfg(vdev, PCIR_REVID, 1U, 0xbU);
	pci_vdev_write_vcfg(vdev, PCIR_SUBCLASS, 1U, PCIS_BRIDGE_HOST);
	pci_vdev_write_vcfg(vdev, PCIR_CLASS, 1U, PCIC_BRIDGE);
	pci_vdev_write_vcfg(vdev, PCIR_HDRTYPE, 1U, (PCIM_HDRTYPE_NORMAL | PCIM_MFDEV));
	/* First Capability Register is CAPID0_CAPCTRL0 */
	pci_vdev_write_vcfg(vdev, PCIR_CAP_PTR, 1U, 0xe0U);
	pci_vdev_write_vcfg(vdev, PCIR_INTERRUPT_LINE, 1U, 0xe0U);

	/* Memory Controller Hub Base Address Register, MCHBAR_LO */
	/* MCHBAR[38:15] is {MCHBAR_HI[6:0],MCHBAR_LO[31:15]} */
	pci_vdev_write_vcfg(vdev, 0x48U, 4U, 0xfed10001U);
	/* Graphics and Memory Controller Hub Graphics Control Register, GGC */
	/* [15:8] is Graphics Memory Select (GMS), 512MB */
	pci_vdev_write_vcfg(vdev, 0x50U, 4U, 0x000002c1U);
	/* Device Enable Register, DEVEN */
	pci_vdev_write_vcfg(vdev, 0x54U, 4U, 0x00000033U);
	/* Protected Audio Video Path Control, PAVPC */
	pci_vdev_write_vcfg(vdev, 0x58U, 4U, 0x7ff00007U);
	/* Top of Upper Usable DRAM Low, TOUUD_LO */
	pci_vdev_write_vcfg(vdev, 0xa8U, 4U, 0x80000000U);
	/* Top of Upper Usable DRAM High, TOUUD_HI */
	pci_vdev_write_vcfg(vdev, 0xacU, 4U, 0x00000002U);
	/* Base of Data Stolen Memory, BDSM */
	pci_vdev_write_vcfg(vdev, 0xb0U, 4U, 0x7c000001U);
	/* Base of Graphics Stolen Memory, BGSM */
	pci_vdev_write_vcfg(vdev, 0xb4U, 4U, 0x7b800001U);
	/* Top Segment Memory Base, TSEGMB */
	pci_vdev_write_vcfg(vdev, 0xb8U, 4U, 0x7b000001U);
	/* Top of Lower Usable DRAM, TOLUD */
	pci_vdev_write_vcfg(vdev, 0xbcU, 4U, 0x80000001U);
	/* Capability ID0 Capability Control, CAPID0_CAPCTRL0 */
	/* CAP_ID: 9h, NEXT_CAP: 0h, CAPIDLEN: Ch, CAPID_VER: 1h */
	pci_vdev_write_vcfg(vdev, 0xe0U, 4U, 0x010c0009U);
	pci_vdev_write_vcfg(vdev, 0xf4U, 4U, 0x011c0f00U);

	if (is_prelaunched_vm(container_of(vdev->vpci, struct acrn_vm, vpci))) {
		/* For pre-launched VMs, we only need to write an GPA that's reserved in guest ve820,
		 * and USER_VM_VIRT_PCI_MMCFG_BASE(0xE0000000) is fine. The trailing 1 is a ECAM enable-bit
		 */
		pciexbar_low = USER_VM_VIRT_PCI_MMCFG_BASE | 0x1U;
	} else {
		/* Inject physical ECAM value to Service VM vhostbridge since Service VM may check PCIe-MMIO Base
		Address with it */
		phys_did = pci_pdev_read_cfg(hostbridge_bdf, PCIR_DEVICE, 2);
		for (i = 0U; i < (sizeof(hostbridge_did_highbytes) / sizeof(uint32_t)); i++) {
			if (((phys_did & 0xff00U) >> 8) == hostbridge_did_highbytes[i]) {
				/* The offset of PCIEXBAR register is 0x60 on Intel platforms, and no counter-case is
				encountered yet */
				pciexbar_low = pci_pdev_read_cfg(hostbridge_bdf, 0x60U, 4);
				pciexbar_high = pci_pdev_read_cfg(hostbridge_bdf, 0x64U, 4);
				break;
			}
		}
	}
	/* PCI Express Enhanced Configuration Range Base Address Low, PCIEXBAR_LO */
	pci_vdev_write_vcfg(vdev, 0x60U, 4, pciexbar_low);
	/* PCI Express Enhanced Configuration Range Base Address High, PCIEXBAR_HI */
	pci_vdev_write_vcfg(vdev, 0x64U, 4, pciexbar_high);
	vdev->parent_user = NULL;
	vdev->user = vdev;
}

/**
 * @brief Deinitialize the virtual host bridge.
 *
 * This function deinitializes the specified virtual PCI device that was previously initialized as a host bridge.
 *
 * For the specified vdev, it sets the fields parent_user and user to NULL, indicating that this virtual device is not
 * owned by any VM.
 *
 * @param[inout] vdev Pointer to the virtual PCI device.
 *
 * @return None
 *
 * @pre vdev != NULL
 *
 * @post N/A
 */
static void deinit_vhostbridge(struct pci_vdev *vdev)
{
	vdev->parent_user = NULL;
	vdev->user = NULL;
}

/**
 * @brief Read the configuration space of the virtual host bridge.
 *
 * This function reads the configuration space of the specified virtual PCI device that is configured as a host bridge.
 * It is used to retrieve specific configuration data of the virtual host bridge for further processing or validation.
 *
 * It reads the configuration space of the virtual host bridge and stores the read configuration data in the provided
 * buffer.
 *
 * @param[in] vdev Pointer to the virtual PCI device whose configuration space is to be read.
 * @param[in] offset Offset within the configuration space to read from.
 * @param[in] bytes Number of bytes to read from the configuration space.
 * @param[inout] val Pointer to the buffer where the read configuration data will be stored.
 *
 * @return Always return 0.
 *
 * @pre vdev != NULL
 * @pre val != NULL
 *
 * @post retval == 0
 */
static int32_t read_vhostbridge_cfg(struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes, uint32_t *val)
{
	*val = pci_vdev_read_vcfg(vdev, offset, bytes);
	return 0;
}

/**
 * @brief Write to the virtual host bridge configuration space.
 *
 * This function writes to the configuration space of the specified virtual PCI device that is configured as a host
 * bridge. It is used to update specific configuration settings based on the provided parameters.
 *
 * For the non-BAR configuration space, it writes the provided value to the configuration space of the virtual host
 * bridge. For the BAR configuration space, it is read-only and the write operation is ignored.
 *
 * @param[inout] vdev Pointer to the virtual PCI device whose configuration space is to be written.
 * @param[in] offset Offset within the configuration space to start writing to.
 * @param[in] bytes Number of bytes to write to the configuration space.
 * @param[in] val Value to be written to the configuration space.
 *
 * @return Always return 0.
 *
 * @pre vdev != NULL
 *
 * @post retval == 0
 */
static int32_t write_vhostbridge_cfg(struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes, uint32_t val)
{
	if (!is_bar_offset(PCI_BAR_COUNT, offset)) {
		pci_vdev_write_vcfg(vdev, offset, bytes, val);
	}
	return 0;
}

/**
 * @brief Data structure implementation for virtual host bridge operations.
 *
 * Struct pci_vdev_ops is used to define the operations of virtual PCI device and definition here is used to support
 * virtual host bridge.
 *
 * A pre-launched VM may have some pci devices and a host bridge is needed to support these devices. This struct is used
 * to define the operations of virtual host bridge in this case for now.
 *
 * @consistency N/A
 * @alignment N/A
 *
 * @remark N/A
 */
const struct pci_vdev_ops vhostbridge_ops = {
	.init_vdev	= init_vhostbridge,
	.deinit_vdev	= deinit_vhostbridge,
	.write_vdev_cfg	= write_vhostbridge_cfg,
	.read_vdev_cfg	= read_vhostbridge_cfg,
};

/**
 * @brief Initializes the vPCI bridge.
 *
 * A PCI bridge is also a PCI device. This function initializes the specified virtual PCI device as a PCI bridge. A vPCI
 * bridge is based on a physical PCI bridge and is used for Service VM. It's usually used in the initialization phase of
 * Service VM, when the pre-launched VM exists.
 *
 * It initializes most of the virtual PCI configuration space registers based on the physical PCI bridge registers,
 * except those that specify the type information, which is emulated. Such type related registers include Vendor ID,
 * Device ID, Revision ID, Header Type, class and sub-class code. Finally, it sets the field parent_user to NULL and
 * the field user to vdev, indicating that this vPCI bridge is used by a VM.
 * Note that the physical PCI bridge registers are already initialized and configured during the initialization of the
 * physical PCI hierarchy.
 *
 * @param[inout] vdev Pointer to the virtual PCI device to be initialized.
 *
 * @return None
 *
 * @pre vdev != NULL
 *
 * @post N/A
 */
static void init_vpci_bridge(struct pci_vdev *vdev)
{
	uint32_t offset, val;

	/* read PCI config space to virtual space */
	for (offset = 0x00U; offset < 0x100U; offset += 4U) {
		val = pci_pdev_read_cfg(vdev->pdev->bdf, offset, 4U);
		pci_vdev_write_vcfg(vdev, offset, 4U, val);
	}

	/* emulated for type info */
	pci_vdev_write_vcfg(vdev, PCIR_VENDOR, 2U, 0x8086U);
	pci_vdev_write_vcfg(vdev, PCIR_DEVICE, 2U, 0x9d12U);

	pci_vdev_write_vcfg(vdev, PCIR_REVID, 1U, 0xf1U);

	pci_vdev_write_vcfg(vdev, PCIR_HDRTYPE, 1U, (PCIM_HDRTYPE_BRIDGE | PCIM_MFDEV));
	pci_vdev_write_vcfg(vdev, PCIR_CLASS, 1U, PCIC_BRIDGE);
	pci_vdev_write_vcfg(vdev, PCIR_SUBCLASS, 1U, PCIS_BRIDGE_PCI);

	vdev->parent_user = NULL;
	vdev->user = vdev;
}

/**
 * @brief Deinitializes the vPCI bridge.
 *
 * This function deinitializes the specified virtual PCI device that was previously initialized as a PCI bridge.
 *
 * For the specified vdev, it sets the fields parent_user and user to NULL, indicating that this virtual device is not
 * owned by any VM.
 *
 * @param[inout] vdev Pointer to the virtual PCI device to be deinitialized.
 *
 * @return None
 *
 * @pre vdev != NULL
 *
 * @post N/A
 */
static void deinit_vpci_bridge(struct pci_vdev *vdev)
{
	vdev->parent_user = NULL;
	vdev->user = NULL;
}

/**
 * @brief Reads the configuration of the vPCI bridge.
 *
 * This function reads the configuration space of the specified virtual PCI device that is configured as a PCI bridge.
 * It is used to retrieve the configuration data of the vPCI bridge for further processing or validation.
 *
 * - For PCI configuration space (offset <= 0x100U), it reads the configuration space of the vPCI bridge.
 * - For PCI Express Extended configuration space (offset > 0x100U), it simply passthrough by reading directly from the
 *   physical device.
 * - The read configuration data is stored in the buffer pointed to by val.
 *
 * @param[in] vdev Pointer to the virtual PCI device whose configuration is to be read.
 * @param[in] offset Offset within the configuration space to start reading from.
 * @param[in] bytes Number of bytes to read from the configuration space.
 * @param[inout] val Pointer to the buffer where the read configuration data will be stored.
 *
 * @return Always return 0.
 *
 * @pre vdev != NULL
 * @pre val != NULL
 *
 * @post retval == 0
 */
static int32_t read_vpci_bridge_cfg(struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes, uint32_t *val)
{
	if ((offset + bytes) <= 0x100U) {
		*val = pci_vdev_read_vcfg(vdev, offset, bytes);
	} else {
		/* just passthru read to physical device when read PCIE sapce > 0x100 */
		*val = pci_pdev_read_cfg(vdev->pdev->bdf, offset, bytes);
	}

	return 0;
}

/**
 * @brief Writes the configuration of the vPCI bridge.
 *
 * This function writes to the configuration space of the specified virtual PCI device that is configured as a PCI
 * bridge. It is used to update the configuration data of the vPCI bridge. However, the configuration space of the vPCI
 * bridge is read-only, so this function does not perform any operation.
 *
 * It just returns 0 without any operation.
 *
 * @param[in] vdev Pointer to the virtual PCI device whose configuration is to be written (unused in this function).
 * @param[in] offset Offset within the configuration space to start writing to (unused in this function).
 * @param[in] bytes Number of bytes to write to the configuration space (unused in this function).
 * @param[in] val Value to be written to the configuration space (unused in this function).
 *
 * @return Always return 0.
 *
 * @pre vdev != NULL
 *
 * @post retval == 0
 */
static int32_t write_vpci_bridge_cfg(__unused struct pci_vdev *vdev, __unused uint32_t offset,
	__unused uint32_t bytes, __unused uint32_t val)
{
	return 0;
}

/**
 * @brief Data structure implementation for virtual PCI bridge operations.
 *
 * Struct pci_vdev_ops is used to define the operations of virtual PCI device and definition here is used to support PCI
 * bridge.
 *
 * All PCI devices (including PCI bridge) on platform are passed to Service VM by default. But PCI bridges should be
 * emulated by hypervisor if pre-launched VM exists. This struct is used to define the operations of virtual PCI bridge
 * in this case.
 *
 * @consistency N/A
 * @alignment N/A
 *
 * @remark N/A
 */
const struct pci_vdev_ops vpci_bridge_ops = {
	.init_vdev         = init_vpci_bridge,
	.deinit_vdev       = deinit_vpci_bridge,
	.write_vdev_cfg    = write_vpci_bridge_cfg,
	.read_vdev_cfg     = read_vpci_bridge_cfg,
};

/* config space of dummy multifunction device */
#define PCI_DUMMY_DEVICE_VENDOR		0x1D94U
#define PCI_DUMMY_DEVICE_ID		0x145AU
#define DUMMY_MF_REV			0x1U
#define DUMMY_MF_CLASS			0x0U

static void init_vpci_mf_dev(struct pci_vdev *vdev)
{
	pci_vdev_write_vcfg(vdev, PCIR_VENDOR, 2U, PCI_DUMMY_DEVICE_VENDOR);
	pci_vdev_write_vcfg(vdev, PCIR_DEVICE, 2U, PCI_DUMMY_DEVICE_ID);
	pci_vdev_write_vcfg(vdev, PCIR_REVID, 1U, DUMMY_MF_REV);
	pci_vdev_write_vcfg(vdev, PCIR_CLASS, 1U, DUMMY_MF_CLASS);
	pci_vdev_write_vcfg(vdev, PCIR_HDRTYPE, 1U, PCIM_HDRTYPE_NORMAL | PCIM_MFDEV);

	vdev->parent_user = NULL;
	vdev->user = vdev;
}

static void deinit_vpci_mf_dev(struct pci_vdev *vdev)
{
	vdev->parent_user = NULL;
	vdev->user = NULL;
}

static int32_t read_vpci_mf_dev(struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes, uint32_t *val)
{
	*val = pci_vdev_read_vcfg(vdev, offset, bytes);

	return 0;
}

static int32_t write_vpci_mf_dev(__unused struct pci_vdev *vdev, __unused uint32_t offset,
	__unused uint32_t bytes, __unused uint32_t val)
{
	return 0;
}

const struct pci_vdev_ops vpci_mf_dev_ops = {
	.init_vdev         = init_vpci_mf_dev,
	.deinit_vdev       = deinit_vpci_mf_dev,
	.write_vdev_cfg    = write_vpci_mf_dev,
	.read_vdev_cfg     = read_vpci_mf_dev,
};

#define PCIE_CAP_VPOS		0x40				/* pcie capability reg position */
#define PTM_CAP_VPOS		PCI_ECAP_BASE_PTR	/* ptm capability reg postion */

static void init_vrp(struct pci_vdev *vdev)
{
	/* vendor and device */
	pci_vdev_write_vcfg(vdev, PCIR_VENDOR, 2U, VRP_VENDOR);
	pci_vdev_write_vcfg(vdev, PCIR_DEVICE, 2U, VRP_DEVICE);

	/* status register */
	pci_vdev_write_vcfg(vdev, PCIR_STATUS, 2U, PCIM_STATUS_CAPPRESENT);

	/* rev id */
	pci_vdev_write_vcfg(vdev, PCIR_REVID, 1U, 0x01U);

	/* sub class */
	pci_vdev_write_vcfg(vdev, PCIR_SUBCLASS, 1U, PCIS_BRIDGE_PCI);

	/* class */
	pci_vdev_write_vcfg(vdev, PCIR_CLASS, 1U, PCIC_BRIDGE);

	/* Header Type */
	pci_vdev_write_vcfg(vdev, PCIR_HDRTYPE, 1U, PCIM_HDRTYPE_BRIDGE);

	/* capability pointer */
	pci_vdev_write_vcfg(vdev, PCIR_CAP_PTR, 1U, PCIE_CAP_VPOS);

	/* pcie capability registers  */
	pci_vdev_write_vcfg(vdev, PCIE_CAP_VPOS + PCICAP_ID, 1U, PCIY_PCIE);

	/* bits (3:0): capability version = 010b
	 * bits (7:4)  device/port type = 0100b (root port of pci-e)
	 * bits (8) -- slot implemented = 1b
	 */
	pci_vdev_write_vcfg(vdev, PCIE_CAP_VPOS + PCICAP_EXP_CAP, 2U, 0x0142);

	/* It seems important that passthru device's max payload settings match
	 * the settings on the native device otherwise passthru device may not work.
	 * So we have to set vrp's max payload capacity as native root port
	 * otherwise we may accidentally change passthru device's max payload since
	 * during guest OS's pci device enumeration, pass-thru device will renegotiate
	 * its max payload's setting with vrp.
	 */
	pci_vdev_write_vcfg(vdev, PCIE_CAP_VPOS + PCIR_PCIE_DEVCAP, 4U,
			vdev->pci_dev_config->vrp_max_payload);

	/* In theory, we don't need to program dev ctr's max payload and hopefully OS
	 * will program it but we cannot always rely on OS to program
	 * this register.
	 */
	pci_vdev_write_vcfg(vdev, PCIE_CAP_VPOS + PCIR_PCIE_DEVCTRL, 2U,
			(vdev->pci_dev_config->vrp_max_payload << 5) & PCIM_PCIE_DEV_CTRL_MAX_PAYLOAD);

	vdev->parent_user = NULL;
	vdev->user = vdev;
}

static void deinit_vrp(__unused struct pci_vdev *vdev)
{
	vdev->parent_user = NULL;
	vdev->user = NULL;
}

static int32_t read_vrp_cfg(struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes, uint32_t *val)
{
	*val = pci_vdev_read_vcfg(vdev, offset, bytes);

	return 0;
}

static int32_t write_vrp_cfg(__unused struct pci_vdev *vdev, __unused uint32_t offset,
	__unused uint32_t bytes, __unused uint32_t val)
{
	pci_vdev_write_vcfg(vdev, offset, bytes, val);

	return 0;
}

/*
 * @pre vdev != NULL
 * @pre vrp_config != NULL
 */
static void init_ptm(struct pci_vdev *vdev, struct vrp_config *vrp_config)
{
	/* ptm capability register */
	if (vrp_config->ptm_capable)
	{
		pci_vdev_write_vcfg(vdev, PTM_CAP_VPOS, PCI_PTM_CAP_LEN, 0x0001001f);

		pci_vdev_write_vcfg(vdev, PTM_CAP_VPOS + PCIR_PTM_CAP, PCI_PTM_CAP_LEN, 0x406);

		pci_vdev_write_vcfg(vdev, PTM_CAP_VPOS + PCIR_PTM_CTRL, PCI_PTM_CAP_LEN, 0x3);
	}

	/* emulate bus numbers */
	pci_vdev_write_vcfg(vdev, PCIR_PRIBUS_1, 1U, 0x00); /* virtual root port always connects to host bridge */
	pci_vdev_write_vcfg(vdev, PCIR_SECBUS_1, 1U, vrp_config->secondary_bus);
	pci_vdev_write_vcfg(vdev, PCIR_SUBBUS_1, 1U, vrp_config->subordinate_bus);
}

int32_t create_vrp(struct acrn_vm *vm, struct acrn_vdev *dev)
{
	int32_t ret = 0;
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	struct acrn_vm_pci_dev_config *dev_config = NULL;
	struct pci_vdev *vdev;
	struct vrp_config *vrp_config;

	uint16_t i;

	vrp_config = (struct vrp_config*)dev->args;

	LOG_INF("%s: virtual root port phy_bdf=0x%x, vbdf=0x%x, vendor_id=0x%x, dev_id=0x%x,\
			primary_bus=0x%x, secondary_bus=0x%x, sub_bus=0x%x.\n",
			__func__, vrp_config->phy_bdf, dev->slot,
			dev->id.fields.vendor, dev->id.fields.device,
			vrp_config->primary_bus, vrp_config->secondary_bus, vrp_config->subordinate_bus);

	for (i = 0U; i < vm_config->pci_dev_num; i++) {
		dev_config = &vm_config->pci_devs[i];
		if (dev_config->vrp_sec_bus == vrp_config->secondary_bus) {
			dev_config->vbdf.value = (uint16_t)dev->slot;
			dev_config->pbdf.value = vrp_config->phy_bdf;
			dev_config->vrp_max_payload = vrp_config->max_payload;
			dev_config->vdev_ops = &vrp_ops;

			spinlock_obtain(&vm->vpci.lock);
			vdev = vpci_init_vdev(&vm->vpci, dev_config, NULL);
			spinlock_release(&vm->vpci.lock);
			if (vdev == NULL) {
				LOG_ERR("%s: failed to create virtual root port\n", __func__);
				ret = -EFAULT;
				break;
			}

			init_ptm(vdev, vrp_config);

			break;
		}
	}

	return ret;
}

int32_t destroy_vrp(struct pci_vdev *vdev)
{
	struct acrn_vpci *vpci = vdev->vpci;

	spinlock_obtain(&vpci->lock);
	vpci_deinit_vdev(vdev);
	spinlock_release(&vpci->lock);

	return 0;
}

const struct pci_vdev_ops vrp_ops = {
	.init_vdev         = init_vrp,
	.deinit_vdev       = deinit_vrp,
	.write_vdev_cfg    = write_vrp_cfg,
	.read_vdev_cfg     = read_vrp_cfg,
};
