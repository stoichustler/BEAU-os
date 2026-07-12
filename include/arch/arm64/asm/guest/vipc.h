/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VIPC_H
#define ARM64_GUEST_VIPC_H

#include <types.h>
#include <acrn_hv_defs.h>

struct acrn_vcpu;
struct acrn_vm;

#define ARM64_VIPC_MAX_STATIC_CHANNELS	4U
#define ARM64_VIPC_RING_SIZE_DEFAULT	0x00010000U
#define ARM64_VIPC_RING_SIZE_MAX	0x00010000U

struct arm64_vipc_channel_config {
	uint32_t channel_id;
	uint16_t endpoint_vmid[ACRN_IPC_RING_COUNT];
	uint64_t gpa_base;
	uint32_t ring_size;
	uint32_t notify_virq;
};

struct arm64_vipc_channel_stats {
	uint32_t channel_id;
	uint16_t endpoint_vmid[ACRN_IPC_RING_COUNT];
	uint64_t gpa_base;
	uint32_t ring_size;
	uint32_t ring_count;
	uint32_t mapped_mask;
	uint32_t notify_virq;
	uint64_t notify_count[ACRN_IPC_RING_COUNT];
	uint64_t ack_count[ACRN_IPC_RING_COUNT];
	uint64_t wake_count[ACRN_IPC_RING_COUNT];
	uint64_t irq_count[ACRN_IPC_RING_COUNT];
	uint64_t irq_fail_count[ACRN_IPC_RING_COUNT];
	uint64_t bad_hcall_count;
	uint64_t last_notify_tick[ACRN_IPC_RING_COUNT];
};

int32_t arm64_vipc_register_channel(const struct arm64_vipc_channel_config *config);
void arm64_vipc_init_vm(struct acrn_vm *vm);
int32_t arm64_vipc_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa);
uint32_t arm64_vipc_get_stats(struct arm64_vipc_channel_stats *stats, uint32_t max_count);

#endif /* ARM64_GUEST_VIPC_H */
