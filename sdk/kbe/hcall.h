/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_HCALL_H
#define _BEAU_HCALL_H

#include <linux/delay.h>
#include <linux/types.h>

#define BEAU_PROXY_ABI_VERSION		3U

#define BEAU_PROXY_OP_REGISTER		0U
#define BEAU_PROXY_OP_POLL		1U
#define BEAU_PROXY_OP_REPLY		2U
#define BEAU_PROXY_OP_HEARTBEAT		3U
#define BEAU_PROXY_OP_BATCH_POLL	4U
#define BEAU_PROXY_OP_BATCH_REPLY	5U

#define BEAU_PROXY_DATA_MAX		8192U
#define BEAU_PROXY_DESC_MAX		8U
#define BEAU_PROXY_BATCH_MAX		4U
#define BEAU_PROXY_BATCH_DATA_MAX	BEAU_PROXY_DATA_MAX
#define BEAU_PROXY_FLAG_RO		0x1U
#define BEAU_PROXY_REG_F_FEATURES	0x1U
#define BEAU_PROXY_REG_F_CONFIG		0x2U
#define BEAU_PROXY_CAP_WAIT_HINT	0x1U
#define BEAU_PROXY_CAP_HEARTBEAT	0x2U
#define BEAU_PROXY_CAP_STATS		0x4U
#define BEAU_PROXY_CAP_BATCH		0x8U
#define BEAU_PROXY_CAP_SHARED_RING	0x10U
#define BEAU_PROXY_BATCH_F_LOCAL_REPLY	0x80000000U

struct beau_proxy_desc {
	u32 len;
	u32 flags;
} __aligned(8);

struct beau_proxy_batch_entry {
	u32 status;
	u16 queue_id;
	u16 head;
	u16 desc_count;
	u32 in_len;
	u32 out_len;
	u32 reply_len;
	u32 flags;
	struct beau_proxy_desc desc[BEAU_PROXY_DESC_MAX];
	u8 in[BEAU_PROXY_BATCH_DATA_MAX];
	u8 out[BEAU_PROXY_BATCH_DATA_MAX];
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
	u64 device_features;
	u64 config_gpa;
	u32 config_len;
	u32 register_flags;
	u32 abi_version;
	u32 ioc_size;
	u32 backend_caps;
	u32 wait_us;
	u64 heartbeat_seq;
	u64 batch_gpa;
	u32 batch_len;
	u32 batch_count;
	u32 batch_entry_size;
	u32 batch_flags;
	struct beau_proxy_desc desc[BEAU_PROXY_DESC_MAX];
} __aligned(8);

static inline void beau_proxy_poll_idle_delay(unsigned int *idle_polls)
{
	if (idle_polls == NULL)
		return;

	if (*idle_polls < 8U) {
		(*idle_polls)++;
		usleep_range(50, 100);
	} else if (*idle_polls < 32U) {
		(*idle_polls)++;
		usleep_range(500, 1000);
	} else {
		msleep(1);
	}
}

static inline void beau_proxy_poll_active(unsigned int *idle_polls)
{
	if (idle_polls != NULL)
		*idle_polls = 0U;
}

long beau_hcall_vm_wdt_kick(unsigned long token);
long beau_hcall_virtio_proxy_backend(struct beau_proxy_ioc *ioc);

#endif /* _BEAU_HCALL_H */
