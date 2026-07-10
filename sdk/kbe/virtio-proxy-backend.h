/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_VIRTIO_PROXY_BACKEND_H
#define _BEAU_VIRTIO_PROXY_BACKEND_H

#include <linux/types.h>

#include "hcall.h"

#define BEAU_PROXY_FRONTEND_VM2		2U

struct task_struct;

struct beau_proxy_backend {
	const char *name;
	const char *thread_name;
	u32 device_id;
	u16 frontend_vmid;
	const u16 *queues;
	u16 queue_count;
	void *owner;
	void *in;
	void *out;
	void *fallback_in;
	void *fallback_out;
	struct beau_proxy_batch_entry *batch;
	struct task_struct *thread;
	struct beau_proxy_ioc ioc;
	u64 heartbeat_seq;
	unsigned int queue_index;
	unsigned int idle_polls;
	unsigned long last_heartbeat;
	unsigned long heartbeat_interval;
	u32 negotiated_caps;
	u16 batch_entries;
	u32 batch_len;
	int (*handle_one)(struct beau_proxy_backend *backend,
			  struct beau_proxy_ioc *ioc);
	void (*prepare_register)(struct beau_proxy_backend *backend,
				 struct beau_proxy_ioc *ioc);
};

bool beau_proxy_backend_is_vm1(void);
int beau_proxy_backend_alloc_io(struct beau_proxy_backend *backend);
void beau_proxy_backend_free_io(struct beau_proxy_backend *backend);
int beau_proxy_backend_start(struct beau_proxy_backend *backend);
void beau_proxy_backend_stop(struct beau_proxy_backend *backend);
int beau_proxy_backend_reply(struct beau_proxy_ioc *ioc, const void *out,
			     u32 out_len);
int beau_proxy_backend_reply_empty(struct beau_proxy_ioc *ioc);

#endif /* _BEAU_VIRTIO_PROXY_BACKEND_H */
