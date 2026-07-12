/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <memory.h>
#include <spinlock.h>
#include <vm.h>
#include <vcpu.h>
#include <event.h>
#include <guest_memory.h>
#include <pgtable.h>
#include <ticks.h>
#include <acrn_hv_defs.h>
#include <asm/page.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vipc.h>

/* [20260712] VIPC static-channel framework
 *
 * platform.dts
 *     |
 *     v
 * arm64_vipc_register_channel()
 *     |
 *     +--> channel policy: channel-id, endpoint VMIDs, guest GPA, notify virq
 *     |
 *     +--> host ring storage: ring[0] ep0 -> ep1, ring[1] ep1 -> ep0
 *              |
 *              v
 *        arm64_vipc_init_vm()
 *              |
 *              +--> map the same host pages into each endpoint VM only
 *
 * Key rule:
 *   - platform data owns channel topology; VIPC core never hard-codes VM IDs;
 *   - host owns channel registration, stage-2 mappings, and notification stats;
 *   - endpoint guests own ring payload movement after QUERY returns the GPA;
 *   - non-endpoint VMs fail closed because they receive neither a stage-2
 *     mapping nor a successful channel query.
 */
struct arm64_vipc_channel {
	bool valid;
	struct arm64_vipc_channel_config config;
	uint32_t mapped_mask;
	uint64_t notify_count[ACRN_IPC_RING_COUNT];
	uint64_t ack_count[ACRN_IPC_RING_COUNT];
	uint64_t wake_count[ACRN_IPC_RING_COUNT];
	uint64_t irq_count[ACRN_IPC_RING_COUNT];
	uint64_t irq_fail_count[ACRN_IPC_RING_COUNT];
	uint64_t bad_hcall_count;
	uint64_t last_notify_tick[ACRN_IPC_RING_COUNT];
};

static struct arm64_vipc_channel arm64_vipc_channels[ARM64_VIPC_MAX_STATIC_CHANNELS];
static uint8_t arm64_vipc_rings[ARM64_VIPC_MAX_STATIC_CHANNELS][ACRN_IPC_RING_COUNT]
	[ARM64_VIPC_RING_SIZE_MAX] __aligned(PAGE_SIZE);
static spinlock_t arm64_vipc_lock = { .head = 0U, .tail = 0U };

/* [20260712] Ring ownership and ordering
 *
 * One channel contains two independent single-producer/single-consumer rings:
 *
 * endpoint[0] producer                 endpoint[1] consumer
 *        prod,payload  ---- ring[0] ---->  cons
 *
 * endpoint[1] producer                 endpoint[0] consumer
 *        prod,payload  ---- ring[1] ---->  cons
 *
 * Key rule:
 *   - producer writes payload before publishing prod;
 *   - consumer reads prod before payload and advances cons after consuming;
 *   - VIPC does not parse or copy payload bytes on the hot path;
 *   - HVC NOTIFY is a doorbell, not the data transport.
 */
static bool arm64_vipc_endpoint_index(const struct arm64_vipc_channel *channel,
	uint16_t vmid, uint32_t *endpoint_index)
{
	uint32_t idx;

	for (idx = 0U; idx < ACRN_IPC_RING_COUNT; idx++) {
		if (channel->config.endpoint_vmid[idx] == vmid) {
			if (endpoint_index != NULL) {
				*endpoint_index = idx;
			}
			return true;
		}
	}

	return false;
}

static uint32_t arm64_vipc_tx_dir(uint32_t endpoint_index)
{
	return endpoint_index == 0U ? ACRN_IPC_DIR_EP0_TO_EP1 :
		ACRN_IPC_DIR_EP1_TO_EP0;
}

static uint32_t arm64_vipc_rx_dir(uint32_t endpoint_index)
{
	return endpoint_index == 0U ? ACRN_IPC_DIR_EP1_TO_EP0 :
		ACRN_IPC_DIR_EP0_TO_EP1;
}

static uint16_t arm64_vipc_peer_vmid(const struct arm64_vipc_channel *channel,
	uint32_t endpoint_index)
{
	return channel->config.endpoint_vmid[endpoint_index == 0U ? 1U : 0U];
}

static struct acrn_ipc_ring_header *arm64_vipc_ring_header(uint32_t channel_index,
	uint32_t dir)
{
	return (struct acrn_ipc_ring_header *)&arm64_vipc_rings[channel_index][dir][0];
}

static void arm64_vipc_init_ring(uint32_t channel_index, uint32_t dir)
{
	const struct arm64_vipc_channel_config *config =
		&arm64_vipc_channels[channel_index].config;
	struct acrn_ipc_ring_header *header = arm64_vipc_ring_header(channel_index, dir);
	uint32_t owner_index = dir == ACRN_IPC_DIR_EP0_TO_EP1 ? 0U : 1U;
	uint32_t peer_index = owner_index == 0U ? 1U : 0U;
	uint32_t data_size = config->ring_size - sizeof(*header);

	(void)memset(header, 0U, config->ring_size);
	header->magic = ACRN_IPC_RING_MAGIC;
	header->version = ACRN_IPC_ABI_VERSION;
	header->header_size = sizeof(*header);
	header->ring_size = config->ring_size;
	header->owner_vmid = config->endpoint_vmid[owner_index];
	header->peer_vmid = config->endpoint_vmid[peer_index];
	header->direction = (uint16_t)dir;
	header->flags = 0U;
	header->elem_size = 1U;
	header->elem_count = data_size;
}

static bool arm64_vipc_channel_id_exists(uint32_t channel_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARRAY_SIZE(arm64_vipc_channels); idx++) {
		if (arm64_vipc_channels[idx].valid &&
			(arm64_vipc_channels[idx].config.channel_id == channel_id)) {
			return true;
		}
	}

	return false;
}

static int32_t arm64_vipc_validate_config(const struct arm64_vipc_channel_config *config)
{
	if ((config->endpoint_vmid[0] >= CONFIG_MAX_VM_NUM) ||
		(config->endpoint_vmid[1] >= CONFIG_MAX_VM_NUM) ||
		(config->endpoint_vmid[0] == config->endpoint_vmid[1])) {
		return -EINVAL;
	}
	if ((config->gpa_base == 0UL) ||
		((config->gpa_base & (PAGE_SIZE - 1UL)) != 0UL)) {
		return -EINVAL;
	}
	if ((config->ring_size < (sizeof(struct acrn_ipc_ring_header) + PAGE_SIZE)) ||
		(config->ring_size > ARM64_VIPC_RING_SIZE_MAX) ||
		((config->ring_size & (PAGE_SIZE - 1UL)) != 0U)) {
		return -EINVAL;
	}
	if (config->gpa_base > (UINT64_MAX - (uint64_t)(config->ring_size *
		ACRN_IPC_RING_COUNT))) {
		return -EINVAL;
	}
	if (arm64_vipc_channel_id_exists(config->channel_id)) {
		return -EBUSY;
	}

	return 0;
}

int32_t arm64_vipc_register_channel(const struct arm64_vipc_channel_config *config)
{
	struct arm64_vipc_channel_config normalized;
	uint32_t idx;
	int32_t ret;

	if (config == NULL) {
		return -EINVAL;
	}

	normalized = *config;
	if (normalized.ring_size == 0U) {
		normalized.ring_size = ARM64_VIPC_RING_SIZE_DEFAULT;
	}

	spinlock_obtain(&arm64_vipc_lock);
	ret = arm64_vipc_validate_config(&normalized);
	if (ret == 0) {
		for (idx = 0U; idx < ARRAY_SIZE(arm64_vipc_channels); idx++) {
			if (!arm64_vipc_channels[idx].valid) {
				(void)memset(&arm64_vipc_channels[idx], 0U,
					sizeof(arm64_vipc_channels[idx]));
				arm64_vipc_channels[idx].config = normalized;
				arm64_vipc_init_ring(idx, ACRN_IPC_DIR_EP0_TO_EP1);
				arm64_vipc_init_ring(idx, ACRN_IPC_DIR_EP1_TO_EP0);
				arm64_vipc_channels[idx].valid = true;
				ret = 0;
				break;
			}
		}
		if (idx == ARRAY_SIZE(arm64_vipc_channels)) {
			ret = -ENOMEM;
		}
	}
	spinlock_release(&arm64_vipc_lock);

	return ret;
}

static struct arm64_vipc_channel *arm64_vipc_find_channel(uint16_t vmid,
	uint32_t channel_id, uint32_t *channel_index, uint32_t *endpoint_index)
{
	uint32_t idx;

	for (idx = 0U; idx < ARRAY_SIZE(arm64_vipc_channels); idx++) {
		struct arm64_vipc_channel *channel = &arm64_vipc_channels[idx];

		if (!channel->valid) {
			continue;
		}
		if ((channel_id != ACRN_IPC_CHANNEL_ANY) &&
			(channel->config.channel_id != channel_id)) {
			continue;
		}
		if (arm64_vipc_endpoint_index(channel, vmid, endpoint_index)) {
			if (channel_index != NULL) {
				*channel_index = idx;
			}
			return channel;
		}
	}

	return NULL;
}

void arm64_vipc_init_vm(struct acrn_vm *vm)
{
	uint32_t idx;

	if (vm == NULL) {
		return;
	}

	for (idx = 0U; idx < ARRAY_SIZE(arm64_vipc_channels); idx++) {
		struct arm64_vipc_channel *channel = &arm64_vipc_channels[idx];
		uint32_t endpoint_index;
		uint32_t dir;

		if (!channel->valid ||
			!arm64_vipc_endpoint_index(channel, vm->vm_id, &endpoint_index)) {
			continue;
		}

		/* Map both directions into each endpoint so peers share identical
		 * ring headers and payload storage while unrelated VMs stay unmapped.
		 */
		for (dir = 0U; dir < ACRN_IPC_RING_COUNT; dir++) {
			uint64_t ipa = channel->config.gpa_base +
				((uint64_t)dir * channel->config.ring_size);
			uint64_t hpa = hva2hpa(&arm64_vipc_rings[idx][dir][0]);

			arm64_stage2_map_normal(vm, hpa, ipa, channel->config.ring_size, true);
		}

		spinlock_obtain(&arm64_vipc_lock);
		if (vm->vm_id < 32U) {
			channel->mapped_mask |= (1U << vm->vm_id);
		}
		spinlock_release(&arm64_vipc_lock);
	}
}

static void arm64_vipc_record_bad(struct arm64_vipc_channel *channel)
{
	if (channel != NULL) {
		spinlock_obtain(&arm64_vipc_lock);
		channel->bad_hcall_count++;
		spinlock_release(&arm64_vipc_lock);
	}
}

static void arm64_vipc_fill_ioc(struct arm64_vipc_channel *channel, uint32_t endpoint_index,
	struct acrn_ipc_ioc *ioc)
{
	uint32_t rx_dir = arm64_vipc_rx_dir(endpoint_index);

	ioc->status = ACRN_IPC_STATUS_OK;
	ioc->abi_version = ACRN_IPC_ABI_VERSION;
	ioc->ioc_size = sizeof(*ioc);
	ioc->channel_id = channel->config.channel_id;
	ioc->peer_vmid = arm64_vipc_peer_vmid(channel, endpoint_index);
	ioc->flags = channel->config.notify_virq != 0U ? ACRN_IPC_FLAG_NOTIFY_IRQ : 0U;
	ioc->gpa_base = channel->config.gpa_base;
	ioc->ring_size = channel->config.ring_size;
	ioc->ring_count = ACRN_IPC_RING_COUNT;
	ioc->notify_count = (uint32_t)channel->notify_count[arm64_vipc_tx_dir(endpoint_index)];
	ioc->ack_count = (uint32_t)channel->ack_count[rx_dir];
}

static void arm64_vipc_wake_peer(struct arm64_vipc_channel *channel, uint32_t dir)
{
	uint16_t peer_vmid = dir == ACRN_IPC_DIR_EP0_TO_EP1 ?
		channel->config.endpoint_vmid[1] : channel->config.endpoint_vmid[0];
	struct acrn_vm *peer_vm = get_vm_from_vmid(peer_vmid);
	struct acrn_vcpu *peer_vcpu;
	int32_t ret = -ENODEV;

	if ((peer_vm == NULL) || is_poweroff_vm(peer_vm) || (peer_vm->hw.created_vcpus == 0U)) {
		spinlock_obtain(&arm64_vipc_lock);
		channel->irq_fail_count[dir]++;
		spinlock_release(&arm64_vipc_lock);
		return;
	}

	peer_vcpu = vcpu_from_vid(peer_vm, 0U);
	/* If a virtual IRQ is configured, deliver a guest-visible interrupt.
	 * Otherwise nudge vCPU0 so polling guests can leave the run queue path
	 * promptly without adding an interrupt ABI.
	 */
	if (channel->config.notify_virq != 0U) {
		ret = arm64_vgicv3_inject_irq(peer_vcpu, channel->config.notify_virq, false);
	}

	spinlock_obtain(&arm64_vipc_lock);
	if (channel->config.notify_virq != 0U) {
		if (ret == 0) {
			channel->irq_count[dir]++;
		} else {
			channel->irq_fail_count[dir]++;
		}
	} else {
		channel->wake_count[dir]++;
	}
	spinlock_release(&arm64_vipc_lock);

	if (channel->config.notify_virq == 0U) {
		vcpu_make_request(peer_vcpu, ARM64_VCPU_REQUEST_EVENT);
		signal_event(&peer_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
	}
}

static int32_t arm64_vipc_handle_query(struct acrn_vcpu *vcpu,
	struct acrn_ipc_ioc *ioc, uint64_t ioc_gpa)
{
	struct arm64_vipc_channel *channel;
	uint32_t endpoint_index = UINT32_MAX;

	channel = arm64_vipc_find_channel(vcpu->vm->vm_id, ioc->channel_id, NULL,
		&endpoint_index);
	if (channel == NULL) {
		ioc->status = ACRN_IPC_STATUS_NO_CHANNEL;
		return copy_to_gpa(vcpu->vm, ioc, ioc_gpa, sizeof(*ioc));
	}

	spinlock_obtain(&arm64_vipc_lock);
	arm64_vipc_fill_ioc(channel, endpoint_index, ioc);
	spinlock_release(&arm64_vipc_lock);

	return copy_to_gpa(vcpu->vm, ioc, ioc_gpa, sizeof(*ioc));
}

static int32_t arm64_vipc_handle_notify(struct acrn_vcpu *vcpu,
	struct acrn_ipc_ioc *ioc, uint64_t ioc_gpa)
{
	struct arm64_vipc_channel *channel;
	struct acrn_ipc_ring_header *header;
	uint32_t channel_index = UINT32_MAX;
	uint32_t endpoint_index = UINT32_MAX;
	uint32_t dir;

	channel = arm64_vipc_find_channel(vcpu->vm->vm_id, ioc->channel_id, &channel_index,
		&endpoint_index);
	if (channel == NULL) {
		ioc->status = ACRN_IPC_STATUS_NO_CHANNEL;
		return copy_to_gpa(vcpu->vm, ioc, ioc_gpa, sizeof(*ioc));
	}

	dir = arm64_vipc_tx_dir(endpoint_index);
	header = arm64_vipc_ring_header(channel_index, dir);

	spinlock_obtain(&arm64_vipc_lock);
	channel->notify_count[dir]++;
	channel->last_notify_tick[dir] = cpu_ticks();
	header->notify_count++;
	arm64_vipc_fill_ioc(channel, endpoint_index, ioc);
	spinlock_release(&arm64_vipc_lock);

	arm64_vipc_wake_peer(channel, dir);
	return copy_to_gpa(vcpu->vm, ioc, ioc_gpa, sizeof(*ioc));
}

static int32_t arm64_vipc_handle_ack(struct acrn_vcpu *vcpu, struct acrn_ipc_ioc *ioc,
	uint64_t ioc_gpa)
{
	struct arm64_vipc_channel *channel;
	uint32_t endpoint_index = UINT32_MAX;
	uint32_t rx_dir;

	channel = arm64_vipc_find_channel(vcpu->vm->vm_id, ioc->channel_id, NULL,
		&endpoint_index);
	if (channel == NULL) {
		ioc->status = ACRN_IPC_STATUS_NO_CHANNEL;
		return copy_to_gpa(vcpu->vm, ioc, ioc_gpa, sizeof(*ioc));
	}

	rx_dir = arm64_vipc_rx_dir(endpoint_index);
	spinlock_obtain(&arm64_vipc_lock);
	channel->ack_count[rx_dir]++;
	arm64_vipc_fill_ioc(channel, endpoint_index, ioc);
	spinlock_release(&arm64_vipc_lock);

	return copy_to_gpa(vcpu->vm, ioc, ioc_gpa, sizeof(*ioc));
}

int32_t arm64_vipc_hcall(struct acrn_vcpu *vcpu, uint64_t ioc_gpa)
{
	struct acrn_ipc_ioc ioc;
	struct arm64_vipc_channel *channel = NULL;
	uint32_t endpoint_index = UINT32_MAX;
	int32_t ret;

	/* [20260712] HVC control flow
	 *
	 * guest IOC GPA
	 *     |
	 *     v
	 * copy_from_gpa()
	 *     |
	 *     +--> validate ABI/version/size
	 *     |
	 *     +--> QUERY:  return channel metadata and shared GPA
	 *     +--> NOTIFY: count doorbell, update ring header, wake peer
	 *     +--> ACK:    count receiver progress for diagnostics
	 *     |
	 *     v
	 * copy_to_gpa()
	 *
	 * Key rule:
	 *   - IOC is copied in and out as a guest GPA object;
	 *   - invalid ABI or operation records bad_hcall_count and returns
	 *     BAD_PARAM to the caller;
	 *   - channel lookup is always scoped to the caller VMID.
	 */
	if ((vcpu == NULL) || (ioc_gpa == 0UL) ||
		(copy_from_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc)) != 0)) {
		return -EFAULT;
	}

	if ((ioc.abi_version != ACRN_IPC_ABI_VERSION) ||
		(ioc.ioc_size < sizeof(ioc))) {
		channel = arm64_vipc_find_channel(vcpu->vm->vm_id, ioc.channel_id, NULL,
			&endpoint_index);
		arm64_vipc_record_bad(channel);
		ioc.status = ACRN_IPC_STATUS_BAD_PARAM;
		(void)copy_to_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc));
		return -EINVAL;
	}

	switch (ioc.op) {
	case ACRN_IPC_OP_QUERY:
		ret = arm64_vipc_handle_query(vcpu, &ioc, ioc_gpa);
		break;
	case ACRN_IPC_OP_NOTIFY:
		ret = arm64_vipc_handle_notify(vcpu, &ioc, ioc_gpa);
		break;
	case ACRN_IPC_OP_ACK:
		ret = arm64_vipc_handle_ack(vcpu, &ioc, ioc_gpa);
		break;
	default:
		channel = arm64_vipc_find_channel(vcpu->vm->vm_id, ioc.channel_id, NULL,
			&endpoint_index);
		arm64_vipc_record_bad(channel);
		ioc.status = ACRN_IPC_STATUS_BAD_PARAM;
		(void)copy_to_gpa(vcpu->vm, &ioc, ioc_gpa, sizeof(ioc));
		ret = -EINVAL;
		break;
	}

	return ret;
}

uint32_t arm64_vipc_get_stats(struct arm64_vipc_channel_stats *stats, uint32_t max_count)
{
	uint32_t idx;
	uint32_t count = 0U;

	if ((stats == NULL) || (max_count == 0U)) {
		return 0U;
	}

	spinlock_obtain(&arm64_vipc_lock);
	for (idx = 0U; (idx < ARRAY_SIZE(arm64_vipc_channels)) && (count < max_count); idx++) {
		struct arm64_vipc_channel *channel = &arm64_vipc_channels[idx];

		if (!channel->valid) {
			continue;
		}

		(void)memset(&stats[count], 0U, sizeof(stats[count]));
		stats[count].channel_id = channel->config.channel_id;
		stats[count].endpoint_vmid[0] = channel->config.endpoint_vmid[0];
		stats[count].endpoint_vmid[1] = channel->config.endpoint_vmid[1];
		stats[count].gpa_base = channel->config.gpa_base;
		stats[count].ring_size = channel->config.ring_size;
		stats[count].ring_count = ACRN_IPC_RING_COUNT;
		stats[count].mapped_mask = channel->mapped_mask;
		stats[count].notify_virq = channel->config.notify_virq;
		stats[count].notify_count[0] = channel->notify_count[0];
		stats[count].notify_count[1] = channel->notify_count[1];
		stats[count].ack_count[0] = channel->ack_count[0];
		stats[count].ack_count[1] = channel->ack_count[1];
		stats[count].wake_count[0] = channel->wake_count[0];
		stats[count].wake_count[1] = channel->wake_count[1];
		stats[count].irq_count[0] = channel->irq_count[0];
		stats[count].irq_count[1] = channel->irq_count[1];
		stats[count].irq_fail_count[0] = channel->irq_fail_count[0];
		stats[count].irq_fail_count[1] = channel->irq_fail_count[1];
		stats[count].bad_hcall_count = channel->bad_hcall_count;
		stats[count].last_notify_tick[0] = channel->last_notify_tick[0];
		stats[count].last_notify_tick[1] = channel->last_notify_tick[1];
		count++;
	}
	spinlock_release(&arm64_vipc_lock);

	return count;
}
