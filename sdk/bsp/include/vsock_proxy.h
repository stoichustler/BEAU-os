/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VSOCK_PROXY_H
#define VSOCK_PROXY_H

#include <types.h>

struct acrn_vm;
struct acrn_vcpu;
struct io_request;

void vsock_proxy_init_vm(struct acrn_vm *vm);
void vsock_proxy_reset_vm(struct acrn_vm *vm);
void vsock_proxy_release_vm(struct acrn_vm *vm);
int32_t vsock_proxy_mmio_handler(struct io_request *io_req,
	void *handler_private_data);
void *vsock_proxy_get_dev(struct acrn_vm *vm);
int32_t vsock_proxy_backend_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa);

#endif /* VSOCK_PROXY_H */
