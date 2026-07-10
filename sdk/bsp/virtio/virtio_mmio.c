/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <guest_memory.h>
#include <bsp/io_req.h>
#include <rtl.h>
#include <virtio_mmio.h>

/*
 * 2026-07-10, virtio-mmio virtualization principle:
 *
 * This file is the common virtio-mmio transport layer. It owns the guest-visible
 * MMIO register block, queue address capture, used-ring IRQ status, and helpers
 * for reading/writing descriptor rings in guest memory. Device-specific files
 * own payload semantics through the ops callbacks.
 *
 *   guest virtio-mmio register access
 *              |
 *              v
 *   io_req.c MMIO dispatch
 *              |
 *              v
 *   virtio_mmio_mmio_handler()
 *      |
 *      +-- feature/status/config registers -> common state or ops->config
 *      |
 *      +-- queue setup registers           -> virtio_mmio_queue shadow
 *      |
 *      +-- QueueNotify                     -> ops->notify(queue)
 *                                             - walk guest descriptors
 *                                             - add used-ring entries
 *                                             - raise used IRQ
 *
 * Ownership rule: the guest owns vring memory, EL2 owns the transport shadow
 * and interrupt status, and each backend owns how descriptor payloads are
 * interpreted.
 */

#define VIRTIO_MMIO_MAGIC_VALUE		0x000U
#define VIRTIO_MMIO_VERSION		0x004U
#define VIRTIO_MMIO_DEVICE_ID		0x008U
#define VIRTIO_MMIO_VENDOR_ID		0x00CU
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010U
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014U
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020U
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024U
#define VIRTIO_MMIO_QUEUE_SEL		0x030U
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034U
#define VIRTIO_MMIO_QUEUE_NUM		0x038U
#define VIRTIO_MMIO_QUEUE_READY	0x044U
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050U
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060U
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064U
#define VIRTIO_MMIO_STATUS		0x070U
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080U
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084U
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090U
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094U
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0A0U
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0A4U
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0FCU

#define VIRTIO_MMIO_MAGIC		0x74726976U
#define VIRTIO_MMIO_VERSION_2		2U

static uint64_t virtio_mmio_resize_value(uint64_t value, uint64_t size)
{
	uint64_t ret;

	if (size == 1UL) {
		ret = value & 0xffU;
	} else if (size == 2UL) {
		ret = value & 0xffffU;
	} else {
		ret = value & 0xffffffffUL;
	}

	return ret;
}

static uint16_t virtio_mmio_read_u16(struct virtio_mmio_dev *dev, uint64_t gpa)
{
	uint16_t value = 0U;

	(void)virtio_mmio_read_gpa(dev, gpa, &value, sizeof(value));
	return value;
}

static void virtio_mmio_write_u16(struct virtio_mmio_dev *dev, uint64_t gpa,
	uint16_t value)
{
	(void)virtio_mmio_write_gpa(dev, gpa, &value, sizeof(value));
}

static void virtio_mmio_update_irq(struct virtio_mmio_dev *dev)
{
	if ((dev->interrupt_status != 0U) && !dev->irq_asserted) {
		arch_trigger_level_intr(dev->vm, dev->irq, true);
		dev->irq_asserted = true;
	} else if ((dev->interrupt_status == 0U) && dev->irq_asserted) {
		arch_trigger_level_intr(dev->vm, dev->irq, false);
		dev->irq_asserted = false;
	}
}

static void virtio_mmio_reset_state(struct virtio_mmio_dev *dev)
{
	bool asserted = dev->irq_asserted;

	(void)memset(dev->queues, 0U, sizeof(dev->queues));
	dev->driver_features = 0UL;
	dev->device_features_sel = 0U;
	dev->driver_features_sel = 0U;
	dev->queue_sel = 0U;
	dev->interrupt_status = 0U;
	dev->status = 0U;
	dev->irq_asserted = false;
	if (asserted) {
		arch_trigger_level_intr(dev->vm, dev->irq, false);
	}
	if ((dev->ops != NULL) && (dev->ops->reset != NULL)) {
		dev->ops->reset(dev);
	}
}

void virtio_mmio_reset_dev(struct virtio_mmio_dev *dev)
{
	if (dev != NULL) {
		virtio_mmio_reset_state(dev);
	}
}

void virtio_mmio_init(struct virtio_mmio_dev *dev,
	const struct virtio_mmio_init *init)
{
	if ((dev == NULL) || (init == NULL) || (init->vm == NULL) ||
		(init->queue_num > VIRTIO_MMIO_MAX_QUEUES)) {
		return;
	}

	(void)memset(dev, 0U, sizeof(*dev));
	dev->name = init->name;
	dev->vm = init->vm;
	dev->base = init->base;
	dev->size = init->size;
	dev->irq = init->irq;
	dev->device_id = init->device_id;
	dev->queue_num = init->queue_num;
	dev->queue_size = init->queue_size;
	dev->device_features = init->device_features | (1ULL << VIRTIO_F_VERSION_1);
	dev->ops = init->ops;
	dev->priv = init->priv;
}

void *virtio_mmio_priv(struct virtio_mmio_dev *dev)
{
	return dev != NULL ? dev->priv : NULL;
}

struct acrn_vm *virtio_mmio_vm(struct virtio_mmio_dev *dev)
{
	return dev != NULL ? dev->vm : NULL;
}

bool virtio_mmio_read_gpa(struct virtio_mmio_dev *dev, uint64_t gpa,
	void *buf, uint32_t size)
{
	return (dev != NULL) && (copy_from_gpa(dev->vm, buf, gpa, size) == 0);
}

bool virtio_mmio_write_gpa(struct virtio_mmio_dev *dev, uint64_t gpa,
	void *buf, uint32_t size)
{
	return (dev != NULL) && (copy_to_gpa(dev->vm, buf, gpa, size) == 0);
}

struct virtio_mmio_queue *virtio_mmio_get_queue(struct virtio_mmio_dev *dev,
	uint16_t queue_id)
{
	return ((dev != NULL) && (queue_id < dev->queue_num)) ?
		&dev->queues[queue_id] : NULL;
}

bool virtio_mmio_queue_valid(const struct virtio_mmio_dev *dev,
	const struct virtio_mmio_queue *vq)
{
	return (dev != NULL) && (vq != NULL) && vq->ready &&
		(vq->num != 0U) && (vq->num <= dev->queue_size) &&
		(vq->desc != 0UL) && (vq->avail != 0UL) && (vq->used != 0UL);
}

bool virtio_mmio_read_desc(struct virtio_mmio_dev *dev,
	const struct virtio_mmio_queue *vq, uint16_t id,
	struct virtio_ring_desc *desc)
{
	bool ret = false;

	if (virtio_mmio_queue_valid(dev, vq) && (id < vq->num)) {
		ret = virtio_mmio_read_gpa(dev,
			vq->desc + ((uint64_t)id * sizeof(*desc)),
			desc, sizeof(*desc));
	}

	return ret;
}

bool virtio_mmio_pop_avail(struct virtio_mmio_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t *head)
{
	uint16_t avail_idx;
	uint64_t ring_gpa;
	bool ret = false;

	if ((head != NULL) && virtio_mmio_queue_valid(dev, vq)) {
		avail_idx = virtio_mmio_read_u16(dev, vq->avail + 2UL);
		if (vq->last_avail_idx != avail_idx) {
			ring_gpa = vq->avail + 4UL +
				((uint64_t)(vq->last_avail_idx % vq->num) * sizeof(uint16_t));
			*head = virtio_mmio_read_u16(dev, ring_gpa);
			vq->last_avail_idx++;
			ret = true;
		}
	}

	return ret;
}

bool virtio_mmio_add_used(struct virtio_mmio_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t id, uint32_t len)
{
	uint16_t used_idx;
	uint64_t elem_gpa;
	uint32_t elem[2];
	bool ret = false;

	if (virtio_mmio_queue_valid(dev, vq)) {
		used_idx = virtio_mmio_read_u16(dev, vq->used + 2UL);
		elem_gpa = vq->used + 4UL +
			((uint64_t)(used_idx % vq->num) * sizeof(elem));
		elem[0] = id;
		elem[1] = len;
		if (virtio_mmio_write_gpa(dev, elem_gpa, elem, sizeof(elem))) {
			virtio_mmio_write_u16(dev, vq->used + 2UL, used_idx + 1U);
			ret = true;
		}
	}

	return ret;
}

void virtio_mmio_raise_used_irq(struct virtio_mmio_dev *dev)
{
	if (dev != NULL) {
		dev->interrupt_status |= VIRTIO_MMIO_INT_USED_RING;
		virtio_mmio_update_irq(dev);
	}
}

static struct virtio_mmio_queue *virtio_mmio_selected_queue(
	struct virtio_mmio_dev *dev)
{
	return (dev != NULL) && (dev->queue_sel < dev->queue_num) ?
		&dev->queues[dev->queue_sel] : NULL;
}

static uint32_t virtio_mmio_read_reg(struct virtio_mmio_dev *dev,
	uint32_t offset, uint32_t size)
{
	const struct virtio_mmio_queue *vq = virtio_mmio_selected_queue(dev);
	uint32_t value = 0U;

	if (offset >= VIRTIO_MMIO_CONFIG_OFFSET) {
		if ((dev->ops != NULL) && (dev->ops->read_config != NULL)) {
			value = dev->ops->read_config(dev,
				offset - VIRTIO_MMIO_CONFIG_OFFSET, size);
		}
		return value;
	}

	switch (offset) {
	case VIRTIO_MMIO_MAGIC_VALUE:
		value = VIRTIO_MMIO_MAGIC;
		break;
	case VIRTIO_MMIO_VERSION:
		value = VIRTIO_MMIO_VERSION_2;
		break;
	case VIRTIO_MMIO_DEVICE_ID:
		value = dev->device_id;
		break;
	case VIRTIO_MMIO_VENDOR_ID:
		value = VIRTIO_VENDOR_ID_BEAU;
		break;
	case VIRTIO_MMIO_DEVICE_FEATURES:
		value = (dev->device_features_sel == 0U) ?
			(uint32_t)dev->device_features :
			(uint32_t)(dev->device_features >> 32U);
		break;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		value = dev->queue_sel < dev->queue_num ? dev->queue_size : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		value = vq != NULL ? vq->num : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		value = (vq != NULL) && vq->ready ? 1U : 0U;
		break;
	case VIRTIO_MMIO_INTERRUPT_STATUS:
		value = dev->interrupt_status;
		break;
	case VIRTIO_MMIO_STATUS:
		value = dev->status;
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
		value = vq != NULL ? (uint32_t)vq->desc : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		value = vq != NULL ? (uint32_t)(vq->desc >> 32U) : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
		value = vq != NULL ? (uint32_t)vq->avail : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
		value = vq != NULL ? (uint32_t)(vq->avail >> 32U) : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_USED_LOW:
		value = vq != NULL ? (uint32_t)vq->used : 0U;
		break;
	case VIRTIO_MMIO_QUEUE_USED_HIGH:
		value = vq != NULL ? (uint32_t)(vq->used >> 32U) : 0U;
		break;
	case VIRTIO_MMIO_CONFIG_GENERATION:
		value = 0U;
		break;
	default:
		break;
	}

	return (uint32_t)virtio_mmio_resize_value(value, size);
}

static void virtio_mmio_write_queue_addr(struct virtio_mmio_queue *vq,
	uint32_t offset, uint32_t value)
{
	switch (offset) {
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
		vq->desc = (vq->desc & 0xffffffff00000000UL) | value;
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		vq->desc = (vq->desc & 0xffffffffUL) | ((uint64_t)value << 32U);
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
		vq->avail = (vq->avail & 0xffffffff00000000UL) | value;
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
		vq->avail = (vq->avail & 0xffffffffUL) | ((uint64_t)value << 32U);
		break;
	case VIRTIO_MMIO_QUEUE_USED_LOW:
		vq->used = (vq->used & 0xffffffff00000000UL) | value;
		break;
	case VIRTIO_MMIO_QUEUE_USED_HIGH:
		vq->used = (vq->used & 0xffffffffUL) | ((uint64_t)value << 32U);
		break;
	default:
		break;
	}
}

static void virtio_mmio_write_status(struct virtio_mmio_dev *dev, uint32_t value)
{
	uint64_t unsupported;

	if (value == 0U) {
		virtio_mmio_reset_state(dev);
		return;
	}

	dev->status = value;
	unsupported = dev->driver_features & ~dev->device_features;
	if (((value & VIRTIO_STATUS_FEATURES_OK) != 0U) && (unsupported != 0UL)) {
		dev->status &= ~VIRTIO_STATUS_FEATURES_OK;
	}
}

static void virtio_mmio_write_reg(struct virtio_mmio_dev *dev,
	uint32_t offset, uint32_t size, uint32_t value)
{
	struct virtio_mmio_queue *vq = virtio_mmio_selected_queue(dev);

	if (offset >= VIRTIO_MMIO_CONFIG_OFFSET) {
		if ((dev->ops != NULL) && (dev->ops->write_config != NULL)) {
			dev->ops->write_config(dev,
				offset - VIRTIO_MMIO_CONFIG_OFFSET, size, value);
		}
		return;
	}

	switch (offset) {
	case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
		dev->device_features_sel = value;
		break;
	case VIRTIO_MMIO_DRIVER_FEATURES:
		if (dev->driver_features_sel == 0U) {
			dev->driver_features =
				(dev->driver_features & 0xffffffff00000000UL) | value;
		} else {
			dev->driver_features =
				(dev->driver_features & 0xffffffffUL) |
				((uint64_t)value << 32U);
		}
		break;
	case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
		dev->driver_features_sel = value;
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		dev->queue_sel = value;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		if ((vq != NULL) && (value <= dev->queue_size)) {
			vq->num = (uint16_t)value;
		}
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		if (vq != NULL) {
			vq->ready = (value != 0U);
			if (!vq->ready) {
				vq->last_avail_idx = 0U;
			} else if ((dev->ops != NULL) && (dev->ops->queue_ready != NULL)) {
				dev->ops->queue_ready(dev, (uint16_t)dev->queue_sel);
			}
		}
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		if ((dev->ops != NULL) && (dev->ops->notify_queue != NULL)) {
			dev->ops->notify_queue(dev, (uint16_t)value);
		}
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		dev->interrupt_status &= ~value;
		virtio_mmio_update_irq(dev);
		break;
	case VIRTIO_MMIO_STATUS:
		virtio_mmio_write_status(dev, value);
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
	case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
	case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
	case VIRTIO_MMIO_QUEUE_USED_LOW:
	case VIRTIO_MMIO_QUEUE_USED_HIGH:
		if (vq != NULL) {
			virtio_mmio_write_queue_addr(vq, offset, value);
		}
		break;
	default:
		break;
	}
}

int32_t virtio_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct virtio_mmio_dev *dev = (struct virtio_mmio_dev *)handler_private_data;
	struct acrn_mmio_request *mmio;
	uint32_t offset;
	uint32_t value;
	int32_t ret = -EINVAL;

	if ((io_req == NULL) || (dev == NULL)) {
		return ret;
	}

	mmio = &io_req->reqs.mmio_request;
	if ((mmio->size == 1UL) || (mmio->size == 2UL) || (mmio->size == 4UL)) {
		offset = (uint32_t)(mmio->address - dev->base);
		if (mmio->direction == ACRN_IOREQ_DIR_READ) {
			mmio->value = virtio_mmio_read_reg(dev, offset,
				(uint32_t)mmio->size);
		} else {
			value = (uint32_t)virtio_mmio_resize_value(mmio->value,
				mmio->size);
			virtio_mmio_write_reg(dev, offset, (uint32_t)mmio->size,
				value);
		}
		ret = 0;
	}

	return ret;
}
