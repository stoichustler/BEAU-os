/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <acrn_hv_defs.h>
#include <guest_memory.h>
#include <logmsg.h>
#include <spinlock.h>
#include <vconfig.h>
#include <vm.h>
#include <vcpu.h>
#include <bsp/io_req.h>
#include <virtio_mmio.h>
#include <vsock_proxy.h>

#define VSOCK_PROXY_BACKEND_CID	3U
#define VSOCK_PROXY_RX_QUEUE		0U
#define VSOCK_PROXY_TX_QUEUE		1U
#define VSOCK_PROXY_EVENT_QUEUE	2U
#define VSOCK_PROXY_DESC_LIMIT	64U
#define VSOCK_PROXY_COPY_CHUNK	256U
#define VSOCK_PROXY_EVENT_RESET	0U

struct vsock_proxy_hdr {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint32_t len;
	uint16_t type;
	uint16_t op;
	uint32_t flags;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
} __packed;

struct vsock_proxy_event {
	uint32_t id;
} __packed;

struct vsock_proxy_dev {
	struct virtio_mmio_dev mmio;
	spinlock_t lock;
	uint16_t frontend_vmid;
	uint16_t backend_vmid;
	uint64_t frontend_cid;
	bool valid;
	bool backend_registered;
	bool reset_pending;
};

static struct vsock_proxy_dev vsock_proxy_devs[CONFIG_MAX_VM_NUM];
static uint32_t vsock_proxy_bad_abi_log_count;

/* [20260806] vsock proxy ownership boundary
 *
 * VM2/VM3 virtqueue -> EL2 validates static CID direction -> VM1 HVC buffer
 * VM1 HVC buffer   -> EL2 validates static CID direction -> VM2/VM3 RX ring
 *
 * Key rule:
 *   - frontend vrings and VM1 buffers remain owned by their respective VMs;
 *   - EL2 copies data only during the HVC which supplied the VM1 GPA;
 *   - no cross-VM GPA, descriptor, or packet pointer is retained after return.
 */

static struct vsock_proxy_dev *vsock_proxy_dev_for_vm(uint16_t vmid)
{
	return vmid < CONFIG_MAX_VM_NUM ? &vsock_proxy_devs[vmid] : NULL;
}

static struct vsock_proxy_dev *vsock_proxy_dev_for_cid(uint64_t cid)
{
	for (uint16_t i = 0U; i < CONFIG_MAX_VM_NUM; i++) {
		if (vsock_proxy_devs[i].valid && (vsock_proxy_devs[i].frontend_cid == cid)) {
			return &vsock_proxy_devs[i];
		}
	}
	return NULL;
}

void *vsock_proxy_get_dev(struct acrn_vm *vm)
{
	struct vsock_proxy_dev *dev = vm == NULL ? NULL :
		vsock_proxy_dev_for_vm(vm->vm_id);

	return (dev != NULL) && dev->valid ? dev : NULL;
}

static bool vsock_proxy_copy_frontend_to_backend(struct vsock_proxy_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t head, struct acrn_vm *backend,
	uint64_t backend_gpa, uint32_t capacity, uint32_t *packet_len)
{
	struct virtio_ring_desc desc;
	uint8_t buffer[VSOCK_PROXY_COPY_CHUNK];
	uint32_t copied = 0U;
	uint16_t id = head;
	uint16_t count = 0U;
	bool more = true;

	while (more && (count++ < VSOCK_PROXY_DESC_LIMIT)) {
		uint32_t offset = 0U;

		if ((id >= vq->num) || !virtio_mmio_read_desc(&dev->mmio, vq, id, &desc) ||
			((desc.flags & VIRTIO_RING_F_WRITE) != 0U) ||
			(desc.len > (capacity - copied))) {
			return false;
		}
		while (offset < desc.len) {
			uint32_t chunk = (desc.len - offset) < sizeof(buffer) ?
				desc.len - offset : (uint32_t)sizeof(buffer);

			if (!virtio_mmio_read_gpa(&dev->mmio, desc.addr + offset, buffer, chunk) ||
				(copy_to_gpa(backend, buffer, backend_gpa + copied + offset, chunk) != 0)) {
				return false;
			}
			offset += chunk;
		}
		copied += desc.len;
		more = (desc.flags & VIRTIO_RING_F_NEXT) != 0U;
		id = desc.next;
	}
	if (more || (copied < sizeof(struct vsock_proxy_hdr))) {
		return false;
	}
	*packet_len = copied;
	return true;
}

static bool vsock_proxy_copy_backend_to_frontend(struct vsock_proxy_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t head, struct acrn_vm *backend,
	uint64_t backend_gpa, uint32_t packet_len)
{
	struct virtio_ring_desc desc;
	uint8_t buffer[VSOCK_PROXY_COPY_CHUNK];
	uint32_t copied = 0U;
	uint16_t id = head;
	uint16_t count = 0U;
	bool more = true;

	while (more && (count++ < VSOCK_PROXY_DESC_LIMIT) && (copied < packet_len)) {
		uint32_t offset = 0U;

		if ((id >= vq->num) || !virtio_mmio_read_desc(&dev->mmio, vq, id, &desc) ||
			((desc.flags & VIRTIO_RING_F_WRITE) == 0U)) {
			return false;
		}
		while ((offset < desc.len) && (copied < packet_len)) {
			uint32_t chunk = (desc.len - offset) < (packet_len - copied) ?
				desc.len - offset : packet_len - copied;

			chunk = chunk < sizeof(buffer) ? chunk : (uint32_t)sizeof(buffer);
			if ((copy_from_gpa(backend, buffer, backend_gpa + copied, chunk) != 0) ||
				!virtio_mmio_write_gpa(&dev->mmio, desc.addr + offset, buffer, chunk)) {
				return false;
			}
			offset += chunk;
			copied += chunk;
		}
		more = (desc.flags & VIRTIO_RING_F_NEXT) != 0U;
		id = desc.next;
	}

	return copied == packet_len;
}

static bool vsock_proxy_header_valid(const struct vsock_proxy_hdr *hdr,
	uint64_t src_cid, uint64_t dst_cid, uint32_t packet_len)
{
	return (hdr != NULL) && (hdr->src_cid == src_cid) && (hdr->dst_cid == dst_cid) &&
		(hdr->type == 1U) && (hdr->op >= 1U) && (hdr->op <= 7U) &&
		(hdr->len <= (ACRN_VSOCK_PKT_MAX - sizeof(*hdr))) &&
		(packet_len == (sizeof(*hdr) + hdr->len));
}

static void vsock_proxy_publish_used(struct vsock_proxy_dev *dev,
	struct virtio_mmio_queue *vq, uint16_t head, uint32_t len)
{
	if (virtio_mmio_add_used(&dev->mmio, vq, head, len)) {
		virtio_mmio_raise_used_irq(&dev->mmio);
	}
}

static void vsock_proxy_publish_reset_locked(struct vsock_proxy_dev *dev)
{
	struct virtio_mmio_queue *vq;
	struct virtio_ring_desc desc;
	struct vsock_proxy_event event = { .id = VSOCK_PROXY_EVENT_RESET };
	uint16_t head;

	if (!dev->reset_pending) {
		return;
	}
	vq = virtio_mmio_get_queue(&dev->mmio, VSOCK_PROXY_EVENT_QUEUE);
	if ((vq == NULL) || !virtio_mmio_pop_avail(&dev->mmio, vq, &head)) {
		return;
	}
	if (!virtio_mmio_read_desc(&dev->mmio, vq, head, &desc) ||
		((desc.flags & VIRTIO_RING_F_WRITE) == 0U) ||
		(desc.len < sizeof(event)) ||
		!virtio_mmio_write_gpa(&dev->mmio, desc.addr, &event, sizeof(event))) {
		vsock_proxy_publish_used(dev, vq, head, 0U);
		return;
	}
	vsock_proxy_publish_used(dev, vq, head, sizeof(event));
	dev->reset_pending = false;
}

static void vsock_proxy_reset(struct virtio_mmio_dev *mmio)
{
	struct vsock_proxy_dev *dev = (struct vsock_proxy_dev *)virtio_mmio_priv(mmio);

	if (dev != NULL) {
		dev->reset_pending = true;
	}
}

static void vsock_proxy_notify_queue(struct virtio_mmio_dev *mmio, uint16_t queue_id)
{
	struct vsock_proxy_dev *dev = (struct vsock_proxy_dev *)virtio_mmio_priv(mmio);

	if ((dev != NULL) && (queue_id == VSOCK_PROXY_EVENT_QUEUE)) {
		spinlock_obtain(&dev->lock);
		vsock_proxy_publish_reset_locked(dev);
		spinlock_release(&dev->lock);
	}
}

static uint32_t vsock_proxy_read_config(struct virtio_mmio_dev *mmio,
	uint32_t offset, uint32_t size)
{
	struct vsock_proxy_dev *dev = (struct vsock_proxy_dev *)virtio_mmio_priv(mmio);
	uint64_t cid = dev == NULL ? 0UL : dev->frontend_cid;
	uint32_t value = 0U;

	if ((offset < sizeof(cid)) && (size <= sizeof(value)) &&
		((offset + size) <= sizeof(cid))) {
		(void)memcpy(&value, ((uint8_t *)&cid) + offset, size);
	}
	return value;
}

static const struct virtio_mmio_ops vsock_proxy_mmio_ops = {
	.reset = vsock_proxy_reset,
	.notify_queue = vsock_proxy_notify_queue,
	.read_config = vsock_proxy_read_config,
};

void vsock_proxy_init_vm(struct acrn_vm *vm)
{
	struct acrn_vm_config *config;

	if (vm == NULL) {
		return;
	}
	config = get_vm_config(vm->vm_id);
	for (uint16_t i = 0U; i < config->arch.guest_virtio_proxy_num; i++) {
		const struct arm64_virtio_proxy_config *proxy =
			&config->arch.guest_virtio_proxy[i];
		struct vsock_proxy_dev *dev;
		struct virtio_mmio_init init;

		if (proxy->device_id != VIRTIO_DEVICE_ID_VSOCK) {
			continue;
		}
		dev = vsock_proxy_dev_for_vm(vm->vm_id);
		if ((dev == NULL) || dev->valid) {
			continue;
		}
		(void)memset(dev, 0U, sizeof(*dev));
		dev->frontend_vmid = vm->vm_id;
		dev->backend_vmid = proxy->backend_vmid;
		dev->frontend_cid = proxy->vsock_cid;
		dev->valid = true;
		dev->reset_pending = true;
		init.name = "vsock-proxy";
		init.vm = vm;
		init.base = proxy->base;
		init.size = proxy->size;
		init.irq = proxy->irq;
		init.device_id = VIRTIO_DEVICE_ID_VSOCK;
		init.queue_num = 3U;
		init.queue_size = proxy->queue_size;
		init.device_features = 0UL;
		init.ops = &vsock_proxy_mmio_ops;
		init.priv = dev;
		virtio_mmio_init(&dev->mmio, &init);
		spinlock_init(&dev->lock);
	}
}

void vsock_proxy_reset_vm(struct acrn_vm *vm)
{
	struct vsock_proxy_dev *dev = vm == NULL ? NULL : vsock_proxy_dev_for_vm(vm->vm_id);

	if ((dev != NULL) && dev->valid) {
		virtio_mmio_reset_dev(&dev->mmio);
	}
}

void vsock_proxy_release_vm(struct acrn_vm *vm)
{
	if (vm == NULL) {
		return;
	}
	for (uint16_t i = 0U; i < CONFIG_MAX_VM_NUM; i++) {
		struct vsock_proxy_dev *dev = &vsock_proxy_devs[i];

		if (dev->valid && ((dev->frontend_vmid == vm->vm_id) ||
			(dev->backend_vmid == vm->vm_id))) {
			spinlock_obtain(&dev->lock);
			if (dev->backend_vmid == vm->vm_id) {
				dev->backend_registered = false;
				dev->reset_pending = true;
			}
			spinlock_release(&dev->lock);
		}
	}
}

int32_t vsock_proxy_mmio_handler(struct io_request *io_req, void *handler_private_data)
{
	struct vsock_proxy_dev *dev = (struct vsock_proxy_dev *)handler_private_data;

	return (dev != NULL) && dev->valid ? virtio_mmio_handler(io_req, &dev->mmio) : -EINVAL;
}

static int32_t vsock_proxy_register_backend(struct acrn_vcpu *vcpu,
	struct acrn_virtio_vsock_ioc *ioc)
{
	bool found = false;

	if (ioc->local_cid != VSOCK_PROXY_BACKEND_CID) {
		return -EPERM;
	}
	for (uint16_t i = 0U; i < CONFIG_MAX_VM_NUM; i++) {
		struct vsock_proxy_dev *dev = &vsock_proxy_devs[i];

		if (!dev->valid || (dev->backend_vmid != vcpu->vm->vm_id)) {
			continue;
		}
		spinlock_obtain(&dev->lock);
		dev->backend_registered = true;
		dev->reset_pending = true;
		vsock_proxy_publish_reset_locked(dev);
		spinlock_release(&dev->lock);
		found = true;
	}
	return found ? 0 : -ENODEV;
}

static int32_t vsock_proxy_poll_tx(struct acrn_vcpu *vcpu,
	struct acrn_virtio_vsock_ioc *ioc)
{
	struct vsock_proxy_hdr hdr;

	if ((ioc->buffer_gpa == 0UL) || (ioc->buffer_len < sizeof(hdr)) ||
		(ioc->buffer_len > ACRN_VSOCK_PKT_MAX)) {
		return -EINVAL;
	}
	for (uint16_t i = 0U; i < CONFIG_MAX_VM_NUM; i++) {
		struct vsock_proxy_dev *dev = &vsock_proxy_devs[i];
		struct virtio_mmio_queue *vq;
		struct virtio_ring_desc first;
		uint16_t head;
		uint32_t len = 0U;

		if (!dev->valid || (dev->backend_vmid != vcpu->vm->vm_id)) {
			continue;
		}
		spinlock_obtain(&dev->lock);
		vq = virtio_mmio_get_queue(&dev->mmio, VSOCK_PROXY_TX_QUEUE);
		if (!dev->backend_registered || (vq == NULL) ||
			!virtio_mmio_pop_avail(&dev->mmio, vq, &head)) {
			spinlock_release(&dev->lock);
			continue;
		}
		if (
			!virtio_mmio_read_desc(&dev->mmio, vq, head, &first) ||
			((first.flags & VIRTIO_RING_F_WRITE) != 0U) ||
			(first.len < sizeof(hdr)) ||
			!virtio_mmio_read_gpa(&dev->mmio, first.addr, &hdr, sizeof(hdr)) ||
			!vsock_proxy_header_valid(&hdr, dev->frontend_cid,
				VSOCK_PROXY_BACKEND_CID, sizeof(hdr) + hdr.len) ||
			!vsock_proxy_copy_frontend_to_backend(dev, vq, head, vcpu->vm,
				ioc->buffer_gpa, ioc->buffer_len, &len)) {
			vsock_proxy_publish_used(dev, vq, head, 0U);
			spinlock_release(&dev->lock);
			return -EINVAL;
		}
		vsock_proxy_publish_used(dev, vq, head, len);
		spinlock_release(&dev->lock);
		ioc->peer_cid = (uint32_t)dev->frontend_cid;
		ioc->packet_len = len;
		return 0;
	}
	return -ENODATA;
}

static int32_t vsock_proxy_send_rx(struct acrn_vcpu *vcpu,
	struct acrn_virtio_vsock_ioc *ioc)
{
	struct vsock_proxy_hdr hdr;
	struct vsock_proxy_dev *dev;
	struct virtio_mmio_queue *vq;
	uint16_t head;
	int32_t ret = -EAGAIN;

	if ((ioc->buffer_gpa == 0UL) || (ioc->buffer_len < ioc->packet_len) ||
		(ioc->packet_len < sizeof(hdr)) ||
		(ioc->packet_len > ACRN_VSOCK_PKT_MAX) ||
		(copy_from_gpa(vcpu->vm, &hdr, ioc->buffer_gpa, sizeof(hdr)) != 0)) {
		return -EINVAL;
	}
	dev = vsock_proxy_dev_for_cid(hdr.dst_cid);
	if ((dev == NULL) || !dev->valid || (dev->backend_vmid != vcpu->vm->vm_id) ||
		!vsock_proxy_header_valid(&hdr, VSOCK_PROXY_BACKEND_CID,
			dev->frontend_cid, ioc->packet_len)) {
		return -EPERM;
	}
	spinlock_obtain(&dev->lock);
	vq = virtio_mmio_get_queue(&dev->mmio, VSOCK_PROXY_RX_QUEUE);
	if (!dev->backend_registered || (vq == NULL) ||
		!virtio_mmio_pop_avail(&dev->mmio, vq, &head)) {
		spinlock_release(&dev->lock);
		return ret;
	}
	if (vsock_proxy_copy_backend_to_frontend(dev, vq, head, vcpu->vm,
		ioc->buffer_gpa, ioc->packet_len)) {
		vsock_proxy_publish_used(dev, vq, head, ioc->packet_len);
		ret = 0;
	} else {
		vsock_proxy_publish_used(dev, vq, head, 0U);
	}
	spinlock_release(&dev->lock);
	return ret;
}

int32_t vsock_proxy_backend_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa)
{
	struct acrn_virtio_vsock_ioc ioc;
	int32_t ret;

	if ((vcpu == NULL) || (ioc_gpa == 0UL) ||
		(copy_from_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc)) != 0)) {
		return -EFAULT;
	}
	if ((ioc.abi_version != ACRN_VSOCK_ABI_VERSION) ||
		(ioc.ioc_size < sizeof(ioc))) {
		if (vsock_proxy_bad_abi_log_count < 4U) {
			LOG_WRN("vsock: vm%u ABI op:%u ver:%u size:%u expected:%u/%u",
				vcpu->vm->vm_id, ioc.op, ioc.abi_version, ioc.ioc_size,
				ACRN_VSOCK_ABI_VERSION, (uint32_t)sizeof(ioc));
			vsock_proxy_bad_abi_log_count++;
		}
		ioc.status = (uint32_t)-EINVAL;
		(void)copy_to_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc));
		return -EINVAL;
	}
	ioc.status = ACRN_VSOCK_STATUS_OK;
	switch (ioc.op) {
	case ACRN_VSOCK_OP_REGISTER:
		ret = vsock_proxy_register_backend(vcpu, &ioc);
		break;
	case ACRN_VSOCK_OP_POLL_TX:
		ret = vsock_proxy_poll_tx(vcpu, &ioc);
		if (ret == -ENODATA) {
			ioc.status = ACRN_VSOCK_STATUS_EMPTY;
		}
		break;
	case ACRN_VSOCK_OP_SEND_RX:
		ret = vsock_proxy_send_rx(vcpu, &ioc);
		if (ret == -EAGAIN) {
			ioc.status = ACRN_VSOCK_STATUS_BACKPRESSURE;
		}
		break;
	case ACRN_VSOCK_OP_HEARTBEAT:
		ret = vsock_proxy_register_backend(vcpu, &ioc);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret < 0) {
		ioc.status = (ioc.status == ACRN_VSOCK_STATUS_OK) ? (uint32_t)ret : ioc.status;
	}
	return copy_to_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc)) == 0 ? ret : -EFAULT;
}
