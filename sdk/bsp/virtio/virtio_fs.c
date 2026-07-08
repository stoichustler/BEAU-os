/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vm_config.h>
#include <rtl.h>
#include <io_req.h>
#include <virtio_mmio.h>
#include <virtio_fs.h>

#define VIRTIO_FS_QUEUE_HIPRIO		0U
#define VIRTIO_FS_QUEUE_REQUEST		1U
#define VIRTIO_FS_QUEUE_NUM		2U
#define VIRTIO_FS_QUEUE_SIZE		64U
#define VIRTIO_FS_TAG_SIZE		36U
#define VIRTIO_FS_CHAIN_LIMIT		VIRTIO_FS_QUEUE_SIZE

struct virtio_fs_config {
	char tag[VIRTIO_FS_TAG_SIZE];
	uint32_t num_request_queues;
};

struct virtio_fs_dev {
	struct virtio_mmio_dev mmio;
	struct virtio_fs_config config;
	uint32_t access;
	uint64_t request_count;
};

static struct virtio_fs_dev virtio_fs_devs[CONFIG_MAX_VM_NUM];

static struct virtio_fs_dev *virtio_fs_get_dev(const struct acrn_vm *vm)
{
	return (vm != NULL) && (vm->vm_id < CONFIG_MAX_VM_NUM) ?
		&virtio_fs_devs[vm->vm_id] : NULL;
}

static bool virtio_fs_enabled(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_fs_size != 0UL);
}

static void virtio_fs_reset(struct virtio_mmio_dev *mmio)
{
	struct virtio_fs_dev *dev = (struct virtio_fs_dev *)virtio_mmio_priv(mmio);

	if (dev != NULL) {
		dev->request_count = 0UL;
	}
}

static uint32_t virtio_fs_read_config(struct virtio_mmio_dev *mmio,
	uint32_t offset, uint32_t size)
{
	struct virtio_fs_dev *dev = (struct virtio_fs_dev *)virtio_mmio_priv(mmio);
	uint8_t *config;
	uint32_t value = 0U;
	uint32_t i;

	if ((dev == NULL) || ((offset + size) > sizeof(dev->config)) ||
		(size > sizeof(value))) {
		return 0U;
	}

	config = (uint8_t *)&dev->config;
	for (i = 0U; i < size; i++) {
		value |= ((uint32_t)config[offset + i]) << (i * 8U);
	}

	return value;
}

static void virtio_fs_complete_one(struct virtio_fs_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t head)
{
	/*
	 * 2026-07-08, virtio-fs staging principle:
	 *
	 * The MMIO and vring transport is complete, but no FUSE backend owns file
	 * operations yet. Complete the chain with zero bytes so guests observe a
	 * live transport endpoint without pretending that /var/beau is serviced.
	 *
	 *   guest virtqueue -> BEAU transport -> backend later
	 *                         |
	 *                         +--> used len 0 for now
	 */
	(void)virtio_mmio_add_used(&dev->mmio, vq, head, 0U);
	dev->request_count++;
}

static void virtio_fs_process_queue(struct virtio_fs_dev *dev, uint16_t queue_id)
{
	struct virtio_mmio_queue *vq = virtio_mmio_get_queue(&dev->mmio, queue_id);
	uint16_t head;
	bool used = false;
	uint32_t chains = 0U;

	while ((chains < VIRTIO_FS_CHAIN_LIMIT) &&
		virtio_mmio_pop_avail(&dev->mmio, vq, &head)) {
		virtio_fs_complete_one(dev, vq, head);
		used = true;
		chains++;
	}

	if (used) {
		virtio_mmio_raise_used_irq(&dev->mmio);
	}
}

static void virtio_fs_notify_queue(struct virtio_mmio_dev *mmio,
	uint16_t queue_id)
{
	struct virtio_fs_dev *dev = (struct virtio_fs_dev *)virtio_mmio_priv(mmio);

	if ((dev != NULL) && (queue_id < VIRTIO_FS_QUEUE_NUM)) {
		virtio_fs_process_queue(dev, queue_id);
	}
}

static const struct virtio_mmio_ops virtio_fs_mmio_ops = {
	.reset = virtio_fs_reset,
	.notify_queue = virtio_fs_notify_queue,
	.read_config = virtio_fs_read_config,
};

void virtio_fs_init_vm(struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	struct virtio_fs_dev *dev = virtio_fs_get_dev(vm);
	struct virtio_mmio_init init;

	if ((dev == NULL) || !virtio_fs_enabled(vm_config)) {
		return;
	}

	(void)memset(dev, 0U, sizeof(*dev));
	(void)strncpy_s(dev->config.tag, sizeof(dev->config.tag),
		vm_config->arch.guest_virtio_fs_tag, sizeof(dev->config.tag) - 1U);
	dev->config.num_request_queues = 1U;
	dev->access = vm_config->arch.guest_virtio_fs_access;

	/*
	 * Access policy is carried in BEAU's device state first. A real backend
	 * must enforce it when it starts serving FUSE operations:
	 *   VM1 /var/beau -> read-only
	 *   VM2 /var/beau -> read-write
	 */
	init.name = "virtio-fs";
	init.vm = vm;
	init.base = vm_config->arch.guest_virtio_fs_base;
	init.size = vm_config->arch.guest_virtio_fs_size;
	init.irq = vm_config->arch.guest_virtio_fs_irq;
	init.device_id = VIRTIO_DEVICE_ID_FS;
	init.queue_num = VIRTIO_FS_QUEUE_NUM;
	init.queue_size = VIRTIO_FS_QUEUE_SIZE;
	init.device_features = 0UL;
	init.ops = &virtio_fs_mmio_ops;
	init.priv = dev;
	virtio_mmio_init(&dev->mmio, &init);
}

void virtio_fs_reset_vm(struct acrn_vm *vm)
{
	struct virtio_fs_dev *dev = virtio_fs_get_dev(vm);

	if (dev != NULL) {
		virtio_mmio_reset_dev(&dev->mmio);
	}
}

int32_t virtio_fs_mmio_handler(struct io_request *io_req,
	void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct virtio_fs_dev *dev = virtio_fs_get_dev(vm);

	return dev != NULL ? virtio_mmio_handler(io_req, &dev->mmio) : -EINVAL;
}
