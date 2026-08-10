// SPDX-License-Identifier: GPL-2.0-only
/* BEAU static shared-memory IPC ring platform driver. */
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/uaccess.h>

#include "hcall.h"

#define BEAU_IPC_MESSAGE_MAX 1024U

struct beau_ipc_ring_dev {
	struct device *dev;
	u32 channel_id;
	struct beau_ipc_ioc ioc;
	struct beau_ipc_ring_header *tx;
	struct beau_ipc_ring_header *rx;
	struct mutex lock;
	wait_queue_head_t rx_wait;
	struct miscdevice misc;
};

static bool beau_ipc_ring_valid(const struct beau_ipc_ring_header *ring)
{
	return ring != NULL && READ_ONCE(ring->magic) == BEAU_IPC_RING_MAGIC &&
		READ_ONCE(ring->version) == BEAU_IPC_ABI_VERSION &&
		READ_ONCE(ring->header_size) >= sizeof(*ring) &&
		READ_ONCE(ring->header_size) < READ_ONCE(ring->ring_size) &&
		READ_ONCE(ring->elem_size) == 1U &&
		READ_ONCE(ring->elem_count) <=
		(READ_ONCE(ring->ring_size) - READ_ONCE(ring->header_size));
}

static u8 *beau_ipc_data(struct beau_ipc_ring_header *ring)
{
	return (u8 *)ring + READ_ONCE(ring->header_size);
}

static long beau_ipc_hcall(struct beau_ipc_ring_dev *ipc, u32 op)
{
	long ret;

	memset(&ipc->ioc, 0, sizeof(ipc->ioc));
	ipc->ioc.op = op;
	ipc->ioc.abi_version = BEAU_IPC_ABI_VERSION;
	ipc->ioc.ioc_size = sizeof(ipc->ioc);
	ipc->ioc.channel_id = ipc->channel_id;
	ret = beau_hcall_ipc(&ipc->ioc);
	if (ret != 0)
		return ret;
	return ipc->ioc.status == BEAU_IPC_STATUS_OK ? 0 : -ENODEV;
}

static int beau_ipc_write_msg(struct beau_ipc_ring_header *ring, const u8 *data,
			      size_t length)
{
	u32 cap, need, idx;
	u64 prod, cons, used;

	if (!beau_ipc_ring_valid(ring) || length == 0U || length > BEAU_IPC_MESSAGE_MAX)
		return -EINVAL;
	cap = READ_ONCE(ring->elem_count);
	need = (u32)length + 2U;
	prod = READ_ONCE(ring->prod);
	cons = smp_load_acquire(&ring->cons);
	used = prod - cons;
	if (used > cap)
		return -EIO;
	if ((cap - used) < need)
		return -ENOSPC;
	beau_ipc_data(ring)[prod % cap] = (u8)(length & 0xffU);
	beau_ipc_data(ring)[(prod + 1U) % cap] = (u8)(length >> 8U);
	for (idx = 0U; idx < length; idx++)
		beau_ipc_data(ring)[(prod + 2U + idx) % cap] = data[idx];
	WRITE_ONCE(ring->bytes, READ_ONCE(ring->bytes) + length);
	smp_store_release(&ring->prod, prod + need);
	return 0;
}

static int beau_ipc_read_msg(struct beau_ipc_ring_header *ring, u8 *data,
			     size_t capacity)
{
	u32 cap, idx;
	u64 prod, cons, available;
	u16 length;

	if (!beau_ipc_ring_valid(ring))
		return -EIO;
	cap = READ_ONCE(ring->elem_count);
	cons = READ_ONCE(ring->cons);
	prod = smp_load_acquire(&ring->prod);
	available = prod - cons;
	if (available == 0U)
		return 0;
	if ((available > cap) || (available < 2U))
		return -EIO;
	length = beau_ipc_data(ring)[cons % cap];
	length |= (u16)beau_ipc_data(ring)[(cons + 1U) % cap] << 8U;
	if (length == 0U || length > capacity || ((u32)length + 2U) > available)
		return -EMSGSIZE;
	for (idx = 0U; idx < length; idx++)
		data[idx] = beau_ipc_data(ring)[(cons + 2U + idx) % cap];
	smp_store_release(&ring->cons, cons + (u32)length + 2U);
	return length;
}

static irqreturn_t beau_ipc_irq(int irq, void *arg)
{
	struct beau_ipc_ring_dev *ipc = arg;

	(void)irq;
	wake_up_interruptible(&ipc->rx_wait);
	return IRQ_HANDLED;
}

static ssize_t beau_ipc_read(struct file *file, char __user *buffer, size_t length,
			     loff_t *offset)
{
	struct beau_ipc_ring_dev *ipc = container_of(file->private_data,
		struct beau_ipc_ring_dev, misc);
	u8 message[BEAU_IPC_MESSAGE_MAX];
	int ret;

	(void)offset;
	if (length < 1U)
		return -EINVAL;
	for (;;) {
		ret = beau_ipc_read_msg(ipc->rx, message, min_t(size_t, length, sizeof(message)));
		if (ret != 0)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(ipc->rx_wait,
			smp_load_acquire(&ipc->rx->prod) != READ_ONCE(ipc->rx->cons));
		if (ret != 0)
			return ret;
	}
	if (ret < 0)
		return ret;
	if (copy_to_user(buffer, message, ret))
		return -EFAULT;
	(void)beau_ipc_hcall(ipc, BEAU_IPC_OP_ACK);
	return ret;
}

static ssize_t beau_ipc_write(struct file *file, const char __user *buffer, size_t length,
			      loff_t *offset)
{
	struct beau_ipc_ring_dev *ipc = container_of(file->private_data,
		struct beau_ipc_ring_dev, misc);
	u8 message[BEAU_IPC_MESSAGE_MAX];
	int ret;

	(void)offset;
	if (length == 0U || length > sizeof(message))
		return -EMSGSIZE;
	if (copy_from_user(message, buffer, length))
		return -EFAULT;
	mutex_lock(&ipc->lock);
	ret = beau_ipc_write_msg(ipc->tx, message, length);
	if (ret == 0)
		ret = beau_ipc_hcall(ipc, BEAU_IPC_OP_NOTIFY);
	mutex_unlock(&ipc->lock);
	return ret == 0 ? length : ret;
}

static __poll_t beau_ipc_poll(struct file *file, poll_table *wait)
{
	struct beau_ipc_ring_dev *ipc = container_of(file->private_data,
		struct beau_ipc_ring_dev, misc);

	poll_wait(file, &ipc->rx_wait, wait);
	return smp_load_acquire(&ipc->rx->prod) != READ_ONCE(ipc->rx->cons) ?
		EPOLLIN | EPOLLRDNORM : 0U;
}

static const struct file_operations beau_ipc_fops = {
	.owner = THIS_MODULE, .read = beau_ipc_read, .write = beau_ipc_write,
	.poll = beau_ipc_poll, .llseek = noop_llseek,
};

static int beau_ipc_probe(struct platform_device *pdev)
{
	struct beau_ipc_ring_dev *ipc;
	struct resource *res;
	u32 channel_id;
	int irq, ret, dir;

	if (device_property_read_u32(&pdev->dev, "beau,channel-id", &channel_id))
		return -EINVAL;
	ipc = devm_kzalloc(&pdev->dev, sizeof(*ipc), GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;
	ipc->dev = &pdev->dev;
	ipc->channel_id = channel_id;
	ret = beau_ipc_hcall(ipc, BEAU_IPC_OP_QUERY);
	if (ret != 0 || (ipc->ioc.flags & BEAU_IPC_FLAG_NOTIFY_IRQ) == 0U)
		return ret != 0 ? ret : -EPROTO;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || ipc->ioc.gpa_base != res->start ||
		((u64)ipc->ioc.ring_size * ipc->ioc.ring_count) != resource_size(res))
		return -EINVAL;
	ipc->tx = devm_memremap(&pdev->dev, res->start, resource_size(res), MEMREMAP_WB);
	if (IS_ERR_OR_NULL(ipc->tx))
		return -ENOMEM;
	for (dir = 0; dir < BEAU_IPC_RING_COUNT; dir++) {
		struct beau_ipc_ring_header *ring = (void *)ipc->tx + dir * ipc->ioc.ring_size;
		if (!beau_ipc_ring_valid(ring))
			return -EPROTO;
		if (READ_ONCE(ring->peer_vmid) == ipc->ioc.peer_vmid)
			ipc->tx = ring;
		if (READ_ONCE(ring->owner_vmid) == ipc->ioc.peer_vmid)
			ipc->rx = ring;
	}
	if (!ipc->tx || !ipc->rx)
		return -EPROTO;
	mutex_init(&ipc->lock);
	init_waitqueue_head(&ipc->rx_wait);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(&pdev->dev, irq, beau_ipc_irq, 0, dev_name(&pdev->dev), ipc);
	if (ret)
		return ret;
	ipc->misc.minor = MISC_DYNAMIC_MINOR;
	ipc->misc.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "beau-ipc-%u", channel_id);
	ipc->misc.fops = &beau_ipc_fops;
	ipc->misc.parent = &pdev->dev;
	ret = misc_register(&ipc->misc);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, ipc);
	return 0;
}

static void beau_ipc_remove(struct platform_device *pdev)
{
	struct beau_ipc_ring_dev *ipc = platform_get_drvdata(pdev);
	if (ipc)
		misc_deregister(&ipc->misc);
}

static const struct of_device_id beau_ipc_of_match[] = {
	{ .compatible = "beau,ipc-ring" }, { }
};
MODULE_DEVICE_TABLE(of, beau_ipc_of_match);
static struct platform_driver beau_ipc_driver = {
	.probe = beau_ipc_probe, .remove = beau_ipc_remove,
	.driver = { .name = "beau-ipc", .of_match_table = beau_ipc_of_match },
};
module_platform_driver(beau_ipc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BEAU shared-memory IPC ring driver");
