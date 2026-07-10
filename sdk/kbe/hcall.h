/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_HCALL_H
#define _BEAU_HCALL_H

#include <linux/delay.h>
#include <linux/types.h>

#define BEAU_PROXY_OP_REGISTER		0U
#define BEAU_PROXY_OP_POLL		1U
#define BEAU_PROXY_OP_REPLY		2U

#define BEAU_PROXY_DATA_MAX		8192U
#define BEAU_PROXY_DESC_MAX		8U
#define BEAU_PROXY_FLAG_RO		0x1U
#define BEAU_PROXY_REG_F_FEATURES	0x1U
#define BEAU_PROXY_REG_F_CONFIG		0x2U

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
	u64 device_features;
	u64 config_gpa;
	u32 config_len;
	u32 register_flags;
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
