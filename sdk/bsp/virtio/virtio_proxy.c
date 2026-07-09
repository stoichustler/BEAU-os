/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <acrn_hv_defs.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <guest_memory.h>
#include <logmsg.h>
#include <rtl.h>
#include <bsp/io_req.h>
#include <spinlock.h>
#include <ticks.h>
#include "virtio_proxy.h"

static struct virtio_proxy_dev virtio_proxy_devs[CONFIG_MAX_VM_NUM][ARM64_VIRTIO_PROXY_MAX];

static struct virtio_proxy_dev *virtio_proxy_get_dev_by_id_index(uint16_t vm_id,
	uint16_t index)
{
	return (vm_id < CONFIG_MAX_VM_NUM) && (index < ARM64_VIRTIO_PROXY_MAX) ?
		&virtio_proxy_devs[vm_id][index] : NULL;
}

struct virtio_proxy_dev *virtio_proxy_get_dev(struct acrn_vm *vm, uint16_t index)
{
	return (vm != NULL) ? virtio_proxy_get_dev_by_id_index(vm->vm_id, index) : NULL;
}

static struct virtio_proxy_dev *virtio_proxy_get_first_dev_by_id(uint16_t vm_id)
{
	return virtio_proxy_get_dev_by_id_index(vm_id, 0U);
}

static bool virtio_proxy_dev_matches(const struct virtio_proxy_dev *dev,
	uint32_t device_id)
{
	return (dev != NULL) && (dev->mmio.vm != NULL) &&
		((device_id == 0U) || (dev->device_id == device_id));
}

static struct virtio_proxy_dev *virtio_proxy_find_dev_by_device_id(uint16_t vm_id,
	uint32_t device_id)
{
	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return NULL;
	}

	for (uint16_t i = 0U; i < ARM64_VIRTIO_PROXY_MAX; i++) {
		struct virtio_proxy_dev *candidate =
			virtio_proxy_get_dev_by_id_index(vm_id, i);

		if (virtio_proxy_dev_matches(candidate, device_id)) {
			return candidate;
		}
	}

	return NULL;
}

static struct virtio_proxy_dev *virtio_proxy_find_hcall_target(uint16_t backend_vmid,
	uint16_t frontend_vmid, uint32_t device_id)
{
	struct virtio_proxy_dev *dev = NULL;

	if (frontend_vmid < CONFIG_MAX_VM_NUM) {
		dev = virtio_proxy_find_dev_by_device_id(frontend_vmid, device_id);
		if ((dev == NULL) || !dev->hcall_backend_registered ||
			(dev->backend_vmid != backend_vmid)) {
			dev = NULL;
		}
	} else {
		for (uint16_t i = 0U; i < CONFIG_MAX_VM_NUM; i++) {
			for (uint16_t j = 0U; j < ARM64_VIRTIO_PROXY_MAX; j++) {
				struct virtio_proxy_dev *candidate =
					virtio_proxy_get_dev_by_id_index(i, j);

				if (virtio_proxy_dev_matches(candidate, device_id) &&
					candidate->hcall_backend_registered &&
					(candidate->backend_vmid == backend_vmid)) {
					dev = candidate;
					break;
				}
			}
			if (dev != NULL) {
				break;
			}
		}
	}

	return dev;
}

static struct virtio_proxy_dev *virtio_proxy_hcall_stat_target(uint16_t backend_vmid,
	const struct acrn_virtio_proxy_ioc *ioc)
{
	struct virtio_proxy_dev *dev = NULL;

	if (ioc != NULL) {
		dev = (ioc->op == ACRN_VIRTIO_PROXY_OP_REGISTER) ?
			virtio_proxy_find_dev_by_device_id(ioc->frontend_vmid,
				ioc->device_id) :
			virtio_proxy_find_hcall_target(backend_vmid, ioc->frontend_vmid,
				ioc->device_id);
	}

	return dev;
}

static void virtio_proxy_record_hcall_result(uint16_t backend_vmid,
	const struct acrn_virtio_proxy_ioc *ioc, int32_t ret)
{
	struct virtio_proxy_dev *dev = virtio_proxy_hcall_stat_target(backend_vmid, ioc);

	if (dev != NULL) {
		spinlock_obtain(&dev->lock);
		dev->last_hcall_op = ioc->op;
		dev->last_hcall_ret = ret;
		spinlock_release(&dev->lock);
	}
}

static uint16_t virtio_proxy_pending_active(const struct virtio_proxy_dev *dev)
{
	uint16_t active = 0U;

	if (dev == NULL) {
		return 0U;
	}

	for (uint16_t i = 0U; i < dev->pending_limit; i++) {
		if (dev->pending[i].valid) {
			active++;
		}
	}

	return active;
}

static void virtio_proxy_latency_accum(struct virtio_proxy_latency_accum *stats,
	uint64_t delta)
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
	if (stats->sum > (UINT64_MAX - delta)) {
		stats->sum = UINT64_MAX;
	} else {
		stats->sum += delta;
	}
	stats->count++;
}

static void virtio_proxy_latency_export(
	const struct virtio_proxy_latency_accum *src,
	struct virtio_proxy_latency_stats *dst)
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

static void virtio_proxy_latency_complete_locked(struct virtio_proxy_dev *dev,
	const struct virtio_proxy_pending *pending)
{
	if ((dev == NULL) || (pending == NULL) ||
		(pending->notify_tick == 0UL) || (pending->poll_tick == 0UL) ||
		(pending->reply_tick == 0UL) || (pending->irq_tick == 0UL)) {
		return;
	}

	virtio_proxy_latency_accum(&dev->latency_notify_poll,
		pending->poll_tick - pending->notify_tick);
	virtio_proxy_latency_accum(&dev->latency_poll_reply,
		pending->reply_tick - pending->poll_tick);
	virtio_proxy_latency_accum(&dev->latency_reply_irq,
		pending->irq_tick - pending->reply_tick);
	virtio_proxy_latency_accum(&dev->latency_total,
		pending->irq_tick - pending->notify_tick);
}

static void virtio_proxy_check_timeout_locked(struct virtio_proxy_dev *dev,
	struct virtio_proxy_pending *pending, uint64_t now)
{
	uint64_t timeout_ticks = us_to_ticks(VIRTIO_PROXY_TIMEOUT_US);

	if ((dev != NULL) && (pending != NULL) && pending->valid &&
		pending->sent && !pending->done && !pending->timeout_reported &&
		(pending->poll_tick != 0UL) &&
		((now - pending->poll_tick) > timeout_ticks)) {
		dev->timeout_count++;
		pending->timeout_reported = true;
	}
}

static void virtio_proxy_check_timeouts_locked(struct virtio_proxy_dev *dev,
	uint64_t now)
{
	if (dev == NULL) {
		return;
	}

	for (uint16_t i = 0U; i < dev->pending_limit; i++) {
		virtio_proxy_check_timeout_locked(dev, &dev->pending[i], now);
	}
}

static bool virtio_proxy_frontend_ready(const struct virtio_proxy_dev *dev)
{
	bool ready = false;

	if (dev != NULL) {
		for (uint16_t i = 0U; i < dev->mmio.queue_num; i++) {
			if (dev->mmio.queues[i].ready) {
				ready = true;
				break;
			}
		}
	}

	return ready;
}

static void virtio_proxy_update_state_locked(struct virtio_proxy_dev *dev)
{
	bool frontend_ready;
	bool backend_ready;

	if (dev == NULL) {
		return;
	}

	frontend_ready = virtio_proxy_frontend_ready(dev);
	backend_ready = dev->hcall_backend_registered || (dev->backend_ops != NULL);
	if (frontend_ready && backend_ready) {
		dev->state = VIRTIO_PROXY_STATE_RUNNING;
	} else if (backend_ready) {
		dev->state = VIRTIO_PROXY_STATE_BACKEND_READY;
	} else if (frontend_ready) {
		dev->state = VIRTIO_PROXY_STATE_FRONTEND_READY;
	} else {
		dev->state = VIRTIO_PROXY_STATE_WAIT_BACKEND;
	}
}

static void virtio_proxy_drop_backend_locked(struct virtio_proxy_dev *dev,
	int32_t reason)
{
	if (dev == NULL) {
		return;
	}

	dev->hcall_backend_registered = false;
	dev->backend_vmid = ACRN_INVALID_VMID;
	dev->no_backend_logged = false;
	for (uint16_t i = 0U; i < dev->pending_limit; i++) {
		if (dev->pending[i].valid && dev->pending[i].sent &&
			!dev->pending[i].done) {
			dev->pending[i].sent = false;
		}
	}
	dev->last_hcall_ret = reason;
	dev->state = VIRTIO_PROXY_STATE_BACKEND_LOST;
}

static bool virtio_proxy_enabled(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_proxy_num != 0U);
}

static void virtio_proxy_build_config(struct virtio_proxy_dev *dev,
	const struct arm64_virtio_proxy_config *proxy_config)
{
	struct virtio_proxy_fs_config fs_config;
	struct virtio_proxy_blk_config blk_config;

	(void)memset(dev->config, 0U, sizeof(dev->config));
	dev->config_size = 0U;

	/*
	 * 2026-07-08, virtio-proxy config principle:
	 *
	 * DTS chooses which frontend driver the guest will bind by selecting a
	 * virtio device id. Config space is then just bytes carried by the
	 * transport. A backend can replace these bytes via
	 * virtio_proxy_set_config() or serve config reads directly through
	 * backend_ops->read_config().
	 *
	 * The default QEMU scenario still advertises device-id 26, so Linux reads
	 * the standard virtio-fs tag layout. That is compatibility glue only; FUSE
	 * request handling still belongs to an fs backend.
	 *
	 *   DTS device-id/tag -> default config bytes -> frontend probe
	 *                               |
	 *                               +--> backend may override before use
	 */
	if (dev->device_id == VIRTIO_DEVICE_ID_FS) {
		(void)memset(&fs_config, 0U, sizeof(fs_config));
		(void)strncpy_s(fs_config.tag, sizeof(fs_config.tag),
			dev->tag,
			sizeof(fs_config.tag) - 1U);
		fs_config.num_request_queues =
			proxy_config->queue_num > 1U ?
			(uint32_t)proxy_config->queue_num - 1U : 1U;
		(void)memcpy(dev->config, &fs_config, sizeof(fs_config));
		dev->config_size = sizeof(fs_config);
	} else if (dev->device_id == VIRTIO_DEVICE_ID_BLOCK) {
		(void)memset(&blk_config, 0U, sizeof(blk_config));
		blk_config.capacity = VIRTIO_PROXY_BLK_CAPACITY;
		blk_config.size_max = VIRTIO_PROXY_BLK_SIZE_MAX;
		blk_config.seg_max = VIRTIO_PROXY_BLK_SEG_MAX;
		(void)memcpy(dev->config, &blk_config, sizeof(blk_config));
		dev->config_size = sizeof(blk_config);
	}
}

static void virtio_proxy_reset(struct virtio_mmio_dev *mmio)
{
	struct virtio_proxy_dev *dev =
		(struct virtio_proxy_dev *)virtio_mmio_priv(mmio);

	if (dev != NULL) {
		/*
		 * A VM reset drops guest-visible transport state first in
		 * virtio_mmio_reset_dev(). The backend reset callback is the matching
		 * protocol-side boundary: cancel outstanding host work, release any
		 * in-flight descriptor references, and be ready for the frontend to
		 * negotiate again from status 0.
		 */
		dev->no_backend_logged = false;
		spinlock_obtain(&dev->lock);
		dev->notify_count = 0UL;
		dev->reset_count++;
		(void)memset(dev->last_notify_tick, 0U, sizeof(dev->last_notify_tick));
		(void)memset(dev->pending, 0U, sizeof(dev->pending));
		dev->last_hcall_ret = 0;
		dev->last_poll_status = 0U;
		virtio_proxy_update_state_locked(dev);
		spinlock_release(&dev->lock);
		if ((dev->backend_ops != NULL) && (dev->backend_ops->reset != NULL)) {
			dev->backend_ops->reset(dev, dev->backend_priv);
		}
	}
}

static void virtio_proxy_queue_ready(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct virtio_proxy_dev *dev =
		(struct virtio_proxy_dev *)virtio_mmio_priv(mmio);

	/*
	 * Queue-ready is the earliest point where descriptor, avail, and used
	 * ring GPAs are valid. A backend that needs per-queue state should cache
	 * only metadata here; descriptor chains are owned by the frontend until a
	 * later queue notify.
	 */
	if ((dev != NULL) && (dev->backend_ops != NULL) &&
		(dev->backend_ops->queue_ready != NULL)) {
		dev->backend_ops->queue_ready(dev, queue_id, dev->backend_priv);
	}
	if (dev != NULL) {
		spinlock_obtain(&dev->lock);
		virtio_proxy_update_state_locked(dev);
		spinlock_release(&dev->lock);
	}
}

static uint32_t virtio_proxy_read_config(struct virtio_mmio_dev *mmio,
	uint32_t offset, uint32_t size)
{
	struct virtio_proxy_dev *dev =
		(struct virtio_proxy_dev *)virtio_mmio_priv(mmio);
	uint32_t value = 0U;
	uint32_t i;

	if ((dev == NULL) || (size > sizeof(value))) {
		return 0U;
	}
	/*
	 * Backends get first refusal on config space because some protocols expose
	 * live state there: net MAC/status, block capacity, or controller-specific
	 * I2C/SPI limits. The local byte buffer is only a static fallback.
	 */
	if ((dev->backend_ops != NULL) && (dev->backend_ops->read_config != NULL)) {
		return dev->backend_ops->read_config(dev, offset, size, dev->backend_priv);
	}
	if ((offset > dev->config_size) || (size > (dev->config_size - offset))) {
		return 0U;
	}

	for (i = 0U; i < size; i++) {
		value |= ((uint32_t)dev->config[offset + i]) << (i * 8U);
	}

	return value;
}

static void virtio_proxy_write_config(struct virtio_mmio_dev *mmio,
	uint32_t offset, uint32_t size, uint32_t value)
{
	struct virtio_proxy_dev *dev =
		(struct virtio_proxy_dev *)virtio_mmio_priv(mmio);

	if ((dev != NULL) && (dev->backend_ops != NULL) &&
		(dev->backend_ops->write_config != NULL)) {
		/*
		 * Config writes are protocol-defined. The proxy deliberately avoids
		 * decoding them, because a write may mean queue-pair control for one
		 * class and be invalid or reserved for another.
		 */
		dev->backend_ops->write_config(dev, offset, size, value,
			dev->backend_priv);
	}
}

static void virtio_proxy_notify_queue(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct virtio_proxy_dev *dev =
		(struct virtio_proxy_dev *)virtio_mmio_priv(mmio);

	if ((dev == NULL) || (queue_id >= mmio->queue_num)) {
		return;
	}

	spinlock_obtain(&dev->lock);
	dev->notify_count++;
	dev->last_notify_tick[queue_id] = cpu_ticks();
	spinlock_release(&dev->lock);
	if ((dev->backend_ops != NULL) && (dev->backend_ops->notify_queue != NULL)) {
		/*
		 * Notify is the main bridge handoff:
		 *
		 *   frontend writes QueueNotify
		 *        -> virtio_mmio traps to BEAU
		 *        -> proxy identifies vm/queue/device-id
		 *        -> backend parses descriptor chains with virtio_mmio helpers
		 *        -> backend writes used ring and raises IRQ when complete
		 *
		 * For blk/net/fs/i2c/spi, only the backend knows which queue IDs are
		 * control queues, request queues, RX/TX queues, or event queues.
		 */
		dev->backend_ops->notify_queue(dev, queue_id, dev->backend_priv);
		return;
	}
	if (dev->hcall_backend_registered) {
		/*
		 * A registered VM backend consumes the descriptor chain through
		 * HC_VIRTIO_PROXY_BACKEND. Do not pop the avail ring here; the poll
		 * path owns that transition so it can atomically snapshot one chain.
		 */
		return;
	}
	if (dev->hcall_backend_expected) {
		/*
		 * A frontend may notify before the backend VM reaches its BEAU
		 * late_initcall and registers through HC_VIRTIO_PROXY_BACKEND.
		 * Keep the avail ring untouched; the first backend poll after
		 * registration will consume the same descriptor chain.
		 */
		return;
	}

	/*
	 * 2026-07-08, transport-only failure mode:
	 *
	 * No protocol backend means no owner can safely consume avail entries or
	 * produce used entries. Leave the vring untouched; a later backend can
	 * bind and process the same guest-visible state without BEAU inventing fs,
	 * blk, net, or other protocol semantics in the transport layer.
	 *
	 *   guest notify -> proxy transport
	 *                      |
	 *                      +--> backend? yes: dispatch
	 *                      |
	 *                      +--> backend? no : keep avail ring intact
	 */
	if (!dev->no_backend_logged) {
		LOG_WRN("vm%u virtio-proxy device %u queue%u notify without backend",
			mmio->vm->vm_id, dev->device_id, queue_id);
		dev->no_backend_logged = true;
	}
}

static const struct virtio_mmio_ops virtio_proxy_mmio_ops = {
	.reset = virtio_proxy_reset,
	.queue_ready = virtio_proxy_queue_ready,
	.notify_queue = virtio_proxy_notify_queue,
	.read_config = virtio_proxy_read_config,
	.write_config = virtio_proxy_write_config,
};

void virtio_proxy_init_vm(struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	if (!virtio_proxy_enabled(vm_config)) {
		return;
	}

	for (uint16_t i = 0U; i < vm_config->arch.guest_virtio_proxy_num; i++) {
		const struct arm64_virtio_proxy_config *proxy_config =
			&vm_config->arch.guest_virtio_proxy[i];
		struct virtio_proxy_dev *dev = virtio_proxy_get_dev(vm, i);
		struct virtio_mmio_init init;

		if (dev == NULL) {
			continue;
		}

		(void)memset(dev, 0U, sizeof(*dev));
		dev->index = i;
		dev->device_id = proxy_config->device_id;
		dev->access = proxy_config->access;
		dev->throughput = proxy_config->throughput;
		dev->hcall_backend_expected = true;
		dev->backend_vmid = ACRN_INVALID_VMID;
		dev->state = VIRTIO_PROXY_STATE_WAIT_BACKEND;
		dev->pending_limit =
			(dev->throughput == VIRTIO_PROXY_THROUGHPUT_HIGH) ?
			VIRTIO_PROXY_PENDING_HIGH : VIRTIO_PROXY_PENDING_LOW;
		if (dev->pending_limit > ARRAY_SIZE(dev->pending)) {
			dev->pending_limit = ARRAY_SIZE(dev->pending);
		}
		(void)strncpy_s(dev->tag, sizeof(dev->tag),
			proxy_config->tag, sizeof(dev->tag) - 1U);
		virtio_proxy_build_config(dev, proxy_config);

		/*
		 * Access policy is transport metadata. BEAU records the board-selected
		 * read-only versus read-write hint here, but enforcement belongs to the
		 * backend that knows the protocol operation being served.
		 */
		init.name = "virtio-proxy";
		init.vm = vm;
		init.base = proxy_config->base;
		init.size = proxy_config->size;
		init.irq = proxy_config->irq;
		init.device_id = dev->device_id;
		init.queue_num = proxy_config->queue_num;
		init.queue_size = proxy_config->queue_size;
		init.device_features =
			(dev->device_id == VIRTIO_DEVICE_ID_BLOCK) ?
			((1UL << VIRTIO_BLK_F_SIZE_MAX) |
			(1UL << VIRTIO_BLK_F_SEG_MAX)) : 0UL;
		init.ops = &virtio_proxy_mmio_ops;
		init.priv = dev;
		virtio_mmio_init(&dev->mmio, &init);
		spinlock_init(&dev->lock);
	}
}

void virtio_proxy_reset_vm(struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	for (uint16_t i = 0U; i < vm_config->arch.guest_virtio_proxy_num; i++) {
		struct virtio_proxy_dev *dev = virtio_proxy_get_dev(vm, i);

		if (dev != NULL) {
			virtio_mmio_reset_dev(&dev->mmio);
		}
	}
}

void virtio_proxy_release_vm(struct acrn_vm *vm)
{
	if (vm == NULL) {
		return;
	}

	for (uint16_t vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		for (uint16_t index = 0U; index < ARM64_VIRTIO_PROXY_MAX; index++) {
			struct virtio_proxy_dev *dev =
				virtio_proxy_get_dev_by_id_index(vm_id, index);

			if ((dev == NULL) || (dev->mmio.vm == NULL)) {
				continue;
			}

			spinlock_obtain(&dev->lock);
			if (dev->hcall_backend_registered &&
				(dev->backend_vmid == vm->vm_id)) {
				virtio_proxy_drop_backend_locked(dev, -ENODEV);
			}
			spinlock_release(&dev->lock);
		}
	}
}

int32_t virtio_proxy_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct virtio_proxy_dev *dev = (struct virtio_proxy_dev *)handler_private_data;

	return dev != NULL ? virtio_mmio_handler(io_req, &dev->mmio) : -EINVAL;
}

int32_t virtio_proxy_bind_backend(uint16_t vm_id,
	const struct virtio_proxy_backend_ops *ops, void *priv)
{
	struct virtio_proxy_dev *dev = virtio_proxy_get_first_dev_by_id(vm_id);

	if ((dev == NULL) || (ops == NULL)) {
		return -EINVAL;
	}
	if (dev->mmio.vm == NULL) {
		return -ENODEV;
	}
	if (dev->backend_ops != NULL) {
		return -EBUSY;
	}

	/*
	 * Binding is one-to-one per frontend VM device. If a future topology needs
	 * multiple devices per VM, the lookup key should become (vm_id, base) or
	 * an explicit proxy handle. For the current static VM layout there is one
	 * virtio_proxy MMIO window per Linux VM, so vm_id is sufficient.
	 */
	dev->backend_ops = ops;
	dev->backend_priv = priv;
	dev->no_backend_logged = false;
	spinlock_obtain(&dev->lock);
	virtio_proxy_update_state_locked(dev);
	spinlock_release(&dev->lock);
	return 0;
}

void virtio_proxy_unbind_backend(uint16_t vm_id,
	const struct virtio_proxy_backend_ops *ops, void *priv)
{
	struct virtio_proxy_dev *dev = virtio_proxy_get_first_dev_by_id(vm_id);

	if ((dev != NULL) && (dev->backend_ops == ops) && (dev->backend_priv == priv)) {
		dev->backend_ops = NULL;
		dev->backend_priv = NULL;
		spinlock_obtain(&dev->lock);
		virtio_proxy_update_state_locked(dev);
		spinlock_release(&dev->lock);
	}
}

struct virtio_mmio_dev *virtio_proxy_mmio(struct virtio_proxy_dev *proxy)
{
	return proxy != NULL ? &proxy->mmio : NULL;
}

struct virtio_mmio_queue *virtio_proxy_get_queue(struct virtio_proxy_dev *proxy,
	uint16_t queue_id)
{
	return proxy != NULL ? virtio_mmio_get_queue(&proxy->mmio, queue_id) : NULL;
}

uint16_t virtio_proxy_frontend_vmid(const struct virtio_proxy_dev *proxy)
{
	return (proxy != NULL) && (proxy->mmio.vm != NULL) ? proxy->mmio.vm->vm_id :
		ACRN_INVALID_VMID;
}

uint32_t virtio_proxy_device_id(const struct virtio_proxy_dev *proxy)
{
	return proxy != NULL ? proxy->device_id : 0U;
}

uint32_t virtio_proxy_access(const struct virtio_proxy_dev *proxy)
{
	return proxy != NULL ? proxy->access : VIRTIO_PROXY_ACCESS_READONLY;
}

const char *virtio_proxy_tag(const struct virtio_proxy_dev *proxy)
{
	return proxy != NULL ? proxy->tag : "";
}

bool virtio_proxy_backend_ready(const struct virtio_proxy_dev *proxy)
{
	return (proxy != NULL) &&
		(proxy->hcall_backend_registered || (proxy->backend_ops != NULL));
}

bool virtio_proxy_set_config(struct virtio_proxy_dev *proxy, const void *config,
	uint32_t size)
{
	bool ret = false;

	if ((proxy != NULL) && (size <= sizeof(proxy->config)) &&
		((size == 0U) || (config != NULL))) {
		(void)memset(proxy->config, 0U, sizeof(proxy->config));
		if (size != 0U) {
			(void)memcpy(proxy->config, config, size);
		}
		proxy->config_size = size;
		ret = true;
	}

	return ret;
}

bool virtio_proxy_set_device_features(struct virtio_proxy_dev *proxy,
	uint64_t features)
{
	bool ret = false;

	if (proxy != NULL) {
		/*
		 * Device features are part of frontend/backend contract, not part of
		 * the proxy policy. Preserve VIRTIO_F_VERSION_1 because
		 * virtio_mmio_init() always advertises the modern virtio-mmio
		 * transport. Backend-provided bits describe protocol capabilities:
		 * block flush/discard, net checksum/MQ, fs notification, I2C/SPI mode
		 * flags, and similar class-specific options.
		 */
		proxy->mmio.device_features = features | (1ULL << VIRTIO_F_VERSION_1);
		ret = true;
	}

	return ret;
}

uint16_t virtio_proxy_device_count(uint16_t vm_id)
{
	const struct acrn_vm_config *vm_config;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		return 0U;
	}

	vm_config = get_vm_config(vm_id);
	return virtio_proxy_enabled(vm_config) ? vm_config->arch.guest_virtio_proxy_num : 0U;
}

bool virtio_proxy_get_stats(uint16_t vm_id, uint16_t index,
	struct virtio_proxy_stats *stats)
{
	struct virtio_proxy_dev *dev = virtio_proxy_get_dev_by_id_index(vm_id, index);
	const struct virtio_proxy_pending *pending = NULL;
	bool ret = false;

	if ((dev != NULL) && (stats != NULL) && (dev->mmio.vm != NULL)) {
		(void)memset(stats, 0U, sizeof(*stats));
		spinlock_obtain(&dev->lock);
		for (uint16_t i = 0U; i < dev->pending_limit; i++) {
			if (dev->pending[i].valid || dev->pending[i].done) {
				pending = &dev->pending[i];
				break;
			}
		}
		stats->vm_id = vm_id;
		stats->index = dev->index;
		stats->device_id = dev->device_id;
		stats->access = dev->access;
		stats->throughput = dev->throughput;
		stats->state = dev->state;
		(void)strncpy_s(stats->tag, sizeof(stats->tag), dev->tag,
			sizeof(stats->tag) - 1U);
		stats->base = dev->mmio.base;
		stats->size = dev->mmio.size;
		stats->irq = dev->mmio.irq;
		stats->status = dev->mmio.status;
		stats->interrupt_status = dev->mmio.interrupt_status;
		stats->queue_num = dev->mmio.queue_num;
		stats->queue_size = dev->mmio.queue_size;
		stats->notify_count = dev->notify_count;
		stats->backend_bound = dev->backend_ops != NULL;
		stats->hcall_backend_registered = dev->hcall_backend_registered;
		stats->backend_vmid = dev->backend_vmid;
		stats->pending_limit = dev->pending_limit;
		stats->pending_active = virtio_proxy_pending_active(dev);
		if (pending != NULL) {
			stats->pending_valid = pending->valid;
			stats->pending_sent = pending->sent;
			stats->pending_done = pending->done;
			stats->pending_queue_id = pending->queue_id;
			stats->pending_head = pending->head;
			stats->pending_in_len = pending->in_len;
			stats->pending_out_len = pending->out_len;
			stats->pending_out_count = pending->out_count;
		}
		stats->hcall_register_count = dev->hcall_register_count;
		stats->hcall_poll_count = dev->hcall_poll_count;
		stats->hcall_poll_ok_count = dev->hcall_poll_ok_count;
		stats->hcall_reply_count = dev->hcall_reply_count;
		stats->hcall_reply_ok_count = dev->hcall_reply_ok_count;
		stats->hcall_busy_count = dev->hcall_busy_count;
		stats->hcall_backpressure_count = dev->hcall_backpressure_count;
		stats->timeout_count = dev->timeout_count;
		stats->reset_count = dev->reset_count;
		virtio_proxy_latency_export(&dev->latency_notify_poll,
			&stats->latency_notify_poll);
		virtio_proxy_latency_export(&dev->latency_poll_reply,
			&stats->latency_poll_reply);
		virtio_proxy_latency_export(&dev->latency_reply_irq,
			&stats->latency_reply_irq);
		virtio_proxy_latency_export(&dev->latency_total,
			&stats->latency_total);
		stats->last_hcall_op = dev->last_hcall_op;
		stats->last_hcall_ret = dev->last_hcall_ret;
		stats->last_poll_queue_id = dev->last_poll_queue_id;
		stats->last_poll_head = dev->last_poll_head;
		stats->last_poll_status = dev->last_poll_status;
		stats->last_reply_queue_id = dev->last_reply_queue_id;
		stats->last_reply_head = dev->last_reply_head;
		stats->last_reply_len = dev->last_reply_len;
		for (uint16_t i = 0U; i < dev->mmio.queue_num; i++) {
			const struct virtio_mmio_queue *vq = &dev->mmio.queues[i];

			stats->queues[i].num = vq->num;
			stats->queues[i].last_avail_idx = vq->last_avail_idx;
			stats->queues[i].desc = vq->desc;
			stats->queues[i].avail = vq->avail;
			stats->queues[i].used = vq->used;
			stats->queues[i].ready = vq->ready;
		}
		spinlock_release(&dev->lock);
		ret = true;
	}

	return ret;
}

static struct virtio_proxy_pending *virtio_proxy_free_pending_slot(
	struct virtio_proxy_dev *dev)
{
	if (dev == NULL) {
		return NULL;
	}

	for (uint16_t i = 0U; i < dev->pending_limit; i++) {
		if (!dev->pending[i].valid && !dev->pending[i].sent) {
			return &dev->pending[i];
		}
	}

	return NULL;
}

static struct virtio_proxy_pending *virtio_proxy_unsent_pending_slot(
	struct virtio_proxy_dev *dev, uint16_t queue_id)
{
	if (dev == NULL) {
		return NULL;
	}

	for (uint16_t i = 0U; i < dev->pending_limit; i++) {
		if (dev->pending[i].valid && !dev->pending[i].sent &&
			(dev->pending[i].queue_id == queue_id)) {
			return &dev->pending[i];
		}
	}

	return NULL;
}

static struct virtio_proxy_pending *virtio_proxy_sent_pending_slot(
	struct virtio_proxy_dev *dev, uint16_t queue_id, uint16_t head)
{
	if (dev == NULL) {
		return NULL;
	}

	for (uint16_t i = 0U; i < dev->pending_limit; i++) {
		if (dev->pending[i].valid && dev->pending[i].sent &&
			(dev->pending[i].queue_id == queue_id) &&
			(dev->pending[i].head == head)) {
			return &dev->pending[i];
		}
	}

	return NULL;
}

static bool virtio_proxy_copy_chain_to_pending(struct virtio_proxy_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t queue_id, uint16_t head,
	struct virtio_proxy_pending *pending)
{
	struct virtio_ring_desc desc;
	uint16_t id = head;
	uint16_t nr_desc = 0U;
	uint32_t in_len = 0U;
	uint16_t out_count = 0U;
	bool ok = true;

	if (pending == NULL) {
		return false;
	}

	(void)memset(pending, 0U, sizeof(*pending));
	while (nr_desc < VIRTIO_PROXY_CHAIN_LIMIT) {
		if (!virtio_mmio_read_desc(&dev->mmio, vq, id, &desc)) {
			ok = false;
			break;
		}
		if ((desc.flags & VIRTIO_RING_F_WRITE) != 0U) {
			if (out_count >= ARRAY_SIZE(pending->out)) {
				ok = false;
				break;
			}
			pending->out[out_count].gpa = desc.addr;
			pending->out[out_count].len = desc.len;
			out_count++;
			if (pending->out_len > (UINT32_MAX - desc.len)) {
				ok = false;
				break;
			}
			pending->out_len += desc.len;
		} else {
			if ((desc.len > ACRN_VIRTIO_PROXY_DATA_MAX) ||
				(in_len > (ACRN_VIRTIO_PROXY_DATA_MAX - desc.len))) {
				ok = false;
				break;
			}
			if (!virtio_mmio_read_gpa(&dev->mmio, desc.addr,
				&pending->in[in_len], desc.len)) {
				ok = false;
				break;
			}
			in_len += desc.len;
		}
		nr_desc++;
		if ((desc.flags & VIRTIO_RING_F_NEXT) == 0U) {
			break;
		}
		id = desc.next;
	}

	if (ok && (nr_desc != 0U)) {
		pending->queue_id = queue_id;
		pending->head = head;
		pending->in_len = (uint16_t)in_len;
		pending->out_count = out_count;
		pending->valid = true;
		pending->sent = false;
		pending->done = false;
	} else {
		(void)memset(pending, 0U, sizeof(*pending));
	}

	return ok && (nr_desc != 0U);
}

static int32_t virtio_proxy_hcall_register(struct acrn_vcpu *vcpu,
	struct acrn_virtio_proxy_ioc *ioc)
{
	struct virtio_proxy_dev *dev = virtio_proxy_find_dev_by_device_id(
		ioc->frontend_vmid, ioc->device_id);
	int32_t ret = -ENODEV;

	if ((dev != NULL) && (dev->mmio.vm != NULL) &&
		(dev->mmio.vm->vm_id != vcpu->vm->vm_id)) {
		spinlock_obtain(&dev->lock);
		dev->hcall_register_count++;
		if (dev->hcall_backend_registered &&
			(dev->backend_vmid != vcpu->vm->vm_id)) {
			ret = -EBUSY;
		} else {
			dev->hcall_backend_registered = true;
			dev->backend_vmid = vcpu->vm->vm_id;
			dev->no_backend_logged = false;
			virtio_proxy_update_state_locked(dev);
			ret = 0;
		}
		spinlock_release(&dev->lock);
	}

	return ret;
}

static int32_t virtio_proxy_hcall_poll(struct acrn_vcpu *vcpu,
	struct acrn_virtio_proxy_ioc *ioc)
{
	struct virtio_proxy_dev *dev = virtio_proxy_find_hcall_target(vcpu->vm->vm_id,
		ioc->frontend_vmid, ioc->device_id);
	struct virtio_proxy_pending *pending = NULL;
	struct virtio_mmio_queue *vq;
	uint64_t now;
	uint16_t head;
	int32_t ret = -ENODEV;

	if (dev == NULL) {
		return ret;
	}

	spinlock_obtain(&dev->lock);
	now = cpu_ticks();
	virtio_proxy_check_timeouts_locked(dev, now);
	dev->hcall_poll_count++;
	dev->last_poll_queue_id = ioc->queue_id;
	vq = virtio_mmio_get_queue(&dev->mmio, ioc->queue_id);
	pending = virtio_proxy_unsent_pending_slot(dev, ioc->queue_id);
	if (pending == NULL) {
		pending = virtio_proxy_free_pending_slot(dev);
		if (pending == NULL) {
			dev->hcall_backpressure_count++;
			ret = -EBUSY;
			goto out;
		}
		if (virtio_mmio_pop_avail(&dev->mmio, vq, &head)) {
			if (!virtio_proxy_copy_chain_to_pending(dev, vq, ioc->queue_id,
				head, pending)) {
				ret = -EFAULT;
				goto out;
			}
			pending->notify_tick = dev->last_notify_tick[ioc->queue_id] != 0UL ?
				dev->last_notify_tick[ioc->queue_id] : now;
		}
	}
	if (!pending->valid || pending->sent) {
		dev->hcall_busy_count++;
		ret = -EBUSY;
		goto out;
	}
	if ((ioc->in_gpa == 0UL) || (ioc->in_len < pending->in_len)) {
		ret = -EINVAL;
		goto out;
	}
	ret = copy_to_gpa(vcpu->vm, pending->in, ioc->in_gpa, pending->in_len);
	if (ret == 0) {
		ioc->frontend_vmid = virtio_proxy_frontend_vmid(dev);
		ioc->device_id = virtio_proxy_device_id(dev);
		ioc->queue_id = pending->queue_id;
		ioc->head = pending->head;
		ioc->in_len = pending->in_len;
		ioc->out_len = pending->out_len;
		ioc->desc_count = pending->out_count;
		ioc->status = (virtio_proxy_access(dev) == VIRTIO_PROXY_ACCESS_READONLY) ?
			ACRN_VIRTIO_PROXY_FLAG_RO : 0U;
		dev->last_poll_status = ioc->status;
		for (uint16_t i = 0U; i < pending->out_count; i++) {
			ioc->desc[i].len = pending->out[i].len;
			ioc->desc[i].flags = VIRTIO_RING_F_WRITE;
		}
		pending->poll_tick = now;
		pending->sent = true;
		dev->hcall_poll_ok_count++;
		dev->last_poll_queue_id = pending->queue_id;
		dev->last_poll_head = pending->head;
	}

out:
	spinlock_release(&dev->lock);
	return ret;
}

static int32_t virtio_proxy_hcall_reply(struct acrn_vcpu *vcpu,
	const struct acrn_virtio_proxy_ioc *ioc)
{
	struct virtio_proxy_dev *dev = virtio_proxy_find_hcall_target(vcpu->vm->vm_id,
		ioc->frontend_vmid, ioc->device_id);
	struct virtio_proxy_pending *pending;
	struct virtio_mmio_queue *vq;
	uint8_t buf[128];
	uint64_t now;
	uint32_t copied = 0U;
	uint32_t remaining;
	uint32_t chunk;
	int32_t ret = -ENODEV;

	if (dev == NULL) {
		return ret;
	}

	spinlock_obtain(&dev->lock);
	now = cpu_ticks();
	dev->hcall_reply_count++;
	dev->last_reply_queue_id = ioc->queue_id;
	dev->last_reply_head = ioc->head;
	dev->last_reply_len = ioc->out_len;
	vq = virtio_mmio_get_queue(&dev->mmio, ioc->queue_id);
	pending = virtio_proxy_sent_pending_slot(dev, ioc->queue_id, ioc->head);
	if ((pending == NULL) || (ioc->out_len > pending->out_len)) {
		ret = -EINVAL;
		goto out;
	}
	pending->reply_tick = now;
	remaining = ioc->out_len;
	for (uint16_t i = 0U; (i < pending->out_count) && (remaining > 0U); i++) {
		uint32_t desc_off = 0U;
		uint32_t desc_remaining = pending->out[i].len;

		while ((desc_remaining > 0U) && (remaining > 0U)) {
			chunk = remaining < desc_remaining ? remaining : desc_remaining;
			if (chunk > sizeof(buf)) {
				chunk = sizeof(buf);
			}
			ret = copy_from_gpa(vcpu->vm, buf, ioc->out_gpa + copied, chunk);
			if (ret != 0) {
				goto out;
			}
			if (!virtio_mmio_write_gpa(&dev->mmio,
				pending->out[i].gpa + desc_off, buf, chunk)) {
				ret = -EFAULT;
				goto out;
			}
			copied += chunk;
			desc_off += chunk;
			desc_remaining -= chunk;
			remaining -= chunk;
		}
	}
	if (remaining != 0U) {
		ret = -EINVAL;
		goto out;
	}
	if (!virtio_mmio_add_used(&dev->mmio, vq, pending->head, ioc->out_len)) {
		ret = -EFAULT;
		goto out;
	}
	virtio_mmio_raise_used_irq(&dev->mmio);
	pending->irq_tick = cpu_ticks();
	virtio_proxy_latency_complete_locked(dev, pending);
	(void)memset(pending, 0U, sizeof(*pending));
	pending->done = true;
	dev->hcall_reply_ok_count++;
	ret = 0;

out:
	spinlock_release(&dev->lock);
	return ret;
}

int32_t virtio_proxy_backend_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa)
{
	struct acrn_virtio_proxy_ioc ioc;
	int32_t ret;

	if ((vcpu == NULL) || (ioc_gpa == 0UL) ||
		(copy_from_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc)) != 0)) {
		return -EFAULT;
	}

	ioc.status = 0U;
	switch (ioc.op) {
	case ACRN_VIRTIO_PROXY_OP_REGISTER:
		ret = virtio_proxy_hcall_register(vcpu, &ioc);
		break;
	case ACRN_VIRTIO_PROXY_OP_POLL:
		ret = virtio_proxy_hcall_poll(vcpu, &ioc);
		break;
	case ACRN_VIRTIO_PROXY_OP_REPLY:
		ret = virtio_proxy_hcall_reply(vcpu, &ioc);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	virtio_proxy_record_hcall_result(vcpu->vm->vm_id, &ioc, ret);
	if (ret != 0) {
		ioc.status = (uint32_t)ret;
	}
	if (copy_to_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc)) != 0) {
		ret = -EFAULT;
	}

	return ret;
}
