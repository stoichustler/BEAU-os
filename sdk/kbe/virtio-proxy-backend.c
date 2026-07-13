// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common BEAU virtio_proxy backend worker.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/memory.h>

#include "virtio-proxy-backend.h"

#define BEAU_PROXY_HEARTBEAT_MS		1000U
#define BEAU_PROXY_REGISTER_RETRY_MS	1000U

bool beau_proxy_backend_is_vm2(void)
{
	const char *model = NULL;

	return of_root && !of_property_read_string(of_root, "model", &model) &&
		model && strstr(model, "VM2");
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_is_vm2);

int beau_proxy_backend_alloc_io(struct beau_proxy_backend *backend)
{
	if (backend == NULL)
		return -EINVAL;

	if (backend->batch_entries > BEAU_PROXY_BATCH_MAX)
		backend->batch_entries = BEAU_PROXY_BATCH_MAX;

	backend->fallback_in = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	backend->fallback_out = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	backend->in = backend->fallback_in;
	backend->out = backend->fallback_out;
	if (!backend->in || !backend->out) {
		beau_proxy_backend_free_io(backend);
		return -ENOMEM;
	}
	if (backend->batch_entries != 0U) {
		backend->batch_len = sizeof(*backend->batch) * backend->batch_entries;
		backend->batch = kzalloc(backend->batch_len, GFP_KERNEL);
		if (backend->batch == NULL) {
			beau_proxy_backend_free_io(backend);
			return -ENOMEM;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_alloc_io);

void beau_proxy_backend_free_io(struct beau_proxy_backend *backend)
{
	if (backend == NULL)
		return;

	kfree(backend->fallback_in);
	kfree(backend->fallback_out);
	kfree(backend->batch);
	backend->in = NULL;
	backend->out = NULL;
	backend->fallback_in = NULL;
	backend->fallback_out = NULL;
	backend->batch = NULL;
	backend->batch_len = 0U;
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_free_io);

int beau_proxy_backend_reply(struct beau_proxy_ioc *ioc, const void *out,
			     u32 out_len)
{
	if (ioc == NULL)
		return -EINVAL;

	if ((ioc->batch_flags & BEAU_PROXY_BATCH_F_LOCAL_REPLY) != 0U) {
		struct beau_proxy_batch_entry *entry =
			(struct beau_proxy_batch_entry *)(unsigned long)ioc->batch_gpa;

		if ((entry == NULL) || (out_len > BEAU_PROXY_BATCH_DATA_MAX))
			return -EINVAL;
		if ((out_len != 0U) && (out != entry->out))
			memcpy(entry->out, out, out_len);
		entry->reply_len = out_len;
		return 0;
	}

	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = out_len != 0U ? virt_to_phys(out) : 0;
	ioc->out_len = out_len;
	return beau_hcall_virtio_proxy_backend(ioc);
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_reply);

int beau_proxy_backend_reply_empty(struct beau_proxy_ioc *ioc)
{
	return beau_proxy_backend_reply(ioc, NULL, 0);
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_reply_empty);

static void beau_proxy_backend_fill_common(struct beau_proxy_backend *backend,
					   struct beau_proxy_ioc *ioc, u32 op)
{
	memset(ioc, 0, sizeof(*ioc));
	ioc->op = op;
	ioc->device_id = backend->device_id;
	ioc->frontend_vmid = backend->frontend_vmid;
	ioc->abi_version = BEAU_PROXY_ABI_VERSION;
	ioc->ioc_size = sizeof(*ioc);
	ioc->backend_caps = BEAU_PROXY_CAP_WAIT_HINT |
		BEAU_PROXY_CAP_HEARTBEAT | BEAU_PROXY_CAP_STATS;
	if (backend->batch != NULL)
		ioc->backend_caps |= BEAU_PROXY_CAP_BATCH |
			BEAU_PROXY_CAP_SHARED_RING;
}

static int beau_proxy_backend_register(struct beau_proxy_backend *backend)
{
	struct beau_proxy_ioc *ioc = &backend->ioc;
	long ret;

	while (!kthread_should_stop()) {
		beau_proxy_backend_fill_common(backend, ioc, BEAU_PROXY_OP_REGISTER);
		if (backend->prepare_register != NULL)
			backend->prepare_register(backend, ioc);

		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (ret == 0) {
			backend->negotiated_caps = ioc->backend_caps;
			backend->last_heartbeat = jiffies;
			return 0;
		}
		msleep(BEAU_PROXY_REGISTER_RETRY_MS);
	}

	return -EINTR;
}

static void beau_proxy_backend_heartbeat(struct beau_proxy_backend *backend,
					 bool force)
{
	struct beau_proxy_ioc *ioc = &backend->ioc;
	long ret;

	if ((backend->negotiated_caps & BEAU_PROXY_CAP_HEARTBEAT) == 0U)
		return;
	if (!force && time_before(jiffies, backend->last_heartbeat +
				  backend->heartbeat_interval))
		return;

	beau_proxy_backend_fill_common(backend, ioc, BEAU_PROXY_OP_HEARTBEAT);
	ioc->heartbeat_seq = ++backend->heartbeat_seq;
	ret = beau_hcall_virtio_proxy_backend(ioc);
	if (ret == 0)
		backend->last_heartbeat = jiffies;
}

static void beau_proxy_backend_idle_sleep(struct beau_proxy_backend *backend,
					  const struct beau_proxy_ioc *ioc,
					  long ret)
{
	u32 wait_us = 0U;

	beau_proxy_backend_heartbeat(backend, false);

	if ((ret == -ENODATA) &&
	    ((backend->negotiated_caps & BEAU_PROXY_CAP_WAIT_HINT) != 0U)) {
		wait_us = ioc->wait_us;
	}

	if (wait_us == 0U) {
		beau_proxy_poll_idle_delay(&backend->idle_polls);
	} else if (wait_us < 1000U) {
		usleep_range(wait_us, wait_us * 2U);
	} else {
		msleep(DIV_ROUND_UP(wait_us, 1000U));
	}
}

static int beau_proxy_backend_poll_once(struct beau_proxy_backend *backend)
{
	struct beau_proxy_ioc *ioc = &backend->ioc;
	u16 queue;
	long ret;

	beau_proxy_backend_fill_common(backend, ioc, BEAU_PROXY_OP_POLL);
	queue = backend->queues[backend->queue_index++ % backend->queue_count];
	ioc->queue_id = queue;
	ioc->in_gpa = virt_to_phys(backend->in);
	ioc->in_len = BEAU_PROXY_DATA_MAX;

	ret = beau_hcall_virtio_proxy_backend(ioc);
	if (ret != 0) {
		beau_proxy_backend_idle_sleep(backend, ioc, ret);
		return ret;
	}

	beau_proxy_poll_active(&backend->idle_polls);
	ret = backend->handle_one(backend, ioc);
	if (ret != 0)
		beau_proxy_backend_idle_sleep(backend, ioc, ret);

	return ret;
}

static void beau_proxy_backend_entry_to_ioc(struct beau_proxy_ioc *ioc,
					    struct beau_proxy_batch_entry *entry)
{
	memset(ioc, 0, sizeof(*ioc));
	ioc->status = entry->status;
	ioc->device_id = 0U;
	ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM3;
	ioc->queue_id = entry->queue_id;
	ioc->head = entry->head;
	ioc->desc_count = entry->desc_count;
	ioc->in_len = entry->in_len;
	ioc->out_len = entry->out_len;
	ioc->batch_flags = BEAU_PROXY_BATCH_F_LOCAL_REPLY;
	ioc->batch_gpa = (unsigned long)entry;
	for (u16 i = 0; i < entry->desc_count && i < BEAU_PROXY_DESC_MAX; i++)
		ioc->desc[i] = entry->desc[i];
}

static int beau_proxy_backend_handle_batch(struct beau_proxy_backend *backend,
					   u32 count)
{
	void *saved_in = backend->in;
	void *saved_out = backend->out;
	u32 handled = 0U;

	for (u32 i = 0U; i < count; i++) {
		struct beau_proxy_batch_entry *entry = &backend->batch[i];
		struct beau_proxy_ioc item_ioc;
		int ret;

		beau_proxy_backend_entry_to_ioc(&item_ioc, entry);
		backend->in = entry->in;
		backend->out = entry->out;
		ret = backend->handle_one(backend, &item_ioc);
		if ((ret != 0) && (entry->reply_len == 0U))
			entry->reply_len = 0U;
		handled++;
	}

	backend->in = saved_in;
	backend->out = saved_out;
	return handled;
}

static int beau_proxy_backend_batch_reply(struct beau_proxy_backend *backend,
					  u32 count)
{
	struct beau_proxy_ioc *ioc = &backend->ioc;

	beau_proxy_backend_fill_common(backend, ioc, BEAU_PROXY_OP_BATCH_REPLY);
	ioc->batch_gpa = virt_to_phys(backend->batch);
	ioc->batch_len = backend->batch_len;
	ioc->batch_count = count;
	ioc->batch_entry_size = sizeof(*backend->batch);
	return beau_hcall_virtio_proxy_backend(ioc);
}

static int beau_proxy_backend_batch_poll_once(struct beau_proxy_backend *backend)
{
	struct beau_proxy_ioc *ioc = &backend->ioc;
	u32 count;
	u16 queue;
	long ret;

	memset(backend->batch, 0, backend->batch_len);
	beau_proxy_backend_fill_common(backend, ioc, BEAU_PROXY_OP_BATCH_POLL);
	queue = backend->queues[backend->queue_index++ % backend->queue_count];
	ioc->queue_id = queue;
	ioc->batch_gpa = virt_to_phys(backend->batch);
	ioc->batch_len = backend->batch_len;
	ioc->batch_count = backend->batch_entries;
	ioc->batch_entry_size = sizeof(*backend->batch);

	ret = beau_hcall_virtio_proxy_backend(ioc);
	if (ret != 0) {
		beau_proxy_backend_idle_sleep(backend, ioc, ret);
		return ret;
	}

	count = min_t(u32, ioc->batch_count, backend->batch_entries);
	if (count == 0U)
		return 0;

	beau_proxy_poll_active(&backend->idle_polls);
	count = beau_proxy_backend_handle_batch(backend, count);
	ret = beau_proxy_backend_batch_reply(backend, count);
	if (ret != 0)
		beau_proxy_backend_idle_sleep(backend, ioc, ret);

	return ret;
}

static bool beau_proxy_backend_can_batch(const struct beau_proxy_backend *backend)
{
	return (backend != NULL) && (backend->batch != NULL) &&
		((backend->negotiated_caps & BEAU_PROXY_CAP_BATCH) != 0U) &&
		((backend->negotiated_caps & BEAU_PROXY_CAP_SHARED_RING) != 0U);
}

static int beau_proxy_backend_thread(void *data)
{
	struct beau_proxy_backend *backend = data;
	int ret;

	ret = beau_proxy_backend_register(backend);
	if (ret != 0)
		return ret;

	beau_proxy_backend_heartbeat(backend, true);
	while (!kthread_should_stop()) {
		if (beau_proxy_backend_can_batch(backend))
			(void)beau_proxy_backend_batch_poll_once(backend);
		else
			(void)beau_proxy_backend_poll_once(backend);
	}

	return 0;
}

int beau_proxy_backend_start(struct beau_proxy_backend *backend)
{
	if ((backend == NULL) || (backend->name == NULL) ||
	    (backend->thread_name == NULL) || (backend->queues == NULL) ||
	    (backend->queue_count == 0U) || (backend->handle_one == NULL) ||
	    (backend->in == NULL) || (backend->out == NULL))
		return -EINVAL;

	backend->frontend_vmid = backend->frontend_vmid != 0U ?
		backend->frontend_vmid : BEAU_PROXY_FRONTEND_VM3;
	backend->heartbeat_interval = msecs_to_jiffies(BEAU_PROXY_HEARTBEAT_MS);
	backend->thread = kthread_run(beau_proxy_backend_thread, backend,
				      "%s", backend->thread_name);
	if (IS_ERR(backend->thread))
		return PTR_ERR(backend->thread);

	return 0;
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_start);

void beau_proxy_backend_stop(struct beau_proxy_backend *backend)
{
	if ((backend != NULL) && backend->thread && !IS_ERR(backend->thread)) {
		kthread_stop(backend->thread);
		backend->thread = NULL;
	}
}
EXPORT_SYMBOL_GPL(beau_proxy_backend_stop);

MODULE_DESCRIPTION("BEAU virtio_proxy backend common worker");
MODULE_LICENSE("GPL");
