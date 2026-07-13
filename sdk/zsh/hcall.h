/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BEAU_ZSH_HCALL_H
#define BEAU_ZSH_HCALL_H

#include <stdint.h>
#include <zephyr/toolchain.h>

#define BEAU_IPC_ABI_VERSION		1U
#define BEAU_IPC_OP_QUERY		0U
#define BEAU_IPC_OP_NOTIFY		1U
#define BEAU_IPC_OP_ACK		2U
#define BEAU_IPC_STATUS_OK		0U
#define BEAU_IPC_STATUS_BAD_PARAM	1U
#define BEAU_IPC_STATUS_NO_CHANNEL	2U
#define BEAU_IPC_RING_MAGIC		0x42495043U
#define BEAU_IPC_CHANNEL_ANY		UINT32_MAX
#define BEAU_IPC_RING_COUNT		2U
#define BEAU_IPC_DIR_EP0_TO_EP1	0U
#define BEAU_IPC_DIR_EP1_TO_EP0	1U
#define BEAU_IPC_FLAG_NOTIFY_IRQ	0x1U

struct beau_ipc_ioc {
	uint32_t op;
	uint32_t status;
	uint32_t abi_version;
	uint32_t ioc_size;
	uint32_t channel_id;
	uint16_t peer_vmid;
	uint16_t flags;
	uint64_t gpa_base;
	uint32_t ring_size;
	uint32_t ring_count;
	uint32_t notify_count;
	uint32_t ack_count;
	uint32_t reserved;
} __aligned(8);

struct beau_ipc_ring_header {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t ring_size;
	uint16_t owner_vmid;
	uint16_t peer_vmid;
	uint16_t direction;
	uint16_t flags;
	uint32_t elem_size;
	uint32_t elem_count;
	uint64_t prod __aligned(64);
	uint64_t cons __aligned(64);
	uint64_t notify_count;
	uint64_t drop_count;
	uint64_t bytes;
} __aligned(64);

long beau_hcall_vm_wdt_kick(unsigned long token);
long beau_hcall_ipc(struct beau_ipc_ioc *ioc);

#endif /* BEAU_ZSH_HCALL_H */
