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
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio_blk.h>
#include <linux/vmalloc.h>

#include "virtio-proxy-backend.h"

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
	struct beau_proxy_backend proxy;
	u8 *disk;
	struct beau_blk_config config;
};

static struct beau_blk_backend beau_blk_backend;
static const u16 beau_blk_queues[] = {
	BEAU_BLK_QUEUE_REQUEST,
};

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
	return beau_proxy_backend_reply(ioc, beau_blk_backend.proxy.out, out_len);
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

	memset(beau_blk_backend.proxy.out, 0, ioc->out_len);
	((u8 *)beau_blk_backend.proxy.out)[ioc->out_len - 1U] = status;
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
	memcpy(beau_blk_backend.proxy.out, beau_blk_backend.disk + offset, data_len);
	((u8 *)beau_blk_backend.proxy.out)[data_len] = VIRTIO_BLK_S_OK;
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
	       (u8 *)beau_blk_backend.proxy.in + sizeof(struct virtio_blk_outhdr),
	       data_len);
	return beau_blk_reply_status(ioc, VIRTIO_BLK_S_OK);
}

static int beau_blk_handle_get_id(struct beau_proxy_ioc *ioc)
{
	u32 id_len;

	if ((ioc->out_len == 0U) || (ioc->out_len > BEAU_PROXY_DATA_MAX))
		return beau_blk_reply_status(ioc, VIRTIO_BLK_S_IOERR);

	id_len = ioc->out_len - 1U;
	memset(beau_blk_backend.proxy.out, 0, id_len);
	memcpy(beau_blk_backend.proxy.out, BEAU_BLK_ID,
	       min_t(u32, id_len, sizeof(BEAU_BLK_ID) - 1U));
	((u8 *)beau_blk_backend.proxy.out)[id_len] = VIRTIO_BLK_S_OK;
	return beau_blk_reply(ioc, id_len + 1U);
}

static int beau_blk_handle_one(struct beau_proxy_backend *proxy,
			       struct beau_proxy_ioc *ioc)
{
	const struct virtio_blk_outhdr *hdr = proxy->in;
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

static void beau_blk_prepare_register(struct beau_proxy_backend *proxy,
				      struct beau_proxy_ioc *ioc)
{
	ioc->device_features = (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
		(1ULL << VIRTIO_BLK_F_SEG_MAX);
	ioc->config_gpa = virt_to_phys(&beau_blk_backend.config);
	ioc->config_len = sizeof(beau_blk_backend.config);
	ioc->register_flags = BEAU_PROXY_REG_F_FEATURES | BEAU_PROXY_REG_F_CONFIG;
}

static int __init beau_virtioblk_backend_init(void)
{
	struct beau_proxy_backend *proxy = &beau_blk_backend.proxy;
	int ret;

	if (!beau_proxy_backend_is_vm1())
		return 0;

	proxy->name = "virtio-blk";
	proxy->thread_name = "beau-virtioblk-backend";
	proxy->device_id = BEAU_PROXY_DEVICE_BLK;
	proxy->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
	proxy->queues = beau_blk_queues;
	proxy->queue_count = ARRAY_SIZE(beau_blk_queues);
	proxy->batch_entries = BEAU_PROXY_BATCH_MAX;
	proxy->handle_one = beau_blk_handle_one;
	proxy->prepare_register = beau_blk_prepare_register;

	ret = beau_proxy_backend_alloc_io(proxy);
	if (ret != 0)
		return ret;
	beau_blk_backend.disk = vzalloc(BEAU_BLK_DISK_BYTES);
	if (!beau_blk_backend.disk) {
		beau_proxy_backend_free_io(proxy);
		vfree(beau_blk_backend.disk);
		return -ENOMEM;
	}
	beau_blk_backend.config.capacity = BEAU_BLK_DISK_BYTES / BEAU_BLK_SECTOR_SIZE;
	beau_blk_backend.config.size_max = 4096U;
	beau_blk_backend.config.seg_max = 1U;

	ret = beau_proxy_backend_start(proxy);
	if (ret != 0) {
		beau_proxy_backend_free_io(proxy);
		vfree(beau_blk_backend.disk);
		return ret;
	}

	pr_info("BEAU virtio-blk backend started, %u KiB RAM disk\n",
		BEAU_BLK_DISK_BYTES / 1024U);
	return 0;
}

static void __exit beau_virtioblk_backend_exit(void)
{
	beau_proxy_backend_stop(&beau_blk_backend.proxy);
	beau_proxy_backend_free_io(&beau_blk_backend.proxy);
	vfree(beau_blk_backend.disk);
}

late_initcall(beau_virtioblk_backend_init);
module_exit(beau_virtioblk_backend_exit);

MODULE_DESCRIPTION("BEAU VM1 virtio-blk RAM backend");
MODULE_LICENSE("GPL");
