/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BSP_VIRTIO_PROXY_H
#define BSP_VIRTIO_PROXY_H

#include <types.h>
#include <acrn_hv_defs.h>
#include <spinlock.h>
#include <virtio_mmio.h>
#include <virtio_proxy.h>

/*
 * virtio-proxy bridge model:
 *
 * virtio_proxy is a protocol-neutral virtio-mmio endpoint that lets a guest
 * frontend driver talk to a matching backend implementation through one common
 * transport shell.
 *
 *   guest VM frontend          BEAU proxy transport             backend owner
 *   -----------------          --------------------             -------------
 *   virtio-blk driver   <-->   device-id/config/vring   <-->   blk backend
 *   virtio-net driver   <-->   device-id/config/vring   <-->   net backend
 *   virtio-fs  driver   <-->   device-id/config/vring   <-->   fs backend
 *   virtio-i2c driver   <-->   device-id/config/vring   <-->   i2c backend
 *   virtio-spi driver   <-->   device-id/config/vring   <-->   spi backend
 *
 * BEAU owns MMIO register emulation, queue lifetime notifications, opaque
 * config-space plumbing, and IRQ signaling through virtio_mmio. Backends own
 * protocol semantics: descriptor-chain layout, request validation, feature
 * bits, access-policy enforcement, and used-ring completion.
 *
 * Runtime framework:
 *
 *   frontend Linux                             BEAU EL2                         backend Linux
 *   --------------                             -------                         -------------
 *
 *   virtio driver
 *      |
 *      | MMIO probe/read config
 *      v
 *   virtio-mmio regs  <---------------->  virtio_proxy_dev
 *      |                                  - device_id / tag / features
 *      |                                  - static config bytes
 *      |                                  - queue ready state
 *      |
 *      | QueueReady: desc/avail/used GPA
 *      v
 *   frontend vring   ----------------->   virtio_mmio_queue
 *      |                                  - descriptor table GPA
 *      |                                  - avail / used ring GPA
 *      |
 *      | QueueNotify trap
 *      v
 *   MMIO QueueNotify ----------------->   virtio_proxy_copy_chain_to_pending()
 *                                         - copies frontend-readable descs
 *                                         - records frontend-writable descs
 *                                         - preserves queue id and head desc
 *                                                  |
 *                                                  | HVC poll(device_id, vmid, queue)
 *                                                  v
 *                                         virtio_proxy_pending  ------------> backend thread
 *                                                  ^                         - fs/rng/blk semantics
 *                                                  | HVC reply(out bytes)    - access policy
 *                                                  |                         - status byte / payload
 *                                                  |
 *                                         virtio_proxy_hcall_reply()
 *                                         - copies reply to writable descs
 *                                         - pushes used-ring entry
 *                                         - injects frontend IRQ
 *      ^                                           |
 *      | interrupt                                 |
 *      +-------------------------------------------+
 *
 * The transport never parses FUSE, RNG, block, net, I2C, or SPI payloads.
 * It only moves descriptor-chain bytes and reports completion. That keeps one
 * high/low-throughput transport usable by multiple virtio protocols while the
 * backend code remains responsible for protocol correctness and safety checks.
 *
 * Reset/recovery state model:
 *
 *   WAIT_BACKEND -------------- backend register ------------> BACKEND_READY
 *        |                                                        |
 *        | frontend queue ready                                  | frontend queue ready
 *        v                                                        v
 *   FRONTEND_READY ----------- backend register ------------> RUNNING
 *        ^                                                        |
 *        | frontend VM reset clears queues/pending                | backend VM reset/release
 *        +--------------------------------------------------------+
 *                                                                 |
 *                                                                 v
 *                                                            BACKEND_LOST
 *                                                                 |
 *                                      backend re-register + poll |
 *                                                                 v
 *                                                            RUNNING
 *
 * Frontend notify before backend registration never consumes the avail ring.
 * If the backend resets after BEAU has already popped a frontend request into a
 * pending slot, BEAU marks that slot unsent instead of dropping it. The rebuilt
 * backend can register again and poll the same request, while a frontend reset
 * clears only its transport state and pending slots before Linux negotiates again.
 */

/* Common transport limits. */
#define VIRTIO_PROXY_CONFIG_SIZE	64U
#define VIRTIO_PROXY_CHAIN_LIMIT	ACRN_VIRTIO_PROXY_DESC_MAX
#define VIRTIO_PROXY_PENDING_LOW	1U
#define VIRTIO_PROXY_PENDING_HIGH	VIRTIO_PROXY_PENDING_MAX
#define VIRTIO_PROXY_TIMEOUT_US		100000U
#define VIRTIO_PROXY_HEARTBEAT_TIMEOUT_US	5000000U
#define VIRTIO_PROXY_WAIT_LOW_US	1000U
#define VIRTIO_PROXY_WAIT_HIGH_US	100U
#define VIRTIO_PROXY_CAP_SUPPORTED	\
	(ACRN_VIRTIO_PROXY_CAP_WAIT_HINT | \
	 ACRN_VIRTIO_PROXY_CAP_HEARTBEAT | \
	 ACRN_VIRTIO_PROXY_CAP_STATS | \
	 ACRN_VIRTIO_PROXY_CAP_BATCH | \
	 ACRN_VIRTIO_PROXY_CAP_SHARED_RING)

/* Minimal virtio-net feature/config values used by the QEMU validation path. */
#define VIRTIO_NET_F_MAC		5U
#define VIRTIO_NET_F_STATUS		16U
#define VIRTIO_NET_S_LINK_UP		1U

/* Minimal virtio-blk feature/config values used by the QEMU validation path. */
#define VIRTIO_BLK_F_SIZE_MAX		1U
#define VIRTIO_BLK_F_SEG_MAX		2U
#define VIRTIO_PROXY_BLK_SIZE_MAX	4096U
#define VIRTIO_PROXY_BLK_SEG_MAX	1U
#define VIRTIO_PROXY_BLK_CAPACITY	2048UL

/* virtio-i2c feature bits used by the QEMU validation path. */
#define VIRTIO_I2C_F_ZERO_LENGTH_REQUEST	0U

/*
 * struct virtio_proxy_net_config - static virtio-net config-space image.
 *
 * @mac: Stable MAC selected for the owning frontend VM.
 * @status: Link status when VIRTIO_NET_F_STATUS is offered.
 */
struct virtio_proxy_net_config {
	uint8_t mac[6];
	uint16_t status;
};

/*
 * struct virtio_proxy_fs_config - static virtio-fs config-space image.
 *
 * @tag: Mount tag exposed to the frontend virtio-fs driver.
 * @num_request_queues: Number of request queues after the high-priority queue.
 */
struct virtio_proxy_fs_config {
	char tag[VIRTIO_PROXY_TAG_MAX];
	uint32_t num_request_queues;
};

/*
 * struct virtio_proxy_blk_config - static virtio-blk config-space image.
 *
 * @capacity: Disk size in 512-byte sectors.
 * @size_max: Maximum byte size of one data segment when SIZE_MAX is offered.
 * @seg_max: Maximum number of data segments when SEG_MAX is offered.
 * @cylinders: Legacy geometry cylinder count, unused by the validation disk.
 * @heads: Legacy geometry head count, unused by the validation disk.
 * @sectors: Legacy geometry sector count, unused by the validation disk.
 * @blk_size: Logical block size when BLK_SIZE is offered.
 * @physical_block_exp: Physical block exponent when TOPOLOGY is offered.
 * @alignment_offset: Alignment offset when TOPOLOGY is offered.
 * @min_io_size: Minimum I/O size when TOPOLOGY is offered.
 * @opt_io_size: Optimal I/O size when TOPOLOGY is offered.
 * @wce: Writeback cache enable when CONFIG_WCE is offered.
 * @unused: Reserved padding byte in the legacy-compatible layout.
 * @num_queues: Queue count when MQ is offered.
 */
struct virtio_proxy_blk_config {
	uint64_t capacity;
	uint32_t size_max;
	uint32_t seg_max;
	uint16_t cylinders;
	uint8_t heads;
	uint8_t sectors;
	uint32_t blk_size;
	uint8_t physical_block_exp;
	uint8_t alignment_offset;
	uint16_t min_io_size;
	uint32_t opt_io_size;
	uint8_t wce;
	uint8_t unused;
	uint16_t num_queues;
};

/*
 * struct virtio_proxy_pending_out - one frontend-writable descriptor slice.
 *
 * @gpa: Guest physical address of the writable descriptor.
 * @len: Writable byte capacity of this descriptor.
 */
struct virtio_proxy_pending_out {
	uint64_t gpa;
	uint32_t len;
};

/*
 * struct virtio_proxy_latency_accum - tick-based min/avg/max accumulator.
 *
 * @count: Number of completed latency samples.
 * @min: Minimum observed latency in ticks, zero until the first sample.
 * @max: Maximum observed latency in ticks.
 * @sum: Sum of observed latencies in ticks for average calculation.
 */
struct virtio_proxy_latency_accum {
	uint64_t count;
	uint64_t min;
	uint64_t max;
	uint64_t sum;
};

/*
 * struct virtio_proxy_pending - descriptor chain held for a backend poll.
 *
 * @valid: Slot contains a complete frontend descriptor chain.
 * @sent: Chain has been delivered to a backend through HVC poll.
 * @done: Backend replied and the chain has been completed to the frontend.
 * @timeout_reported: Timeout for the current backend-owned request was counted.
 * @queue_id: Virtqueue index that produced this request.
 * @head: Head descriptor id from the frontend avail ring.
 * @in_len: Bytes copied from frontend-readable descriptors into @in.
 * @out_len: Total writable capacity across @out descriptors.
 * @notify_tick: QueueNotify timestamp associated with this request.
 * @poll_tick: Successful backend-poll timestamp.
 * @reply_tick: Backend-reply entry timestamp.
 * @irq_tick: Frontend used-ring IRQ timestamp.
 * @in: Contiguous copy of frontend-readable descriptor data.
 * @out: Frontend-writable descriptor slices preserved for reply copy-back.
 * @out_count: Number of valid entries in @out.
 */
struct virtio_proxy_pending {
	bool valid;
	bool sent;
	bool done;
	bool timeout_reported;
	uint16_t queue_id;
	uint16_t head;
	uint16_t in_len;
	uint32_t out_len;
	uint64_t notify_tick;
	uint64_t poll_tick;
	uint64_t reply_tick;
	uint64_t irq_tick;
	uint8_t in[ACRN_VIRTIO_PROXY_DATA_MAX];
	struct virtio_proxy_pending_out out[ACRN_VIRTIO_PROXY_DESC_MAX];
	uint16_t out_count;
};

/*
 * struct virtio_proxy_dev - per-VM virtio-proxy endpoint runtime state.
 *
 * @mmio: Embedded virtio-mmio transport device.
 * @lock: Protects backend registration, pending slots, and counters.
 * @index: Endpoint index within the owning VM's virtio-proxy array.
 * @config: Static fallback config-space bytes read by the frontend.
 * @config_size: Valid byte count in @config.
 * @device_id: Virtio device id advertised to the frontend.
 * @access: Board-selected access hint, read-only or read-write.
 * @throughput: Board-selected throughput profile for pending slot depth.
 * @state: Coarse frontend/backend lifecycle state for reset recovery.
 * @tag: Human-readable endpoint tag and protocol-specific config source.
 * @backend_ops: Optional in-hypervisor backend callbacks.
 * @backend_priv: Private pointer passed to @backend_ops callbacks.
 * @notify_count: Number of frontend QueueNotify events received.
 * @notify_coalesced_count: QueueNotify events merged into pending work.
 * @notify_prefetch_count: Descriptor chains copied to pending during notify.
 * @notify_backend_kick_count: In-hypervisor backend notify callbacks issued.
 * @notify_backpressure_count: Notify-time prefetches blocked by pending slots.
 * @no_backend_logged: Suppresses repeated notify-without-backend logs.
 * @hcall_backend_registered: True after a VM backend registers by HVC.
 * @hcall_backend_expected: True when this endpoint expects an HVC backend.
 * @configured_backend_vmid: Static VM id allowed to register this endpoint.
 * @backend_vmid: VM id of the registered HVC backend.
 * @pending_limit: Active pending slot count for this throughput profile.
 * @pending: Descriptor chains waiting for or owned by a backend.
 * @hcall_register_count: Total backend register attempts.
 * @hcall_poll_count: Total backend poll attempts.
 * @hcall_poll_ok_count: Polls that returned a descriptor chain.
 * @hcall_reply_count: Total backend reply attempts.
 * @hcall_reply_ok_count: Replies that completed a frontend descriptor chain.
 * @hcall_empty_poll_count: Polls that found no frontend request.
 * @hcall_heartbeat_count: Backend liveness heartbeat count.
 * @hcall_busy_count: Polls that found no available frontend request.
 * @hcall_backpressure_count: Polls rejected because pending slots were full.
 * @timeout_count: Number of in-flight requests that exceeded timeout budget.
 * @reset_count: Number of frontend transport resets.
 * @completed_count: Requests completed back to the frontend used ring.
 * @irq_count: Frontend used-ring IRQ injections requested by proxy hcalls.
 * @batch_irq_saved_count: Extra per-request IRQs avoided by batch replies.
 * @request_bytes: Bytes copied from frontend-readable descriptors.
 * @reply_bytes: Bytes copied into frontend-writable descriptors.
 * @first_activity_tick: First byte-transfer timestamp for throughput stats.
 * @last_activity_tick: Last byte-transfer timestamp for throughput stats.
 * @last_notify_tick: Last QueueNotify timestamp per virtqueue.
 * @latency_notify_poll: QueueNotify to successful backend poll latency.
 * @latency_poll_reply: Successful backend poll to backend reply latency.
 * @latency_reply_irq: Backend reply to frontend IRQ latency.
 * @latency_total: QueueNotify to frontend IRQ latency.
 * @last_hcall_op: Last backend HVC operation id.
 * @last_hcall_ret: Last backend HVC return value.
 * @last_poll_queue_id: Queue id from the last backend poll.
 * @last_poll_head: Head descriptor id returned by the last successful poll.
 * @last_poll_status: Status flags returned by the last successful poll.
 * @last_reply_queue_id: Queue id from the last backend reply.
 * @last_reply_head: Head descriptor id completed by the last reply.
 * @last_reply_len: Used-ring length reported by the last reply.
 */
struct virtio_proxy_dev {
	struct virtio_mmio_dev mmio;
	spinlock_t lock;
	uint16_t index;
	uint8_t config[VIRTIO_PROXY_CONFIG_SIZE];
	uint32_t config_size;
	uint32_t device_id;
	uint32_t access;
	uint32_t throughput;
	uint32_t state;
	char tag[VIRTIO_PROXY_TAG_MAX];
	const struct virtio_proxy_backend_ops *backend_ops;
	void *backend_priv;
	uint64_t notify_count;
	uint64_t notify_coalesced_count;
	uint64_t notify_prefetch_count;
	uint64_t notify_backend_kick_count;
	uint64_t notify_backpressure_count;
	bool no_backend_logged;
	bool hcall_backend_registered;
	bool hcall_backend_expected;
	uint16_t configured_backend_vmid;
	uint16_t backend_vmid;
	uint16_t pending_limit;
	struct virtio_proxy_pending pending[VIRTIO_PROXY_PENDING_MAX];
	uint64_t hcall_register_count;
	uint64_t hcall_poll_count;
	uint64_t hcall_poll_ok_count;
	uint64_t hcall_reply_count;
	uint64_t hcall_reply_ok_count;
	uint64_t hcall_batch_poll_count;
	uint64_t hcall_batch_poll_ok_count;
	uint64_t hcall_batch_reply_count;
	uint64_t hcall_batch_reply_ok_count;
	uint64_t hcall_batch_poll_item_count;
	uint64_t hcall_batch_reply_item_count;
	uint64_t hcall_empty_poll_count;
	uint64_t hcall_heartbeat_count;
	uint64_t hcall_busy_count;
	uint64_t hcall_backpressure_count;
	uint32_t backend_abi_version;
	uint32_t backend_caps;
	uint64_t last_heartbeat_tick;
	uint32_t last_wait_us;
	uint64_t timeout_count;
	uint64_t reset_count;
	uint64_t completed_count;
	uint64_t irq_count;
	uint64_t batch_irq_saved_count;
	uint64_t request_bytes;
	uint64_t reply_bytes;
	uint64_t first_activity_tick;
	uint64_t last_activity_tick;
	uint64_t last_notify_tick[VIRTIO_MMIO_MAX_QUEUES];
	struct virtio_proxy_latency_accum latency_notify_poll;
	struct virtio_proxy_latency_accum latency_poll_reply;
	struct virtio_proxy_latency_accum latency_reply_irq;
	struct virtio_proxy_latency_accum latency_total;
	uint32_t last_hcall_op;
	int32_t last_hcall_ret;
	uint16_t last_poll_queue_id;
	uint16_t last_poll_head;
	uint32_t last_poll_status;
	uint32_t last_batch_count;
	uint16_t last_reply_queue_id;
	uint16_t last_reply_head;
	uint32_t last_reply_len;
};

#endif /* BSP_VIRTIO_PROXY_H */
