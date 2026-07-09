/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vm_config.h>
#include <console.h>
#include <bsp/vuart.h>
#include <rtl.h>
#include <bsp/io_req.h>
#include <virtio_mmio.h>
#include <virtio_console.h>

/*
 * virtio-console runtime framework:
 *
 * virtio_console is a built-in BEAU console transport for each Linux VM. It is
 * not a virtio-proxy device and does not forward requests to a VM backend by
 * HVC. BEAU owns the full console data path and bridges the guest's
 * virtio-console frontend to the per-VM console vUART used by the BEAU shell.
 *
 *   Linux VM frontend                          BEAU EL2
 *   -----------------                          -------
 *
 *   virtio_console driver
 *      |
 *      | MMIO probe / queue setup
 *      v
 *   virtio-mmio regs  <---------------->  virtio_console_dev
 *      |                                  - embedded virtio_mmio_dev
 *      |                                  - tx/rx byte counters
 *      |
 *      | QueueReady RX
 *      v
 *   RX virtqueue    ------------------->  virtio_console_process_rx()
 *      ^                                  - console_vm_rx_refill()
 *      |                                  - vuart_get_rx_char()
 *      |                                  - copy bytes into writable descs
 *      |                                  - add used ring and inject IRQ
 *      |
 *      | guest reads from hvc/tty
 *      |
 *      | QueueNotify TX
 *      v
 *   TX virtqueue    ------------------->  virtio_console_process_tx()
 *                                         - copy frontend-readable descs
 *                                         - console_vm_tx_put()
 *                                         - add used ring and inject IRQ
 *
 *   BEAU shell input  ---> console_vm_rx_refill() ---> VM console vUART
 *   VM guest output   ---> console_vm_tx_put()    ---> BEAU shell/vsh ring
 *
 * Direction naming follows virtio-console convention from the device view:
 * RX queue carries BEAU-to-guest input, TX queue carries guest-to-BEAU output.
 */

#define VIRTIO_CONSOLE_QUEUE_RX		0U
#define VIRTIO_CONSOLE_QUEUE_TX		1U
#define VIRTIO_CONSOLE_QUEUE_NUM	2U
#define VIRTIO_CONSOLE_QUEUE_SIZE	64U

#define VIRTIO_CONSOLE_COPY_BUF_SIZE	64U
#define VIRTIO_CONSOLE_CHAIN_LIMIT	VIRTIO_CONSOLE_QUEUE_SIZE

struct virtio_console_dev {
	struct virtio_mmio_dev mmio;
	uint64_t tx_count;
	uint64_t rx_count;
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

static void virtio_console_reset(struct virtio_mmio_dev *mmio)
{
	struct virtio_console_dev *dev = (struct virtio_console_dev *)virtio_mmio_priv(mmio);

	if (dev != NULL) {
		dev->tx_count = 0UL;
		dev->rx_count = 0UL;
	}
}

static bool virtio_console_copy_tx_desc(struct virtio_mmio_dev *mmio,
	const struct virtio_ring_desc *desc, uint32_t *total)
{
	struct acrn_vm *vm = virtio_mmio_vm(mmio);
	char buf[VIRTIO_CONSOLE_COPY_BUF_SIZE];
	uint32_t copied = 0U;
	uint32_t chunk;

	while (copied < desc->len) {
		chunk = desc->len - copied;
		if (chunk > sizeof(buf)) {
			chunk = sizeof(buf);
		}
		if (!virtio_mmio_read_gpa(mmio, desc->addr + copied, buf, chunk)) {
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

static uint32_t virtio_console_handle_tx_chain(struct virtio_mmio_dev *mmio,
	struct virtio_mmio_queue *vq, uint16_t head)
{
	struct virtio_ring_desc desc;
	uint16_t id = head;
	uint32_t total = 0U;
	uint32_t nr_desc = 0U;
	bool ok = true;

	do {
		if (!virtio_mmio_read_desc(mmio, vq, id, &desc)) {
			ok = false;
			break;
		}
		if ((desc.flags & VIRTIO_RING_F_WRITE) == 0U) {
			ok = virtio_console_copy_tx_desc(mmio, &desc, &total);
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

static void virtio_console_process_tx(struct virtio_console_dev *dev)
{
	struct virtio_mmio_dev *mmio = &dev->mmio;
	struct virtio_mmio_queue *vq = virtio_mmio_get_queue(mmio,
		VIRTIO_CONSOLE_QUEUE_TX);
	uint16_t head;
	uint32_t total;
	bool used = false;

	while (virtio_mmio_pop_avail(mmio, vq, &head)) {
		total = virtio_console_handle_tx_chain(mmio, vq, head);
		if (virtio_mmio_add_used(mmio, vq, head, total)) {
			dev->tx_count += total;
			used = true;
		}
	}

	if (used) {
		virtio_mmio_raise_used_irq(mmio);
	}
}

static uint32_t virtio_console_fill_rx_desc(struct virtio_mmio_dev *mmio,
	const struct virtio_ring_desc *desc)
{
	struct acrn_vm *vm = virtio_mmio_vm(mmio);
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
			if (!virtio_mmio_write_gpa(mmio,
				desc->addr + filled - chunk, buf, chunk)) {
				return 0U;
			}
			chunk = 0U;
		}
	}

	return filled;
}

static uint32_t virtio_console_handle_rx_chain(struct virtio_mmio_dev *mmio,
	struct virtio_mmio_queue *vq, uint16_t head)
{
	struct acrn_vm *vm = virtio_mmio_vm(mmio);
	struct virtio_ring_desc desc;
	uint16_t id = head;
	uint32_t total = 0U;
	uint32_t nr_desc = 0U;
	uint32_t filled;

	do {
		if (!virtio_mmio_read_desc(mmio, vq, id, &desc)) {
			break;
		}
		if ((desc.flags & VIRTIO_RING_F_WRITE) != 0U) {
			filled = virtio_console_fill_rx_desc(mmio, &desc);
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

static void virtio_console_process_rx(struct virtio_console_dev *dev)
{
	struct virtio_mmio_dev *mmio = &dev->mmio;
	struct acrn_vuart *console = vm_console_vuart(virtio_mmio_vm(mmio));
	struct virtio_mmio_queue *vq = virtio_mmio_get_queue(mmio,
		VIRTIO_CONSOLE_QUEUE_RX);
	uint16_t head;
	uint32_t total;
	bool used = false;

	(void)console_vm_rx_refill(console);
	while (vuart_rx_pending(console) && virtio_mmio_pop_avail(mmio, vq, &head)) {
		total = virtio_console_handle_rx_chain(mmio, vq, head);
		if (total == 0U) {
			break;
		}
		if (virtio_mmio_add_used(mmio, vq, head, total)) {
			dev->rx_count += total;
			used = true;
		}
	}

	if (used) {
		virtio_mmio_raise_used_irq(mmio);
	}
}

static void virtio_console_queue_ready(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct virtio_console_dev *dev = (struct virtio_console_dev *)virtio_mmio_priv(mmio);

	if ((dev != NULL) && (queue_id == VIRTIO_CONSOLE_QUEUE_RX)) {
		virtio_console_process_rx(dev);
	}
}

static void virtio_console_notify_queue(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct virtio_console_dev *dev = (struct virtio_console_dev *)virtio_mmio_priv(mmio);

	if (dev == NULL) {
		return;
	}
	if (queue_id == VIRTIO_CONSOLE_QUEUE_TX) {
		virtio_console_process_tx(dev);
	} else if (queue_id == VIRTIO_CONSOLE_QUEUE_RX) {
		virtio_console_process_rx(dev);
	}
}

static const struct virtio_mmio_ops virtio_console_mmio_ops = {
	.reset = virtio_console_reset,
	.queue_ready = virtio_console_queue_ready,
	.notify_queue = virtio_console_notify_queue,
};

static void virtio_console_notify_rx(struct acrn_vuart *console)
{
	struct acrn_vm *vm = console->vm;
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);

	if (dev != NULL) {
		virtio_console_process_rx(dev);
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
	struct virtio_mmio_init init = {
		.name = "virtio-console",
		.vm = vm,
		.base = get_vm_config(vm->vm_id)->arch.guest_virtio_console_base,
		.size = get_vm_config(vm->vm_id)->arch.guest_virtio_console_size,
		.irq = irq,
		.device_id = VIRTIO_DEVICE_ID_CONSOLE,
		.queue_num = VIRTIO_CONSOLE_QUEUE_NUM,
		.queue_size = VIRTIO_CONSOLE_QUEUE_SIZE,
		.device_features = 0UL,
		.ops = &virtio_console_mmio_ops,
		.priv = dev,
	};

	if (dev != NULL) {
		virtio_mmio_init(&dev->mmio, &init);
		init_console_vuart(vm, irq);
		console = vm_console_vuart(vm);
		vuart_set_backend(console, &virtio_console_backend_ops);
	}
}

void virtio_console_reset_vm(struct acrn_vm *vm)
{
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);

	if (dev != NULL) {
		virtio_mmio_reset_dev(&dev->mmio);
	}
}

int32_t virtio_console_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);

	return dev != NULL ? virtio_mmio_handler(io_req, &dev->mmio) : -EINVAL;
}
