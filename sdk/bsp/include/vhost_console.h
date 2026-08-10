/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VHOST_CONSOLE_H
#define VHOST_CONSOLE_H

#include <types.h>

#define VHOST_CONSOLE_STAT_QUEUE_NUM	2U

struct acrn_vm;
struct io_request;

struct vhost_console_queue_stats {
	uint16_t num;
	uint16_t last_avail_idx;
	uint64_t desc;
	uint64_t avail;
	uint64_t used;
	bool ready;
};

struct vhost_console_stats {
	bool active;
	uint64_t base;
	uint64_t size;
	uint32_t irq;
	uint32_t status;
	uint32_t interrupt_status;
	uint64_t device_features;
	uint64_t driver_features;
	uint64_t tx_count;
	uint64_t rx_count;
	uint64_t tx_notify_count;
	uint64_t rx_notify_count;
	uint64_t tx_irq_count;
	uint64_t rx_irq_count;
	struct vhost_console_queue_stats queues[VHOST_CONSOLE_STAT_QUEUE_NUM];
};

void vhost_console_init_vm(struct acrn_vm *vm);
void vhost_console_reset_vm(struct acrn_vm *vm);
bool vhost_console_get_stats(uint16_t vm_id, struct vhost_console_stats *stats);
int32_t vhost_console_mmio_handler(struct io_request *io_req,
	void *handler_private_data);

#endif /* VHOST_CONSOLE_H */
