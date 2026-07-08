/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIRTIO_PROXY_H
#define VIRTIO_PROXY_H

#include <types.h>
#include <virtio_mmio.h>

struct acrn_vm;
struct acrn_vcpu;
struct io_request;
struct virtio_proxy_dev;

#define VIRTIO_PROXY_ACCESS_READONLY	0U
#define VIRTIO_PROXY_ACCESS_READWRITE	1U
#define VIRTIO_PROXY_TAG_MAX		36U
#define VIRTIO_PROXY_QUEUE_NUM_DEFAULT	2U
#define VIRTIO_PROXY_QUEUE_SIZE_DEFAULT	64U

struct virtio_proxy_queue_stats {
	uint16_t num;
	uint16_t last_avail_idx;
	uint64_t desc;
	uint64_t avail;
	uint64_t used;
	bool ready;
};

struct virtio_proxy_stats {
	uint16_t vm_id;
	uint32_t device_id;
	uint32_t access;
	char tag[VIRTIO_PROXY_TAG_MAX];
	uint64_t base;
	uint64_t size;
	uint32_t irq;
	uint32_t status;
	uint32_t interrupt_status;
	uint16_t queue_num;
	uint16_t queue_size;
	uint64_t notify_count;
	bool backend_bound;
	bool hcall_backend_registered;
	bool pending_valid;
	bool pending_sent;
	bool pending_done;
	uint16_t pending_queue_id;
	uint16_t pending_head;
	uint16_t pending_in_len;
	uint32_t pending_out_len;
	uint16_t pending_out_count;
	uint64_t hcall_register_count;
	uint64_t hcall_poll_count;
	uint64_t hcall_poll_ok_count;
	uint64_t hcall_reply_count;
	uint64_t hcall_reply_ok_count;
	uint32_t last_hcall_op;
	int32_t last_hcall_ret;
	uint16_t last_poll_queue_id;
	uint16_t last_poll_head;
	uint16_t last_reply_queue_id;
	uint16_t last_reply_head;
	uint32_t last_reply_len;
	struct virtio_proxy_queue_stats queues[VIRTIO_MMIO_MAX_QUEUES];
};

struct virtio_proxy_backend_ops {
	void (*reset)(struct virtio_proxy_dev *proxy, void *priv);
	void (*queue_ready)(struct virtio_proxy_dev *proxy, uint16_t queue_id,
		void *priv);
	void (*notify_queue)(struct virtio_proxy_dev *proxy, uint16_t queue_id,
		void *priv);
	uint32_t (*read_config)(struct virtio_proxy_dev *proxy, uint32_t offset,
		uint32_t size, void *priv);
	void (*write_config)(struct virtio_proxy_dev *proxy, uint32_t offset,
		uint32_t size, uint32_t value, void *priv);
};

void virtio_proxy_init_vm(struct acrn_vm *vm);
void virtio_proxy_reset_vm(struct acrn_vm *vm);
int32_t virtio_proxy_mmio_handler(struct io_request *io_req,
	void *handler_private_data);

int32_t virtio_proxy_bind_backend(uint16_t vm_id,
	const struct virtio_proxy_backend_ops *ops, void *priv);
void virtio_proxy_unbind_backend(uint16_t vm_id,
	const struct virtio_proxy_backend_ops *ops, void *priv);

struct virtio_mmio_dev *virtio_proxy_mmio(struct virtio_proxy_dev *proxy);
struct virtio_mmio_queue *virtio_proxy_get_queue(struct virtio_proxy_dev *proxy,
	uint16_t queue_id);
uint16_t virtio_proxy_frontend_vmid(const struct virtio_proxy_dev *proxy);
uint32_t virtio_proxy_device_id(const struct virtio_proxy_dev *proxy);
uint32_t virtio_proxy_access(const struct virtio_proxy_dev *proxy);
const char *virtio_proxy_tag(const struct virtio_proxy_dev *proxy);
bool virtio_proxy_backend_ready(const struct virtio_proxy_dev *proxy);
bool virtio_proxy_set_config(struct virtio_proxy_dev *proxy, const void *config,
	uint32_t size);
bool virtio_proxy_set_device_features(struct virtio_proxy_dev *proxy,
	uint64_t features);
int32_t virtio_proxy_backend_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa);
bool virtio_proxy_get_stats(uint16_t vm_id, struct virtio_proxy_stats *stats);

#endif /* VIRTIO_PROXY_H */
