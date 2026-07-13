// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU virtio-net backend for the VM2 <-> virtio_proxy <-> VM3 test path.
 *
 * VM3 uses the standard virtio-net frontend. BEAU only transports descriptor
 * bytes. VM2 owns the network backend semantics and forwards VM3 Ethernet
 * frames through one VM2 uplink netdev.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio_net.h>
#include <linux/wait.h>
#include <net/net_namespace.h>
#include <asm/memory.h>

#include "virtio-proxy-backend.h"

#define BEAU_PROXY_DEVICE_NET		1U
#define BEAU_NET_QUEUE_RX		0U
#define BEAU_NET_QUEUE_TX		1U
#define BEAU_NET_RX_BACKLOG_MAX		128U
#define BEAU_NET_HEARTBEAT_MS		1000U
#define BEAU_NET_REGISTER_RETRY_MS	1000U
#define BEAU_NET_UPLINK_RETRY_MS	1000U
#define BEAU_NET_FRAME_MAX		(BEAU_PROXY_DATA_MAX - sizeof(struct virtio_net_hdr))
#define BEAU_NET_FEATURES		((1ULL << VIRTIO_NET_F_MAC) | \
					 (1ULL << VIRTIO_NET_F_STATUS))

struct beau_net_config {
	u8 mac[ETH_ALEN];
	__le16 status;
} __packed;

struct beau_net_frame {
	struct list_head node;
	u32 len;
	u8 data[];
};

struct beau_net_backend {
	struct beau_proxy_backend proxy;
	struct net_device *dev;
	struct packet_type packet_tap;
	struct beau_net_config config;
	u8 mac[ETH_ALEN];
	spinlock_t rx_lock;
	struct list_head rx_frames;
	wait_queue_head_t rx_wait;
	u32 rx_queued;
	u64 tx_packets;
	u64 tx_dropped;
	u64 rx_packets;
	u64 rx_dropped;
};

static char *beau_net_uplink = "eth0";
module_param_named(uplink, beau_net_uplink, charp, 0444);
MODULE_PARM_DESC(uplink, "VM2 uplink netdev used by the BEAU virtio-net backend");

static struct beau_net_backend beau_net_backend = {
	.mac = { 0x52, 0x54, 0x00, 0xbe, 0x03, 0x00 },
};

static int beau_net_open_uplink(struct beau_net_backend *net);

static struct net_device *beau_net_find_auto_uplink(void)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if ((dev->flags & IFF_LOOPBACK) != 0)
			continue;

		dev_hold(dev);
		rtnl_unlock();
		pr_info("BEAU virtio-net backend auto-selected uplink %s (requested %s)\n",
			dev->name, beau_net_uplink);
		return dev;
	}
	rtnl_unlock();

	return NULL;
}

static struct net_device *beau_net_get_uplink(void)
{
	struct net_device *dev;

	dev = dev_get_by_name(&init_net, beau_net_uplink);
	if (dev != NULL)
		return dev;

	return beau_net_find_auto_uplink();
}

static void beau_net_fill_common(struct beau_net_backend *net,
				 struct beau_proxy_ioc *ioc, u32 op)
{
	memset(ioc, 0, sizeof(*ioc));
	ioc->op = op;
	ioc->device_id = BEAU_PROXY_DEVICE_NET;
	ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM3;
	ioc->abi_version = BEAU_PROXY_ABI_VERSION;
	ioc->ioc_size = sizeof(*ioc);
	ioc->backend_caps = BEAU_PROXY_CAP_WAIT_HINT |
		BEAU_PROXY_CAP_HEARTBEAT | BEAU_PROXY_CAP_STATS;
}

static int beau_net_register_backend(struct beau_net_backend *net)
{
	struct beau_proxy_ioc *ioc = &net->proxy.ioc;
	long ret;

	while (!kthread_should_stop()) {
		beau_net_fill_common(net, ioc, BEAU_PROXY_OP_REGISTER);
		ioc->device_features = BEAU_NET_FEATURES;
		ioc->config_gpa = virt_to_phys(&net->config);
		ioc->config_len = sizeof(net->config);
		ioc->register_flags = BEAU_PROXY_REG_F_FEATURES |
			BEAU_PROXY_REG_F_CONFIG;

		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (ret == 0) {
			net->proxy.negotiated_caps = ioc->backend_caps;
			net->proxy.last_heartbeat = jiffies;
			return 0;
		}
		msleep(BEAU_NET_REGISTER_RETRY_MS);
	}

	return -EINTR;
}

static void beau_net_heartbeat(struct beau_net_backend *net, bool force)
{
	struct beau_proxy_ioc *ioc = &net->proxy.ioc;
	long ret;

	if ((net->proxy.negotiated_caps & BEAU_PROXY_CAP_HEARTBEAT) == 0U)
		return;
	if (!force && time_before(jiffies, net->proxy.last_heartbeat +
				  net->proxy.heartbeat_interval))
		return;

	beau_net_fill_common(net, ioc, BEAU_PROXY_OP_HEARTBEAT);
	ioc->heartbeat_seq = ++net->proxy.heartbeat_seq;
	ret = beau_hcall_virtio_proxy_backend(ioc);
	if (ret == 0)
		net->proxy.last_heartbeat = jiffies;
}

static void beau_net_idle_sleep(struct beau_net_backend *net,
				const struct beau_proxy_ioc *ioc, long ret)
{
	u32 wait_us = 0U;

	beau_net_heartbeat(net, false);
	if ((ret == -ENODATA) &&
	    ((net->proxy.negotiated_caps & BEAU_PROXY_CAP_WAIT_HINT) != 0U))
		wait_us = ioc->wait_us;

	if (wait_us == 0U) {
		beau_proxy_poll_idle_delay(&net->proxy.idle_polls);
	} else if (wait_us < 1000U) {
		usleep_range(wait_us, wait_us * 2U);
	} else {
		msleep(DIV_ROUND_UP(wait_us, 1000U));
	}
}

static long beau_net_poll_queue(struct beau_net_backend *net, u16 queue)
{
	struct beau_proxy_ioc *ioc = &net->proxy.ioc;

	beau_net_fill_common(net, ioc, BEAU_PROXY_OP_POLL);
	ioc->queue_id = queue;
	ioc->in_gpa = virt_to_phys(net->proxy.in);
	ioc->in_len = BEAU_PROXY_DATA_MAX;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static bool beau_net_rx_match(const struct sk_buff *skb, int mac_offset)
{
	struct ethhdr eth;

	if (skb_copy_bits(skb, mac_offset, &eth, sizeof(eth)) != 0)
		return false;

	if (ether_addr_equal(eth.h_source, beau_net_backend.mac))
		return false;

	return ether_addr_equal(eth.h_dest, beau_net_backend.mac) ||
		is_broadcast_ether_addr(eth.h_dest) ||
		is_multicast_ether_addr(eth.h_dest);
}

static void beau_net_rx_enqueue(struct beau_net_backend *net,
				struct beau_net_frame *frame)
{
	unsigned long flags;
	bool drop = false;

	spin_lock_irqsave(&net->rx_lock, flags);
	if (net->rx_queued >= BEAU_NET_RX_BACKLOG_MAX) {
		drop = true;
	} else {
		list_add_tail(&frame->node, &net->rx_frames);
		net->rx_queued++;
	}
	spin_unlock_irqrestore(&net->rx_lock, flags);

	if (drop) {
		net->rx_dropped++;
		kfree(frame);
	} else {
		wake_up_interruptible(&net->rx_wait);
	}
}

static int beau_net_packet_tap(struct sk_buff *skb, struct net_device *dev,
			       struct packet_type *pt,
			       struct net_device *orig_dev)
{
	struct beau_net_backend *net = &beau_net_backend;
	struct beau_net_frame *frame;
	int mac_offset;
	u32 frame_len;

	if (!net->dev || skb->dev != net->dev || !skb_mac_header_was_set(skb))
		return 0;

	mac_offset = skb_mac_header(skb) - skb->data;
	if (mac_offset > 0)
		return 0;

	frame_len = skb->len + (u32)(-mac_offset);
	if ((frame_len < ETH_HLEN) || (frame_len > BEAU_NET_FRAME_MAX) ||
	    !beau_net_rx_match(skb, mac_offset))
		return 0;

	frame = kmalloc(sizeof(*frame) + frame_len, GFP_ATOMIC);
	if (frame == NULL) {
		net->rx_dropped++;
		return 0;
	}
	INIT_LIST_HEAD(&frame->node);
	frame->len = frame_len;
	if (skb_copy_bits(skb, mac_offset, frame->data, frame_len) != 0) {
		net->rx_dropped++;
		kfree(frame);
		return 0;
	}

	beau_net_rx_enqueue(net, frame);
	return 0;
}

static struct beau_net_frame *beau_net_rx_peek(struct beau_net_backend *net)
{
	struct beau_net_frame *frame = NULL;
	unsigned long flags;

	spin_lock_irqsave(&net->rx_lock, flags);
	if (!list_empty(&net->rx_frames))
		frame = list_first_entry(&net->rx_frames,
					 struct beau_net_frame, node);
	spin_unlock_irqrestore(&net->rx_lock, flags);
	return frame;
}

static void beau_net_rx_pop(struct beau_net_backend *net,
			    struct beau_net_frame *frame)
{
	unsigned long flags;

	spin_lock_irqsave(&net->rx_lock, flags);
	list_del(&frame->node);
	net->rx_queued--;
	spin_unlock_irqrestore(&net->rx_lock, flags);
}

static bool beau_net_rx_pending(struct beau_net_backend *net)
{
	bool pending;
	unsigned long flags;

	spin_lock_irqsave(&net->rx_lock, flags);
	pending = !list_empty(&net->rx_frames);
	spin_unlock_irqrestore(&net->rx_lock, flags);
	return pending;
}

static void beau_net_rx_purge(struct beau_net_backend *net)
{
	struct beau_net_frame *frame;
	struct beau_net_frame *next;
	unsigned long flags;
	LIST_HEAD(local);

	spin_lock_irqsave(&net->rx_lock, flags);
	list_splice_init(&net->rx_frames, &local);
	net->rx_queued = 0U;
	spin_unlock_irqrestore(&net->rx_lock, flags);

	list_for_each_entry_safe(frame, next, &local, node) {
		list_del(&frame->node);
		kfree(frame);
	}
}

static int beau_net_handle_tx(struct beau_net_backend *net,
			      struct beau_proxy_ioc *ioc)
{
	const struct virtio_net_hdr *hdr = net->proxy.in;
	const struct ethhdr *eth;
	struct sk_buff *skb;
	u32 frame_len;
	long ret;

	if ((ioc->queue_id != BEAU_NET_QUEUE_TX) ||
	    (ioc->in_len < sizeof(*hdr) + ETH_HLEN)) {
		net->tx_dropped++;
		return beau_proxy_backend_reply_empty(ioc);
	}

	frame_len = ioc->in_len - sizeof(*hdr);
	if (frame_len > BEAU_NET_FRAME_MAX) {
		net->tx_dropped++;
		return beau_proxy_backend_reply_empty(ioc);
	}

	eth = (const struct ethhdr *)((const u8 *)net->proxy.in + sizeof(*hdr));
	if (!ether_addr_equal(eth->h_source, net->mac)) {
		net->tx_dropped++;
		return beau_proxy_backend_reply_empty(ioc);
	}
	if (!netif_running(net->dev)) {
		net->tx_dropped++;
		return beau_proxy_backend_reply_empty(ioc);
	}

	skb = netdev_alloc_skb(net->dev, frame_len + NET_IP_ALIGN);
	if (skb == NULL) {
		net->tx_dropped++;
		return beau_proxy_backend_reply_empty(ioc);
	}

	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, eth, frame_len);
	skb->dev = net->dev;
	skb_reset_mac_header(skb);
	skb->protocol = eth->h_proto;
	skb->ip_summed = CHECKSUM_NONE;

	ret = dev_queue_xmit(skb);
	if (ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN)
		net->tx_packets++;
	else
		net->tx_dropped++;

	return beau_proxy_backend_reply_empty(ioc);
}

static int beau_net_handle_rx(struct beau_net_backend *net,
			      struct beau_proxy_ioc *ioc)
{
	struct beau_net_frame *frame = beau_net_rx_peek(net);
	struct virtio_net_hdr *hdr = net->proxy.out;
	u32 total_len;
	int ret;

	if (frame == NULL)
		return -ENODATA;

	if (ioc->queue_id != BEAU_NET_QUEUE_RX || ioc->out_len == 0U) {
		net->rx_dropped++;
		return beau_proxy_backend_reply_empty(ioc);
	}

	total_len = sizeof(*hdr) + frame->len;
	if ((total_len > ioc->out_len) || (total_len > BEAU_PROXY_DATA_MAX)) {
		beau_net_rx_pop(net, frame);
		net->rx_dropped++;
		kfree(frame);
		return beau_proxy_backend_reply_empty(ioc);
	}

	memset(hdr, 0, sizeof(*hdr));
	memcpy((u8 *)net->proxy.out + sizeof(*hdr), frame->data, frame->len);
	beau_net_rx_pop(net, frame);
	ret = beau_proxy_backend_reply(ioc, net->proxy.out, total_len);
	if (ret == 0)
		net->rx_packets++;
	else
		net->rx_dropped++;
	kfree(frame);
	return ret;
}

static int beau_net_thread(void *data)
{
	struct beau_net_backend *net = data;
	long ret = 0;

	while (!kthread_should_stop()) {
		ret = beau_net_open_uplink(net);
		if (ret == 0) {
			pr_info("BEAU virtio-net backend uplink ready, uplink=%s mac=%pM\n",
				beau_net_uplink, net->mac);
			break;
		}

		pr_info_once("BEAU virtio-net backend waiting for uplink %s: %ld\n",
			     beau_net_uplink, ret);
		msleep(BEAU_NET_UPLINK_RETRY_MS);
	}
	if (kthread_should_stop())
		return -EINTR;

	ret = beau_net_register_backend(net);
	if (ret != 0)
		return ret;

	beau_net_heartbeat(net, true);
	while (!kthread_should_stop()) {
		bool worked = false;

		ret = beau_net_poll_queue(net, BEAU_NET_QUEUE_TX);
		if (ret == 0) {
			beau_proxy_poll_active(&net->proxy.idle_polls);
			(void)beau_net_handle_tx(net, &net->proxy.ioc);
			worked = true;
		}

		if (beau_net_rx_pending(net)) {
			ret = beau_net_poll_queue(net, BEAU_NET_QUEUE_RX);
			if (ret == 0) {
				beau_proxy_poll_active(&net->proxy.idle_polls);
				(void)beau_net_handle_rx(net, &net->proxy.ioc);
				worked = true;
			}
		}

		if (!worked)
			beau_net_idle_sleep(net, &net->proxy.ioc, ret);
	}

	return 0;
}

static int beau_net_open_uplink(struct beau_net_backend *net)
{
	int ret;

	net->dev = beau_net_get_uplink();
	if (net->dev == NULL)
		return -ENODEV;

	rtnl_lock();
	if (!netif_running(net->dev)) {
		ret = dev_open(net->dev, NULL);
		if (ret != 0)
			goto out_unlock;
	}

	ret = dev_set_promiscuity(net->dev, 1);
out_unlock:
	rtnl_unlock();
	if (ret != 0) {
		dev_put(net->dev);
		net->dev = NULL;
		return ret;
	}

	net->packet_tap.type = cpu_to_be16(ETH_P_ALL);
	net->packet_tap.dev = net->dev;
	net->packet_tap.func = beau_net_packet_tap;
	net->packet_tap.ignore_outgoing = true;
	dev_add_pack(&net->packet_tap);
	return 0;
}

static void beau_net_close_uplink(struct beau_net_backend *net)
{
	if (net->dev == NULL)
		return;

	dev_remove_pack(&net->packet_tap);
	rtnl_lock();
	(void)dev_set_promiscuity(net->dev, -1);
	rtnl_unlock();
	dev_put(net->dev);
	net->dev = NULL;
}

static int __init beau_virtionet_backend_init(void)
{
	struct beau_proxy_backend *proxy = &beau_net_backend.proxy;
	int ret;

	if (!beau_proxy_backend_is_vm2())
		return 0;

	spin_lock_init(&beau_net_backend.rx_lock);
	INIT_LIST_HEAD(&beau_net_backend.rx_frames);
	init_waitqueue_head(&beau_net_backend.rx_wait);
	memcpy(beau_net_backend.config.mac, beau_net_backend.mac, ETH_ALEN);
	beau_net_backend.config.status = cpu_to_le16(VIRTIO_NET_S_LINK_UP);

	proxy->name = "virtio-net";
	proxy->thread_name = "beau-virtionet-backend";
	proxy->device_id = BEAU_PROXY_DEVICE_NET;
	proxy->frontend_vmid = BEAU_PROXY_FRONTEND_VM3;
	proxy->heartbeat_interval = msecs_to_jiffies(BEAU_NET_HEARTBEAT_MS);

	ret = beau_proxy_backend_alloc_io(proxy);
	if (ret != 0)
		return ret;

	proxy->thread = kthread_run(beau_net_thread, &beau_net_backend,
				    "%s", proxy->thread_name);
	if (IS_ERR(proxy->thread)) {
		ret = PTR_ERR(proxy->thread);
		proxy->thread = NULL;
		beau_proxy_backend_free_io(proxy);
		return ret;
	}

	pr_info("BEAU virtio-net backend worker started, uplink=%s\n",
		beau_net_uplink);
	return 0;
}

static void __exit beau_virtionet_backend_exit(void)
{
	if (beau_net_backend.proxy.thread) {
		kthread_stop(beau_net_backend.proxy.thread);
		beau_net_backend.proxy.thread = NULL;
	}
	beau_net_close_uplink(&beau_net_backend);
	beau_net_rx_purge(&beau_net_backend);
	beau_proxy_backend_free_io(&beau_net_backend.proxy);
	pr_info("BEAU virtio-net backend stopped, tx=%llu/%llu rx=%llu/%llu\n",
		beau_net_backend.tx_packets, beau_net_backend.tx_dropped,
		beau_net_backend.rx_packets, beau_net_backend.rx_dropped);
}

late_initcall(beau_virtionet_backend_init);
module_exit(beau_virtionet_backend_exit);

MODULE_DESCRIPTION("BEAU VM2 virtio-net uplink backend");
MODULE_LICENSE("GPL");
