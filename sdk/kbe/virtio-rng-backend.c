// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU virtio-rng backend for the VM1 <-> virtio_proxy <-> VM2 test path.
 *
 * Principle:
 *
 *   VM2 virtio-rng frontend
 *        -> BEAU virtio_proxy copies one writable descriptor chain
 *        -> VM1 backend HVC poll receives an empty request with an output cap
 *        -> VM1 fills bytes from its kernel random generator
 *        -> VM1 backend HVC reply copies random bytes back
 *
 * virtio-rng is intentionally small: queue 0 contains only host-writable
 * buffers. That makes it a good proof point for virtio_proxy devices whose
 * protocol payload is just descriptor ownership plus used-ring completion.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/memory.h>

#include "hcall.h"

#define BEAU_PROXY_FRONTEND_VM2		2U
#define BEAU_PROXY_DEVICE_RNG		4U
#define BEAU_RNG_QUEUE_INPUT		0U

struct beau_rng_backend {
	struct task_struct *thread;
	void *in;
	void *out;
	struct beau_proxy_ioc ioc;
};

static struct beau_rng_backend beau_rng_backend;

static int beau_rng_reply(struct beau_proxy_ioc *ioc, void *out, u32 out_len)
{
	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = virt_to_phys(out);
	ioc->out_len = out_len;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static int beau_rng_reply_empty(struct beau_proxy_ioc *ioc)
{
	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = 0;
	ioc->out_len = 0;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static int beau_rng_handle_one(struct beau_proxy_ioc *ioc)
{
	u32 len;

	if (ioc->queue_id != BEAU_RNG_QUEUE_INPUT || ioc->out_len == 0) {
		pr_debug("BEAU virtio-rng ignored q=%u in=%u outcap=%u\n",
			 ioc->queue_id, ioc->in_len, ioc->out_len);
		return beau_rng_reply_empty(ioc);
	}

	if (ioc->in_len != 0)
		pr_debug("BEAU virtio-rng unexpected input len=%u\n", ioc->in_len);

	len = min_t(u32, ioc->out_len, BEAU_PROXY_DATA_MAX);
	get_random_bytes(beau_rng_backend.out, len);

	pr_debug("BEAU virtio-rng q=%u outcap=%u used=%u desc=%u\n",
		 ioc->queue_id, ioc->out_len, len, ioc->desc_count);
	return beau_rng_reply(ioc, beau_rng_backend.out, len);
}

static int beau_rng_backend_thread(void *data)
{
	struct beau_proxy_ioc *ioc = &beau_rng_backend.ioc;
	long ret;

	memset(ioc, 0, sizeof(*ioc));
	ioc->op = BEAU_PROXY_OP_REGISTER;
	ioc->device_id = BEAU_PROXY_DEVICE_RNG;
	ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
	while (!kthread_should_stop()) {
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (!ret)
			break;
		msleep(1000);
	}

	while (!kthread_should_stop()) {
		memset(ioc, 0, sizeof(*ioc));
		ioc->op = BEAU_PROXY_OP_POLL;
		ioc->device_id = BEAU_PROXY_DEVICE_RNG;
		ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
		ioc->queue_id = BEAU_RNG_QUEUE_INPUT;
		ioc->in_gpa = virt_to_phys(beau_rng_backend.in);
		ioc->in_len = BEAU_PROXY_DATA_MAX;
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (ret) {
			msleep(10);
			continue;
		}
		ret = beau_rng_handle_one(ioc);
		if (ret)
			msleep(10);
	}

	return 0;
}

static int __init beau_virtiorng_backend_init(void)
{
	const char *model = NULL;

	if (!of_root || of_property_read_string(of_root, "model", &model) ||
	    !model || !strstr(model, "VM1"))
		return 0;

	beau_rng_backend.in = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	beau_rng_backend.out = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	if (!beau_rng_backend.in || !beau_rng_backend.out) {
		kfree(beau_rng_backend.in);
		kfree(beau_rng_backend.out);
		return -ENOMEM;
	}

	beau_rng_backend.thread = kthread_run(beau_rng_backend_thread, NULL,
					      "beau-virtiorng-backend");
	if (IS_ERR(beau_rng_backend.thread)) {
		long ret = PTR_ERR(beau_rng_backend.thread);

		kfree(beau_rng_backend.in);
		kfree(beau_rng_backend.out);
		return ret;
	}

	pr_info("BEAU virtio-rng backend started\n");
	return 0;
}

static void __exit beau_virtiorng_backend_exit(void)
{
	if (beau_rng_backend.thread && !IS_ERR(beau_rng_backend.thread))
		kthread_stop(beau_rng_backend.thread);
	kfree(beau_rng_backend.in);
	kfree(beau_rng_backend.out);
}

late_initcall(beau_virtiorng_backend_init);
module_exit(beau_virtiorng_backend_exit);

MODULE_DESCRIPTION("BEAU VM1 virtio-rng backend");
MODULE_LICENSE("GPL");
