/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_HCALL_H
#define _BEAU_HCALL_H

#include <linux/types.h>

#define BEAU_PROXY_OP_REGISTER		0U
#define BEAU_PROXY_OP_POLL		1U
#define BEAU_PROXY_OP_REPLY		2U

#define BEAU_PROXY_DATA_MAX		4096U
#define BEAU_PROXY_DESC_MAX		8U
#define BEAU_PROXY_FLAG_RO		0x1U

struct beau_proxy_desc {
	u32 len;
	u32 flags;
} __aligned(8);

struct beau_proxy_ioc {
	u32 op;
	u32 status;
	u32 device_id;
	u16 frontend_vmid;
	u16 queue_id;
	u16 head;
	u16 desc_count;
	u32 in_len;
	u32 out_len;
	u64 in_gpa;
	u64 out_gpa;
	struct beau_proxy_desc desc[BEAU_PROXY_DESC_MAX];
} __aligned(8);

long beau_hcall_vm_wdt_kick(unsigned long token);
long beau_hcall_virtio_proxy_backend(struct beau_proxy_ioc *ioc);

#endif /* _BEAU_HCALL_H */
