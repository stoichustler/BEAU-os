/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bits.h>
#include <cpu.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <guest_memory.h>
#include <rtl.h>
#include <asm/page.h>
#include <asm/platform.h>
#include <asm/guest/vgicv3.h>

#include "vgicv3_its.h"

/* [20260630] ITS virtualization principle:
 *
 * vITS model:
 *
 * The ITS is a message-to-LPI translation unit. It owns GITS_* MMIO state, the
 * command queue reader, and a compact Device/Event/Collection table. It does
 * not own the GIC CPU interface or list registers.
 *
 *   guest command queue / TRANSLATER write
 *        -> vITS table lookup or update
 *        -> LPI INTID + target vCPU
 *        -> vGIC core injection helper
 *        -> descriptor/LR lifecycle stays in vgicv3.c
 *
 * Keeping ITS and CPU-interface responsibilities separate avoids duplicating
 * pending/active accounting. ITS code can translate MSI messages while vGIC
 * core remains the single place that decides when an interrupt becomes visible
 * through ICH_LR<n> and when guest EOI/deactivation clears it.
 */
#define BIT32(n)			(1U << (n))

#define GITS_CTLR			0x0000U
#define GITS_IIDR			0x0004U
#define GITS_TYPER			0x0008U
#define GITS_MPIDR			0x0018U
#define GITS_CBASER			0x0080U
#define GITS_CWRITER			0x0088U
#define GITS_CREADR			0x0090U
#define GITS_BASER			0x0100U
#define GITS_TRANSLATER		0x10040U
#define GITS_PIDR2			0xFFE8U
#define GITS_CIDR0			0xFFF0U
#define GITS_CIDR1			0xFFF4U
#define GITS_CIDR2			0xFFF8U
#define GITS_CIDR3			0xFFFCU

#define GITS_CTLR_ENABLE		BIT32(0U)
#define GITS_CTLR_QUIESCENT		BIT32(31U)
#define GITS_CBASER_VALID		(1UL << 63U)
#define GITS_CBASER_ADDRESS_MASK	0x0000fffffffff000UL
#define GITS_CBASER_SIZE_MASK		0xffUL
#define GITS_BASER_VALID		(1UL << 63U)
#define VGICR_CTLR_ENABLE_LPIS	BIT32(0U)
#define GITS_CMD_ITT_ADDRESS_MASK	0x000fffffffffff00UL
#define GITS_BASER_TYPE_SHIFT		56U
#define GITS_BASER_ENTRY_SIZE_SHIFT	48U
#define GITS_BASER_TYPE_DEVICE	1U
#define GITS_BASER_TYPE_COLLECTION	4U
#define GITS_TYPER_PLPIS		(1UL << 0U)
#define GITS_TYPER_ITT_ENTRY_SIZE_SHIFT	4U
#define GITS_TYPER_IDBITS_SHIFT	8U
#define GITS_TYPER_DEVBITS_SHIFT	13U
#define GITS_CMD_SIZE			32U
#define GITS_CMD_MAPD			0x08U
#define GITS_CMD_MAPC			0x09U
#define GITS_CMD_MAPTI			0x0aU
#define GITS_CMD_MAPI			0x0bU
#define GITS_CMD_MOVI			0x01U
#define GITS_CMD_INT			0x03U
#define GITS_CMD_CLEAR			0x04U
#define GITS_CMD_SYNC			0x05U
#define GITS_CMD_INV			0x0cU
#define GITS_CMD_INVALL		0x0dU
#define GITS_CMD_MOVALL		0x0eU
#define GITS_CMD_DISCARD		0x0fU
#define GITS_DEVBITS			15U
#define GITS_ITT_ENTRY_SIZE		16U
#define GITS_CMD_QUEUE_BUDGET		1024U
#define GICR_PROPBASER_ADDRESS_MASK	0x000FFFFFFFFFF000UL
#define LPI_PROP_PRIO_MASK		0xfcU
#define LPI_PROP_ENABLED		BIT32(0U)

#define VGIC_ITS_PRIORITY_DEFAULT	0x80U
#define VGIC_ITS_INVALID_VCPU_ID	0xffffU
#define VGIC_ITS_PIDR2_ARCH_GICV3	(3U << 4U)

static uint64_t vgic_its_baser_type(uint32_t type, uint32_t entry_size)
{
	return ((uint64_t)type << GITS_BASER_TYPE_SHIFT) |
		((uint64_t)(entry_size - 1U) << GITS_BASER_ENTRY_SIZE_SHIFT);
}

static bool vits_offset_aligned(uint64_t offset)
{
	return (offset & (GITS_CMD_SIZE - 1UL)) == 0UL;
}

static bool vits_word_access(uint32_t offset, uint64_t size)
{
	return (size == 4UL) && ((offset & 0x3U) == 0U);
}

static bool vits_irq_is_lpi(uint32_t virq)
{
	return (virq >= ARM64_VGIC_LPI_BASE) &&
		(virq < (ARM64_VGIC_LPI_BASE + ARM64_VGIC_LPI_NUM));
}

static uint32_t vits_lpi_index(uint32_t lpi)
{
	return lpi - ARM64_VGIC_LPI_BASE;
}

static uint16_t vits_valid_target_vcpu(const struct arm64_vgicv3 *vgic,
	uint16_t target_vcpu_id)
{
	return (target_vcpu_id < vgic->vcpu_count) ? target_vcpu_id : 0U;
}

static void vits_record_last(struct arm64_vits *its, uint32_t opcode,
	uint32_t device_id, uint32_t event_id, uint32_t lpi,
	uint16_t collection_id, uint16_t target_vcpu, int32_t status)
{
	its->stats.last_opcode = opcode;
	its->stats.last_device = device_id;
	its->stats.last_event = event_id;
	its->stats.last_lpi = lpi;
	its->stats.last_collection = collection_id;
	its->stats.last_target = target_vcpu;
	its->stats.last_status = status;
}

void arm64_vgicv3_its_init(struct arm64_vgicv3 *vgic)
{
	struct arm64_vits *its = &vgic->its;
	uint32_t idx;

	(void)memset(its, 0U, sizeof(*its));
	/*
	 * Advertise a conservative ITS: physical LPIs only, 14-bit INTID space
	 * starting at 8192, 16-bit device IDs, and 16-byte ITT entries. vGICv4
	 * VLPIs are deliberately not exposed in this first model. The software
	 * vITS stores only a small active event window; it does not preallocate
	 * descriptors for the whole advertised LPI space.
	 */
	its->ctlr = GITS_CTLR_QUIESCENT;
	its->typer = GITS_TYPER_PLPIS |
		((uint64_t)(GITS_ITT_ENTRY_SIZE - 1U) << GITS_TYPER_ITT_ENTRY_SIZE_SHIFT) |
		((uint64_t)(ARM64_VGIC_LPI_IDBITS - 1U) << GITS_TYPER_IDBITS_SHIFT) |
		((uint64_t)(GITS_DEVBITS - 1U) << GITS_TYPER_DEVBITS_SHIFT);
	its->baser[0U] = vgic_its_baser_type(GITS_BASER_TYPE_DEVICE, 8U);
	its->baser[1U] = vgic_its_baser_type(GITS_BASER_TYPE_COLLECTION, 8U);

	for (idx = 0U; idx < ARM64_VGIC_ITS_COLLECTION_NUM; idx++) {
		its->collection[idx].collection_id = (uint16_t)idx;
		its->collection[idx].target_vcpu = (uint16_t)idx;
		its->collection[idx].valid = (idx < vgic->vcpu_count);
	}
}

static struct arm64_vits_collection *vits_find_collection(struct arm64_vits *its,
	uint16_t collection_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VGIC_ITS_COLLECTION_NUM; idx++) {
		if (its->collection[idx].collection_id == collection_id) {
			return &its->collection[idx];
		}
	}

	return NULL;
}

static struct arm64_vits_device *vits_find_device(struct arm64_vits *its,
	uint32_t device_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VGIC_ITS_DEVICE_NUM; idx++) {
		if (its->device[idx].valid && (its->device[idx].device_id == device_id)) {
			return &its->device[idx];
		}
	}

	return NULL;
}

static struct arm64_vits_device *vits_alloc_device(struct arm64_vits *its,
	uint32_t device_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VGIC_ITS_DEVICE_NUM; idx++) {
		if (!its->device[idx].valid) {
			its->device[idx].device_id = device_id;
			its->device[idx].valid = true;
			return &its->device[idx];
		}
	}

	return NULL;
}

static uint32_t vits_device_index(const struct arm64_vits *its,
	const struct arm64_vits_device *device)
{
	uint32_t idx = ARM64_VGIC_ITS_DEVICE_NUM;

	if (device != NULL) {
		idx = (uint32_t)(device - its->device);
		if (idx >= ARM64_VGIC_ITS_DEVICE_NUM) {
			idx = ARM64_VGIC_ITS_DEVICE_NUM;
		}
	}

	return idx;
}

static struct arm64_vits_event *vits_find_event(struct arm64_vits *its,
	uint32_t device_id, uint32_t event_id)
{
	uint32_t dev_idx;
	uint32_t evt_idx;

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid || (its->device[dev_idx].device_id != device_id)) {
			continue;
		}
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			if (its->event[dev_idx][evt_idx].valid &&
				(its->event[dev_idx][evt_idx].event_id == event_id)) {
				return &its->event[dev_idx][evt_idx];
			}
		}
	}

	return NULL;
}

static struct arm64_vits_event *vits_alloc_event(struct arm64_vits *its,
	uint32_t device_id, uint32_t event_id)
{
	uint32_t dev_idx;
	uint32_t evt_idx;

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid || (its->device[dev_idx].device_id != device_id)) {
			continue;
		}
		if (event_id >= its->device[dev_idx].event_count) {
			break;
		}
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			if (!its->event[dev_idx][evt_idx].valid) {
				its->event[dev_idx][evt_idx].device_id = device_id;
				its->event[dev_idx][evt_idx].event_id = event_id;
				its->event[dev_idx][evt_idx].valid = true;
				return &its->event[dev_idx][evt_idx];
			}
		}
	}

	return NULL;
}

static uint16_t vits_target_vcpu_id(struct arm64_vgicv3 *vgic, uint16_t collection_id)
{
	struct arm64_vits_collection *collection = vits_find_collection(&vgic->its,
		collection_id);
	uint16_t target = VGIC_ITS_INVALID_VCPU_ID;

	if ((collection != NULL) && collection->valid) {
		target = vits_valid_target_vcpu(vgic, collection->target_vcpu);
	}

	return target;
}

static bool vits_update_lpi_config_locked(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, uint16_t target_vcpu_id, uint32_t lpi)
{
	struct arm64_vgic_irq *desc;
	uint64_t propbase;
	uint8_t property;
	bool updated = false;

	if ((vm == NULL) || (vgic == NULL) ||
		(target_vcpu_id >= vgic->vcpu_count) || !vits_irq_is_lpi(lpi) ||
		((vgic->gicr_ctlr[target_vcpu_id] & VGICR_CTLR_ENABLE_LPIS) == 0U)) {
		return false;
	}

	propbase = vgic->gicr_propbaser[target_vcpu_id] & GICR_PROPBASER_ADDRESS_MASK;
	if (propbase == 0UL) {
		return false;
	}
	if (copy_from_gpa(vm, &property, propbase + vits_lpi_index(lpi),
		sizeof(property)) != 0) {
		return false;
	}

	desc = &vgic->lpi[target_vcpu_id][vits_lpi_index(lpi)];
	desc->target_vcpu = (uint8_t)target_vcpu_id;
	desc->priority = property & LPI_PROP_PRIO_MASK;
	desc->enabled = ((property & LPI_PROP_ENABLED) != 0U);
	arm64_vgicv3_update_irq_row_lr_locked(vm, target_vcpu_id, desc);
	updated = true;
	vgic->its.stats.config_update_ok++;

	return updated;
}

static bool vits_update_event_lpi_config_locked(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, const struct arm64_vits_event *event)
{
	uint16_t target_vcpu_id;

	if ((event == NULL) || !event->valid || !vits_irq_is_lpi(event->lpi)) {
		return false;
	}

	target_vcpu_id = vits_target_vcpu_id(vgic, event->collection_id);
	return vits_update_lpi_config_locked(vm, vgic, target_vcpu_id, event->lpi);
}

static void vits_record_config_update(struct arm64_vgicv3 *vgic, bool updated)
{
	if (!updated) {
		vgic->its.stats.config_update_fail++;
	}
}

static void vits_clear_event_state_locked(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, const struct arm64_vits_event *event)
{
	uint16_t target_vcpu_id;

	if ((event == NULL) || !event->valid || !vits_irq_is_lpi(event->lpi)) {
		return;
	}

	target_vcpu_id = vits_target_vcpu_id(vgic, event->collection_id);
	if (target_vcpu_id < vgic->vcpu_count) {
		struct arm64_vgic_irq *desc =
			&vgic->lpi[target_vcpu_id][vits_lpi_index(event->lpi)];

		arm64_vgicv3_set_pending_locked(vgic, target_vcpu_id, desc, false);
		desc->active = false;
		arm64_vgicv3_update_irq_row_lr_locked(vm, target_vcpu_id, desc);
	}
}

static int32_t vits_inject_event_locked(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	struct arm64_vits_event *event)
{
	struct acrn_vcpu *target_vcpu;
	uint16_t target_vcpu_id;
	int32_t ret = -EINVAL;

	if ((event == NULL) || !event->valid || !vits_irq_is_lpi(event->lpi)) {
		return -EINVAL;
	}

	target_vcpu_id = vits_target_vcpu_id(vgic, event->collection_id);
	if (target_vcpu_id >= vgic->vcpu_count) {
		return -ENODEV;
	}

	vgic->lpi[target_vcpu_id][vits_lpi_index(event->lpi)].target_vcpu =
		(uint8_t)target_vcpu_id;
	target_vcpu = vcpu_from_vid(vm, target_vcpu_id);
	ret = arm64_vgicv3_inject_irq_locked(vgic, target_vcpu, event->lpi, false);
	if ((ret == 0) && (vcpu_get_state(target_vcpu) != VCPU_OFFLINE)) {
		vcpu_make_request(target_vcpu, ARM64_VCPU_REQUEST_EVENT);
		signal_event(&target_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
		if (is_vcpu_running(target_vcpu)) {
			kick_vcpu(target_vcpu);
		}
	}

	return ret;
}

static int32_t vits_inject_event_by_id_locked(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, uint32_t device_id, uint32_t event_id)
{
	struct arm64_vits *its = &vgic->its;
	struct arm64_vits_event *event;
	uint16_t target_vcpu_id = VGIC_ITS_INVALID_VCPU_ID;
	uint32_t lpi = 0U;
	int32_t ret;

	event = vits_find_event(its, device_id, event_id);
	if (event == NULL) {
		ret = -EINVAL;
		its->stats.inject_fail++;
		its->stats.inject_no_event++;
		vits_record_last(its, GITS_CMD_INT, device_id, event_id, lpi,
			0U, target_vcpu_id, ret);
		return ret;
	}

	lpi = event->lpi;
	target_vcpu_id = vits_target_vcpu_id(vgic, event->collection_id);
	ret = vits_inject_event_locked(vm, vgic, event);
	if (ret == 0) {
		its->stats.inject_ok++;
	} else {
		its->stats.inject_fail++;
		if (ret == -ENODEV) {
			its->stats.inject_bad_target++;
		}
	}
	vits_record_last(its, GITS_CMD_INT, device_id, event_id, lpi,
		event->collection_id, target_vcpu_id, ret);

	return ret;
}

int32_t arm64_vgicv3_its_inject_msi(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	uint32_t device_id, uint32_t event_id)
{
	return vits_inject_event_by_id_locked(vm, vgic, device_id, event_id);
}

bool arm64_vgicv3_its_inv_lpi_locked(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, uint16_t vcpu_id, uint32_t lpi)
{
	bool updated = vits_update_lpi_config_locked(vm, vgic, vcpu_id, lpi);

	vits_record_config_update(vgic, updated);

	return updated;
}

bool arm64_vgicv3_its_invall_vcpu_locked(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, uint16_t vcpu_id)
{
	struct arm64_vits *its = &vgic->its;
	uint32_t dev_idx;
	uint32_t evt_idx;
	bool updated = false;

	if ((vm == NULL) || (vgic == NULL) || (vcpu_id >= vgic->vcpu_count)) {
		return false;
	}

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			struct arm64_vits_event *event = &its->event[dev_idx][evt_idx];
			uint16_t target_vcpu_id;

			if (!event->valid) {
				continue;
			}
			target_vcpu_id = vits_target_vcpu_id(vgic, event->collection_id);
			if (target_vcpu_id == vcpu_id) {
				updated |= vits_update_event_lpi_config_locked(vm, vgic, event);
			}
		}
	}

	vits_record_config_update(vgic, updated);

	return updated;
}

static void vits_drop_device(struct arm64_vits *its, uint32_t device_id)
{
	uint32_t dev_idx;
	uint32_t evt_idx;

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid || (its->device[dev_idx].device_id != device_id)) {
			continue;
		}
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			its->event[dev_idx][evt_idx].valid = false;
		}
		(void)memset(&its->device[dev_idx], 0U, sizeof(its->device[dev_idx]));
		break;
	}
}

static bool vits_execute_cmd(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	const uint64_t cmd[4])
{
	struct arm64_vits *its = &vgic->its;
	uint32_t opcode = (uint32_t)(cmd[0] & 0xffUL);
	uint32_t device_id = (uint32_t)(cmd[0] >> 32U);
	uint32_t event_id = (uint32_t)(cmd[1] & UINT32_MAX);
	uint32_t lpi = (uint32_t)(cmd[1] >> 32U);
	uint16_t collection_id = (uint16_t)(cmd[2] & 0xffffUL);
	bool valid = ((cmd[2] & (1UL << 63U)) != 0UL);
	struct arm64_vits_device *device;
	struct arm64_vits_event *event;
	struct arm64_vits_collection *collection;
	uint32_t dev_idx;
	bool accepted = true;
	bool supported = true;

	switch (opcode) {
	case GITS_CMD_MAPD:
		if (!valid) {
			vits_drop_device(its, device_id);
			break;
		}
		device = vits_find_device(its, device_id);
		if (device == NULL) {
			device = vits_alloc_device(its, device_id);
		}
		if (device != NULL) {
			uint32_t size = (uint32_t)(cmd[1] & 0x1fUL) + 1U;
			uint64_t itt_addr = cmd[2] & GITS_CMD_ITT_ADDRESS_MASK;
			uint16_t event_count = (size < 16U) ? (uint16_t)(1U << size) :
				ARM64_VGIC_ITS_EVENT_NUM;

			if (event_count > ARM64_VGIC_ITS_EVENT_NUM) {
				event_count = ARM64_VGIC_ITS_EVENT_NUM;
			}
			if ((device->itt_addr != itt_addr) || (device->event_count != event_count)) {
				dev_idx = vits_device_index(its, device);
				if (dev_idx < ARM64_VGIC_ITS_DEVICE_NUM) {
					(void)memset(its->event[dev_idx], 0U,
						sizeof(its->event[dev_idx]));
				}
			}

			device->itt_addr = itt_addr;
			device->event_count = event_count;
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_MAPC:
		collection = vits_find_collection(its, collection_id);
		if (collection != NULL) {
			collection->valid = valid;
			collection->target_vcpu =
				vits_valid_target_vcpu(vgic, (uint16_t)((cmd[2] >> 16U) &
					0xffffU));
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_MAPI:
		lpi = ARM64_VGIC_LPI_BASE + event_id;
		/* fall through */
	case GITS_CMD_MAPTI:
		device = vits_find_device(its, device_id);
		collection = vits_find_collection(its, collection_id);
		if (!vits_irq_is_lpi(lpi) || (device == NULL) ||
			(event_id >= device->event_count) ||
			(collection == NULL) || !collection->valid) {
			accepted = false;
			break;
		}
		event = vits_find_event(its, device_id, event_id);
		if (event == NULL) {
			event = vits_alloc_event(its, device_id, event_id);
		}
		if (event != NULL) {
			event->lpi = lpi;
			event->collection_id = collection_id;
			vits_record_config_update(vgic,
				vits_update_event_lpi_config_locked(vm, vgic, event));
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_MOVI:
		event = vits_find_event(its, device_id, event_id);
		collection = vits_find_collection(its, collection_id);
		if ((event != NULL) && (collection != NULL) && collection->valid) {
			event->collection_id = collection_id;
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_DISCARD:
		event = vits_find_event(its, device_id, event_id);
		if (event != NULL) {
			vits_clear_event_state_locked(vm, vgic, event);
			event->valid = false;
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_INT:
		accepted = vits_inject_event_by_id_locked(vm, vgic,
			device_id, event_id) == 0;
		break;
	case GITS_CMD_CLEAR:
		event = vits_find_event(its, device_id, event_id);
		if ((event != NULL) && vits_irq_is_lpi(event->lpi)) {
			uint16_t target = vits_target_vcpu_id(vgic, event->collection_id);

			if (target < vgic->vcpu_count) {
				struct arm64_vgic_irq *desc =
					&vgic->lpi[target][vits_lpi_index(event->lpi)];

				arm64_vgicv3_set_pending_locked(vgic, target, desc, false);
				desc->active = false;
				arm64_vgicv3_update_irq_row_lr_locked(vm, target, desc);
			}
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_INV:
		event = vits_find_event(its, device_id, event_id);
		accepted = vits_update_event_lpi_config_locked(vm, vgic, event);
		vits_record_config_update(vgic, accepted);
		break;
	case GITS_CMD_INVALL:
		collection = vits_find_collection(its, collection_id);
		if ((collection != NULL) && collection->valid) {
			accepted = arm64_vgicv3_its_invall_vcpu_locked(vm, vgic,
				collection->target_vcpu);
		} else {
			accepted = false;
		}
		break;
	case GITS_CMD_SYNC:
	case GITS_CMD_MOVALL:
		break;
	default:
		supported = false;
		accepted = false;
		break;
	}

	its->stats.cmd_processed++;
	if (!supported) {
		its->stats.cmd_unsupported++;
	} else if (!accepted) {
		its->stats.cmd_invalid++;
	}
	if (opcode != GITS_CMD_INT) {
		vits_record_last(its, opcode, device_id, event_id, lpi,
			collection_id, VGIC_ITS_INVALID_VCPU_ID, accepted ? 0 : -EINVAL);
	}

	return accepted;
}

static uint64_t vits_cmd_queue_base(const struct arm64_vits *its)
{
	return its->cbaser & GITS_CBASER_ADDRESS_MASK;
}

static uint64_t vits_cmd_queue_size(const struct arm64_vits *its)
{
	return (((its->cbaser & GITS_CBASER_SIZE_MASK) + 1UL) * PAGE_SIZE);
}

static bool vits_process_command_queue(struct acrn_vm *vm, struct arm64_vgicv3 *vgic)
{
	struct arm64_vits *its = &vgic->its;
	uint64_t base = vits_cmd_queue_base(its);
	uint64_t size = vits_cmd_queue_size(its);
	uint64_t reader = its->creadr;
	uint64_t writer = its->cwriter;
	uint32_t processed = 0U;
	bool flush = false;

	if (((its->ctlr & GITS_CTLR_ENABLE) == 0U) ||
		((its->cbaser & GITS_CBASER_VALID) == 0UL) ||
		(size < GITS_CMD_SIZE)) {
		its->creadr = writer;
		if (reader != writer) {
			its->stats.cmd_queue_errors++;
		}
		return false;
	}

	/*
	 * The ITS command queue is guest owned, but EL2 processes it while holding
	 * the vGIC lock. Enforce command-size alignment and a per-entry budget so
	 * malformed queues cannot keep unrelated vGIC operations blocked forever.
	 */
	if ((reader >= size) || (writer >= size) ||
		!vits_offset_aligned(reader) || !vits_offset_aligned(writer)) {
		its->creadr = writer;
		its->stats.cmd_queue_errors++;
		return false;
	}

	/*
	 * Performance bottleneck: command consumption runs while the VM vGIC lock
	 * is held by the caller. Large MSI/LPI setup bursts therefore serialize
	 * interrupt injection and GIC MMIO emulation until this bounded batch ends.
	 */
	while ((reader != writer) && (processed < GITS_CMD_QUEUE_BUDGET)) {
		uint64_t cmd[4];

		if (copy_from_gpa(vm, cmd, base + reader, sizeof(cmd)) != 0) {
			its->stats.cmd_copy_fail++;
			break;
		}
		(void)vits_execute_cmd(vm, vgic, cmd);
		flush = true;
		reader += GITS_CMD_SIZE;
		processed++;
		if (reader >= size) {
			reader = 0UL;
		}
	}

	if ((reader != writer) && (processed >= GITS_CMD_QUEUE_BUDGET)) {
		its->stats.cmd_budget_exhausted++;
	}
	its->creadr = reader;
	return flush;
}

static void vits_write_translater(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	uint32_t event_id)
{
	/*
	 * A real MSI write carries the requester/device identity in the bus
	 * transaction. The current QEMU SDK path has no PCI requester plumbing yet,
	 * so direct writes to the vITS doorbell use DeviceID 0 as a deterministic
	 * local test path. Passthrough/MSI code should call arm64_vgicv3_inject_msi()
	 * with the real DeviceID once that layer exists.
	 */
	vgic->its.stats.translater_write++;
	(void)vits_inject_event_by_id_locked(vm, vgic, 0U, event_id);
}

static uint32_t vits_baser_index(uint32_t offset)
{
	return (offset - GITS_BASER) / 8U;
}

static bool vits_mmio_read(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct arm64_vits *its = &vgic->its;
	uint64_t value64 = 0UL;
	bool flush = false;

	its->stats.mmio_read++;
	if ((offset >= GITS_BASER) &&
		(offset < (GITS_BASER + (ARM64_VGIC_ITS_BASER_NUM * 8U)))) {
		uint32_t idx = vits_baser_index(offset);

		value64 = its->baser[idx];
		mmio->value = ((offset & 0x4U) == 0U) ?
			(value64 & UINT32_MAX) : (value64 >> 32U);
		return false;
	}

	switch (offset) {
	case GITS_CTLR:
		mmio->value = its->ctlr;
		break;
	case GITS_IIDR:
		mmio->value = beau_config.gic_iidr;
		break;
	case GITS_TYPER:
		mmio->value = its->typer & UINT32_MAX;
		break;
	case GITS_TYPER + 4U:
		mmio->value = its->typer >> 32U;
		break;
	case GITS_MPIDR:
		mmio->value = vcpu_get_vmpidr(vcpu_from_vid(vcpu->vm, BSP_CPU_ID));
		break;
	case GITS_CBASER:
		mmio->value = its->cbaser & UINT32_MAX;
		break;
	case GITS_CBASER + 4U:
		mmio->value = its->cbaser >> 32U;
		break;
	case GITS_CWRITER:
		mmio->value = its->cwriter & UINT32_MAX;
		break;
	case GITS_CWRITER + 4U:
		mmio->value = its->cwriter >> 32U;
		break;
	case GITS_CREADR:
		flush = vits_process_command_queue(vcpu->vm, vgic);
		mmio->value = its->creadr & UINT32_MAX;
		break;
	case GITS_CREADR + 4U:
		flush = vits_process_command_queue(vcpu->vm, vgic);
		mmio->value = its->creadr >> 32U;
		break;
	case GITS_PIDR2:
		mmio->value = VGIC_ITS_PIDR2_ARCH_GICV3;
		break;
	case GITS_CIDR0:
		mmio->value = 0x0dU;
		break;
	case GITS_CIDR1:
		mmio->value = 0xf0U;
		break;
	case GITS_CIDR2:
		mmio->value = 0x05U;
		break;
	case GITS_CIDR3:
		mmio->value = 0xb1U;
		break;
	default:
		mmio->value = 0UL;
		break;
	}

	return flush;
}

static bool vits_mmio_write(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	const struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct arm64_vits *its = &vgic->its;
	uint64_t value = mmio->value & UINT32_MAX;
	bool flush = false;

	its->stats.mmio_write++;
	if ((offset >= GITS_BASER) &&
		(offset < (GITS_BASER + (ARM64_VGIC_ITS_BASER_NUM * 8U)))) {
		uint32_t idx = vits_baser_index(offset);

		if ((offset & 0x4U) == 0U) {
			its->baser[idx] = (its->baser[idx] & 0xffffffff00000000UL) | value;
		} else {
			its->baser[idx] = (its->baser[idx] & UINT32_MAX) | (value << 32U);
		}
		return false;
	}

	switch (offset) {
	case GITS_CTLR:
		its->ctlr = ((uint32_t)value & GITS_CTLR_ENABLE) |
			(((value & GITS_CTLR_ENABLE) == 0U) ? GITS_CTLR_QUIESCENT : 0U);
		if ((its->ctlr & GITS_CTLR_ENABLE) != 0U) {
			flush = vits_process_command_queue(vcpu->vm, vgic);
		}
		break;
	case GITS_CBASER:
		its->cbaser = (its->cbaser & 0xffffffff00000000UL) | value;
		its->creadr = 0UL;
		its->cwriter = 0UL;
		break;
	case GITS_CBASER + 4U:
		its->cbaser = (its->cbaser & UINT32_MAX) | (value << 32U);
		its->creadr = 0UL;
		its->cwriter = 0UL;
		break;
	case GITS_CWRITER:
		its->cwriter = (its->cwriter & 0xffffffff00000000UL) | value;
		flush = vits_process_command_queue(vcpu->vm, vgic);
		break;
	case GITS_CWRITER + 4U:
		its->cwriter = (its->cwriter & UINT32_MAX) | (value << 32U);
		flush = vits_process_command_queue(vcpu->vm, vgic);
		break;
	case GITS_TRANSLATER:
		vits_write_translater(vcpu->vm, vgic, (uint32_t)value);
		flush = true;
		break;
	default:
		break;
	}

	return flush;
}

int32_t arm64_vgicv3_its_mmio_access(struct acrn_vcpu *vcpu,
	struct arm64_vgicv3 *vgic, struct acrn_mmio_request *mmio)
{
	uint32_t offset = (uint32_t)(mmio->address -
		get_vm_config(vcpu->vm->vm_id)->arch.guest_its_base);
	int32_t ret = 0;

	if (!vits_word_access(offset, mmio->size)) {
		ret = -EINVAL;
	} else {
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			if (vits_mmio_read(vcpu, vgic, mmio, offset)) {
				arm64_vgicv3_flush_vm_locked(vcpu);
			}
			break;
		case ACRN_IOREQ_DIR_WRITE:
			if (vits_mmio_write(vcpu, vgic, mmio, offset)) {
				arm64_vgicv3_flush_vm_locked(vcpu);
			}
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

bool arm64_vgicv3_its_mmio_access64(struct acrn_vcpu *vcpu,
	struct arm64_vgicv3 *vgic, struct acrn_mmio_request *mmio, int32_t *status)
{
	struct arm64_vits *its = &vgic->its;
	uint32_t offset = (uint32_t)(mmio->address -
		get_vm_config(vcpu->vm->vm_id)->arch.guest_its_base);
	bool handled = false;

	if (status != NULL) {
		*status = -ENODEV;
	}
	if ((offset == GITS_CWRITER) && (mmio->size == 8UL) &&
		(mmio->direction == ACRN_IOREQ_DIR_WRITE)) {
		handled = true;
		its->stats.mmio_write++;
		/*
		 * CWRITER is a 64-bit producer pointer. Update the full value before
		 * consuming commands so a split low/high emulation does not run the
		 * queue against a half-written writer offset.
		 */
		its->cwriter = mmio->value;
		if (vits_process_command_queue(vcpu->vm, vgic)) {
			arm64_vgicv3_flush_vm_locked(vcpu);
		}
		if (status != NULL) {
			*status = 0;
		}
	}

	return handled;
}
