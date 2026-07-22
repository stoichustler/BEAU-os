/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VRPROC_H
#define ARM64_GUEST_VRPROC_H

#include <types.h>

struct acrn_vm;
struct io_request;

#define ARM64_VRPROC_MAX_STATIC_CHANNELS	2U
#define ARM64_VRPROC_SHARED_SIZE_MAX		0x00100000U
#define ARM64_VRPROC_SHARED_SIZE_MIN		0x00010000U
#define ARM64_VRPROC_DOORBELL_SIZE		0x00001000U
#define ARM64_VRPROC_VRING_COUNT		2U
#define ARM64_VRPROC_VRING_NUM_MAX		256U
#define ARM64_VRPROC_VIRQ_MIN			32U
#define ARM64_VRPROC_VIRQ_MAX			1019U

struct arm64_vrproc_channel_config {
	uint32_t channel_id;
	uint16_t endpoint_vmid[ARM64_VRPROC_VRING_COUNT];
	uint64_t shared_gpa;
	uint32_t shared_size;
	uint64_t doorbell_gpa[ARM64_VRPROC_VRING_COUNT];
	uint32_t notify_virq[ARM64_VRPROC_VRING_COUNT];
	uint32_t vring_num;
	uint32_t vring_align;
};

struct arm64_vrproc_channel_stats {
	uint32_t channel_id;
	uint16_t endpoint_vmid[ARM64_VRPROC_VRING_COUNT];
	uint64_t shared_gpa;
	uint32_t shared_size;
	uint64_t doorbell_gpa[ARM64_VRPROC_VRING_COUNT];
	uint32_t notify_virq[ARM64_VRPROC_VRING_COUNT];
	uint32_t vring_num;
	uint32_t vring_align;
	uint32_t mapped_mask;
	uint64_t kick_count[ARM64_VRPROC_VRING_COUNT];
	uint64_t irq_count[ARM64_VRPROC_VRING_COUNT];
	uint64_t irq_fail_count[ARM64_VRPROC_VRING_COUNT];
	uint64_t bad_mmio_count;
	uint64_t last_kick_tick[ARM64_VRPROC_VRING_COUNT];
};

int32_t arm64_vrproc_register_channel(const struct arm64_vrproc_channel_config *config);
void arm64_vrproc_init_vm(struct acrn_vm *vm);
void arm64_vrproc_register_mmio(struct acrn_vm *vm);
int32_t arm64_vrproc_mmio_handler(struct io_request *io_req, void *handler_private_data);
uint32_t arm64_vrproc_get_stats(struct arm64_vrproc_channel_stats *stats, uint32_t max_count);

#endif /* ARM64_GUEST_VRPROC_H */
