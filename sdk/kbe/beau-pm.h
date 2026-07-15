/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_PM_H
#define _BEAU_PM_H

#include <linux/types.h>

#define ACRN_PM_ABI_VERSION		1U

#define ACRN_PM_QUERY_CAPS		0U
#define ACRN_PM_REQUEST_SUSPEND		1U
#define ACRN_PM_GET_EVENT		2U
#define ACRN_PM_ABORT			3U
#define ACRN_PM_GET_STATUS		4U
#define ACRN_PM_GET_WAKE_REASON		5U
#define ACRN_PM_RESUME_COMPLETE		6U

#define ACRN_PM_FLAG_REQUIRED		(1U << 0)
#define ACRN_PM_EVENT_PREPARE		(1U << 1)
#define ACRN_PM_EVENT_RESUME		(1U << 2)
#define ACRN_PM_CAP_SYSTEM_SUSPEND	(1U << 8)
#define ACRN_INVALID_VMID		0xffffU

struct acrn_pm_ioc {
	u32 abi_version;
	u32 ioc_size;
	u32 op;
	s32 status;
	u64 epoch;
	u64 wake_reason;
	u64 required_vm_mask;
	u32 pm_state;
	u32 vm_state;
	u16 vmid;
	u16 flags;
	u32 event_virq;
	u64 reserved;
} __aligned(64);

static_assert(sizeof(struct acrn_pm_ioc) == 64);

#endif /* _BEAU_PM_H */
