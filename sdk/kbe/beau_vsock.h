/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BEAU_VSOCK_H
#define _BEAU_VSOCK_H

#include <linux/virtio_vsock.h>

#define BEAU_VSOCK_HCALL_ID		0x80000069UL
#define BEAU_VSOCK_ABI_VERSION		1U

#define BEAU_VSOCK_OP_REGISTER		0U
#define BEAU_VSOCK_OP_POLL_TX		1U
#define BEAU_VSOCK_OP_SEND_RX		2U
#define BEAU_VSOCK_OP_HEARTBEAT	3U

#define BEAU_VSOCK_VM1_CID		3U
#define BEAU_VSOCK_VM2_CID		4U
#define BEAU_VSOCK_VM3_CID		5U

#define BEAU_VSOCK_PACKET_MAX		(VIRTIO_VSOCK_MAX_PKT_BUF_SIZE + \
					 sizeof(struct virtio_vsock_hdr))

/*
 * One HVC exchanges one complete virtio-vsock packet.  buffer_gpa always
 * names caller-owned, physically contiguous memory.  POLL_TX returns the
 * source CID and packet_len; SEND_RX supplies the destination CID and packet.
 */
struct beau_vsock_ioc {
	__u32 op;
	__u32 status;
	__u32 abi_version;
	__u32 ioc_size;
	__u32 local_cid;
	__u32 peer_cid;
	__u32 packet_len;
	__u32 buffer_len;
	__u64 buffer_gpa;
	__u64 heartbeat_seq;
	__u32 wait_us;
	__u32 flags;
} __aligned(8);

static_assert(sizeof(struct beau_vsock_ioc) == 56U);

#endif /* _BEAU_VSOCK_H */
