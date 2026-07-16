/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <types.h>

#define VIRTIO_MMIO_REGION_SIZE		0x200UL
#define VIRTIO_MMIO_CONFIG_OFFSET	0x100U

#define VIRTIO_DEVICE_ID_NET		1U
#define VIRTIO_DEVICE_ID_BLOCK		2U
#define VIRTIO_DEVICE_ID_CONSOLE	3U
#define VIRTIO_DEVICE_ID_RNG		4U
#define VIRTIO_DEVICE_ID_FS		26U
#define VIRTIO_DEVICE_ID_I2C		34U
#define VIRTIO_VENDOR_ID_BEAU		0x42454155U

#define VIRTIO_RING_F_NEXT		1U
#define VIRTIO_RING_F_WRITE		2U
#define VIRTIO_MMIO_INT_USED_RING	1U
#define VIRTIO_STATUS_FEATURES_OK	8U
#define VIRTIO_F_VERSION_1		32U

#define VIRTIO_MMIO_MAX_QUEUES		4U

struct acrn_vm;
struct io_request;
struct virtio_mmio_dev;

struct virtio_ring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct virtio_mmio_queue {
	uint16_t num;
	uint16_t last_avail_idx;
	uint64_t desc;
	uint64_t avail;
	uint64_t used;
	bool ready;
};

struct virtio_mmio_ops {
	void (*reset)(struct virtio_mmio_dev *dev);
	void (*queue_ready)(struct virtio_mmio_dev *dev, uint16_t queue_id);
	void (*notify_queue)(struct virtio_mmio_dev *dev, uint16_t queue_id);
	uint32_t (*read_config)(struct virtio_mmio_dev *dev, uint32_t offset,
		uint32_t size);
	void (*write_config)(struct virtio_mmio_dev *dev, uint32_t offset,
		uint32_t size, uint32_t value);
};

struct virtio_mmio_init {
	const char *name;
	struct acrn_vm *vm;
	uint64_t base;
	uint64_t size;
	uint32_t irq;
	uint32_t device_id;
	uint16_t queue_num;
	uint16_t queue_size;
	uint64_t device_features;
	const struct virtio_mmio_ops *ops;
	void *priv;
};

struct virtio_mmio_dev {
	const char *name;
	struct acrn_vm *vm;
	uint64_t base;
	uint64_t size;
	uint32_t irq;
	uint32_t device_id;
	uint16_t queue_num;
	uint16_t queue_size;
	uint64_t device_features;
	const struct virtio_mmio_ops *ops;
	void *priv;

	struct virtio_mmio_queue queues[VIRTIO_MMIO_MAX_QUEUES];
	uint64_t driver_features;
	uint32_t device_features_sel;
	uint32_t driver_features_sel;
	uint32_t queue_sel;
	uint32_t interrupt_status;
	uint32_t status;
	uint64_t pm_epoch;
	volatile uint64_t deferred_queue_mask;
	bool irq_asserted;
	bool pm_suspended;
};

void virtio_mmio_init(struct virtio_mmio_dev *dev,
	const struct virtio_mmio_init *init);
void virtio_mmio_reset_dev(struct virtio_mmio_dev *dev);
int32_t virtio_mmio_handler(struct io_request *io_req,
	void *handler_private_data);
void virtio_mmio_raise_used_irq(struct virtio_mmio_dev *dev);
struct virtio_mmio_queue *virtio_mmio_get_queue(struct virtio_mmio_dev *dev,
	uint16_t queue_id);
bool virtio_mmio_queue_valid(const struct virtio_mmio_dev *dev,
	const struct virtio_mmio_queue *vq);
bool virtio_mmio_pop_avail(struct virtio_mmio_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t *head);
bool virtio_mmio_add_used(struct virtio_mmio_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t id, uint32_t len);
bool virtio_mmio_read_desc(struct virtio_mmio_dev *dev,
	const struct virtio_mmio_queue *vq, uint16_t id,
	struct virtio_ring_desc *desc);
bool virtio_mmio_read_gpa(struct virtio_mmio_dev *dev, uint64_t gpa,
	void *buf, uint32_t size);
bool virtio_mmio_write_gpa(struct virtio_mmio_dev *dev, uint64_t gpa,
	void *buf, uint32_t size);
void *virtio_mmio_priv(struct virtio_mmio_dev *dev);
struct acrn_vm *virtio_mmio_vm(struct virtio_mmio_dev *dev);
int32_t virtio_mmio_pm_suspend(struct virtio_mmio_dev *dev, uint64_t epoch);
int32_t virtio_mmio_pm_resume(struct virtio_mmio_dev *dev, uint64_t epoch);

#endif /* VIRTIO_MMIO_H */
