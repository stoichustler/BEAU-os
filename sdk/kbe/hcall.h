/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_HCALL_H
#define _BEAU_HCALL_H

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/limits.h>
#include <linux/types.h>

#define BEAU_PROXY_ABI_VERSION		3U
#define BEAU_VM_CRASH_MAGIC		0x42435253U
#define BEAU_VM_CRASH_ABI_VERSION	2U
#define BEAU_VM_CRASH_TEXT_MAX		96U
#define BEAU_VM_CRASH_COMM_MAX		16U
#define BEAU_VM_CRASH_STACK_MAX		16U
#define BEAU_VM_CRASH_REPORT_SIZE	320U

#define BEAU_VM_WDT_KICK_F_PER_VCPU_V1	0x4257445400000001UL

#define BEAU_VM_CRASH_F_REGS_VALID	BIT(0)
#define BEAU_VM_CRASH_F_STACK_VALID	BIT(1)

#define BEAU_VM_CRASH_PANIC		1U
#define BEAU_VM_CRASH_OOPS		2U

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

#define BEAU_IPC_ABI_VERSION		1U
#define BEAU_IPC_OP_QUERY		0U
#define BEAU_IPC_OP_NOTIFY		1U
#define BEAU_IPC_OP_ACK		2U
#define BEAU_IPC_STATUS_OK		0U
#define BEAU_IPC_STATUS_BAD_PARAM	1U
#define BEAU_IPC_STATUS_NO_CHANNEL	2U
#define BEAU_IPC_RING_MAGIC		0x42495043U
#define BEAU_IPC_CHANNEL_ANY		U32_MAX
#define BEAU_IPC_RING_COUNT		2U
#define BEAU_IPC_DIR_EP0_TO_EP1	0U
#define BEAU_IPC_DIR_EP1_TO_EP0	1U
#define BEAU_IPC_FLAG_NOTIFY_IRQ	BIT(0)

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

struct beau_ipc_ioc {
	u32 op;
	u32 status;
	u32 abi_version;
	u32 ioc_size;
	u32 channel_id;
	u16 peer_vmid;
	u16 flags;
	u64 gpa_base;
	u32 ring_size;
	u32 ring_count;
	u32 notify_count;
	u32 ack_count;
	u32 reserved;
} __aligned(8);

struct beau_ipc_ring_header {
	u32 magic;
	u32 version;
	u32 header_size;
	u32 ring_size;
	u16 owner_vmid;
	u16 peer_vmid;
	u16 direction;
	u16 flags;
	u32 elem_size;
	u32 elem_count;
	u64 prod __aligned(64);
	u64 cons __aligned(64);
	u64 notify_count;
	u64 drop_count;
	u64 bytes;
} __aligned(64);

struct beau_vm_crash_report {
	u32 magic;
	u16 version;
	u16 size;
	u32 kind;
	u32 cpu_id;
	u64 sequence;
	u64 pc;
	u64 fault_address;
	u64 error_code;
	char message[BEAU_VM_CRASH_TEXT_MAX];
	u64 sp;
	u64 pstate;
	u32 pid;
	u32 tgid;
	u16 stack_count;
	u16 flags;
	char comm[BEAU_VM_CRASH_COMM_MAX];
	unsigned long stack[BEAU_VM_CRASH_STACK_MAX];
};

static_assert(offsetof(struct beau_vm_crash_report, message) == 48U);
static_assert(sizeof(struct beau_vm_crash_report) == BEAU_VM_CRASH_REPORT_SIZE);

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
long beau_hcall_vm_wdt_kick_vcpu(unsigned long token);
long beau_hcall_vm_crash_report(const struct beau_vm_crash_report *report);
long beau_hcall_virtio_proxy_backend(struct beau_proxy_ioc *ioc);
long beau_hcall_ipc(struct beau_ipc_ioc *ioc);

#endif /* _BEAU_HCALL_H */
