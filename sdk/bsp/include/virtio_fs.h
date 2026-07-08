/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VIRTIO_FS_H
#define VIRTIO_FS_H

#include <types.h>

struct acrn_vm;
struct io_request;

#define VIRTIO_FS_ACCESS_READONLY	0U
#define VIRTIO_FS_ACCESS_READWRITE	1U

void virtio_fs_init_vm(struct acrn_vm *vm);
void virtio_fs_reset_vm(struct acrn_vm *vm);
int32_t virtio_fs_mmio_handler(struct io_request *io_req,
	void *handler_private_data);

#endif /* VIRTIO_FS_H */
