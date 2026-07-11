/*
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2018-2022 Intel Corporation.
 * Copyright (c) 2026 Hustler Lo.
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

#include <errno.h>
#include <io.h>
#include <ptdev.h>
#include <vm.h>
#include <asm/board.h>
#include <asm/mmu.h>
#include <asm/vtd.h>
#include <bsp/vpci.h>
#include <logmsg.h>
#include <pgtable.h>
#include "vpci_internal.h"

/*
 * MSI/MSI-X remap model
 *
 * Guest driver view:
 *   write virtual MSI address/data or MSI-X table entry
 *
 * Hypervisor action:
 *   1. keep the guest-programmed value in vPCI shadow state;
 *   2. allocate/update an interrupt-remap entry for the VM event;
 *   3. rewrite the physical MSI message with the remapped address/data;
 *   4. program only the remapped message into the physical device.
 *
 * This keeps guest routing values out of the physical device. The device can
 * raise an interrupt, but the final target is selected by the remap entry and
 * delivered as a virtual interrupt to the owning VM.
 */

static inline uint32_t vmsi_mask_offset(const struct pci_vdev *vdev)
{
	return vdev->msi.is_64bit ? PCIR_MSI_MASK : (PCIR_MSI_MASK - 4U);
}

static inline uint32_t vmsi_pending_offset(const struct pci_vdev *vdev)
{
	return vdev->msi.is_64bit ? PCIR_MSI_PENDING : (PCIR_MSI_PENDING - 4U);
}

static inline bool vmsi_has_mask(const struct pci_vdev *vdev)
{
	uint32_t msgctrl = pci_vdev_read_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U);

	return ((msgctrl & PCIM_MSICTRL_PVMC) != 0U);
}

static bool vmsi_masked(const struct pci_vdev *vdev)
{
	uint32_t mask_bits;

	if (!vmsi_has_mask(vdev)) {
		return false;
	}

	mask_bits = pci_vdev_read_vcfg(vdev, vdev->msi.capoff + vmsi_mask_offset(vdev), 4U);
	return ((mask_bits & 0x1U) != 0U);
}

static uint32_t vmsi_vector_count_from_ctrl(uint32_t msgctrl)
{
	uint32_t mme = (msgctrl & PCIM_MSICTRL_MME_MASK) >> PCIM_MSICTRL_MME_SHIFT;

	return 1U << mme;
}

static uint32_t vmsi_max_vector_count(uint32_t msgctrl)
{
	uint32_t mmc = (msgctrl & PCIM_MSICTRL_MMC_MASK) >> PCIM_MSICTRL_MMC_SHIFT;

	return 1U << mmc;
}

static uint32_t vmsi_guest_event_id(uint32_t data, uint32_t vector)
{
	return (data + vector) & 0xffffU;
}

static bool vmsi_offset_writable(const struct pci_vdev *vdev, uint32_t reg)
{
	bool writable = false;
	uint32_t mask_offset = vmsi_mask_offset(vdev);
	uint32_t pending_offset = vmsi_pending_offset(vdev);

	if ((reg >= PCIR_MSI_ADDR) && (reg < (PCIR_MSI_ADDR + 4U))) {
		writable = true;
	} else if (vdev->msi.is_64bit &&
		(reg >= PCIR_MSI_ADDR_HIGH) && (reg < (PCIR_MSI_ADDR_HIGH + 4U))) {
		writable = true;
	} else if (vdev->msi.is_64bit &&
		(reg >= PCIR_MSI_DATA_64BIT) && (reg < (PCIR_MSI_DATA_64BIT + 2U))) {
		writable = true;
	} else if (!vdev->msi.is_64bit &&
		(reg >= PCIR_MSI_DATA) && (reg < (PCIR_MSI_DATA + 2U))) {
		writable = true;
	} else if (vmsi_has_mask(vdev) &&
		(reg >= mask_offset) && (reg < (mask_offset + 4U))) {
		writable = true;
	} else if (vmsi_has_mask(vdev) &&
		(reg >= pending_offset) && (reg < (pending_offset + 4U))) {
		writable = false;
	}

	return writable;
}

static uint8_t vmsi_ro_mask_byte(const struct pci_vdev *vdev, uint32_t reg)
{
	uint8_t ro_mask = 0xffU;

	switch (reg) {
	case 0U:
	case 1U:
		ro_mask = 0xffU;
		break;
	case PCIR_MSI_CTRL:
		ro_mask = 0x8eU;
		break;
	case (PCIR_MSI_CTRL + 1U):
		ro_mask = 0xffU;
		break;
	default:
		ro_mask = vmsi_offset_writable(vdev, reg) ? 0x00U : 0xffU;
		break;
	}

	return ro_mask;
}

static uint32_t vmsi_width_mask(uint32_t bytes)
{
	return (bytes >= 4U) ? UINT32_MAX : ((1UL << (bytes * 8U)) - 1UL);
}

static uint32_t vmsi_ro_mask(const struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes)
{
	uint32_t ro_mask = 0U;
	uint32_t idx;

	for (idx = 0U; idx < bytes; idx++) {
		uint32_t reg = (offset - vdev->msi.capoff) + idx;

		ro_mask |= (uint32_t)vmsi_ro_mask_byte(vdev, reg) << (idx * 8U);
	}

	return ro_mask & vmsi_width_mask(bytes);
}

static bool vmsi_access_valid(const struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes)
{
	return (bytes <= 4U) && (bytes != 0U) &&
		(offset >= vdev->msi.capoff) &&
		((offset + bytes) <= (vdev->msi.capoff + vdev->msi.caplen));
}


/**
 * @pre vdev != NULL
 * @pre vdev->pdev != NULL
 */
static inline void enable_disable_msi(const struct pci_vdev *vdev, bool enable)
{
	union pci_bdf pbdf = vdev->pdev->bdf;
	uint32_t capoff = vdev->msi.capoff;
	uint32_t msgctrl = pci_pdev_read_cfg(pbdf, capoff + PCIR_MSI_CTRL, 2U);

	if (enable) {
		msgctrl |= PCIM_MSICTRL_MSI_ENABLE;
	} else {
		msgctrl &= ~PCIM_MSICTRL_MSI_ENABLE;
	}
	pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_CTRL, 2U, msgctrl);
}
/**
 * @brief Remap vMSI virtual address and data to MSI physical address and data
 * This function is called when physical MSI is disabled.
 *
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->pdev != NULL
 */
static void remap_vmsi(const struct pci_vdev *vdev)
{
	struct msi_info info = {};
	struct msi_info pmsg = {};
	union pci_bdf pbdf = vdev->pdev->bdf;
	struct acrn_vm *vm = vpci2vm(vdev->vpci);
	uint32_t capoff = vdev->msi.capoff;
	uint32_t vmsi_msgdata, vmsi_addrlo, vmsi_addrhi = 0U;
	uint32_t msgctrl;
	uint32_t vector_count;
	uint32_t vector;
	int32_t ret;

	/* Read the MSI capability structure from virtual device */
	vmsi_addrlo = pci_vdev_read_vcfg(vdev, (capoff + PCIR_MSI_ADDR), 4U);
	if (vdev->msi.is_64bit) {
		vmsi_addrhi = pci_vdev_read_vcfg(vdev, (capoff + PCIR_MSI_ADDR_HIGH), 4U);
		vmsi_msgdata = pci_vdev_read_vcfg(vdev, (capoff + PCIR_MSI_DATA_64BIT), 2U);
	} else {
		vmsi_msgdata = pci_vdev_read_vcfg(vdev, (capoff + PCIR_MSI_DATA), 2U);
	}
	info.addr.full = (uint64_t)vmsi_addrlo | ((uint64_t)vmsi_addrhi << 32U);
	info.data.full = vmsi_msgdata;
	msgctrl = pci_vdev_read_vcfg(vdev, capoff + PCIR_MSI_CTRL, 2U);
	vector_count = vmsi_vector_count_from_ctrl(msgctrl);

	/*
	 * MSI vector count and message data are programmed as a set. Rebuild
	 * the whole mapping whenever the guest changes the MSI capability so no
	 * stale vector remains routed to the physical function.
	 */
	if (vdev->msi.vector_count != 0U) {
		ptirq_remove_msi_remapping(vm, pbdf.value, vdev->msi.vector_count);
		((struct pci_vdev *)vdev)->msi.vector_count = 0U;
	}

	for (vector = 0U; vector < vector_count; vector++) {
		struct msi_info vector_info = info;

		vector_info.data.full = vmsi_guest_event_id(vmsi_msgdata, vector);
		ret = ptirq_prepare_msi_remap(vm, vdev->bdf.value, pbdf.value,
			(uint16_t)vector, &vector_info, INVALID_IRTE_ID);
		if (ret != 0) {
			LOG_WRN("vm%u vmsi remap failed for %02x:%02x.%x vector %u ret %d",
				vm->vm_id, vdev->bdf.bits.b, vdev->bdf.bits.d,
				vdev->bdf.bits.f, vector, ret);
			if (vector != 0U) {
				ptirq_remove_msi_remapping(vm, pbdf.value, vector);
			}
			return;
		}
		if (vector == 0U) {
			pmsg = vector_info;
		}
	}

	ret = 0;
	if (ret == 0) {
		uint32_t pctrl;

		pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_ADDR, 0x4U, (uint32_t)pmsg.addr.full);
		if (vdev->msi.is_64bit) {
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_ADDR_HIGH, 0x4U,
					(uint32_t)(pmsg.addr.full >> 32U));
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_DATA_64BIT, 0x2U,
				(uint16_t)pmsg.data.full);
		} else {
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_DATA, 0x2U,
				(uint16_t)pmsg.data.full);
		}
		if (vmsi_has_mask(vdev)) {
			pci_pdev_write_cfg(pbdf, capoff + vmsi_mask_offset(vdev), 0x4U,
				pci_vdev_read_vcfg(vdev, capoff + vmsi_mask_offset(vdev), 4U));
		}

		pctrl = pci_pdev_read_cfg(pbdf, capoff + PCIR_MSI_CTRL, 2U);
		pctrl &= ~PCIM_MSICTRL_MME_MASK;
		pctrl |= msgctrl & PCIM_MSICTRL_MME_MASK;
		pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_CTRL, 2U, pctrl);
		/* If MSI Enable is being set, make sure INTxDIS bit is set */
		enable_disable_pci_intx(pbdf, false);
		enable_disable_msi(vdev, true);
		((struct pci_vdev *)vdev)->msi.vector_count = vector_count;
	}
}

static void disable_vmsi_remap(const struct pci_vdev *vdev)
{
	if (vdev->msi.vector_count != 0U) {
		ptirq_remove_msi_remapping(vpci2vm(vdev->vpci), vdev->pdev->bdf.value,
			vdev->msi.vector_count);
		((struct pci_vdev *)vdev)->msi.vector_count = 0U;
	}
	enable_disable_msi(vdev, false);
}

/**
 * @brief Writing MSI Capability Structure
 *
 * @pre vdev != NULL
 */
void write_vmsi_cap_reg(struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t val)
{
	uint32_t msgctrl, old;
	uint32_t ro_mask;
	uint32_t width_mask;

	if (vmsi_access_valid(vdev, offset, bytes)) {
		width_mask = vmsi_width_mask(bytes);
		ro_mask = vmsi_ro_mask(vdev, offset, bytes);
		if (ro_mask == width_mask) {
			return;
		}

		disable_vmsi_remap(vdev);

		/*
		 * Keep architectural read-only MSI fields virtualized, then
		 * rebuild the physical remap only if the resulting virtual state
		 * is enabled and unmasked.
		 */
		old = pci_vdev_read_vcfg(vdev, offset, bytes);
		pci_vdev_write_vcfg(vdev, offset, bytes, (old & ro_mask) | (val & ~ro_mask));

		msgctrl = pci_vdev_read_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U);
		if (vmsi_vector_count_from_ctrl(msgctrl) > vmsi_max_vector_count(msgctrl)) {
			msgctrl &= ~PCIM_MSICTRL_MME_MASK;
			msgctrl |= ((msgctrl & PCIM_MSICTRL_MMC_MASK) >> PCIM_MSICTRL_MMC_SHIFT) <<
				PCIM_MSICTRL_MME_SHIFT;
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U, msgctrl);
		}
		if (((msgctrl & PCIM_MSICTRL_MSI_ENABLE) != 0U) && !vmsi_masked(vdev)) {
			remap_vmsi(vdev);
		} else {
			disable_vmsi_remap(vdev);
		}
	}
}

/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 */
void deinit_vmsi(const struct pci_vdev *vdev)
{
	if (has_msi_cap(vdev)) {
		disable_vmsi_remap(vdev);
	}
}

/**
 * @pre vdev != NULL
 * @pre vdev->pdev != NULL
 */
void init_vmsi(struct pci_vdev *vdev)
{
	struct pci_pdev *pdev = vdev->pdev;
	uint32_t ctrl;
	uint32_t offset;
	bool has_pvm;

	vdev->msi.capoff = pdev->msi_capoff;

	if (has_msi_cap(vdev)) {
		ctrl = pci_pdev_read_cfg(pdev->bdf, vdev->msi.capoff + PCIR_MSI_CTRL, 2U);
		vdev->msi.is_64bit = ((ctrl & PCIM_MSICTRL_64BIT) != 0U);
		has_pvm = ((ctrl & PCIM_MSICTRL_PVMC) != 0U);
		if (vdev->msi.is_64bit) {
			vdev->msi.caplen = has_pvm ? MSI_CAPLEN_64_PVM : MSI_CAPLEN_64;
		} else {
			vdev->msi.caplen = has_pvm ? MSI_CAPLEN_32_PVM : MSI_CAPLEN_32;
		}

		for (offset = 0U; offset < vdev->msi.caplen; offset++) {
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + offset, 1U,
				pci_pdev_read_cfg(pdev->bdf, vdev->msi.capoff + offset, 1U));
		}

		ctrl &= ~(PCIM_MSICTRL_MSI_ENABLE | PCIM_MSICTRL_MME_MASK);
		vdev->msi.vector_count = 0U;
		pci_vdev_write_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U, ctrl);
		pci_vdev_write_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_ADDR, 4U, 0U);
		if (vdev->msi.is_64bit) {
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_ADDR_HIGH, 4U, 0U);
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_DATA_64BIT, 2U, 0U);
		} else {
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + PCIR_MSI_DATA, 2U, 0U);
		}
		if (has_pvm) {
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + vmsi_mask_offset(vdev), 4U, 0U);
			pci_vdev_write_vcfg(vdev, vdev->msi.capoff + vmsi_pending_offset(vdev), 4U, 0U);
		}
	}
}

/**
 * @brief Reading MSI-X Capability Structure
 *
 * @pre vdev != NULL
 * @pre vdev->pdev != NULL
 */
void read_vmsix_cap_reg(struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t *val)
{
	static const uint8_t msix_pt_mask[12U] = {
		0x0U, 0x0U, 0xffU, 0xffU };	/* Only PT MSI-X Message Control Register */
	uint32_t virt, phy = 0U, ctrl, pt_mask = 0U;

	virt = pci_vdev_read_vcfg(vdev, offset, bytes);
	(void)memcpy_s((void *)&pt_mask, bytes, (void *)&msix_pt_mask[offset - vdev->msix.capoff], bytes);
	if (pt_mask != 0U) {
		phy = pci_pdev_read_cfg(vdev->pdev->bdf, offset, bytes);
		ctrl = pci_pdev_read_cfg(vdev->pdev->bdf, vdev->msix.capoff + PCIR_MSIX_CTRL, 2U);
		if (((ctrl & PCIM_MSIXCTRL_TABLE_SIZE) + 1U) != vdev->msix.table_count) {
			vdev->msix.table_count = (ctrl & PCIM_MSIXCTRL_TABLE_SIZE) + 1U;
			LOG_INF("%s reprogram msi-x table size to %d\n", __func__, vdev->msix.table_count);
			/* In this case, the MSI-X stage-2 mapping does not need to be removed again. */
			ASSERT(vdev->msix.table_count <= (PAGE_SIZE/ MSIX_TABLE_ENTRY_SIZE), "");
		}
	}

	*val = (virt & ~pt_mask) | (phy & pt_mask);
}

/**
 * @brief Writing MSI-X Capability Structure
 *
 * @pre vdev != NULL
 * @pre vdev->pdev != NULL
 */
bool write_vmsix_cap_reg(struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t val)
{
	static const uint8_t msix_ro_mask[12U] = {
		0xffU, 0xffU, 0xffU, 0x3fU,	/* Only Function Mask and MSI-X Enable writable */
		0xffU, 0xffU, 0xffU, 0xffU,
		0xffU, 0xffU, 0xffU, 0xffU };
	bool is_written = false;
	uint32_t old, ro_mask = ~0U;

	(void)memcpy_s((void *)&ro_mask, bytes, (void *)&msix_ro_mask[offset - vdev->msix.capoff], bytes);
	if (ro_mask != ~0U) {
		old = pci_vdev_read_vcfg(vdev, offset, bytes);
		pci_vdev_write_vcfg(vdev, offset, bytes, (old & ro_mask) | (val & ~ro_mask));
		is_written = true;
	}

	return is_written;
}

/**
 * @pre vdev != NULL
 * @pre io_req != NULL
 * @pre mmio->address >= vdev->msix.mmio_gpa
 */
uint32_t rw_vmsix_table(struct pci_vdev *vdev, struct io_request *io_req)
{
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	struct msix_table_entry *entry;
	uint32_t entry_offset, table_offset, index = CONFIG_MAX_MSIX_TABLE_NUM;
	uint64_t offset;
	void *hva;

	if ((mmio->size <= 8U) && mem_aligned_check(mmio->address, mmio->size)) {
		offset = mmio->address - vdev->msix.mmio_gpa;
		if (msixtable_access(vdev, (uint32_t)offset)) {
			/* Must be full DWORD or full QWORD aligned. */
			if ((mmio->size == 4U) || (mmio->size == 8U)) {

				table_offset = (uint32_t)(offset - vdev->msix.table_offset);
				index = table_offset / MSIX_TABLE_ENTRY_SIZE;

				entry = &vdev->msix.table_entries[index];
				entry_offset = table_offset % MSIX_TABLE_ENTRY_SIZE;

				if (mmio->direction == ACRN_IOREQ_DIR_READ) {
					(void)memcpy_s(&mmio->value, (size_t)mmio->size,
						(void *)entry + entry_offset, (size_t)mmio->size);
				} else {
					/*
					 * MSI-X table writes update the shadow table only. The
					 * physical table is programmed later with the remapped
					 * message, never with the raw guest value.
					 */
					(void)memcpy_s((void *)entry + entry_offset, (size_t)mmio->size,
						&mmio->value, (size_t)mmio->size);
				}
			} else {
				LOG_ERR("%s, only dword and qword are permitted", __func__);
			}
		} else {
			if (vdev->pdev != NULL) {
				hva = hpa2hva(vdev->msix.mmio_hpa + (mmio->address - vdev->msix.mmio_gpa));
				pre_user_access();
				if (mmio->direction == ACRN_IOREQ_DIR_READ) {
					mmio->value = mmio_read(hva, mmio->size);
				} else {
					mmio_write(hva, mmio->size, mmio->value);
				}
				post_user_access();
			} else {
				if (mmio->direction == ACRN_IOREQ_DIR_READ) {
					mmio->value = 0UL;
				}
			}
		}
	}

	return index;
}

/**
 * @pre io_req != NULL
 * @pre priv_data != NULL
 */
int32_t vmsix_handle_table_mmio_access(struct io_request *io_req, void *priv_data)
{
	(void)rw_vmsix_table((struct pci_vdev *)priv_data, io_req);
	return 0;
}

/**
 * @pre vdev != NULL
 */
int32_t add_vmsix_capability(struct pci_vdev *vdev, uint32_t entry_num, uint8_t bar_num)
{
	uint32_t table_size, i;
	struct msixcap msixcap;
	int32_t ret = -1;

	if ((bar_num < PCI_BAR_COUNT) &&
		(entry_num <= min(CONFIG_MAX_MSIX_TABLE_NUM, VMSIX_MAX_TABLE_ENTRY_NUM))) {

		table_size = VMSIX_MAX_ENTRY_TABLE_SIZE;

		vdev->msix.caplen = MSIX_CAPLEN;
		vdev->msix.table_bar = bar_num;
		vdev->msix.table_offset = 0U;
		vdev->msix.table_count = entry_num;

		/* set mask bit of vector control register */
		for (i = 0; i < entry_num; i++) {
			vdev->msix.table_entries[i].vector_control |= PCIM_MSIX_VCTRL_MASK;
		}

		(void)memset(&msixcap, 0U, sizeof(struct msixcap));

		msixcap.capid = PCIY_MSIX;
		msixcap.msgctrl = (uint16_t)entry_num - 1U;

		/* - MSI-X table start at offset 0 */
		msixcap.table_info = bar_num;
		msixcap.pba_info = table_size | bar_num;

		vdev->msix.capoff = vpci_add_capability(vdev, (uint8_t *)(&msixcap), sizeof(struct msixcap));
		if (vdev->msix.capoff != 0U) {
			ret = 0;
		}
	}
	return ret;
}

#define PER_VECTOR_MASK_CAP 0x0100U

#if (MAX_VMSIX_ON_MSI_PDEVS_NUM > 0)
/* Pre-assumptions for vMSI-x on MSI emulation:
 * 1. The device is in vmsix_on_msi_devs array.
 * 2. The device should support MSI capability as well as per-vector mask
 * 3. The device doesn't support MSI-x capability.
 * 4. The device should have an unused BAR (this condition is checked inside init_vmsix_on_msi).
 * 5. HV doesn't emulate PBA according to physcial device status, the device driver should not rely on PBA
 *    for functionality.
 */
static bool need_vmsix_on_msi_emulation(__unused struct pci_pdev *pdev, __unused uint16_t *vector_count)
{
	bool ret = false;
#if (MAX_VMSIX_ON_MSI_PDEVS_NUM > 0)
	uint16_t msgctrl;
	uint32_t i;

	for(i = 0U; i < MAX_VMSIX_ON_MSI_PDEVS_NUM; i++) {
		if (pdev->bdf.value == vmsix_on_msi_devs[i].bdf.value) {
			if ((pdev->msi_capoff != 0U) && (pdev->msix.capoff == 0U)) {
				msgctrl = (uint16_t)pci_pdev_read_cfg(pdev->bdf, pdev->msi_capoff + PCIR_MSI_CTRL, 2U);
				*vector_count = 1U << ((msgctrl & PCIM_MSICTRL_MMC_MASK) >>
					PCIM_MSICTRL_MMC_SHIFT);
				if ((*vector_count > 1U) && ((msgctrl & PER_VECTOR_MASK_CAP) != 0U)) {
					ret = true;
				}
			}
			break;
		}
	}
#endif

	return ret;
}
#endif

void reserve_vmsix_on_msi_irtes(struct pci_pdev *pdev)
{
#if (MAX_VMSIX_ON_MSI_PDEVS_NUM > 0)
	struct intr_source intr_src;
	uint16_t count = 0;
	int32_t ret;

	if (need_vmsix_on_msi_emulation(pdev, &count)) {
		intr_src.is_msi = true;
		intr_src.src.msi.value = pdev->bdf.value;
		ret = dmar_reserve_irte(&intr_src, count, &pdev->irte_start);
		if ((ret == 0) && (pdev->irte_start != INVALID_IRTE_ID)) {
			pdev->irte_count = count;
		}
	}
#else
	(void)pdev;
#endif
}

static inline uint32_t get_mask_bits_offset(const struct pci_vdev *vdev)
{
	return vdev->msi.is_64bit ? (vdev->msix.capoff + 0x10U) : (vdev->msix.capoff + 0xCU);
}

/**
 * @pre vdev != NULL
 * @pre vdev->pdev != NULL
 */
void init_vmsix_on_msi(struct pci_vdev *vdev)
{
	struct pci_pdev *pdev = vdev->pdev;
	uint32_t i;

	/* irte_count > 1 only when the device needs vMSI-x on MSI emulation and IRTEs are reserved successfully */
	if (pdev->irte_count > 1U) {
		/* find an unused BAR */
		for (i = 0U; i < vdev->nr_bars; i++) {
			if (vdev->vbars[i].base_hpa == 0UL){
				break;
			}
			if (is_pci_mem64lo_bar(&vdev->vbars[i])) {
				i++;
			}
		}
		if (i < vdev->nr_bars) {
			vdev->msix.capoff = pdev->msi_capoff;
			vdev->msi.capoff = 0U;
			vdev->msix.is_vmsix_on_msi = true;
			/* For a device support MSI with per-vector mask, the length of MSI cap is at least 20 bytes */
			vdev->msix.caplen = MSIX_CAPLEN;
			vdev->msix.table_bar = i;
			vdev->msix.table_offset = 0U;
			vdev->msix.table_count = pdev->irte_count;

			/* capability ID */
			pci_vdev_write_vcfg(vdev, vdev->msix.capoff, 1U, 0x11U);
			/* message control, MSI-X Diabled, Function unamsked */
			pci_vdev_write_vcfg(vdev, vdev->msix.capoff + 2U, 2U, pdev->irte_count - 1U);
			/* Init MSIX table vBAR, offset is 0 */
			pci_vdev_write_vcfg(vdev, vdev->msix.capoff + 4U, 4U, i);
			/* Init PBA table vBAR, offset is 2048 */
			pci_vdev_write_vcfg(vdev, vdev->msix.capoff + 8U, 4U, 2048U + i);

			vdev->vbars[i].size = 4096U;
			vdev->vbars[i].base_hpa = 0x0UL;
			vdev->vbars[i].mask = 0xFFFFF000U & PCI_BASE_ADDRESS_MEM_MASK;
			/* fixed for memory, 32bit, non-prefetchable */
			vdev->vbars[i].bar_type.bits = PCIM_BAR_MEM_32;

			/* About MSI-x bar GPA:
			 * - For Service VM: when first time init, it is programmed as 0, then OS will program
			 *   the value later.
			 * - For Post-launched VM: The GPA is assigned by device model.
			 * - For Pre-launched VM: The GPA is assigned by acrn-config tool.
			 */
			if (is_prelaunched_vm(vpci2vm(vdev->vpci))) {
				vdev->vbars[i].base_gpa = vdev->pci_dev_config->vbar_base[i];
				pci_vdev_write_vbar(vdev, i, (uint32_t)vdev->vbars[i].base_gpa);
			}
		}
	}
}

void write_vmsix_cap_reg_on_msi(struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t val)
{
	uint16_t old_msgctrl, msgctrl;
	uint16_t msi_msgctrl;

	old_msgctrl = (uint16_t)pci_vdev_read_vcfg(vdev, vdev->msix.capoff + PCIR_MSIX_CTRL, 2U);
	/* Write to vdev */
	pci_vdev_write_vcfg(vdev, offset, bytes, val);
	msgctrl = (uint16_t)pci_vdev_read_vcfg(vdev, vdev->msix.capoff + PCIR_MSIX_CTRL, 2U);

	if (((old_msgctrl ^ msgctrl) & (PCIM_MSIXCTRL_MSIX_ENABLE | PCIM_MSIXCTRL_FUNCTION_MASK)) != 0U) {
		msi_msgctrl = (uint16_t)pci_pdev_read_cfg(vdev->pdev->bdf, offset, 2U);

		msi_msgctrl = msi_msgctrl & (~PCIM_MSICTRL_MME_MASK);
		msi_msgctrl &= ~ PCIM_MSICTRL_MSI_ENABLE;

		/* If MSI Enable is being set, make sure INTxDIS bit is set */
		if ((msgctrl & PCIM_MSIXCTRL_MSIX_ENABLE) != 0U) {
			enable_disable_pci_intx(vdev->pdev->bdf, false);
			msi_msgctrl |= ((msi_msgctrl & PCIM_MSICTRL_MMC_MASK) >>
				PCIM_MSICTRL_MMC_SHIFT) << PCIM_MSICTRL_MME_SHIFT;
			msi_msgctrl |= PCIM_MSICTRL_MSI_ENABLE;
		}
		pci_pdev_write_cfg(vdev->pdev->bdf, offset, 2U, msi_msgctrl);

		if ((msgctrl & PCIM_MSIXCTRL_FUNCTION_MASK) != 0U) {
			pci_pdev_write_cfg(vdev->pdev->bdf, get_mask_bits_offset(vdev), 4U, 0xFFFFFFFFU);
		}
	}
}

void remap_one_vmsix_entry_on_msi(struct pci_vdev *vdev, uint32_t index)
{
	const struct msix_table_entry *ventry;
	uint32_t mask_bits;
	uint32_t vector_mask = 1U << index;
	struct msi_info info = {};
	union pci_bdf pbdf = vdev->pdev->bdf;
	union irte_index ir_index;
	int32_t ret = 0;
	uint32_t capoff = vdev->msix.capoff;

	mask_bits = pci_pdev_read_cfg(pbdf, get_mask_bits_offset(vdev), 4U);
	mask_bits |= vector_mask;
	pci_pdev_write_cfg(pbdf, get_mask_bits_offset(vdev), 4U, mask_bits);

	ventry = &vdev->msix.table_entries[index];
	if ((ventry->vector_control & PCIM_MSIX_VCTRL_MASK) == 0U) {
		info.addr.full = vdev->msix.table_entries[index].addr;
		info.data.full = vdev->msix.table_entries[index].data;

		ret = ptirq_prepare_msi_remap(vpci2vm(vdev->vpci), vdev->bdf.value, pbdf.value,
			(uint16_t)index, &info, vdev->pdev->irte_start + (uint16_t)index);
		if (ret == 0) {
			if (!vdev->msix.is_vmsix_on_msi_programmed) {
				ir_index.index = vdev->pdev->irte_start;
				info.addr.ir_bits.shv = 1U;
				info.addr.ir_bits.intr_index_high = ir_index.bits.index_high;
				info.addr.ir_bits.intr_index_low = ir_index.bits.index_low;
				pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_ADDR, 0x4U, (uint32_t)info.addr.full);
				if (vdev->msi.is_64bit) {
					pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_ADDR_HIGH, 0x4U,
							(uint32_t)(info.addr.full >> 32U));
					pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_DATA_64BIT, 0x2U,
							(uint16_t)info.data.full);
				} else {
					pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_DATA, 0x2U,
							(uint16_t)info.data.full);
				}
				vdev->msix.is_vmsix_on_msi_programmed = true;
			}
			mask_bits &= ~vector_mask;
		}
	}
	pci_pdev_write_cfg(pbdf, get_mask_bits_offset(vdev), 4U, mask_bits);
}
