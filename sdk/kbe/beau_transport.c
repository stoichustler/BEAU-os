// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU VM1 HVC transport for AF_VSOCK.
 *
 * VM1 is the BEAU backend and owns CID 3.  VM2 and VM3 retain their standard
 * virtio-vsock frontends and use CIDs 4 and 5 respectively.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio_vsock.h>
#include <asm/memory.h>

#include <net/af_vsock.h>

#include "beau_vsock.h"

#define BEAU_VSOCK_REGISTER_RETRY_MS	1000U
#define BEAU_VSOCK_IDLE_POLL_FAST	8U
#define BEAU_VSOCK_IDLE_POLL_SLOW	32U
#define BEAU_VSOCK_SEND_RETRIES	32U
#define BEAU_VSOCK_HEARTBEAT_MS	1000U
#define BEAU_VSOCK_WAIT_US_MAX	1000U

struct beau_vsock {
	struct virtio_transport transport;
	struct task_struct *rx_thread;
	struct mutex tx_lock;
	struct beau_vsock_ioc *rx_ioc;
	struct beau_vsock_ioc *tx_ioc;
	void *rx_buf;
	void *tx_buf;
	u64 heartbeat_seq;
	unsigned long last_heartbeat;
};

static struct beau_vsock beau_vsock;

static bool beau_vsock_peer_allowed(u32 cid)
{
	return cid == BEAU_VSOCK_VM2_CID || cid == BEAU_VSOCK_VM3_CID;
}

static bool beau_vsock_peer_allowed_u64(u64 cid)
{
	return cid <= U32_MAX && beau_vsock_peer_allowed((u32)cid);
}

static long beau_vsock_hcall(struct beau_vsock_ioc *ioc)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(BEAU_VSOCK_HCALL_ID, virt_to_phys(ioc), &res);
	return res.a0;
}

static void beau_vsock_ioc_init(struct beau_vsock_ioc *ioc, u32 op)
{
	memset(ioc, 0, sizeof(*ioc));
	ioc->op = op;
	ioc->abi_version = BEAU_VSOCK_ABI_VERSION;
	ioc->ioc_size = sizeof(*ioc);
	ioc->local_cid = BEAU_VSOCK_VM1_CID;
}

static void beau_vsock_idle_delay(unsigned int *idle_polls, u32 wait_us)
{
	if (wait_us > BEAU_VSOCK_WAIT_US_MAX)
		wait_us = BEAU_VSOCK_WAIT_US_MAX;

	if (wait_us != 0U) {
		if (wait_us < 1000U)
			usleep_range(wait_us, wait_us * 2U);
		else
			msleep(DIV_ROUND_UP(wait_us, 1000U));
		return;
	}

	if (*idle_polls < BEAU_VSOCK_IDLE_POLL_FAST) {
		(*idle_polls)++;
		usleep_range(50, 100);
	} else if (*idle_polls < BEAU_VSOCK_IDLE_POLL_SLOW) {
		(*idle_polls)++;
		usleep_range(500, 1000);
	} else {
		msleep(1);
	}
}

static u32 beau_vsock_get_local_cid(void)
{
	return BEAU_VSOCK_VM1_CID;
}

static bool beau_vsock_has_remote_cid(struct vsock_sock *vsk, u32 cid)
{
	(void)vsk;
	return beau_vsock_peer_allowed(cid);
}

static bool beau_vsock_stream_allow(struct vsock_sock *vsk, u32 cid,
					  u32 port)
{
	(void)port;
	return vsock_net_mode_global(vsk) && beau_vsock_peer_allowed(cid);
}

static bool beau_vsock_msgzerocopy_allow(void)
{
	return false;
}

static int beau_vsock_cancel_pkt(struct vsock_sock *vsk)
{
	(void)vsk;
	/* HVC SEND_RX is synchronous, so no transport-owned skb remains. */
	return 0;
}

/* [20260806] VM1 packet ownership across HVC
 *
 * AF_VSOCK skb -> VM1 bounded copy buffer -> HVC -> BEAU RX queue
 *        |                                      |
 *        +--> failure: free skb, retain no guest reference
 *
 * Key rule:
 *   - tx_buf is owned by tx_lock until HVC returns;
 *   - the packet is validated before it is copied or made visible to BEAU;
 *   - bounded retry prevents a frontend with no RX descriptor from causing
 *     unbounded CPU use or an skb lifetime leak.
 */
static int beau_vsock_send_pkt(struct sk_buff *skb, struct net *net)
{
	struct beau_vsock_ioc *ioc = beau_vsock.tx_ioc;
	struct virtio_vsock_hdr *hdr;
	size_t packet_len;
	u32 payload_len;
	long ret = -EIO;
	unsigned int retry, idle_polls = 0U;

	(void)net;
	if (skb == NULL)
		return -EINVAL;

	hdr = virtio_vsock_hdr(skb);
	payload_len = le32_to_cpu(hdr->len);
	packet_len = sizeof(*hdr) + (size_t)payload_len;
	if (packet_len > BEAU_VSOCK_PACKET_MAX || payload_len != skb->len ||
	    le64_to_cpu(hdr->src_cid) != BEAU_VSOCK_VM1_CID ||
	    !beau_vsock_peer_allowed_u64(le64_to_cpu(hdr->dst_cid)) ||
	    le16_to_cpu(hdr->type) != VIRTIO_VSOCK_TYPE_STREAM) {
		kfree_skb(skb);
		return -EINVAL;
	}

	mutex_lock(&beau_vsock.tx_lock);
	memcpy(beau_vsock.tx_buf, hdr, sizeof(*hdr));
	if (payload_len && skb_copy_bits(skb, 0,
			beau_vsock.tx_buf + sizeof(*hdr), payload_len)) {
		ret = -EFAULT;
		goto out_unlock;
	}

	for (retry = 0U; retry < BEAU_VSOCK_SEND_RETRIES; retry++) {
		beau_vsock_ioc_init(ioc, BEAU_VSOCK_OP_SEND_RX);
		ioc->peer_cid = le64_to_cpu(hdr->dst_cid);
		ioc->packet_len = packet_len;
		ioc->buffer_len = packet_len;
		ioc->buffer_gpa = virt_to_phys(beau_vsock.tx_buf);
		ret = beau_vsock_hcall(ioc);
		if (ret == 0)
			break;
		if (ret != -EAGAIN && ret != -ENOBUFS && ret != -ENOSPC)
			break;
		beau_vsock_idle_delay(&idle_polls, ioc->wait_us);
	}

out_unlock:
	mutex_unlock(&beau_vsock.tx_lock);
	if (ret == 0) {
		virtio_transport_deliver_tap_pkt(skb);
		virtio_transport_consume_skb_sent(skb, true);
		return payload_len;
	}

	kfree_skb(skb);
	return ret;
}

static bool beau_vsock_valid_rx_packet(const struct beau_vsock_ioc *ioc)
{
	const struct virtio_vsock_hdr *hdr = beau_vsock.rx_buf;
	u32 payload_len;
	u16 op;

	if (ioc->packet_len < sizeof(*hdr) ||
	    ioc->packet_len > BEAU_VSOCK_PACKET_MAX ||
	    !beau_vsock_peer_allowed(ioc->peer_cid))
		return false;

	payload_len = le32_to_cpu(hdr->len);
	if ((size_t)payload_len + sizeof(*hdr) != ioc->packet_len ||
	    le64_to_cpu(hdr->src_cid) != ioc->peer_cid ||
	    le64_to_cpu(hdr->dst_cid) != BEAU_VSOCK_VM1_CID ||
	    le16_to_cpu(hdr->type) != VIRTIO_VSOCK_TYPE_STREAM)
		return false;

	op = le16_to_cpu(hdr->op);
	return op >= VIRTIO_VSOCK_OP_REQUEST &&
	       op <= VIRTIO_VSOCK_OP_CREDIT_REQUEST;
}

static void beau_vsock_deliver_rx(const struct beau_vsock_ioc *ioc)
{
	struct virtio_vsock_hdr *hdr = beau_vsock.rx_buf;
	struct sk_buff *skb;
	u32 payload_len = le32_to_cpu(hdr->len);

	skb = virtio_vsock_alloc_skb(ioc->packet_len, GFP_KERNEL);
	if (skb == NULL) {
		pr_warn_ratelimited("beau-vsock: drop %u-byte packet from CID %u: no memory\n",
				    ioc->packet_len, ioc->peer_cid);
		return;
	}

	memcpy(virtio_vsock_hdr(skb), hdr, sizeof(*hdr));
	if (payload_len) {
		virtio_vsock_skb_put(skb, payload_len);
		if (skb_store_bits(skb, 0, beau_vsock.rx_buf + sizeof(*hdr),
				   payload_len)) {
			kfree_skb(skb);
			return;
		}
	}

	virtio_transport_deliver_tap_pkt(skb);
	virtio_transport_recv_pkt(&beau_vsock.transport, skb, NULL);
}

static int beau_vsock_register(void)
{
	struct beau_vsock_ioc *ioc = beau_vsock.rx_ioc;
	long ret;

	beau_vsock_ioc_init(ioc, BEAU_VSOCK_OP_REGISTER);
	ioc->buffer_len = BEAU_VSOCK_PACKET_MAX;
	ret = beau_vsock_hcall(ioc);
	if (ret != 0)
		return ret;

	if (ioc->abi_version != BEAU_VSOCK_ABI_VERSION ||
	    ioc->ioc_size != sizeof(*ioc))
		return -EPROTO;

	return 0;
}

static void beau_vsock_heartbeat(bool force)
{
	struct beau_vsock_ioc *ioc = beau_vsock.rx_ioc;

	if (!force && time_before(jiffies, beau_vsock.last_heartbeat +
				  msecs_to_jiffies(BEAU_VSOCK_HEARTBEAT_MS)))
		return;

	beau_vsock_ioc_init(ioc, BEAU_VSOCK_OP_HEARTBEAT);
	ioc->heartbeat_seq = ++beau_vsock.heartbeat_seq;
	if (beau_vsock_hcall(ioc) == 0)
		beau_vsock.last_heartbeat = jiffies;
}

static int beau_vsock_rx_thread(void *data)
{
	bool registered = false;
	unsigned int idle_polls = 0U;

	(void)data;
	while (!kthread_should_stop()) {
		struct beau_vsock_ioc *ioc = beau_vsock.rx_ioc;
		long ret;

		if (!registered) {
			ret = beau_vsock_register();
			if (ret != 0) {
				msleep(BEAU_VSOCK_REGISTER_RETRY_MS);
				continue;
			}
			registered = true;
			idle_polls = 0U;
			beau_vsock_heartbeat(true);
		}
		beau_vsock_heartbeat(false);

		beau_vsock_ioc_init(ioc, BEAU_VSOCK_OP_POLL_TX);
		ioc->buffer_len = BEAU_VSOCK_PACKET_MAX;
		ioc->buffer_gpa = virt_to_phys(beau_vsock.rx_buf);
		ret = beau_vsock_hcall(ioc);
		if (ret == -ENODATA) {
			beau_vsock_idle_delay(&idle_polls, ioc->wait_us);
			continue;
		}
		if (ret == -ECONNRESET) {
			registered = false;
			continue;
		}
		if (ret != 0) {
			pr_warn_ratelimited("beau-vsock: TX poll failed: %ld\n", ret);
			beau_vsock_idle_delay(&idle_polls, ioc->wait_us);
			continue;
		}

		idle_polls = 0U;
		if (!beau_vsock_valid_rx_packet(ioc)) {
			pr_warn_ratelimited("beau-vsock: rejected malformed packet from CID %u\n",
				    ioc->peer_cid);
			continue;
		}
		beau_vsock_deliver_rx(ioc);
	}

	return 0;
}

static struct virtio_transport beau_vsock_transport = {
	.transport = {
		.module			= THIS_MODULE,
		.get_local_cid		= beau_vsock_get_local_cid,
		.has_remote_cid		= beau_vsock_has_remote_cid,
		.init			= virtio_transport_do_socket_init,
		.destruct		= virtio_transport_destruct,
		.release		= virtio_transport_release,
		.connect		= virtio_transport_connect,
		.shutdown		= virtio_transport_shutdown,
		.cancel_pkt		= beau_vsock_cancel_pkt,
		.dgram_bind		= virtio_transport_dgram_bind,
		.dgram_dequeue		= virtio_transport_dgram_dequeue,
		.dgram_enqueue		= virtio_transport_dgram_enqueue,
		.dgram_allow		= virtio_transport_dgram_allow,
		.stream_dequeue		= virtio_transport_stream_dequeue,
		.stream_enqueue		= virtio_transport_stream_enqueue,
		.stream_has_data	= virtio_transport_stream_has_data,
		.stream_has_space	= virtio_transport_stream_has_space,
		.stream_rcvhiwat	= virtio_transport_stream_rcvhiwat,
		.stream_is_active	= virtio_transport_stream_is_active,
		.stream_allow		= beau_vsock_stream_allow,
		.seqpacket_dequeue	= virtio_transport_seqpacket_dequeue,
		.seqpacket_enqueue	= virtio_transport_seqpacket_enqueue,
		.seqpacket_has_data	= virtio_transport_seqpacket_has_data,
		.msgzerocopy_allow	= beau_vsock_msgzerocopy_allow,
		.notify_poll_in		= virtio_transport_notify_poll_in,
		.notify_poll_out	= virtio_transport_notify_poll_out,
		.notify_recv_init	= virtio_transport_notify_recv_init,
		.notify_recv_pre_block	= virtio_transport_notify_recv_pre_block,
		.notify_recv_pre_dequeue	= virtio_transport_notify_recv_pre_dequeue,
		.notify_recv_post_dequeue = virtio_transport_notify_recv_post_dequeue,
		.notify_send_init	= virtio_transport_notify_send_init,
		.notify_send_pre_block	= virtio_transport_notify_send_pre_block,
		.notify_send_pre_enqueue	= virtio_transport_notify_send_pre_enqueue,
		.notify_send_post_enqueue = virtio_transport_notify_send_post_enqueue,
		.notify_buffer_size	= virtio_transport_notify_buffer_size,
		.notify_set_rcvlowat	= virtio_transport_notify_set_rcvlowat,
		.unsent_bytes		= virtio_transport_unsent_bytes,
		.read_skb		= virtio_transport_read_skb,
	},
	.send_pkt = beau_vsock_send_pkt,
};

static int __init beau_vsock_init(void)
{
	int ret;
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "beau,vsock-backend");
	if (node == NULL)
		return 0;
	of_node_put(node);

	beau_vsock.rx_buf = kmalloc(BEAU_VSOCK_PACKET_MAX, GFP_KERNEL);
	beau_vsock.tx_buf = kmalloc(BEAU_VSOCK_PACKET_MAX, GFP_KERNEL);
	beau_vsock.rx_ioc = kzalloc(sizeof(*beau_vsock.rx_ioc), GFP_KERNEL);
	beau_vsock.tx_ioc = kzalloc(sizeof(*beau_vsock.tx_ioc), GFP_KERNEL);
	if (beau_vsock.rx_buf == NULL || beau_vsock.tx_buf == NULL ||
	    beau_vsock.rx_ioc == NULL || beau_vsock.tx_ioc == NULL) {
		ret = -ENOMEM;
		goto out_free;
	}

	mutex_init(&beau_vsock.tx_lock);
	beau_vsock.transport = beau_vsock_transport;
	ret = vsock_core_register(&beau_vsock.transport.transport,
				  VSOCK_TRANSPORT_F_H2G | VSOCK_TRANSPORT_F_G2H);
	if (ret != 0)
		goto out_free;

	beau_vsock.rx_thread = kthread_run(beau_vsock_rx_thread, NULL,
					   "beau-vsock-rx");
	if (IS_ERR(beau_vsock.rx_thread)) {
		ret = PTR_ERR(beau_vsock.rx_thread);
		beau_vsock.rx_thread = NULL;
		goto out_unregister;
	}

	pr_info("beau-vsock: VM1 CID %u ready for CIDs %u and %u\n",
		BEAU_VSOCK_VM1_CID, BEAU_VSOCK_VM2_CID, BEAU_VSOCK_VM3_CID);
	return 0;

out_unregister:
	vsock_core_unregister(&beau_vsock.transport.transport);
out_free:
	kfree(beau_vsock.tx_buf);
	kfree(beau_vsock.rx_buf);
	kfree(beau_vsock.tx_ioc);
	kfree(beau_vsock.rx_ioc);
	beau_vsock.tx_buf = NULL;
	beau_vsock.rx_buf = NULL;
	beau_vsock.tx_ioc = NULL;
	beau_vsock.rx_ioc = NULL;
	return ret;
}

static void __exit beau_vsock_exit(void)
{
	if (beau_vsock.rx_thread != NULL)
		kthread_stop(beau_vsock.rx_thread);
	vsock_core_unregister(&beau_vsock.transport.transport);
	kfree(beau_vsock.tx_buf);
	kfree(beau_vsock.rx_buf);
	kfree(beau_vsock.tx_ioc);
	kfree(beau_vsock.rx_ioc);
	beau_vsock.tx_buf = NULL;
	beau_vsock.rx_buf = NULL;
	beau_vsock.tx_ioc = NULL;
	beau_vsock.rx_ioc = NULL;
}

module_init(beau_vsock_init);
module_exit(beau_vsock_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BEAU OS");
MODULE_DESCRIPTION("BEAU VM1 HVC transport for AF_VSOCK");
MODULE_ALIAS_NETPROTO(PF_VSOCK);
