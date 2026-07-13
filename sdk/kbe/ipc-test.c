// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU static IPC validation endpoint.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "hcall.h"

#define BEAU_IPC_MSG_MAX		256U
#define BEAU_IPC_POLL_MS		50U

struct beau_ipc_test {
	u32 channel_id;
	u16 peer_vmid;
	void *base;
	size_t map_size;
	struct beau_ipc_ring_header *tx;
	struct beau_ipc_ring_header *rx;
	struct task_struct *thread;
	struct beau_ipc_ioc hcall_ioc;
	struct mutex hcall_lock;
	struct mutex tx_lock;
	u64 rx_count;
	u64 tx_count;
};

static unsigned int channel_id = BEAU_IPC_CHANNEL_ANY;
module_param(channel_id, uint, 0444);
MODULE_PARM_DESC(channel_id, "BEAU IPC channel id, or U32_MAX for first visible channel");

static struct beau_ipc_test beau_ipc;

static u8 *beau_ipc_ring_data(struct beau_ipc_ring_header *ring)
{
	return ((u8 *)ring) + ring->header_size;
}

static bool beau_ipc_ring_valid(const struct beau_ipc_ring_header *ring)
{
	return ring != NULL &&
	       READ_ONCE(ring->magic) == BEAU_IPC_RING_MAGIC &&
	       READ_ONCE(ring->version) == BEAU_IPC_ABI_VERSION &&
	       READ_ONCE(ring->elem_size) == 1U &&
	       READ_ONCE(ring->header_size) >= sizeof(*ring) &&
	       READ_ONCE(ring->header_size) < READ_ONCE(ring->ring_size) &&
	       READ_ONCE(ring->elem_count) <=
		       (READ_ONCE(ring->ring_size) - READ_ONCE(ring->header_size));
}

static void beau_ipc_ring_write_byte(struct beau_ipc_ring_header *ring, u64 off, u8 value)
{
	u32 cap = READ_ONCE(ring->elem_count);
	u8 *data = beau_ipc_ring_data(ring);

	WRITE_ONCE(data[off % cap], value);
}

static u8 beau_ipc_ring_read_byte(struct beau_ipc_ring_header *ring, u64 off)
{
	u32 cap = READ_ONCE(ring->elem_count);
	u8 *data = beau_ipc_ring_data(ring);

	return READ_ONCE(data[off % cap]);
}

static int beau_ipc_ring_write_msg(struct beau_ipc_ring_header *ring,
				   const u8 *buf, u16 len)
{
	u32 cap = READ_ONCE(ring->elem_count);
	u64 prod = READ_ONCE(ring->prod);
	u64 cons = smp_load_acquire(&ring->cons);
	u64 used = prod - cons;
	u32 need = (u32)len + 2U;
	u32 idx;

	if (!beau_ipc_ring_valid(ring) || len == 0U || need > cap)
		return -EINVAL;
	if (used > cap)
		return -EIO;
	if (((u64)cap - used) < need) {
		WRITE_ONCE(ring->drop_count, READ_ONCE(ring->drop_count) + 1U);
		return -ENOSPC;
	}

	beau_ipc_ring_write_byte(ring, prod, (u8)(len & 0xffU));
	beau_ipc_ring_write_byte(ring, prod + 1U, (u8)(len >> 8U));
	for (idx = 0U; idx < len; idx++)
		beau_ipc_ring_write_byte(ring, prod + 2U + idx, buf[idx]);

	WRITE_ONCE(ring->bytes, READ_ONCE(ring->bytes) + len);
	smp_store_release(&ring->prod, prod + need);

	return 0;
}

static int beau_ipc_ring_read_msg(struct beau_ipc_ring_header *ring, u8 *buf, u16 max_len)
{
	u32 cap = READ_ONCE(ring->elem_count);
	u64 prod = smp_load_acquire(&ring->prod);
	u64 cons = READ_ONCE(ring->cons);
	u64 avail = prod - cons;
	u16 len;
	u32 idx;

	if (!beau_ipc_ring_valid(ring))
		return -EINVAL;
	if (avail == 0U)
		return 0;
	if (avail > cap) {
		smp_store_release(&ring->cons, prod);
		return -EIO;
	}
	if (avail < 2U)
		return 0;

	len = beau_ipc_ring_read_byte(ring, cons);
	len |= (u16)beau_ipc_ring_read_byte(ring, cons + 1U) << 8U;
	if (len == 0U || len > max_len || ((u32)len + 2U) > cap) {
		smp_store_release(&ring->cons, prod);
		return -EOVERFLOW;
	}
	if (avail < ((u32)len + 2U))
		return 0;

	for (idx = 0U; idx < len; idx++)
		buf[idx] = beau_ipc_ring_read_byte(ring, cons + 2U + idx);
	buf[len] = '\0';
	smp_store_release(&ring->cons, cons + (u32)len + 2U);

	return len;
}

static long beau_ipc_hcall(u32 op, struct beau_ipc_ioc *ioc)
{
	struct beau_ipc_ioc *hcall_ioc = &beau_ipc.hcall_ioc;
	u32 status = BEAU_IPC_STATUS_BAD_PARAM;
	long ret;

	mutex_lock(&beau_ipc.hcall_lock);
	memset(hcall_ioc, 0, sizeof(*hcall_ioc));
	hcall_ioc->op = op;
	hcall_ioc->abi_version = BEAU_IPC_ABI_VERSION;
	hcall_ioc->ioc_size = sizeof(*hcall_ioc);
	hcall_ioc->channel_id = beau_ipc.channel_id;

	ret = beau_hcall_ipc(hcall_ioc);
	if (ret == 0) {
		status = hcall_ioc->status;
		if (ioc)
			*ioc = *hcall_ioc;
	}
	mutex_unlock(&beau_ipc.hcall_lock);

	if (ret)
		return ret;
	if (status != BEAU_IPC_STATUS_OK)
		return status == BEAU_IPC_STATUS_NO_CHANNEL ? -ENODEV : -EINVAL;

	return 0;
}

static int beau_ipc_notify(void)
{
	return beau_ipc_hcall(BEAU_IPC_OP_NOTIFY, NULL);
}

static int beau_ipc_ack(void)
{
	return beau_ipc_hcall(BEAU_IPC_OP_ACK, NULL);
}

static int beau_ipc_send(const char *msg)
{
	u16 len = (u16)strnlen(msg, BEAU_IPC_MSG_MAX);
	int ret;

	mutex_lock(&beau_ipc.tx_lock);
	ret = beau_ipc_ring_write_msg(beau_ipc.tx, (const u8 *)msg, len);
	if (ret == 0) {
		beau_ipc.tx_count++;
		ret = beau_ipc_notify();
	}
	mutex_unlock(&beau_ipc.tx_lock);

	return ret;
}

static int beau_ipc_thread_fn(void *data)
{
	u8 msg[BEAU_IPC_MSG_MAX + 1U];

	while (!kthread_should_stop()) {
		int len = beau_ipc_ring_read_msg(beau_ipc.rx, msg, BEAU_IPC_MSG_MAX);

		if (len > 0) {
			char reply[BEAU_IPC_MSG_MAX];

			beau_ipc.rx_count++;
			pr_info("[κ] BEAU IPC ch%u rx peer-vm%u: %.*s\n",
				beau_ipc.channel_id, beau_ipc.peer_vmid, len, msg);
			beau_ipc_ack();
			snprintf(reply, sizeof(reply), "linux-ipc-ack:%llu",
				 (unsigned long long)beau_ipc.rx_count);
			beau_ipc_send(reply);
			continue;
		}
		if (len < 0)
			pr_warn_ratelimited("[κ] BEAU IPC ring read failed:%d\n", len);

		msleep(BEAU_IPC_POLL_MS);
	}

	return 0;
}

static int beau_ipc_select_rings(const struct beau_ipc_ioc *ioc)
{
	u32 dir;

	for (dir = 0U; dir < ioc->ring_count; dir++) {
		struct beau_ipc_ring_header *ring =
			(struct beau_ipc_ring_header *)((u8 *)beau_ipc.base +
				((size_t)dir * ioc->ring_size));

		if (!beau_ipc_ring_valid(ring))
			return -EINVAL;
		if (READ_ONCE(ring->peer_vmid) == ioc->peer_vmid)
			beau_ipc.tx = ring;
		if (READ_ONCE(ring->owner_vmid) == ioc->peer_vmid)
			beau_ipc.rx = ring;
	}

	return beau_ipc.tx != NULL && beau_ipc.rx != NULL ? 0 : -ENODEV;
}

static int __init beau_ipc_init(void)
{
	struct beau_ipc_ioc ioc;
	size_t map_size;
	int ret;

	memset(&beau_ipc, 0, sizeof(beau_ipc));
	mutex_init(&beau_ipc.hcall_lock);
	mutex_init(&beau_ipc.tx_lock);
	beau_ipc.channel_id = channel_id;

	ret = beau_ipc_hcall(BEAU_IPC_OP_QUERY, &ioc);
	if (ret == -ENODEV)
		return 0;
	if (ret)
		return ret;
	if (ioc.ring_count != BEAU_IPC_RING_COUNT ||
	    check_mul_overflow((size_t)ioc.ring_size, (size_t)ioc.ring_count, &map_size))
		return -EINVAL;

	beau_ipc.channel_id = ioc.channel_id;
	beau_ipc.peer_vmid = ioc.peer_vmid;
	beau_ipc.map_size = map_size;
	beau_ipc.base = memremap(ioc.gpa_base, map_size, MEMREMAP_WB);
	if (!beau_ipc.base)
		return -ENOMEM;

	ret = beau_ipc_select_rings(&ioc);
	if (ret)
		goto err_unmap;

	beau_ipc.thread = kthread_run(beau_ipc_thread_fn, NULL, "beau-ipc");
	if (IS_ERR(beau_ipc.thread)) {
		ret = PTR_ERR(beau_ipc.thread);
		beau_ipc.thread = NULL;
		goto err_unmap;
	}

	pr_info("[κ] BEAU IPC enabled: ch%u peer-vm%u gpa:0x%llx size:0x%x\n",
		beau_ipc.channel_id, beau_ipc.peer_vmid,
		(unsigned long long)ioc.gpa_base, ioc.ring_size);
	beau_ipc_send("linux-ipc-online");

	return 0;

err_unmap:
	memunmap(beau_ipc.base);
	beau_ipc.base = NULL;
	return ret;
}

static void __exit beau_ipc_exit(void)
{
	if (beau_ipc.thread)
		kthread_stop(beau_ipc.thread);
	if (beau_ipc.base)
		memunmap(beau_ipc.base);
}

module_init(beau_ipc_init);
module_exit(beau_ipc_exit);

MODULE_DESCRIPTION("BEAU static IPC validation endpoint");
MODULE_AUTHOR("BEAU");
MODULE_LICENSE("GPL");
