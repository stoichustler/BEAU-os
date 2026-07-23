/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/device_mmio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hcall.h"

#define BEAU_IPC_MSG_MAX		192U
#define BEAU_IPC_DRAIN_MAX		16U
#define BEAU_IPC_REPLY_WAIT_MS		1500U
#define BEAU_IPC_REPLY_POLL_MS		20U

struct beau_ipc_channel {
	uint32_t channel_id;
	uint16_t peer_vmid;
	uint64_t gpa_base;
	uint32_t ring_size;
	uint32_t ring_count;
	mm_reg_t map_base;
	struct beau_ipc_ring_header *tx;
	struct beau_ipc_ring_header *rx;
	bool mapped;
};

static struct beau_ipc_channel beau_ipc = {
	.channel_id = BEAU_IPC_CHANNEL_ANY,
};

static struct beau_ipc_ioc beau_ipc_hcall_ioc __aligned(64);
K_MUTEX_DEFINE(beau_ipc_hcall_lock);

static int beau_ipc_call(uint32_t op, struct beau_ipc_ioc *ioc)
{
	struct beau_ipc_ioc *hcall_ioc = &beau_ipc_hcall_ioc;
	uint32_t status = UINT32_MAX;
	long ret;

	k_mutex_lock(&beau_ipc_hcall_lock, K_FOREVER);
	memset(hcall_ioc, 0, sizeof(*hcall_ioc));
	hcall_ioc->op = op;
	hcall_ioc->abi_version = BEAU_IPC_ABI_VERSION;
	hcall_ioc->ioc_size = sizeof(*hcall_ioc);
	hcall_ioc->channel_id = beau_ipc.channel_id;

	ret = beau_hcall_ipc(hcall_ioc);
	if (ret == 0) {
		status = hcall_ioc->status;
		if (ioc != NULL) {
			*ioc = *hcall_ioc;
		}
	}
	k_mutex_unlock(&beau_ipc_hcall_lock);
	if (ret != 0) {
		return (int)ret;
	}
	return status == BEAU_IPC_STATUS_OK ? 0 : -ENOENT;
}

static uint8_t *beau_ipc_ring_data(struct beau_ipc_ring_header *ring)
{
	return ((uint8_t *)ring) + ring->header_size;
}

static bool beau_ipc_ring_valid(struct beau_ipc_ring_header *ring)
{
	return ring != NULL &&
	       ring->magic == BEAU_IPC_RING_MAGIC &&
	       ring->version == BEAU_IPC_ABI_VERSION &&
	       ring->elem_size == 1U &&
	       ring->header_size >= sizeof(*ring) &&
	       ring->header_size < ring->ring_size &&
	       ring->elem_count <= (ring->ring_size - ring->header_size);
}

static void beau_ipc_write_byte(struct beau_ipc_ring_header *ring, uint64_t off,
				uint8_t value)
{
	uint8_t *data = beau_ipc_ring_data(ring);

	data[off % ring->elem_count] = value;
}

static uint8_t beau_ipc_read_byte(struct beau_ipc_ring_header *ring, uint64_t off)
{
	uint8_t *data = beau_ipc_ring_data(ring);

	return data[off % ring->elem_count];
}

static int beau_ipc_ring_write_msg(struct beau_ipc_ring_header *ring,
				   const uint8_t *buf, uint16_t len)
{
	uint32_t need = (uint32_t)len + 2U;
	uint64_t prod = ring->prod;
	uint64_t cons;
	uint64_t used;

	barrier_dmem_fence_full();
	cons = ring->cons;
	used = prod - cons;

	if (!beau_ipc_ring_valid(ring) || len == 0U || need > ring->elem_count) {
		return -EINVAL;
	}
	if (used > ring->elem_count) {
		return -EIO;
	}
	if (((uint64_t)ring->elem_count - used) < need) {
		ring->drop_count++;
		return -ENOSPC;
	}

	beau_ipc_write_byte(ring, prod, (uint8_t)(len & 0xffU));
	beau_ipc_write_byte(ring, prod + 1U, (uint8_t)(len >> 8U));
	for (uint16_t idx = 0U; idx < len; idx++) {
		beau_ipc_write_byte(ring, prod + 2U + idx, buf[idx]);
	}
	ring->bytes += len;
	barrier_dmem_fence_full();
	ring->prod = prod + need;
	barrier_dmem_fence_full();

	return 0;
}

static int beau_ipc_ring_read_msg(struct beau_ipc_ring_header *ring,
				  uint8_t *buf, uint16_t max_len)
{
	uint64_t prod;
	uint64_t cons = ring->cons;
	uint64_t avail;
	uint16_t len;

	barrier_dmem_fence_full();
	prod = ring->prod;
	avail = prod - cons;

	if (!beau_ipc_ring_valid(ring)) {
		return -EINVAL;
	}
	if (avail == 0U) {
		return 0;
	}
	if (avail > ring->elem_count) {
		ring->cons = prod;
		return -EIO;
	}
	if (avail < 2U) {
		return 0;
	}

	len = beau_ipc_read_byte(ring, cons);
	len |= (uint16_t)beau_ipc_read_byte(ring, cons + 1U) << 8U;
	if (len == 0U || len > max_len ||
	    ((uint32_t)len + 2U) > ring->elem_count) {
		ring->cons = prod;
		return -EOVERFLOW;
	}
	if (avail < ((uint32_t)len + 2U)) {
		return 0;
	}

	for (uint16_t idx = 0U; idx < len; idx++) {
		buf[idx] = beau_ipc_read_byte(ring, cons + 2U + idx);
	}
	buf[len] = '\0';
	barrier_dmem_fence_full();
	ring->cons = cons + (uint32_t)len + 2U;
	barrier_dmem_fence_full();

	return len;
}

static int beau_ipc_select_rings(const struct beau_ipc_ioc *ioc)
{
	for (uint32_t dir = 0U; dir < ioc->ring_count; dir++) {
		struct beau_ipc_ring_header *ring =
			(struct beau_ipc_ring_header *)((uint8_t *)beau_ipc.map_base +
				((size_t)dir * ioc->ring_size));

		if (!beau_ipc_ring_valid(ring)) {
			return -EINVAL;
		}
		if (ring->peer_vmid == ioc->peer_vmid) {
			beau_ipc.tx = ring;
		}
		if (ring->owner_vmid == ioc->peer_vmid) {
			beau_ipc.rx = ring;
		}
	}

	return beau_ipc.tx != NULL && beau_ipc.rx != NULL ? 0 : -ENOENT;
}

static int beau_ipc_query_and_map(void)
{
	struct beau_ipc_ioc ioc;
	size_t map_size;
	int ret;

	if (beau_ipc.mapped) {
		return 0;
	}

	ret = beau_ipc_call(BEAU_IPC_OP_QUERY, &ioc);
	if (ret != 0) {
		return ret;
	}
	if (ioc.ring_count != BEAU_IPC_RING_COUNT ||
	    ioc.ring_size == 0U ||
	    ioc.ring_size > (UINT32_MAX / ioc.ring_count)) {
		return -EINVAL;
	}

	map_size = (size_t)ioc.ring_size * ioc.ring_count;
	device_map(&beau_ipc.map_base, (uintptr_t)ioc.gpa_base, map_size,
		   K_MEM_CACHE_WB);

	beau_ipc.channel_id = ioc.channel_id;
	beau_ipc.peer_vmid = ioc.peer_vmid;
	beau_ipc.gpa_base = ioc.gpa_base;
	beau_ipc.ring_size = ioc.ring_size;
	beau_ipc.ring_count = ioc.ring_count;

	ret = beau_ipc_select_rings(&ioc);
	if (ret == 0) {
		beau_ipc.mapped = true;
	}

	return ret;
}

static int beau_ipc_notify(void)
{
	return beau_ipc_call(BEAU_IPC_OP_NOTIFY, NULL);
}

static int beau_ipc_ack(void)
{
	return beau_ipc_call(BEAU_IPC_OP_ACK, NULL);
}

static int beau_ipc_drain_rx(void)
{
	uint8_t msg[BEAU_IPC_MSG_MAX + 1U];

	for (uint32_t idx = 0U; idx < BEAU_IPC_DRAIN_MAX; idx++) {
		int ret = beau_ipc_ring_read_msg(beau_ipc.rx, msg, BEAU_IPC_MSG_MAX);

		if (ret == 0) {
			return 0;
		}
		if (ret < 0) {
			return ret;
		}
		(void)beau_ipc_ack();
	}

	return 0;
}

static int cmd_hipc_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = beau_ipc_query_and_map();

	if (ret != 0) {
		shell_error(sh, "query failed: %d", ret);
		return ret;
	}

	shell_print(sh, "channel:%u peer-vm:%u gpa:0x%llx ring:%u count:%u",
		    beau_ipc.channel_id, beau_ipc.peer_vmid,
		    (unsigned long long)beau_ipc.gpa_base,
		    beau_ipc.ring_size, beau_ipc.ring_count);
	return 0;
}

static int cmd_hipc_send(const struct shell *sh, size_t argc, char **argv)
{
	const char *payload;
	size_t payload_len;
	uint8_t reply[BEAU_IPC_MSG_MAX + 1U];
	int ret;
	int64_t deadline;

	if (argc != 2U) {
		shell_error(sh, "usage: hipc send <payload>");
		return -EINVAL;
	}

	payload = argv[1];
	payload_len = strnlen(payload, BEAU_IPC_MSG_MAX + 1U);
	if (payload_len == 0U || payload_len > BEAU_IPC_MSG_MAX) {
		shell_error(sh, "payload must be 1..%u bytes", BEAU_IPC_MSG_MAX);
		return -EMSGSIZE;
	}

	ret = beau_ipc_query_and_map();

	if (ret != 0) {
		shell_error(sh, "query failed: %d", ret);
		return ret;
	}

	ret = beau_ipc_drain_rx();
	if (ret != 0) {
		shell_error(sh, "drain failed: %d", ret);
		return ret;
	}

	ret = beau_ipc_ring_write_msg(beau_ipc.tx, (const uint8_t *)payload,
				      (uint16_t)payload_len);
	if (ret != 0) {
		shell_error(sh, "tx failed: %d", ret);
		return ret;
	}

	ret = beau_ipc_notify();
	if (ret != 0) {
		shell_error(sh, "notify failed: %d", ret);
		return ret;
	}

	deadline = k_uptime_get() + BEAU_IPC_REPLY_WAIT_MS;
	do {
		ret = beau_ipc_ring_read_msg(beau_ipc.rx, reply, BEAU_IPC_MSG_MAX);
		if (ret > 0) {
			(void)beau_ipc_ack();
			shell_print(sh, "reply:%s", reply);
			return 0;
		}
		if (ret < 0) {
			shell_error(sh, "rx failed: %d", ret);
			return ret;
		}
		k_sleep(K_MSEC(BEAU_IPC_REPLY_POLL_MS));
	} while (k_uptime_get() < deadline);

	shell_error(sh, "reply timeout");
	return -ETIMEDOUT;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hipc,
	SHELL_CMD_ARG(status, NULL, "Show BEAU HVC IPC channel", cmd_hipc_status, 1, 0),
	SHELL_CMD_ARG(send, NULL, "Send BEAU HVC IPC payload", cmd_hipc_send, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(hipc, &sub_hipc, "BEAU HVC IPC validation", NULL);
