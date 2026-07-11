// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU virtio-rng backend for the VM2 <-> virtio_proxy <-> VM3 test path.
 *
 * Principle:
 *
 *   VM3 virtio-rng frontend
 *        -> BEAU virtio_proxy copies one writable descriptor chain
 *        -> VM2 backend HVC poll receives an empty request with an output cap
 *        -> VM2 fills bytes from its kernel random generator
 *        -> VM2 backend HVC reply copies random bytes back
 *
 * virtio-rng is intentionally small: queue 0 contains only host-writable
 * buffers. That makes it a good proof point for virtio_proxy devices whose
 * protocol payload is just descriptor ownership plus used-ring completion.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/string.h>

#include "virtio-proxy-backend.h"

#define BEAU_PROXY_DEVICE_RNG		4U
#define BEAU_RNG_QUEUE_INPUT		0U

struct beau_rng_backend {
	struct beau_proxy_backend proxy;
};

static struct beau_rng_backend beau_rng_backend;
static const u16 beau_rng_queues[] = {
	BEAU_RNG_QUEUE_INPUT,
};

static int beau_rng_handle_one(struct beau_proxy_backend *proxy,
			       struct beau_proxy_ioc *ioc)
{
	u32 len;

	if (ioc->queue_id != BEAU_RNG_QUEUE_INPUT || ioc->out_len == 0) {
		pr_debug("BEAU virtio-rng ignored q=%u in=%u outcap=%u\n",
			 ioc->queue_id, ioc->in_len, ioc->out_len);
		return beau_proxy_backend_reply_empty(ioc);
	}

	if (ioc->in_len != 0)
		pr_debug("BEAU virtio-rng unexpected input len=%u\n", ioc->in_len);

	len = min_t(u32, ioc->out_len, BEAU_PROXY_DATA_MAX);
	get_random_bytes(proxy->out, len);

	pr_debug("BEAU virtio-rng q=%u outcap=%u used=%u desc=%u\n",
		 ioc->queue_id, ioc->out_len, len, ioc->desc_count);
	return beau_proxy_backend_reply(ioc, proxy->out, len);
}

static int __init beau_virtiorng_backend_init(void)
{
	struct beau_proxy_backend *proxy = &beau_rng_backend.proxy;
	int ret;

	if (!beau_proxy_backend_is_vm2())
		return 0;

	proxy->name = "virtio-rng";
	proxy->thread_name = "beau-virtiorng-backend";
	proxy->device_id = BEAU_PROXY_DEVICE_RNG;
	proxy->frontend_vmid = BEAU_PROXY_FRONTEND_VM3;
	proxy->queues = beau_rng_queues;
	proxy->queue_count = ARRAY_SIZE(beau_rng_queues);
	proxy->handle_one = beau_rng_handle_one;

	ret = beau_proxy_backend_alloc_io(proxy);
	if (ret != 0)
		return ret;
	ret = beau_proxy_backend_start(proxy);
	if (ret != 0) {
		beau_proxy_backend_free_io(proxy);
		return ret;
	}

	pr_info("BEAU virtio-rng backend started\n");
	return 0;
}

static void __exit beau_virtiorng_backend_exit(void)
{
	beau_proxy_backend_stop(&beau_rng_backend.proxy);
	beau_proxy_backend_free_io(&beau_rng_backend.proxy);
}

late_initcall(beau_virtiorng_backend_init);
module_exit(beau_virtiorng_backend_exit);

MODULE_DESCRIPTION("BEAU VM2 virtio-rng backend");
MODULE_LICENSE("GPL");
