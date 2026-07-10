// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU virtio-blk backend for the VM1 <-> virtio_proxy <-> VM2 test path.
 *
 * This is intentionally a RAM-backed validation target. VM2's standard
 * virtio-blk frontend owns queueing and request formatting, BEAU owns only
 * descriptor transport, and VM1 owns the block protocol semantics.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio_blk.h>
#include <linux/vmalloc.h>
#include <asm/memory.h>

#include "hcall.h"

#define BEAU_PROXY_FRONTEND_VM2		2U
#define BEAU_PROXY_DEVICE_BLK		2U
#define BEAU_BLK_QUEUE_REQUEST		0U
#define BEAU_BLK_SECTOR_SIZE		512U
#define BEAU_BLK_DISK_BYTES		(1024U * 1024U)
#define BEAU_BLK_ID			"beau-ramblk"

struct beau_blk_config {
	u64 capacity;
	u32 size_max;
	u32 seg_max;
	u16 cylinders;
	u8 heads;
	u8 sectors;
	u32 blk_size;
	u8 physical_block_exp;
	u8 alignment_offset;
	u16 min_io_size;
	u32 opt_io_size;
	u8 wce;
	u8 unused;
	u16 num_queues;
};

struct beau_blk_backend {
	struct task_struct *thread;
	void *in;
	void *out;
	u8 *disk;
	struct beau_blk_config config;
	struct beau_proxy_ioc ioc;
};

static struct beau_blk_backend beau_blk_backend;

static u32 beau_blk_req_type(const struct virtio_blk_outhdr *hdr)
{
	return (__force u32)hdr->type & ~VIRTIO_BLK_T_BARRIER;
}

static u64 beau_blk_req_sector(const struct virtio_blk_outhdr *hdr)
{
	return (__force u64)hdr->sector;
}

static int beau_blk_reply(struct beau_proxy_ioc *ioc, u32 out_len)
{
	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = virt_to_phys(beau_blk_backend.out);
	ioc->out_len = out_len;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static bool beau_blk_range_ok(u64 sector, u32 len, u32 *offset)
{
	u64 start;
	u64 end;

	if (sector > (U64_MAX / BEAU_BLK_SECTOR_SIZE))
		return false;

	start = sector * BEAU_BLK_SECTOR_SIZE;
	end = start + len;
	if ((end < start) || (end > BEAU_BLK_DISK_BYTES))
		return false;

	*offset = (u32)start;
	return true;
}

static int beau_blk_reply_status(struct beau_proxy_ioc *ioc, u8 status)
{
	if ((ioc->out_len == 0U) || (ioc->out_len > BEAU_PROXY_DATA_MAX))
		return -EINVAL;

	memset(beau_blk_backend.out, 0, ioc->out_len);
	((u8 *)beau_blk_backend.out)[ioc->out_len - 1U] = status;
	return beau_blk_reply(ioc, ioc->out_len);
}

static int beau_blk_handle_read(struct beau_proxy_ioc *ioc, u64 sector)
{
	u32 data_len;
	u32 offset;

	if ((ioc->out_len == 0U) || (ioc->out_len > BEAU_PROXY_DATA_MAX) ||
	    !beau_blk_range_ok(sector, ioc->out_len - 1U, &offset))
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_IOERR);

	data_len = ioc->out_len - 1U;
	memcpy(beau_blk_backend.out, beau_blk_backend.disk + offset, data_len);
	((u8 *)beau_blk_backend.out)[data_len] = VIRTIO_BLK_S_OK;
	return beau_blk_reply(ioc, data_len + 1U);
}

static int beau_blk_handle_write(struct beau_proxy_ioc *ioc, u64 sector)
{
	u32 data_len;
	u32 offset;

	if ((ioc->out_len == 0U) || (ioc->in_len < sizeof(struct virtio_blk_outhdr)) ||
	    !beau_blk_range_ok(sector,
			       ioc->in_len - sizeof(struct virtio_blk_outhdr),
			       &offset))
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_IOERR);

	data_len = ioc->in_len - sizeof(struct virtio_blk_outhdr);
	memcpy(beau_blk_backend.disk + offset,
	       (u8 *)beau_blk_backend.in + sizeof(struct virtio_blk_outhdr),
	       data_len);
	return beau_blk_reply_status(ioc, VIRTIO_BLK_S_OK);
}

static int beau_blk_handle_get_id(struct beau_proxy_ioc *ioc)
{
	u32 id_len;

	if ((ioc->out_len == 0U) || (ioc->out_len > BEAU_PROXY_DATA_MAX))
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_IOERR);

	id_len = ioc->out_len - 1U;
	memset(beau_blk_backend.out, 0, id_len);
	memcpy(beau_blk_backend.out, BEAU_BLK_ID,
	       min_t(u32, id_len, sizeof(BEAU_BLK_ID) - 1U));
	((u8 *)beau_blk_backend.out)[id_len] = VIRTIO_BLK_S_OK;
	return beau_blk_reply(ioc, id_len + 1U);
}

static int beau_blk_handle_one(struct beau_proxy_ioc *ioc)
{
	const struct virtio_blk_outhdr *hdr = beau_blk_backend.in;
	u32 type;
	u64 sector;

	if ((ioc->queue_id != BEAU_BLK_QUEUE_REQUEST) ||
	    (ioc->in_len < sizeof(*hdr)) || (ioc->out_len == 0U)) {
		pr_debug("BEAU virtio-blk ignored q=%u in=%u outcap=%u\n",
			 ioc->queue_id, ioc->in_len, ioc->out_len);
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_IOERR);
	}

	type = beau_blk_req_type(hdr);
	sector = beau_blk_req_sector(hdr);

	switch (type) {
	case VIRTIO_BLK_T_IN:
		return beau_blk_handle_read(ioc, sector);
	case VIRTIO_BLK_T_OUT:
		return beau_blk_handle_write(ioc, sector);
	case VIRTIO_BLK_T_FLUSH:
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_OK);
	case VIRTIO_BLK_T_GET_ID:
		return beau_blk_handle_get_id(ioc);
	default:
		pr_debug("BEAU virtio-blk unsupported type=%u sector=%llu\n",
			 type, sector);
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_UNSUPP);
	}
}

static int beau_blk_backend_thread(void *data)
{
	struct beau_proxy_ioc *ioc = &beau_blk_backend.ioc;
	unsigned int idle_polls = 0U;
	long ret;

	memset(ioc, 0, sizeof(*ioc));
	ioc->op = BEAU_PROXY_OP_REGISTER;
	ioc->device_id = BEAU_PROXY_DEVICE_BLK;
	ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
	ioc->device_features = (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
		(1ULL << VIRTIO_BLK_F_SEG_MAX);
	ioc->config_gpa = virt_to_phys(&beau_blk_backend.config);
	ioc->config_len = sizeof(beau_blk_backend.config);
	ioc->register_flags = BEAU_PROXY_REG_F_FEATURES | BEAU_PROXY_REG_F_CONFIG;
	while (!kthread_should_stop()) {
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (!ret)
			break;
		msleep(1000);
	}

	while (!kthread_should_stop()) {
		memset(ioc, 0, sizeof(*ioc));
		ioc->op = BEAU_PROXY_OP_POLL;
		ioc->device_id = BEAU_PROXY_DEVICE_BLK;
		ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
		ioc->queue_id = BEAU_BLK_QUEUE_REQUEST;
		ioc->in_gpa = virt_to_phys(beau_blk_backend.in);
		ioc->in_len = BEAU_PROXY_DATA_MAX;
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (ret) {
			beau_proxy_poll_idle_delay(&idle_polls);
			continue;
		}
		beau_proxy_poll_active(&idle_polls);
		ret = beau_blk_handle_one(ioc);
		if (ret)
			beau_proxy_poll_idle_delay(&idle_polls);
	}

	return 0;
}

static int __init beau_virtioblk_backend_init(void)
{
	const char *model = NULL;

	if (!of_root || of_property_read_string(of_root, "model", &model) ||
	    !model || !strstr(model, "VM1"))
		return 0;

	beau_blk_backend.in = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	beau_blk_backend.out = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	beau_blk_backend.disk = vzalloc(BEAU_BLK_DISK_BYTES);
	if (!beau_blk_backend.in || !beau_blk_backend.out ||
	    !beau_blk_backend.disk) {
		kfree(beau_blk_backend.in);
		kfree(beau_blk_backend.out);
		vfree(beau_blk_backend.disk);
		return -ENOMEM;
	}
	beau_blk_backend.config.capacity = BEAU_BLK_DISK_BYTES / BEAU_BLK_SECTOR_SIZE;
	beau_blk_backend.config.size_max = 4096U;
	beau_blk_backend.config.seg_max = 1U;

	beau_blk_backend.thread = kthread_run(beau_blk_backend_thread, NULL,
					      "beau-virtioblk-backend");
	if (IS_ERR(beau_blk_backend.thread)) {
		long ret = PTR_ERR(beau_blk_backend.thread);

		kfree(beau_blk_backend.in);
		kfree(beau_blk_backend.out);
		vfree(beau_blk_backend.disk);
		return ret;
	}

	pr_info("BEAU virtio-blk backend started, %u KiB RAM disk\n",
		BEAU_BLK_DISK_BYTES / 1024U);
	return 0;
}

static void __exit beau_virtioblk_backend_exit(void)
{
	if (beau_blk_backend.thread && !IS_ERR(beau_blk_backend.thread))
		kthread_stop(beau_blk_backend.thread);
	kfree(beau_blk_backend.in);
	kfree(beau_blk_backend.out);
	vfree(beau_blk_backend.disk);
}

late_initcall(beau_virtioblk_backend_init);
module_exit(beau_virtioblk_backend_exit);

MODULE_DESCRIPTION("BEAU VM1 virtio-blk RAM backend");
MODULE_LICENSE("GPL");
