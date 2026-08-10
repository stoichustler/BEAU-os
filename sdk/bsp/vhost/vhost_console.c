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
#include <spinlock.h>
#include <logmsg.h>
#include <virtio_mmio.h>
#include <vhost_console.h>

/* [20260712] vhost-console runtime framework
 *
 * vhost_console is BEAU's built-in host-side console backend for each Linux
 * VM. It is not a virtio-proxy device and does not forward requests to a VM
 * backend by HVC. BEAU owns the full console data path and bridges the guest's
 * virtio-console frontend to the per-VM console vUART used by the BEAU shell.
 * The intended split is Linux -> virtio-console and RTOS -> vPL011; the
 * frontend differs, but the host-side console ring and vsh backend are shared.
 *
 *   Linux VM frontend                          BEAU EL2
 *   -----------------                          -------
 *
 *   Linux virtio-console driver
 *      |
 *      | MMIO probe / queue setup
 *      v
 *   virtio-mmio regs  <---------------->  vhost_console_dev
 *      |                                  - embedded virtio_mmio_dev
 *      |                                  - tx/rx byte counters
 *      |
 *      | QueueReady RX
 *      v
 *   RX virtqueue    ------------------->  vhost_console_process_rx()
 *      ^                                  - console_vm_rx_refill()
 *      |                                  - vuart_get_rx_char()
 *      |                                  - copy bytes into writable descs
 *      |                                  - add used ring and inject IRQ
 *      |
 *      | guest reads from hvc/tty
 *      |
 *      | QueueNotify TX
 *      v
 *   TX virtqueue    ------------------->  vhost_console_process_tx()
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

#define VHOST_CONSOLE_QUEUE_RX		0U
#define VHOST_CONSOLE_QUEUE_TX		1U
#define VHOST_CONSOLE_QUEUE_NUM	2U
#define VHOST_CONSOLE_QUEUE_SIZE	64U

#define VHOST_CONSOLE_COPY_BUF_SIZE	64U
#define VHOST_CONSOLE_CHAIN_LIMIT	VHOST_CONSOLE_QUEUE_SIZE
#define VHOST_CONSOLE_PENDING_QUEUE(queue_id)	(1U << (queue_id))
#define VHOST_CONSOLE_BOOT_LOG_DRIVER_OK	(1U << 3U)
#define VHOST_CONSOLE_BOOT_LOG_FAILED		(1U << 4U)
#define VHOST_CONSOLE_STATUS_OK	(VIRTIO_STATUS_ACKNOWLEDGE | \
	VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK)

struct vhost_console_boot_log {
	uint16_t vm_id;
	uint32_t phase_mask;
	uint32_t status;
};

/* [20260804] vhost-console transport service ownership
 *
 * guest MMIO -> state_lock -> pending queue -> RX/TX service lock
 * host RX   -> state_lock -> pending RX    -> RX service lock
 *                                             |
 *                                             v
 *                                       irq_lock -> used-ring vIRQ
 *
 * Key rule:
 *   - state_lock owns MMIO configuration, reset, and pending publication;
 *     rx_lock and tx_lock independently own their queue shadow, payload copy,
 *     and direction-specific statistics;
 *   - ordinary MMIO only publishes work while it holds state_lock. Queue setup
 *     and reset add the required queue locks; descriptor service starts after
 *     state_lock is released, so guest output cannot delay host RX;
 *   - reset and statistics snapshot use state -> RX -> TX -> IRQ. Services use
 *     RX/TX -> IRQ only, and take the vUART FIFO lock afterwards, preventing
 *     concurrent avail consumption and lock inversion.
 */
struct vhost_console_dev {
	spinlock_t state_lock;
	spinlock_t rx_lock;
	spinlock_t tx_lock;
	spinlock_t irq_lock;
	struct virtio_mmio_dev mmio;
	uint32_t pending_queue_mask;
	uint32_t pending_boot_log_mask;
	bool pass_reported;
	bool fail_reported;
	uint64_t tx_count;
	uint64_t rx_count;
	uint64_t tx_notify_count;
	uint64_t rx_notify_count;
	uint64_t tx_irq_count;
	uint64_t rx_irq_count;
};

static struct vhost_console_dev vhost_console_devs[CONFIG_MAX_VM_NUM];

static struct vhost_console_dev *vhost_console_get_dev(const struct acrn_vm *vm)
{
	return (vm != NULL) && (vm->vm_id < CONFIG_MAX_VM_NUM) ?
		&vhost_console_devs[vm->vm_id] : NULL;
}

static void vhost_console_lock_state(struct vhost_console_dev *dev,
	uint64_t *rflags)
{
	spinlock_irqsave_obtain(&dev->state_lock, rflags);
}

static void vhost_console_unlock_state(struct vhost_console_dev *dev,
	uint64_t rflags)
{
	spinlock_irqrestore_release(&dev->state_lock, rflags);
}

static void vhost_console_mark_pending(struct vhost_console_dev *dev,
	uint16_t queue_id)
{
	if (queue_id < VHOST_CONSOLE_QUEUE_NUM) {
		dev->pending_queue_mask |= VHOST_CONSOLE_PENDING_QUEUE(queue_id);
	}
}

static uint32_t vhost_console_irq(const struct acrn_vm *vm)
{
	return get_vm_config(vm->vm_id)->arch.guest_virtio_console_irq;
}

static void vhost_console_add_u64(uint64_t *counter, uint64_t delta)
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

/* [20260804] vhost-console boot diagnostic publication
 *
 * guest MMIO -> state_lock -> result bit -> unlock -> daemon_log
 *
 * Key rule:
 *   - state_lock owns the one-shot result bit and pending diagnostic bit;
 *   - a caller snapshots and clears the bit before async shell output, so a
 *     log cannot block guest MMIO or be emitted twice for one session;
 *   - reset clears unpublished bits with its session state, preventing an old
 *     transport session from being reported after the device is reused.
 */
static void vhost_console_record_boot_result(struct vhost_console_dev *dev,
	bool *reported, uint32_t phase_mask)
{
	if (!*reported) {
		*reported = true;
		dev->pending_boot_log_mask |= phase_mask;
	}
}

static void vhost_console_record_status(struct vhost_console_dev *dev)
{
	if (dev == NULL) {
		return;
	}

	if (!dev->pass_reported &&
		((dev->mmio.status & VIRTIO_STATUS_DRIVER_OK) != 0U)) {
		vhost_console_record_boot_result(dev, &dev->pass_reported,
			VHOST_CONSOLE_BOOT_LOG_DRIVER_OK);
	}
	if (!dev->fail_reported &&
		((dev->mmio.status & VIRTIO_STATUS_FAILED) != 0U)) {
		vhost_console_record_boot_result(dev, &dev->fail_reported,
			VHOST_CONSOLE_BOOT_LOG_FAILED);
	}
}

static void vhost_console_collect_boot_log(struct vhost_console_dev *dev,
	struct vhost_console_boot_log *boot_log)
{
	uint64_t rflags;

	if ((dev == NULL) || (boot_log == NULL)) {
		return;
	}

	(void)memset(boot_log, 0U, sizeof(*boot_log));
	vhost_console_lock_state(dev, &rflags);
	if ((dev->pending_boot_log_mask != 0U) && (dev->mmio.vm != NULL)) {
		boot_log->vm_id = dev->mmio.vm->vm_id;
		boot_log->phase_mask = dev->pending_boot_log_mask;
		boot_log->status = dev->mmio.status;
		dev->pending_boot_log_mask = 0U;
	}
	vhost_console_unlock_state(dev, rflags);
}

static void vhost_console_emit_boot_status(
	const struct vhost_console_boot_log *boot_log, bool passed)
{
	if (boot_log == NULL) {
		return;
	}

	LOG_INF("VM%hu:    [hvc0] console verification: %s", boot_log->vm_id,
		passed ? "OK" : "NO");
}

static void vhost_console_emit_boot_log(const struct vhost_console_boot_log *boot_log)
{
	if (boot_log == NULL) {
		return;
	}

	if ((boot_log->phase_mask & VHOST_CONSOLE_BOOT_LOG_FAILED) != 0U) {
		vhost_console_emit_boot_status(boot_log, false);
	} else if (((boot_log->phase_mask & VHOST_CONSOLE_BOOT_LOG_DRIVER_OK) != 0U) &&
		(boot_log->status == VHOST_CONSOLE_STATUS_OK)) {
		vhost_console_emit_boot_status(boot_log, true);
	}
}

static void vhost_console_reset(struct virtio_mmio_dev *mmio)
{
	struct vhost_console_dev *dev = (struct vhost_console_dev *)virtio_mmio_priv(mmio);

	if (dev != NULL) {
		dev->tx_count = 0UL;
		dev->rx_count = 0UL;
		dev->tx_notify_count = 0UL;
		dev->rx_notify_count = 0UL;
		dev->tx_irq_count = 0UL;
		dev->rx_irq_count = 0UL;
		dev->pending_queue_mask = 0U;
		dev->pending_boot_log_mask = 0U;
		dev->pass_reported = false;
		dev->fail_reported = false;
	}
}

static void vhost_console_raise_used_irq(struct vhost_console_dev *dev)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&dev->irq_lock, &rflags);
	virtio_mmio_raise_used_irq(&dev->mmio);
	spinlock_irqrestore_release(&dev->irq_lock, rflags);
}

static bool vhost_console_copy_tx_desc(struct virtio_mmio_dev *mmio,
	const struct virtio_ring_desc *desc, uint32_t *total)
{
	struct acrn_vm *vm = virtio_mmio_vm(mmio);
	char buf[VHOST_CONSOLE_COPY_BUF_SIZE];
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

static uint32_t vhost_console_handle_tx_chain(struct virtio_mmio_dev *mmio,
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
			ok = vhost_console_copy_tx_desc(mmio, &desc, &total);
			if (!ok) {
				break;
			}
		}
		nr_desc++;
		id = desc.next;
	} while (((desc.flags & VIRTIO_RING_F_NEXT) != 0U) &&
		(nr_desc < VHOST_CONSOLE_CHAIN_LIMIT));

	return ok ? total : 0U;
}

static void vhost_console_process_tx(struct vhost_console_dev *dev)
{
	struct virtio_mmio_dev *mmio = &dev->mmio;
	struct virtio_mmio_queue *vq = virtio_mmio_get_queue(mmio,
		VHOST_CONSOLE_QUEUE_TX);
	uint16_t head;
	uint32_t total;
	bool used = false;

	while (virtio_mmio_pop_avail(mmio, vq, &head)) {
		total = vhost_console_handle_tx_chain(mmio, vq, head);
		if (virtio_mmio_add_used(mmio, vq, head, total)) {
			vhost_console_add_u64(&dev->tx_count, total);
			used = true;
		}
	}

	if (used) {
		vhost_console_add_u64(&dev->tx_irq_count, 1UL);
		vhost_console_raise_used_irq(dev);
	}
}

static uint32_t vhost_console_fill_rx_desc(struct virtio_mmio_dev *mmio,
	const struct virtio_ring_desc *desc, bool *copy_failed)
{
	struct acrn_vm *vm = virtio_mmio_vm(mmio);
	struct acrn_vuart *console = vm_console_vuart(vm);
	uint32_t filled = 0U;
	char ch;

	if (copy_failed != NULL) {
		*copy_failed = false;
	}

	/* [20260808] Bounded virtio-console RX coalescing
	 *
	 * host input backlog (console.c budget)
	 *     -> one-byte vUART staging FIFO
	 *     -> writable guest RX descriptor
	 *     -> one used-ring completion
	 *
	 * Key rule:
	 *   - console.c owns input selection, ordering, and the current RX budget;
	 *   - this transport requests another staged byte only after draining the
	 *     previous one, so a guest descriptor cannot bypass that budget;
	 *   - a failed GPA write aborts the RX head chain without a used-ring update,
	 *     preventing a partially verified length from reaching the guest.
	 */
	while (filled < desc->len) {
		if (!vuart_rx_pending(console) && !console_vm_rx_refill(console)) {
			break;
		}
		ch = vuart_get_rx_char(console);
		if (ch == -1) {
			break;
		}
		if (!virtio_mmio_write_gpa(mmio, desc->addr + filled, &ch,
			sizeof(ch))) {
			if (copy_failed != NULL) {
				*copy_failed = true;
			}
			return 0U;
		}
		filled++;
	}

	return filled;
}

static uint32_t vhost_console_handle_rx_chain(struct virtio_mmio_dev *mmio,
	struct virtio_mmio_queue *vq, uint16_t head)
{
	struct acrn_vm *vm = virtio_mmio_vm(mmio);
	struct virtio_ring_desc desc;
	uint16_t id = head;
	uint32_t total = 0U;
	uint32_t nr_desc = 0U;
	uint32_t filled;
	bool copy_failed = false;

	do {
		if (!virtio_mmio_read_desc(mmio, vq, id, &desc)) {
			break;
		}
		if ((desc.flags & VIRTIO_RING_F_WRITE) != 0U) {
			filled = vhost_console_fill_rx_desc(mmio, &desc, &copy_failed);
			if (copy_failed) {
				return 0U;
			}
			total += filled;
			if ((filled < desc.len) || !vuart_rx_pending(vm_console_vuart(vm))) {
				break;
			}
		}
		nr_desc++;
		id = desc.next;
	} while (((desc.flags & VIRTIO_RING_F_NEXT) != 0U) &&
		(nr_desc < VHOST_CONSOLE_CHAIN_LIMIT));

	return total;
}

static void vhost_console_process_rx(struct vhost_console_dev *dev)
{
	struct virtio_mmio_dev *mmio = &dev->mmio;
	struct acrn_vuart *console = vm_console_vuart(virtio_mmio_vm(mmio));
	struct virtio_mmio_queue *vq = virtio_mmio_get_queue(mmio,
		VHOST_CONSOLE_QUEUE_RX);
	uint16_t head;
	uint32_t total;
	bool used = false;

	(void)console_vm_rx_refill(console);
	while (vuart_rx_pending(console) && virtio_mmio_pop_avail(mmio, vq, &head)) {
		total = vhost_console_handle_rx_chain(mmio, vq, head);
		if (total == 0U) {
			break;
		}
		if (virtio_mmio_add_used(mmio, vq, head, total)) {
			vhost_console_add_u64(&dev->rx_count, total);
			used = true;
			/* Keep a virtio-console RX burst within the host input budget. */
			(void)console_vm_rx_refill(console);
		}
	}

	if (used) {
		vhost_console_add_u64(&dev->rx_irq_count, 1UL);
		vhost_console_raise_used_irq(dev);
	}
}

static void vhost_console_dispatch_pending(struct vhost_console_dev *dev)
{
	uint32_t pending;
	uint64_t rflags;

	vhost_console_lock_state(dev, &rflags);
	pending = dev->pending_queue_mask;
	dev->pending_queue_mask = 0U;
	vhost_console_unlock_state(dev, rflags);

	if ((pending & VHOST_CONSOLE_PENDING_QUEUE(VHOST_CONSOLE_QUEUE_TX)) != 0U) {
		spinlock_irqsave_obtain(&dev->tx_lock, &rflags);
		vhost_console_process_tx(dev);
		spinlock_irqrestore_release(&dev->tx_lock, rflags);
	}
	if ((pending & VHOST_CONSOLE_PENDING_QUEUE(VHOST_CONSOLE_QUEUE_RX)) != 0U) {
		spinlock_irqsave_obtain(&dev->rx_lock, &rflags);
		vhost_console_process_rx(dev);
		spinlock_irqrestore_release(&dev->rx_lock, rflags);
	}
}

static void vhost_console_queue_ready(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct vhost_console_dev *dev = (struct vhost_console_dev *)virtio_mmio_priv(mmio);

	if ((dev != NULL) && (queue_id == VHOST_CONSOLE_QUEUE_RX)) {
		vhost_console_mark_pending(dev, queue_id);
	}
}

static void vhost_console_notify_queue(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct vhost_console_dev *dev = (struct vhost_console_dev *)virtio_mmio_priv(mmio);

	if (dev == NULL) {
		return;
	}
	if (queue_id == VHOST_CONSOLE_QUEUE_TX) {
		vhost_console_add_u64(&dev->tx_notify_count, 1UL);
		vhost_console_mark_pending(dev, queue_id);
	} else if (queue_id == VHOST_CONSOLE_QUEUE_RX) {
		vhost_console_add_u64(&dev->rx_notify_count, 1UL);
		vhost_console_mark_pending(dev, queue_id);
	}
}

static const struct virtio_mmio_ops vhost_console_mmio_ops = {
	.reset = vhost_console_reset,
	.queue_ready = vhost_console_queue_ready,
	.notify_queue = vhost_console_notify_queue,
};

static void vhost_console_notify_rx(struct acrn_vuart *console)
{
	struct vhost_console_dev *dev;
	uint64_t rflags;

	if (console == NULL) {
		return;
	}

	dev = vhost_console_get_dev(console->vm);

	if (dev != NULL) {
		vhost_console_lock_state(dev, &rflags);
		vhost_console_add_u64(&dev->rx_notify_count, 1UL);
		vhost_console_mark_pending(dev, VHOST_CONSOLE_QUEUE_RX);
		vhost_console_unlock_state(dev, rflags);
		vhost_console_dispatch_pending(dev);
	}
}

static const struct vuart_backend_ops vhost_console_backend_ops = {
	.notify_rx = vhost_console_notify_rx,
};

void vhost_console_init_vm(struct acrn_vm *vm)
{
	struct vhost_console_dev *dev = vhost_console_get_dev(vm);
	struct acrn_vuart *console;
	uint32_t irq = vhost_console_irq(vm);
	struct virtio_mmio_init init = {
		.name = "virtio-console",
		.vm = vm,
		.base = get_vm_config(vm->vm_id)->arch.guest_virtio_console_base,
		.size = get_vm_config(vm->vm_id)->arch.guest_virtio_console_size,
		.irq = irq,
		.device_id = VIRTIO_DEVICE_ID_CONSOLE,
		.queue_num = VHOST_CONSOLE_QUEUE_NUM,
		.queue_size = VHOST_CONSOLE_QUEUE_SIZE,
		.device_features = 0UL,
		.ops = &vhost_console_mmio_ops,
		.priv = dev,
	};

	if (dev != NULL) {
		spinlock_init(&dev->state_lock);
		spinlock_init(&dev->rx_lock);
		spinlock_init(&dev->tx_lock);
		spinlock_init(&dev->irq_lock);
		dev->pending_queue_mask = 0U;
		dev->pending_boot_log_mask = 0U;
		dev->pass_reported = false;
		dev->fail_reported = false;
		virtio_mmio_init(&dev->mmio, &init);
		init_console_vuart(vm, irq);
		console = vm_console_vuart(vm);
		vuart_set_backend(console, &vhost_console_backend_ops);
	}
}

void vhost_console_reset_vm(struct acrn_vm *vm)
{
	struct vhost_console_dev *dev = vhost_console_get_dev(vm);
	uint64_t state_rflags;
	uint64_t rx_rflags;
	uint64_t tx_rflags;
	uint64_t irq_rflags;

	if (dev != NULL) {
		vhost_console_lock_state(dev, &state_rflags);
		spinlock_irqsave_obtain(&dev->rx_lock, &rx_rflags);
		spinlock_irqsave_obtain(&dev->tx_lock, &tx_rflags);
		spinlock_irqsave_obtain(&dev->irq_lock, &irq_rflags);
		virtio_mmio_reset_dev(&dev->mmio);
		spinlock_irqrestore_release(&dev->irq_lock, irq_rflags);
		spinlock_irqrestore_release(&dev->tx_lock, tx_rflags);
		spinlock_irqrestore_release(&dev->rx_lock, rx_rflags);
		vhost_console_unlock_state(dev, state_rflags);
	}
}

bool vhost_console_get_stats(uint16_t vm_id, struct vhost_console_stats *stats)
{
	struct vhost_console_dev *dev;
	uint64_t state_rflags;
	uint64_t rx_rflags;
	uint64_t tx_rflags;
	uint64_t irq_rflags;
	bool active = false;

	if ((stats == NULL) || (vm_id >= CONFIG_MAX_VM_NUM)) {
		return false;
	}

	(void)memset(stats, 0U, sizeof(*stats));
	dev = &vhost_console_devs[vm_id];
	vhost_console_lock_state(dev, &state_rflags);
	spinlock_irqsave_obtain(&dev->rx_lock, &rx_rflags);
	spinlock_irqsave_obtain(&dev->tx_lock, &tx_rflags);
	spinlock_irqsave_obtain(&dev->irq_lock, &irq_rflags);
	if ((dev->mmio.vm == NULL) || (dev->mmio.size == 0UL)) {
		goto out;
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

	for (uint16_t i = 0U;
		(i < VHOST_CONSOLE_STAT_QUEUE_NUM) && (i < dev->mmio.queue_num); i++) {
		const struct virtio_mmio_queue *vq = &dev->mmio.queues[i];

		stats->queues[i].num = vq->num;
		stats->queues[i].last_avail_idx = vq->last_avail_idx;
		stats->queues[i].desc = vq->desc;
		stats->queues[i].avail = vq->avail;
		stats->queues[i].used = vq->used;
		stats->queues[i].ready = vq->ready;
	}

	active = true;

out:
	spinlock_irqrestore_release(&dev->irq_lock, irq_rflags);
	spinlock_irqrestore_release(&dev->tx_lock, tx_rflags);
	spinlock_irqrestore_release(&dev->rx_lock, rx_rflags);
	vhost_console_unlock_state(dev, state_rflags);
	return active;
}

int32_t vhost_console_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct vhost_console_dev *dev = vhost_console_get_dev(vm);
	uint64_t state_rflags;
	uint64_t rx_rflags;
	uint64_t tx_rflags;
	uint64_t irq_rflags;
	uint32_t access_flags = 0U;
	bool queue_locked = false;
	bool irq_locked = false;
	struct vhost_console_boot_log boot_log = { 0U };
	int32_t ret = -EINVAL;

	if (dev != NULL) {
		access_flags = virtio_mmio_access_flags(io_req, &dev->mmio);
		vhost_console_lock_state(dev, &state_rflags);
		if ((access_flags & (VIRTIO_MMIO_ACCESS_QUEUE_CONFIG |
			VIRTIO_MMIO_ACCESS_RESET)) != 0U) {
			spinlock_irqsave_obtain(&dev->rx_lock, &rx_rflags);
			spinlock_irqsave_obtain(&dev->tx_lock, &tx_rflags);
			queue_locked = true;
		}
		if ((access_flags & (VIRTIO_MMIO_ACCESS_IRQ_ACK |
			VIRTIO_MMIO_ACCESS_RESET)) != 0U) {
			spinlock_irqsave_obtain(&dev->irq_lock, &irq_rflags);
			irq_locked = true;
		}
		ret = virtio_mmio_handler(io_req, &dev->mmio);
		vhost_console_record_status(dev);
		if (irq_locked) {
			spinlock_irqrestore_release(&dev->irq_lock, irq_rflags);
		}
		if (queue_locked) {
			spinlock_irqrestore_release(&dev->tx_lock, tx_rflags);
			spinlock_irqrestore_release(&dev->rx_lock, rx_rflags);
		}
		vhost_console_unlock_state(dev, state_rflags);
		vhost_console_collect_boot_log(dev, &boot_log);
		vhost_console_emit_boot_log(&boot_log);
		vhost_console_dispatch_pending(dev);
	}

	return ret;
}
