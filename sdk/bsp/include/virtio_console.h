/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIRTIO_CONSOLE_H
#define VIRTIO_CONSOLE_H

#include <types.h>

struct acrn_vm;
struct io_request;

void virtio_console_init_vm(struct acrn_vm *vm);
int32_t virtio_console_mmio_handler(struct io_request *io_req,
	void *handler_private_data);

#endif /* VIRTIO_CONSOLE_H */
