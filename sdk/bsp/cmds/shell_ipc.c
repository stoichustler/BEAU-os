/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <ticks.h>
#include <asm/guest/vipc.h>
#include <asm/guest/vrproc.h>

#include "shell_cmds.h"

static uint64_t shell_ipcstat_age_ms(uint64_t now, uint64_t tick)
{
	return tick == 0UL ? UINT64_MAX : ticks_to_ms(now - tick);
}

static void shell_ipcstat_print_dir(const struct arm64_vipc_channel_stats *stats,
	uint32_t dir, uint64_t now)
{
	uint16_t src = dir == ACRN_IPC_DIR_EP0_TO_EP1 ?
		stats->endpoint_vmid[0] : stats->endpoint_vmid[1];
	uint16_t dst = dir == ACRN_IPC_DIR_EP0_TO_EP1 ?
		stats->endpoint_vmid[1] : stats->endpoint_vmid[0];
	uint64_t age_ms = shell_ipcstat_age_ms(now, stats->last_notify_tick[dir]);

	if (age_ms == UINT64_MAX) {
		shell_item_line("dir:vm%hu->vm%hu notify:%lu ack:%lu wake:%lu irq:%lu/%lu last:-",
			src, dst, stats->notify_count[dir], stats->ack_count[dir],
			stats->wake_count[dir], stats->irq_count[dir],
			stats->irq_fail_count[dir]);
	} else {
		shell_item_line("dir:vm%hu->vm%hu notify:%lu ack:%lu wake:%lu irq:%lu/%lu last:%lums",
			src, dst, stats->notify_count[dir], stats->ack_count[dir],
			stats->wake_count[dir], stats->irq_count[dir],
			stats->irq_fail_count[dir], age_ms);
	}
}

/* [20260722] Unified IPC statistics report
 *
 * HVC IPC lock       -> local HVC snapshot
 * remoteproc lock    -> local RPROC snapshot
 *                                |
 *                                v
 *                         shell renders both sections
 *
 * Key rule:
 *   - each transport owns and snapshots its statistics under its own lock;
 *   - rendering must use only local snapshots after both copies complete;
 *   - no transport lock is held across console output or output checkpoints.
 *
 * Output fields:
 *   - ch/endpoints identify the transport and VM peers;
 *   - gpa/shared, ring/count, and mapped describe shared memory;
 *   - notify/ack/wake or kick count producer activity; irq is success/failure;
 *   - last is activity age, and bad records rejected traffic.
 */
int32_t shell_ipcstat(int32_t argc, __unused char **argv)
{
	struct arm64_vipc_channel_stats vipc_stats[ARM64_VIPC_MAX_STATIC_CHANNELS];
	struct arm64_vrproc_channel_stats vrproc_stats[ARM64_VRPROC_MAX_STATIC_CHANNELS];
	uint32_t vipc_count;
	uint32_t vrproc_count;
	uint32_t idx;
	uint64_t now;

	if (argc != 1) {
		return -EINVAL;
	}

	vipc_count = arm64_vipc_get_stats(vipc_stats, ARRAY_SIZE(vipc_stats));
	vrproc_count = arm64_vrproc_get_stats(vrproc_stats, ARRAY_SIZE(vrproc_stats));
	now = cpu_ticks();

	shell_item_begin("IPCSTAT");
	if ((vipc_count == 0U) && (vrproc_count == 0U)) {
		shell_item_line("channels:none");
		shell_item_end();
		return 0;
	}

	if (vipc_count != 0U) {
		shell_item_section("✔   HVC IPC channels:");
	}
	for (idx = 0U; idx < vipc_count; idx++) {
		shell_item_line("ch%u ep0:vm%hu ep1:vm%hu gpa:0x%016lx ring:%u count:%u mapped:0x%08x virq:%u bad:%lu",
			vipc_stats[idx].channel_id, vipc_stats[idx].endpoint_vmid[0],
			vipc_stats[idx].endpoint_vmid[1], vipc_stats[idx].gpa_base,
			vipc_stats[idx].ring_size, vipc_stats[idx].ring_count,
			vipc_stats[idx].mapped_mask, vipc_stats[idx].notify_virq,
			vipc_stats[idx].bad_hcall_count);
		shell_ipcstat_print_dir(&vipc_stats[idx], ACRN_IPC_DIR_EP0_TO_EP1, now);
		shell_ipcstat_print_dir(&vipc_stats[idx], ACRN_IPC_DIR_EP1_TO_EP0, now);
		shell_output_checkpoint();
	}

	if (vrproc_count != 0U) {
		shell_item_section("✔ RPROC IPC channels:");
	}
	for (idx = 0U; idx < vrproc_count; idx++) {
		uint64_t age0 = shell_ipcstat_age_ms(now, vrproc_stats[idx].last_kick_tick[0]);
		uint64_t age1 = shell_ipcstat_age_ms(now, vrproc_stats[idx].last_kick_tick[1]);

		shell_item_line("ch%u ep0:vm%hu ep1:vm%hu shared:0x%016lx size:0x%x mapped:0x%08x",
			vrproc_stats[idx].channel_id, vrproc_stats[idx].endpoint_vmid[0],
			vrproc_stats[idx].endpoint_vmid[1], vrproc_stats[idx].shared_gpa,
			vrproc_stats[idx].shared_size, vrproc_stats[idx].mapped_mask);
		if (age0 == UINT64_MAX) {
			shell_item_line("ep0 bell:0x%016lx virq:%u kick:%lu irq:%lu/%lu last:-",
				vrproc_stats[idx].doorbell_gpa[0], vrproc_stats[idx].notify_virq[0],
				vrproc_stats[idx].kick_count[0], vrproc_stats[idx].irq_count[0],
				vrproc_stats[idx].irq_fail_count[0]);
		} else {
			shell_item_line("ep0 bell:0x%016lx virq:%u kick:%lu irq:%lu/%lu last:%lums",
				vrproc_stats[idx].doorbell_gpa[0], vrproc_stats[idx].notify_virq[0],
				vrproc_stats[idx].kick_count[0], vrproc_stats[idx].irq_count[0],
				vrproc_stats[idx].irq_fail_count[0], age0);
		}
		if (age1 == UINT64_MAX) {
			shell_item_line("ep1 bell:0x%016lx virq:%u kick:%lu irq:%lu/%lu last:- bad:%lu vring:%u/%u",
				vrproc_stats[idx].doorbell_gpa[1], vrproc_stats[idx].notify_virq[1],
				vrproc_stats[idx].kick_count[1], vrproc_stats[idx].irq_count[1],
				vrproc_stats[idx].irq_fail_count[1], vrproc_stats[idx].bad_mmio_count,
				vrproc_stats[idx].vring_num, vrproc_stats[idx].vring_align);
		} else {
			shell_item_line("ep1 bell:0x%016lx virq:%u kick:%lu irq:%lu/%lu last:%lums bad:%lu vring:%u/%u",
				vrproc_stats[idx].doorbell_gpa[1], vrproc_stats[idx].notify_virq[1],
				vrproc_stats[idx].kick_count[1], vrproc_stats[idx].irq_count[1],
				vrproc_stats[idx].irq_fail_count[1], age1, vrproc_stats[idx].bad_mmio_count,
				vrproc_stats[idx].vring_num, vrproc_stats[idx].vring_align);
		}
		shell_output_checkpoint();
	}
	shell_item_end();

	return 0;
}
