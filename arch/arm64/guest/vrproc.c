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
#include <vconfig.h>
#include <ticks.h>
#include <bsp/io_req.h>
#include <acrn_common.h>
#include <asm/page.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vrproc.h>

/* [20260722] Static remoteproc/rpmsg transport
 *
 * platform.dts -> validated static channel -> endpoint-only stage-2 mapping
 *                                               |
 * guest kick(vqid) -> trapped doorbell -> validate -> peer vIRQ
 *
 * Key rule:
 *   - EL2 owns channel topology, shared-page exposure, and doorbell routing;
 *   - RPMsg peers own resource-table and vring contents after EL2 publishes
 *     the complete mapping;
 *   - invalid topology or MMIO never exposes memory or notifies a peer.
 */
struct arm64_vrproc_channel {
	bool valid;
	struct arm64_vrproc_channel_config config;
	uint32_t mapped_mask;
	uint64_t kick_count[ARM64_VRPROC_VRING_COUNT];
	uint64_t irq_count[ARM64_VRPROC_VRING_COUNT];
	uint64_t irq_fail_count[ARM64_VRPROC_VRING_COUNT];
	uint64_t bad_mmio_count;
	uint64_t last_kick_tick[ARM64_VRPROC_VRING_COUNT];
};

static struct arm64_vrproc_channel arm64_vrproc_channels[ARM64_VRPROC_MAX_STATIC_CHANNELS];
static uint8_t arm64_vrproc_shared[ARM64_VRPROC_MAX_STATIC_CHANNELS]
	[ARM64_VRPROC_SHARED_SIZE_MAX] __aligned(PAGE_SIZE);
static spinlock_t arm64_vrproc_lock = { .head = 0U, .tail = 0U };

static bool arm64_vrproc_pow2(uint32_t value)
{
	return (value != 0U) && ((value & (value - 1U)) == 0U);
}

static bool arm64_vrproc_range_end(uint64_t start, uint64_t size, uint64_t *end)
{
	if ((size == 0UL) || (start > (UINT64_MAX - size))) {
		return false;
	}
	*end = start + size;
	return true;
}

static bool arm64_vrproc_ranges_overlap(uint64_t start_a, uint64_t size_a,
	uint64_t start_b, uint64_t size_b)
{
	uint64_t end_a;
	uint64_t end_b;

	return arm64_vrproc_range_end(start_a, size_a, &end_a) &&
		arm64_vrproc_range_end(start_b, size_b, &end_b) &&
		(start_a < end_b) && (start_b < end_a);
}

static bool arm64_vrproc_endpoint_index(const struct arm64_vrproc_channel *channel,
	uint16_t vmid, uint32_t *index)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VRPROC_VRING_COUNT; idx++) {
		if (channel->config.endpoint_vmid[idx] == vmid) {
			if (index != NULL) {
				*index = idx;
			}
			return true;
		}
	}

	return false;
}

static bool arm64_vrproc_channel_conflicts(const struct arm64_vrproc_channel_config *config)
{
	uint32_t idx;
	uint32_t endpoint;

	for (idx = 0U; idx < ARRAY_SIZE(arm64_vrproc_channels); idx++) {
		const struct arm64_vrproc_channel *channel = &arm64_vrproc_channels[idx];

		if (!channel->valid) {
			continue;
		}
		if (channel->config.channel_id == config->channel_id ||
			arm64_vrproc_ranges_overlap(channel->config.shared_gpa,
			channel->config.shared_size, config->shared_gpa, config->shared_size) ||
			arm64_vrproc_ranges_overlap(channel->config.shared_gpa,
				channel->config.shared_size, config->doorbell_gpa[0],
				ARM64_VRPROC_DOORBELL_SIZE) ||
			arm64_vrproc_ranges_overlap(channel->config.shared_gpa,
				channel->config.shared_size, config->doorbell_gpa[1],
				ARM64_VRPROC_DOORBELL_SIZE)) {
			return true;
		}
		for (endpoint = 0U; endpoint < ARM64_VRPROC_VRING_COUNT; endpoint++) {
			if (arm64_vrproc_ranges_overlap(channel->config.doorbell_gpa[endpoint],
				ARM64_VRPROC_DOORBELL_SIZE, config->shared_gpa,
				config->shared_size) ||
				channel->config.doorbell_gpa[endpoint] == config->doorbell_gpa[0] ||
				channel->config.doorbell_gpa[endpoint] == config->doorbell_gpa[1]) {
				return true;
			}
		}
	}

	return false;
}

static int32_t arm64_vrproc_validate_config(const struct arm64_vrproc_channel_config *config)
{
	uint32_t endpoint;
	uint64_t shared_end;

	if ((config->endpoint_vmid[0] >= CONFIG_MAX_VM_NUM) ||
		(config->endpoint_vmid[1] >= CONFIG_MAX_VM_NUM) ||
		(config->endpoint_vmid[0] == config->endpoint_vmid[1]) ||
		(config->shared_gpa == 0UL) ||
		!mem_aligned_check(config->shared_gpa, PAGE_SIZE) ||
		(config->shared_size < ARM64_VRPROC_SHARED_SIZE_MIN) ||
		(config->shared_size > ARM64_VRPROC_SHARED_SIZE_MAX) ||
		!mem_aligned_check(config->shared_size, PAGE_SIZE) ||
		!arm64_vrproc_range_end(config->shared_gpa, config->shared_size, &shared_end) ||
		(config->vring_num == 0U) || (config->vring_num > ARM64_VRPROC_VRING_NUM_MAX) ||
		!arm64_vrproc_pow2(config->vring_num) ||
		!arm64_vrproc_pow2(config->vring_align) || (config->vring_align < PAGE_SIZE)) {
		return -EINVAL;
	}

	for (endpoint = 0U; endpoint < ARM64_VRPROC_VRING_COUNT; endpoint++) {
		if ((config->doorbell_gpa[endpoint] == 0UL) ||
			!mem_aligned_check(config->doorbell_gpa[endpoint], ARM64_VRPROC_DOORBELL_SIZE) ||
			(config->notify_virq[endpoint] < ARM64_VRPROC_VIRQ_MIN) ||
			(config->notify_virq[endpoint] > ARM64_VRPROC_VIRQ_MAX) ||
			arm64_vrproc_ranges_overlap(config->shared_gpa, config->shared_size,
				config->doorbell_gpa[endpoint], ARM64_VRPROC_DOORBELL_SIZE)) {
			return -EINVAL;
		}
	}
	if ((config->doorbell_gpa[0] == config->doorbell_gpa[1]) ||
		arm64_vrproc_channel_conflicts(config)) {
		return -EBUSY;
	}

	return 0;
}

int32_t arm64_vrproc_register_channel(const struct arm64_vrproc_channel_config *config)
{
	uint32_t idx;
	int32_t ret;

	if (config == NULL) {
		return -EINVAL;
	}

	spinlock_obtain(&arm64_vrproc_lock);
	ret = arm64_vrproc_validate_config(config);
	if (ret == 0) {
		for (idx = 0U; idx < ARRAY_SIZE(arm64_vrproc_channels); idx++) {
			if (!arm64_vrproc_channels[idx].valid) {
				(void)memset(&arm64_vrproc_channels[idx], 0U,
					sizeof(arm64_vrproc_channels[idx]));
				(void)memset(&arm64_vrproc_shared[idx][0], 0U,
					ARM64_VRPROC_SHARED_SIZE_MAX);
				arm64_vrproc_channels[idx].config = *config;
				arm64_vrproc_channels[idx].valid = true;
				break;
			}
		}
		if (idx == ARRAY_SIZE(arm64_vrproc_channels)) {
			ret = -ENOMEM;
		}
	}
	spinlock_release(&arm64_vrproc_lock);

	return ret;
}

void arm64_vrproc_init_vm(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config;
	uint32_t idx;

	if (vm == NULL) {
		return;
	}
	arch_config = &get_vm_config(vm->vm_id)->arch;

	for (idx = 0U; idx < ARRAY_SIZE(arm64_vrproc_channels); idx++) {
		struct arm64_vrproc_channel *channel = &arm64_vrproc_channels[idx];

		if (!channel->valid || !arm64_vrproc_endpoint_index(channel, vm->vm_id, NULL)) {
			continue;
		}
		if (arm64_vrproc_ranges_overlap(channel->config.shared_gpa,
			channel->config.shared_size, arch_config->guest_ram_start,
			arch_config->guest_ram_size) ||
			arm64_vrproc_ranges_overlap(channel->config.doorbell_gpa[0],
				ARM64_VRPROC_DOORBELL_SIZE, arch_config->guest_ram_start,
				arch_config->guest_ram_size) ||
			arm64_vrproc_ranges_overlap(channel->config.doorbell_gpa[1],
				ARM64_VRPROC_DOORBELL_SIZE, arch_config->guest_ram_start,
				arch_config->guest_ram_size)) {
			panic("rproc channel %u overlaps vm%u RAM", channel->config.channel_id,
				vm->vm_id);
		}

		arm64_stage2_map_normal(vm, hva2hpa(&arm64_vrproc_shared[idx][0]),
			channel->config.shared_gpa, channel->config.shared_size, true);
		spinlock_obtain(&arm64_vrproc_lock);
		if (vm->vm_id < 32U) {
			channel->mapped_mask |= (1U << vm->vm_id);
		}
		spinlock_release(&arm64_vrproc_lock);
	}
}

void arm64_vrproc_register_mmio(struct acrn_vm *vm)
{
	uint32_t idx;
	uint32_t endpoint;

	if (vm == NULL) {
		return;
	}

	for (idx = 0U; idx < ARRAY_SIZE(arm64_vrproc_channels); idx++) {
		struct arm64_vrproc_channel *channel = &arm64_vrproc_channels[idx];

		if (!channel->valid || !arm64_vrproc_endpoint_index(channel, vm->vm_id, &endpoint)) {
			continue;
		}
		register_mmio_emulation_handler(vm, arm64_vrproc_mmio_handler,
			channel->config.doorbell_gpa[endpoint],
			channel->config.doorbell_gpa[endpoint] + ARM64_VRPROC_DOORBELL_SIZE,
			channel, false);
	}
}

int32_t arm64_vrproc_mmio_handler(struct io_request *io_req, void *handler_private_data)
{
	struct arm64_vrproc_channel *channel = (struct arm64_vrproc_channel *)handler_private_data;
	struct acrn_mmio_request *mmio;
	struct acrn_vm *peer_vm;
	struct acrn_vcpu *peer_vcpu;
	uint32_t endpoint;
	uint32_t peer;
	uint32_t vqid;
	int32_t ret;

	if ((io_req == NULL) || (channel == NULL) || !channel->valid) {
		return -EINVAL;
	}
	mmio = &io_req->reqs.mmio_request;
	endpoint = mmio->address == channel->config.doorbell_gpa[0] ? 0U : 1U;
	if ((mmio->address != channel->config.doorbell_gpa[endpoint]) ||
		(mmio->size != sizeof(uint32_t))) {
		spinlock_obtain(&arm64_vrproc_lock);
		channel->bad_mmio_count++;
		spinlock_release(&arm64_vrproc_lock);
		return -EINVAL;
	}
	if (mmio->direction == ACRN_IOREQ_DIR_READ) {
		mmio->value = 0UL;
		return 0;
	}
	if (mmio->direction != ACRN_IOREQ_DIR_WRITE) {
		spinlock_obtain(&arm64_vrproc_lock);
		channel->bad_mmio_count++;
		spinlock_release(&arm64_vrproc_lock);
		return -EINVAL;
	}
	vqid = (uint32_t)mmio->value;
	if (vqid >= ARM64_VRPROC_VRING_COUNT) {
		spinlock_obtain(&arm64_vrproc_lock);
		channel->bad_mmio_count++;
		spinlock_release(&arm64_vrproc_lock);
		return -EINVAL;
	}

	peer = endpoint == 0U ? 1U : 0U;
	peer_vm = get_vm_from_vmid(channel->config.endpoint_vmid[peer]);
	ret = -ENODEV;
	if ((peer_vm != NULL) && !is_poweroff_vm(peer_vm) &&
		(peer_vm->hw.created_vcpus != 0U)) {
		peer_vcpu = vcpu_from_vid(peer_vm, 0U);
		ret = arm64_vgicv3_inject_irq(peer_vcpu, channel->config.notify_virq[peer], false);
	}

	spinlock_obtain(&arm64_vrproc_lock);
	channel->kick_count[endpoint]++;
	channel->last_kick_tick[endpoint] = cpu_ticks();
	if (ret == 0) {
		channel->irq_count[endpoint]++;
	} else {
		channel->irq_fail_count[endpoint]++;
	}
	spinlock_release(&arm64_vrproc_lock);

	return ret;
}

uint32_t arm64_vrproc_get_stats(struct arm64_vrproc_channel_stats *stats, uint32_t max_count)
{
	uint32_t idx;
	uint32_t count = 0U;

	if ((stats == NULL) || (max_count == 0U)) {
		return 0U;
	}

	spinlock_obtain(&arm64_vrproc_lock);
	for (idx = 0U; (idx < ARRAY_SIZE(arm64_vrproc_channels)) && (count < max_count); idx++) {
		const struct arm64_vrproc_channel *channel = &arm64_vrproc_channels[idx];
		struct arm64_vrproc_channel_stats *out;

		if (!channel->valid) {
			continue;
		}
		out = &stats[count++];
		(void)memset(out, 0U, sizeof(*out));
		out->channel_id = channel->config.channel_id;
		out->endpoint_vmid[0] = channel->config.endpoint_vmid[0];
		out->endpoint_vmid[1] = channel->config.endpoint_vmid[1];
		out->shared_gpa = channel->config.shared_gpa;
		out->shared_size = channel->config.shared_size;
		out->doorbell_gpa[0] = channel->config.doorbell_gpa[0];
		out->doorbell_gpa[1] = channel->config.doorbell_gpa[1];
		out->notify_virq[0] = channel->config.notify_virq[0];
		out->notify_virq[1] = channel->config.notify_virq[1];
		out->vring_num = channel->config.vring_num;
		out->vring_align = channel->config.vring_align;
		out->mapped_mask = channel->mapped_mask;
		out->kick_count[0] = channel->kick_count[0];
		out->kick_count[1] = channel->kick_count[1];
		out->irq_count[0] = channel->irq_count[0];
		out->irq_count[1] = channel->irq_count[1];
		out->irq_fail_count[0] = channel->irq_fail_count[0];
		out->irq_fail_count[1] = channel->irq_fail_count[1];
		out->bad_mmio_count = channel->bad_mmio_count;
		out->last_kick_tick[0] = channel->last_kick_tick[0];
		out->last_kick_tick[1] = channel->last_kick_tick[1];
	}
	spinlock_release(&arm64_vrproc_lock);

	return count;
}
