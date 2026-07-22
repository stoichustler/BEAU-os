/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vconfig.h>
#include <console.h>
#include <bsp/vuart.h>
#include <rtl.h>
#include <bsp/io_req.h>
#include <ticks.h>
#include <virtio_mmio.h>
#include <virtio_console.h>

/* [20260712] virtio-console runtime framework
 *
 * virtio_console is a built-in BEAU console transport for each Linux VM. It is
 * not a virtio-proxy device and does not forward requests to a VM backend by
 * HVC. BEAU owns the full console data path and bridges the guest's
 * virtio-console frontend to the per-VM console vUART used by the BEAU shell.
 * The intended split is Linux -> virtio-console and RTOS -> vPL011; the
 * frontend differs, but the host-side console ring and vsh backend are shared.
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
 *
 * Key rule:
 *   - Linux owns virtqueue memory and queue notifications;
 *   - BEAU owns the MMIO transport shadow and per-VM console vUART bridge;
 *   - descriptors are copied and bounded before bytes enter the shared console
 *     ring, preventing stale or oversized chains from blocking the shell.
 */

#define VIRTIO_CONSOLE_QUEUE_RX		0U
#define VIRTIO_CONSOLE_QUEUE_TX		1U
#define VIRTIO_CONSOLE_QUEUE_NUM	2U
#define VIRTIO_CONSOLE_QUEUE_SIZE	64U

#define VIRTIO_CONSOLE_COPY_BUF_SIZE	64U
#define VIRTIO_CONSOLE_CHAIN_LIMIT	VIRTIO_CONSOLE_QUEUE_SIZE
#define VIRTIO_CONSOLE_USEC_PER_SEC	1000000UL

struct virtio_console_latency_accum {
	uint64_t count;
	uint64_t min;
	uint64_t max;
	uint64_t sum;
};

struct virtio_console_dev {
	struct virtio_mmio_dev mmio;
	uint64_t tx_count;
	uint64_t rx_count;
	uint64_t tx_notify_count;
	uint64_t rx_notify_count;
	uint64_t tx_irq_count;
	uint64_t rx_irq_count;
	uint64_t tx_first_tick;
	uint64_t tx_last_tick;
	uint64_t rx_first_tick;
	uint64_t rx_last_tick;
	struct virtio_console_latency_accum tx_latency;
	struct virtio_console_latency_accum rx_latency;
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

static void virtio_console_add_u64(uint64_t *counter, uint64_t delta)
{
	if (counter == NULL) {
		return;
	}

	if (*counter > (UINT64_MAX - delta)) {
		*counter = UINT64_MAX;
	} else {
		*counter += delta;
	}
}

static void virtio_console_mark_bytes(uint64_t *first_tick,
	uint64_t *last_tick, uint64_t now)
{
	if ((first_tick == NULL) || (last_tick == NULL)) {
		return;
	}

	if (*first_tick == 0UL) {
		*first_tick = now;
	}
	*last_tick = now;
}

static uint64_t virtio_console_byte_rate(uint64_t bytes, uint64_t first_tick,
	uint64_t now)
{
	uint64_t elapsed_us;

	if ((first_tick == 0UL) || (now <= first_tick)) {
		return 0UL;
	}

	elapsed_us = ticks_to_us(now - first_tick);
	if (elapsed_us == 0UL) {
		return 0UL;
	}
	if (bytes > (UINT64_MAX / VIRTIO_CONSOLE_USEC_PER_SEC)) {
		return UINT64_MAX;
	}

	return (bytes * VIRTIO_CONSOLE_USEC_PER_SEC) / elapsed_us;
}

static void virtio_console_latency_accum(
	struct virtio_console_latency_accum *stats, uint64_t delta)
{
	if (stats == NULL) {
		return;
	}

	if ((stats->count == 0UL) || (delta < stats->min)) {
		stats->min = delta;
	}
	if (delta > stats->max) {
		stats->max = delta;
	}
	virtio_console_add_u64(&stats->sum, delta);
	stats->count++;
}

static void virtio_console_latency_export(
	const struct virtio_console_latency_accum *src,
	struct virtio_console_latency_stats *dst)
{
	if ((src == NULL) || (dst == NULL)) {
		return;
	}

	dst->count = src->count;
	if (src->count != 0UL) {
		dst->min_us = ticks_to_us(src->min);
		dst->avg_us = ticks_to_us(src->sum / src->count);
		dst->max_us = ticks_to_us(src->max);
	}
}

static void virtio_console_reset(struct virtio_mmio_dev *mmio)
{
	struct virtio_console_dev *dev = (struct virtio_console_dev *)virtio_mmio_priv(mmio);

	if (dev != NULL) {
		dev->tx_count = 0UL;
		dev->rx_count = 0UL;
		dev->tx_notify_count = 0UL;
		dev->rx_notify_count = 0UL;
		dev->tx_irq_count = 0UL;
		dev->rx_irq_count = 0UL;
		dev->tx_first_tick = 0UL;
		dev->tx_last_tick = 0UL;
		dev->rx_first_tick = 0UL;
		dev->rx_last_tick = 0UL;
		(void)memset(&dev->tx_latency, 0U, sizeof(dev->tx_latency));
		(void)memset(&dev->rx_latency, 0U, sizeof(dev->rx_latency));
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
		(void)console_vm_tx_write(vm->vm_id, buf, chunk);
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
	uint64_t start = cpu_ticks();
	uint64_t now;
	uint16_t head;
	uint32_t total;
	bool used = false;

	while (virtio_mmio_pop_avail(mmio, vq, &head)) {
		total = virtio_console_handle_tx_chain(mmio, vq, head);
		if (virtio_mmio_add_used(mmio, vq, head, total)) {
			virtio_console_add_u64(&dev->tx_count, total);
			used = true;
		}
	}

	if (used) {
		now = cpu_ticks();
		virtio_console_mark_bytes(&dev->tx_first_tick,
			&dev->tx_last_tick, now);
		virtio_console_latency_accum(&dev->tx_latency, now - start);
		virtio_console_add_u64(&dev->tx_irq_count, 1UL);
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
	uint64_t start = cpu_ticks();
	uint64_t now;
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
			virtio_console_add_u64(&dev->rx_count, total);
			used = true;
		}
	}

	if (used) {
		now = cpu_ticks();
		virtio_console_mark_bytes(&dev->rx_first_tick,
			&dev->rx_last_tick, now);
		virtio_console_latency_accum(&dev->rx_latency, now - start);
		virtio_console_add_u64(&dev->rx_irq_count, 1UL);
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
		virtio_console_add_u64(&dev->tx_notify_count, 1UL);
		virtio_console_process_tx(dev);
	} else if (queue_id == VIRTIO_CONSOLE_QUEUE_RX) {
		virtio_console_add_u64(&dev->rx_notify_count, 1UL);
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
		virtio_console_add_u64(&dev->rx_notify_count, 1UL);
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

bool virtio_console_get_stats(uint16_t vm_id, struct virtio_console_stats *stats)
{
	struct virtio_console_dev *dev;
	uint64_t now = cpu_ticks();

	if ((stats == NULL) || (vm_id >= CONFIG_MAX_VM_NUM)) {
		return false;
	}

	(void)memset(stats, 0U, sizeof(*stats));
	dev = &virtio_console_devs[vm_id];
	if ((dev->mmio.vm == NULL) || (dev->mmio.size == 0UL)) {
		return false;
	}

	stats->active = true;
	stats->base = dev->mmio.base;
	stats->size = dev->mmio.size;
	stats->irq = dev->mmio.irq;
	stats->status = dev->mmio.status;
	stats->interrupt_status = dev->mmio.interrupt_status;
	stats->device_features = dev->mmio.device_features;
	stats->driver_features = dev->mmio.driver_features;
	stats->tx_count = dev->tx_count;
	stats->rx_count = dev->rx_count;
	stats->tx_notify_count = dev->tx_notify_count;
	stats->rx_notify_count = dev->rx_notify_count;
	stats->tx_irq_count = dev->tx_irq_count;
	stats->rx_irq_count = dev->rx_irq_count;
	stats->tx_byte_rate = virtio_console_byte_rate(dev->tx_count,
		dev->tx_first_tick, now);
	stats->rx_byte_rate = virtio_console_byte_rate(dev->rx_count,
		dev->rx_first_tick, now);
	virtio_console_latency_export(&dev->tx_latency, &stats->tx_latency);
	virtio_console_latency_export(&dev->rx_latency, &stats->rx_latency);

	for (uint16_t i = 0U;
		(i < VIRTIO_CONSOLE_STAT_QUEUE_NUM) && (i < dev->mmio.queue_num); i++) {
		const struct virtio_mmio_queue *vq = &dev->mmio.queues[i];

		stats->queues[i].num = vq->num;
		stats->queues[i].last_avail_idx = vq->last_avail_idx;
		stats->queues[i].desc = vq->desc;
		stats->queues[i].avail = vq->avail;
		stats->queues[i].used = vq->used;
		stats->queues[i].ready = vq->ready;
	}

	return true;
}

int32_t virtio_console_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct virtio_console_dev *dev = virtio_console_get_dev(vm);

	return dev != NULL ? virtio_mmio_handler(io_req, &dev->mmio) : -EINVAL;
}
