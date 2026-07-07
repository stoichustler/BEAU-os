/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vm_config.h>
#include <guest_memory.h>
#include <io_req.h>
#include <console.h>
#include <vuart.h>
#include <logmsg.h>
#include <rtl.h>
#include <virtio_console.h>

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
#define VIRTIO_MMIO_QUEUE_READY		0x044U
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
#define VIRTIO_MMIO_CONFIG		0x100U

#define VIRTIO_MMIO_MAGIC		0x74726976U
#define VIRTIO_MMIO_VERSION_2		2U
#define VIRTIO_CONSOLE_DEVICE_ID	3U
#define VIRTIO_VENDOR_ID_BEAU		0x42454155U

#define VIRTIO_CONSOLE_QUEUE_RX		0U
#define VIRTIO_CONSOLE_QUEUE_TX		1U
#define VIRTIO_CONSOLE_QUEUE_NUM	2U
#define VIRTIO_CONSOLE_QUEUE_SIZE	64U

#define VIRTIO_RING_F_NEXT		1U
#define VIRTIO_RING_F_WRITE		2U
#define VIRTIO_MMIO_INT_USED_RING	1U
#define VIRTIO_STATUS_FEATURES_OK	8U
#define VIRTIO_F_VERSION_1		32U

#define VIRTIO_CONSOLE_COPY_BUF_SIZE	64U
#define VIRTIO_CONSOLE_CHAIN_LIMIT	VIRTIO_CONSOLE_QUEUE_SIZE

struct virtio_mmio_queue {
	uint16_t num;
	uint16_t last_avail_idx;
	uint64_t desc;
	uint64_t avail;
	uint64_t used;
	bool ready;
};

struct virtio_console_dev {
	struct virtio_mmio_queue queues[VIRTIO_CONSOLE_QUEUE_NUM];
	uint64_t driver_features;
	uint32_t device_features_sel;
	uint32_t driver_features_sel;
	uint32_t queue_sel;
	uint32_t interrupt_status;
	uint32_t status;
	uint64_t tx_count;
	uint64_t rx_count;
	bool irq_asserted;
};

struct virtio_ring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

static struct virtio_console_dev virtio_console_devs[CONFIG_MAX_VM_NUM];

static struct virtio_console_dev *virtio_console_get_dev(const struct acrn_vm *vm)
{
	return (vm != NULL) && (vm->vm_id < CONFIG_MAX_VM_NUM) ?
		&virtio_console_devs[vm->vm_id] : NULL;
}

static uint32_t virtio_console_irq(const struct acrn_vm *vm)
{
	return get_vm_config(vm->vm_id)->arch.guest_virtio_console_irq;
}

static uint64_t virtio_console_device_features(void)
{
	return 1ULL << VIRTIO_F_VERSION_1;
}

static bool virtio_console_read_gpa(struct acrn_vm *vm, uint64_t gpa,
	void *buf, uint32_t size)
{
	return copy_from_gpa(vm, buf, gpa, size) == 0;
}

static bool virtio_console_write_gpa(struct acrn_vm *vm, uint64_t gpa,
	void *buf, uint32_t size)
{
	return copy_to_gpa(vm, buf, gpa, size) == 0;
}

static uint16_t virtio_console_read_u16(struct acrn_vm *vm, uint64_t gpa)
{
	uint16_t value = 0U;

	(void)virtio_console_read_gpa(vm, gpa, &value, sizeof(value));
	return value;
}

static void virtio_console_write_u16(struct acrn_vm *vm, uint64_t gpa,
	uint16_t value)
{
	(void)virtio_console_write_gpa(vm, gpa, &value, sizeof(value));
}

static bool virtio_console_read_desc(struct acrn_vm *vm,
	const struct virtio_mmio_queue *vq, uint16_t id,
	struct virtio_ring_desc *desc)
{
	bool ret = false;

	if ((vq->num != 0U) && (id < vq->num)) {
		ret = virtio_console_read_gpa(vm,
			vq->desc + ((uint64_t)id * sizeof(*desc)),
			desc, sizeof(*desc));
	}

	return ret;
}

static bool virtio_console_queue_valid(const struct virtio_mmio_queue *vq)
{
	return vq->ready && (vq->num != 0U) && (vq->num <= VIRTIO_CONSOLE_QUEUE_SIZE) &&
		(vq->desc != 0UL) && (vq->avail != 0UL) && (vq->used != 0UL);
}

static bool virtio_console_pop_avail(struct acrn_vm *vm,
	struct virtio_mmio_queue *vq, uint16_t *head)
{
	uint16_t avail_idx;
	uint64_t ring_gpa;
	bool ret = false;

	if (virtio_console_queue_valid(vq)) {
		avail_idx = virtio_console_read_u16(vm, vq->avail + 2UL);
		if (vq->last_avail_idx != avail_idx) {
			ring_gpa = vq->avail + 4UL +
				((uint64_t)(vq->last_avail_idx % vq->num) * sizeof(uint16_t));
			*head = virtio_console_read_u16(vm, ring_gpa);
			vq->last_avail_idx++;
			ret = true;
		}
	}

	return ret;
}

static bool virtio_console_add_used(struct acrn_vm *vm,
	struct virtio_mmio_queue *vq, uint16_t id, uint32_t len)
{
	uint16_t used_idx;
	uint64_t elem_gpa;
	uint32_t elem[2];
	bool ret = false;

	if (virtio_console_queue_valid(vq)) {
		used_idx = virtio_console_read_u16(vm, vq->used + 2UL);
		elem_gpa = vq->used + 4UL +
			((uint64_t)(used_idx % vq->num) * sizeof(elem));
		elem[0] = id;
		elem[1] = len;
		if (virtio_console_write_gpa(vm, elem_gpa, elem, sizeof(elem))) {
			virtio_console_write_u16(vm, vq->used + 2UL, used_idx + 1U);
			ret = true;
		}
	}

	return ret;
}

static void virtio_console_update_irq(struct acrn_vm *vm,
	struct virtio_console_dev *dev)
{
	uint32_t irq = virtio_console_irq(vm);

	if ((dev->interrupt_status != 0U) && !dev->irq_asserted) {
		arch_trigger_level_intr(vm, irq, true);
		dev->irq_asserted = true;
	} else if ((dev->interrupt_status == 0U) && dev->irq_asserted) {
		arch_trigger_level_intr(vm, irq, false);
		dev->irq_asserted = false;
	}
}

static void virtio_console_raise_used_irq(struct acrn_vm *vm,
	struct virtio_console_dev *dev)
{
	dev->interrupt_status |= VIRTIO_MMIO_INT_USED_RING;
	virtio_console_update_irq(vm, dev);
}

static void virtio_console_reset(struct acrn_vm *vm, struct virtio_console_dev *dev)
{
	bool asserted = dev->irq_asserted;

	(void)memset(dev, 0U, sizeof(*dev));
	if (asserted) {
		arch_trigger_level_intr(vm, virtio_console_irq(vm), false);
	}
}

static bool virtio_console_copy_tx_desc(struct acrn_vm *vm,
	const struct virtio_ring_desc *desc, uint32_t *total)
{
	char buf[VIRTIO_CONSOLE_COPY_BUF_SIZE];
	uint32_t copied = 0U;
	uint32_t chunk;

	while (copied < desc->len) {
		chunk = desc->len - copied;
		if (chunk > sizeof(buf)) {
			chunk = sizeof(buf);
		}
		if (!virtio_console_read_gpa(vm, desc->addr + copied, buf, chunk)) {
			return false;
		}
		for (uint32_t i = 0U; i < chunk; i++) {
			(void)console_vm_tx_put(vm->vm_id, buf[i]);
		}
		copied += chunk;
	}
	*total += desc->len;

	return true;
}

static uint32_t virtio_console_handle_tx_chain(struct acrn_vm *vm,
	struct virtio_mmio_queue *vq, uint16_t head)
{
	struct virtio_ring_desc desc;
	uint16_t id = head;
	uint32_t total = 0U;
	uint32_t nr_desc = 0U;
	bool ok = true;

	do {
		if (!virtio_console_read_desc(vm, vq, id, &desc)) {
			ok = false;
			break;
		}
		if ((desc.flags & VIRTIO_RING_F_WRITE) == 0U) {
			ok = virtio_console_copy_tx_desc(vm, &desc, &total);
			if (!ok) {
				break;
			}
		}
		nr_desc++;
		id = desc.next;
	} while (((desc.flags & VIRTIO_RING_F_NEXT) != 0U) &&
		(nr_desc < VIRTIO_CONSOLE_CHAIN_LIMIT));

	return ok ? total : 0U;
}

static void virtio_console_process_tx(struct acrn_vm *vm,
	struct virtio_console_dev *dev)
{
	struct virtio_mmio_queue *vq = &dev->queues[VIRTIO_CONSOLE_QUEUE_TX];
	uint16_t head;
	uint32_t total;
	bool used = false;

	while (virtio_console_pop_avail(vm, vq, &head)) {
		total = virtio_console_handle_tx_chain(vm, vq, head);
		if (virtio_console_add_used(vm, vq, head, total)) {
			dev->tx_count += total;
			used = true;
		}
	}

	if (used) {
		virtio_console_raise_used_irq(vm, dev);
	}
}

static uint32_t virtio_console_fill_rx_desc(struct acrn_vm *vm,
	const struct virtio_ring_desc *desc)
{
	struct acrn_vuart *console = vm_console_vuart(vm);
	char buf[VIRTIO_CONSOLE_COPY_BUF_SIZE];
	uint32_t filled = 0U;
	uint32_t chunk = 0U;
	char ch;

	while ((filled < desc->len) && vuart_rx_pending(console)) {
		ch = vuart_get_rx_char(console);
		if (ch == -1) {
			break;
		}
		buf[chunk] = ch;
		chunk++;
		filled++;
		if ((chunk == sizeof(buf)) || (filled == desc->len) ||
			!vuart_rx_pending(console)) {
			if (!virtio_console_write_gpa(vm,
				desc->addr + filled - chunk, buf, chunk)) {
				return 0U;
			}
			chunk = 0U;
		}
	}

	return filled;
}

static uint32_t virtio_console_handle_rx_chain(struct acrn_vm *vm,
	struct virtio_mmio_queue *vq, uint16_t head)
{
	struct virtio_ring_desc desc;
	uint16_t id = head;
	uint32_t total = 0U;
	uint32_t nr_desc = 0U;
	uint32_t filled;

	do {
		if (!virtio_console_read_desc(vm, vq, id, &desc)) {
			break;
		}
		if ((desc.flags & VIRTIO_RING_F_WRITE) != 0U) {
			filled = virtio_console_fill_rx_desc(vm, &desc);
			total += filled;
			if ((filled < desc.len) || !vuart_rx_pending(vm_console_vuart(vm))) {
				break;
			}
		}
		nr_desc++;
		id = desc.next;
	} while (((desc.flags & VIRTIO_RING_F_NEXT) != 0U) &&
		(nr_desc < VIRTIO_CONSOLE_CHAIN_LIMIT));

	return total;
}

static void virtio_console_process_rx(struct acrn_vm *vm,
	struct virtio_console_dev *dev)
{
	struct acrn_vuart *console = vm_console_vuart(vm);
	struct virtio_mmio_queue *vq = &dev->queues[VIRTIO_CONSOLE_QUEUE_RX];
	uint16_t head;
	uint32_t total;
	bool used = false;

	(void)console_vm_rx_refill(console);
	while (vuart_rx_pending(console) && virtio_console_pop_avail(vm, vq, &head)) {
		total = virtio_console_handle_rx_chain(vm, vq, head);
		if (total == 0U) {
			break;
		}
		if (virtio_console_add_used(vm, vq, head, total)) {
			dev->rx_count += total;
			used = true;
		}
	}

	if (used) {
		virtio_console_raise_used_irq(vm, dev);
	}
}

static void virtio_console_notify_rx(struct acrn_vuart *console)
{
	struct acrn_vm *vm = console->vm;
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);

	if (dev != NULL) {
		virtio_console_process_rx(vm, dev);
	}
}

static const struct vuart_backend_ops virtio_console_backend_ops = {
	.notify_rx = virtio_console_notify_rx,
};

void virtio_console_init_vm(struct acrn_vm *vm)
{
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);
	struct acrn_vuart *console;
	uint32_t irq = virtio_console_irq(vm);

	if (dev != NULL) {
		(void)memset(dev, 0U, sizeof(*dev));
		init_console_vuart(vm, irq);
		console = vm_console_vuart(vm);
		vuart_set_backend(console, &virtio_console_backend_ops);
	}
}

static struct virtio_mmio_queue *virtio_console_selected_queue(
	struct virtio_console_dev *dev)
{
	return dev->queue_sel < VIRTIO_CONSOLE_QUEUE_NUM ?
		&dev->queues[dev->queue_sel] : NULL;
}

static uint32_t virtio_console_read_reg(struct acrn_vm *vm,
	struct virtio_console_dev *dev, uint32_t offset)
{
	const struct virtio_mmio_queue *vq = virtio_console_selected_queue(dev);
	uint64_t features = virtio_console_device_features();
	uint32_t value = 0U;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		return 0U;
	}

	switch (offset) {
	case VIRTIO_MMIO_MAGIC_VALUE:
		value = VIRTIO_MMIO_MAGIC;
		break;
	case VIRTIO_MMIO_VERSION:
		value = VIRTIO_MMIO_VERSION_2;
		break;
	case VIRTIO_MMIO_DEVICE_ID:
		value = VIRTIO_CONSOLE_DEVICE_ID;
		break;
	case VIRTIO_MMIO_VENDOR_ID:
		value = VIRTIO_VENDOR_ID_BEAU;
		break;
	case VIRTIO_MMIO_DEVICE_FEATURES:
		value = (dev->device_features_sel == 0U) ?
			(uint32_t)features : (uint32_t)(features >> 32U);
		break;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		value = dev->queue_sel < VIRTIO_CONSOLE_QUEUE_NUM ?
			VIRTIO_CONSOLE_QUEUE_SIZE : 0U;
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

	(void)vm;
	return value;
}

static void virtio_console_write_queue_addr(struct virtio_mmio_queue *vq,
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

static void virtio_console_write_status(struct acrn_vm *vm,
	struct virtio_console_dev *dev, uint32_t value)
{
	uint64_t unsupported = dev->driver_features & ~virtio_console_device_features();

	if (value == 0U) {
		virtio_console_reset(vm, dev);
		return;
	}

	dev->status = value;
	if (((value & VIRTIO_STATUS_FEATURES_OK) != 0U) && (unsupported != 0UL)) {
		dev->status &= ~VIRTIO_STATUS_FEATURES_OK;
	}
}

static void virtio_console_write_reg(struct acrn_vm *vm,
	struct virtio_console_dev *dev, uint32_t offset, uint32_t value)
{
	struct virtio_mmio_queue *vq = virtio_console_selected_queue(dev);

	if (offset >= VIRTIO_MMIO_CONFIG) {
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
		if ((vq != NULL) && (value <= VIRTIO_CONSOLE_QUEUE_SIZE)) {
			vq->num = (uint16_t)value;
		}
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		if (vq != NULL) {
			vq->ready = (value != 0U);
			if (!vq->ready) {
				vq->last_avail_idx = 0U;
			}
			if ((dev->queue_sel == VIRTIO_CONSOLE_QUEUE_RX) && vq->ready) {
				virtio_console_process_rx(vm, dev);
			}
		}
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		if (value == VIRTIO_CONSOLE_QUEUE_TX) {
			virtio_console_process_tx(vm, dev);
		} else if (value == VIRTIO_CONSOLE_QUEUE_RX) {
			virtio_console_process_rx(vm, dev);
		}
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		dev->interrupt_status &= ~value;
		virtio_console_update_irq(vm, dev);
		break;
	case VIRTIO_MMIO_STATUS:
		virtio_console_write_status(vm, dev, value);
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
	case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
	case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
	case VIRTIO_MMIO_QUEUE_USED_LOW:
	case VIRTIO_MMIO_QUEUE_USED_HIGH:
		if (vq != NULL) {
			virtio_console_write_queue_addr(vq, offset, value);
		}
		break;
	default:
		break;
	}
}

static uint64_t virtio_console_resize_value(uint64_t value, uint64_t size)
{
	uint64_t ret = 0UL;

	if (size == 1UL) {
		ret = value & 0xffU;
	} else if (size == 2UL) {
		ret = value & 0xffffU;
	} else {
		ret = value & 0xffffffffUL;
	}

	return ret;
}

int32_t virtio_console_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	uint64_t base;
	uint32_t offset;
	uint32_t value;
	int32_t ret = -EINVAL;

	if ((dev != NULL) && ((mmio->size == 1UL) || (mmio->size == 2UL) ||
		(mmio->size == 4UL))) {
		base = get_vm_config(vm->vm_id)->arch.guest_virtio_console_base;
		offset = (uint32_t)(mmio->address - base);
		if (mmio->direction == ACRN_IOREQ_DIR_READ) {
			mmio->value = virtio_console_resize_value(
				virtio_console_read_reg(vm, dev, offset), mmio->size);
		} else {
			value = (uint32_t)virtio_console_resize_value(mmio->value, mmio->size);
			virtio_console_write_reg(vm, dev, offset, value);
		}
		ret = 0;
	}

	return ret;
}
