// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU virtio-i2c backend for the VM1 <-> virtio_proxy <-> VM2 test path.
 *
 * This backend intentionally exposes a small in-memory device at 7-bit I2C
 * address 0x50. It lets QEMU validation exercise the standard VM2 virtio-i2c
 * frontend and i2c-tools without depending on physical I2C hardware in VM1.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio_i2c.h>
#include <asm/memory.h>

#include "hcall.h"

#define BEAU_PROXY_FRONTEND_VM2		2U
#define BEAU_PROXY_DEVICE_I2C		34U
#define BEAU_I2C_QUEUE_REQUEST		0U
#define BEAU_I2C_EEPROM_ADDR		0x50U
#define BEAU_I2C_EEPROM_SIZE		256U

struct beau_i2c_backend {
	struct task_struct *thread;
	void *in;
	void *out;
	u8 eeprom[BEAU_I2C_EEPROM_SIZE];
	u8 offset;
	bool fail_next;
	struct beau_proxy_ioc ioc;
};

static struct beau_i2c_backend beau_i2c_backend;

static u16 beau_i2c_addr(const struct virtio_i2c_out_hdr *hdr)
{
	return le16_to_cpu(hdr->addr) >> 1;
}

static u32 beau_i2c_flags(const struct virtio_i2c_out_hdr *hdr)
{
	return le32_to_cpu(hdr->flags);
}

static int beau_i2c_reply(struct beau_proxy_ioc *ioc, u32 out_len)
{
	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = virt_to_phys(beau_i2c_backend.out);
	ioc->out_len = out_len;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static void beau_i2c_reply_status(u8 status, u32 out_len)
{
	if (out_len == 0U)
		return;

	memset(beau_i2c_backend.out, 0, out_len);
	((u8 *)beau_i2c_backend.out)[out_len - 1U] = status;
}

static u8 beau_i2c_handle_read(u32 read_len)
{
	u8 *out = beau_i2c_backend.out;
	u32 pos = beau_i2c_backend.offset;

	for (u32 i = 0U; i < read_len; i++)
		out[i] = beau_i2c_backend.eeprom[(pos + i) % BEAU_I2C_EEPROM_SIZE];

	beau_i2c_backend.offset = (u8)((pos + read_len) % BEAU_I2C_EEPROM_SIZE);
	return VIRTIO_I2C_MSG_OK;
}

static u8 beau_i2c_handle_write(const u8 *data, u32 len)
{
	u32 pos;

	if (len == 0U)
		return VIRTIO_I2C_MSG_OK;

	pos = data[0];
	beau_i2c_backend.offset = data[0];
	for (u32 i = 1U; i < len; i++)
		beau_i2c_backend.eeprom[(pos + i - 1U) % BEAU_I2C_EEPROM_SIZE] = data[i];

	if (len > 1U)
		beau_i2c_backend.offset =
			(u8)((pos + len - 1U) % BEAU_I2C_EEPROM_SIZE);

	return VIRTIO_I2C_MSG_OK;
}

static int beau_i2c_handle_one(struct beau_proxy_ioc *ioc)
{
	const struct virtio_i2c_out_hdr *hdr = beau_i2c_backend.in;
	const u8 *payload = (const u8 *)beau_i2c_backend.in + sizeof(*hdr);
	u32 payload_len;
	u32 flags;
	u32 reply_len;
	u8 status = VIRTIO_I2C_MSG_ERR;

	if ((ioc->queue_id != BEAU_I2C_QUEUE_REQUEST) ||
	    (ioc->in_len < sizeof(*hdr)) || (ioc->out_len == 0U) ||
	    (ioc->out_len > BEAU_PROXY_DATA_MAX)) {
		pr_debug("BEAU virtio-i2c ignored q=%u in=%u outcap=%u\n",
			 ioc->queue_id, ioc->in_len, ioc->out_len);
		if (ioc->out_len != 0U && ioc->out_len <= BEAU_PROXY_DATA_MAX) {
			beau_i2c_reply_status(VIRTIO_I2C_MSG_ERR, ioc->out_len);
			return beau_i2c_reply(ioc, ioc->out_len);
		}
		return -EINVAL;
	}

	flags = beau_i2c_flags(hdr);
	payload_len = ioc->in_len - sizeof(*hdr);
	reply_len = ioc->out_len;

	if (beau_i2c_backend.fail_next) {
		status = VIRTIO_I2C_MSG_ERR;
	} else if (beau_i2c_addr(hdr) != BEAU_I2C_EEPROM_ADDR) {
		status = VIRTIO_I2C_MSG_ERR;
	} else if ((flags & VIRTIO_I2C_FLAGS_M_RD) != 0U) {
		if (reply_len > sizeof(struct virtio_i2c_in_hdr))
			status = beau_i2c_handle_read(reply_len -
				sizeof(struct virtio_i2c_in_hdr));
	} else {
		status = beau_i2c_handle_write(payload, payload_len);
	}

	if (((flags & VIRTIO_I2C_FLAGS_FAIL_NEXT) != 0U) &&
	    (status == VIRTIO_I2C_MSG_ERR))
		beau_i2c_backend.fail_next = true;

	beau_i2c_reply_status(status, reply_len);

	if ((flags & VIRTIO_I2C_FLAGS_FAIL_NEXT) == 0U)
		beau_i2c_backend.fail_next = false;

	pr_debug("BEAU virtio-i2c addr=0x%x flags=0x%x in=%u out=%u status=%u\n",
		 beau_i2c_addr(hdr), flags, ioc->in_len, reply_len, status);
	return beau_i2c_reply(ioc, reply_len);
}

static int beau_i2c_backend_thread(void *data)
{
	struct beau_proxy_ioc *ioc = &beau_i2c_backend.ioc;
	unsigned int idle_polls = 0U;
	long ret;

	memset(ioc, 0, sizeof(*ioc));
	ioc->op = BEAU_PROXY_OP_REGISTER;
	ioc->device_id = BEAU_PROXY_DEVICE_I2C;
	ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
	ioc->device_features = 1ULL << VIRTIO_I2C_F_ZERO_LENGTH_REQUEST;
	ioc->register_flags = BEAU_PROXY_REG_F_FEATURES;
	while (!kthread_should_stop()) {
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (!ret)
			break;
		msleep(1000);
	}

	while (!kthread_should_stop()) {
		memset(ioc, 0, sizeof(*ioc));
		ioc->op = BEAU_PROXY_OP_POLL;
		ioc->device_id = BEAU_PROXY_DEVICE_I2C;
		ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
		ioc->queue_id = BEAU_I2C_QUEUE_REQUEST;
		ioc->in_gpa = virt_to_phys(beau_i2c_backend.in);
		ioc->in_len = BEAU_PROXY_DATA_MAX;
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (ret) {
			beau_proxy_poll_idle_delay(&idle_polls);
			continue;
		}
		beau_proxy_poll_active(&idle_polls);
		ret = beau_i2c_handle_one(ioc);
		if (ret)
			beau_proxy_poll_idle_delay(&idle_polls);
	}

	return 0;
}

static int __init beau_virtioi2c_backend_init(void)
{
	const char *model = NULL;

	if (!of_root || of_property_read_string(of_root, "model", &model) ||
	    !model || !strstr(model, "VM1"))
		return 0;

	beau_i2c_backend.in = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	beau_i2c_backend.out = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	if (!beau_i2c_backend.in || !beau_i2c_backend.out) {
		kfree(beau_i2c_backend.in);
		kfree(beau_i2c_backend.out);
		return -ENOMEM;
	}

	for (u32 i = 0U; i < BEAU_I2C_EEPROM_SIZE; i++)
		beau_i2c_backend.eeprom[i] = (u8)i;
	memcpy(beau_i2c_backend.eeprom, "BEAU virtio-i2c proxy", 22U);

	beau_i2c_backend.thread = kthread_run(beau_i2c_backend_thread, NULL,
					      "beau-virtioi2c-backend");
	if (IS_ERR(beau_i2c_backend.thread)) {
		long ret = PTR_ERR(beau_i2c_backend.thread);

		kfree(beau_i2c_backend.in);
		kfree(beau_i2c_backend.out);
		return ret;
	}

	pr_info("BEAU virtio-i2c backend started, EEPROM at 0x%02x\n",
		BEAU_I2C_EEPROM_ADDR);
	return 0;
}

static void __exit beau_virtioi2c_backend_exit(void)
{
	if (beau_i2c_backend.thread && !IS_ERR(beau_i2c_backend.thread))
		kthread_stop(beau_i2c_backend.thread);
	kfree(beau_i2c_backend.in);
	kfree(beau_i2c_backend.out);
}

late_initcall(beau_virtioi2c_backend_init);
module_exit(beau_virtioi2c_backend_exit);

MODULE_DESCRIPTION("BEAU VM1 virtio-i2c EEPROM backend");
MODULE_LICENSE("GPL");
