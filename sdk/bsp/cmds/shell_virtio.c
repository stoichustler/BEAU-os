/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <sprintf.h>
#include <util.h>
#include <vconfig.h>
#include <virtio_proxy.h>

#include "shell_cmds.h"

static const char *shell_virtio_access_to_str(uint32_t access)
{
	return access == VIRTIO_PROXY_ACCESS_READONLY ? "ro" : "rw";
}

static const char *shell_virtio_throughput_to_str(uint32_t throughput)
{
	return throughput == VIRTIO_PROXY_THROUGHPUT_HIGH ? "high" : "low";
}

static const char *shell_virtio_state_to_str(uint32_t state)
{
	const char *str;

	switch (state) {
	case VIRTIO_PROXY_STATE_WAIT_BACKEND:
		str = "wait-BE";
		break;
	case VIRTIO_PROXY_STATE_FRONTEND_READY:
		str = "FE-ready";
		break;
	case VIRTIO_PROXY_STATE_BACKEND_READY:
		str = "BE-ready";
		break;
	case VIRTIO_PROXY_STATE_RUNNING:
		str = "run";
		break;
	case VIRTIO_PROXY_STATE_BACKEND_LOST:
		str = "BE-lost";
		break;
	case VIRTIO_PROXY_STATE_BACKEND_STALE:
		str = "BE-stale";
		break;
	default:
		str = "N/A";
		break;
	}

	return str;
}

static const char *shell_virtio_device_to_str(uint32_t device_id)
{
	const char *str;

	switch (device_id) {
	case VIRTIO_DEVICE_ID_NET:
		str = "virtio-net";
		break;
	case VIRTIO_DEVICE_ID_BLOCK:
		str = "virtio-blk";
		break;
	case VIRTIO_DEVICE_ID_FS:
		str = "virtio-fs";
		break;
	case VIRTIO_DEVICE_ID_RNG:
		str = "virtio-rng";
		break;
	case VIRTIO_DEVICE_ID_I2C:
		str = "virtio-i2c";
		break;
	default:
		str = "virtio-dev";
		break;
	}

	return str;
}

static const char *shell_errno_to_str(int32_t ret)
{
	const char *str;

	switch (ret) {
	case 0:
		str = "OK";
		break;
	case -EBUSY:
		str = "BUSY";
		break;
	case -ENODEV:
		str = "NODEV";
		break;
	case -EINVAL:
		str = "INVAL";
		break;
	case -EFAULT:
		str = "FAULT";
		break;
	case -ENODATA:
		str = "NODATA";
		break;
	default:
		str = "N/A";
		break;
	}

	return str;
}

static void shell_virtio_format_ms(char *buf, size_t size, uint64_t us)
{
	if ((buf != NULL) && (size != 0U)) {
		snprintf(buf, size, "%04lu.%04lums", us / 1000UL,
			((us % 1000UL) * 10UL));
	}
}

static void shell_virtio_latency_range_ms(char *buf, size_t size,
	const struct virtio_proxy_latency_stats *stats)
{
	char min[24];
	char avg[24];
	char max[24];

	if ((buf == NULL) || (size == 0U)) {
		return;
	}

	if ((stats == NULL) || (stats->count == 0UL)) {
		(void)strncpy_s(buf, size, "-/-/-ms", size - 1U);
	} else {
		shell_virtio_format_ms(min, sizeof(min), stats->min_us);
		shell_virtio_format_ms(avg, sizeof(avg), stats->avg_us);
		shell_virtio_format_ms(max, sizeof(max), stats->max_us);
		snprintf(buf, size, "%s/%s/%s", min, avg, max);
	}
}

static bool shell_virtio_stats_active(const struct virtio_proxy_stats *stats)
{
	return (stats != NULL) && ((stats->status != 0U) ||
		(stats->notify_count != 0UL) || stats->hcall_backend_registered ||
		(stats->hcall_register_count != 0UL));
}

static bool shell_virtio_is_grouped_device(uint32_t device_id)
{
	return (device_id == VIRTIO_DEVICE_ID_NET) ||
		(device_id == VIRTIO_DEVICE_ID_FS) ||
		(device_id == VIRTIO_DEVICE_ID_RNG) ||
		(device_id == VIRTIO_DEVICE_ID_BLOCK) ||
		(device_id == VIRTIO_DEVICE_ID_I2C);
}

static void shell_virtiostat_print_summary_device(const struct virtio_proxy_stats *stats)
{
	uint16_t ready = 0U;
	char backend[8];
	char notify_poll[64];
	char poll_reply[64];
	char reply_irq[64];
	char total[64];

	for (uint16_t queue_id = 0U; queue_id < stats->queue_num; queue_id++) {
		if (stats->queues[queue_id].ready) {
			ready++;
		}
	}
	if (stats->hcall_backend_registered) {
		snprintf(backend, sizeof(backend), "vm%hu", stats->backend_vmid);
	} else {
		(void)strncpy_s(backend, sizeof(backend), "-", sizeof(backend) - 1U);
	}
	shell_virtio_latency_range_ms(notify_poll, sizeof(notify_poll),
		&stats->latency_notify_poll);
	shell_virtio_latency_range_ms(poll_reply, sizeof(poll_reply),
		&stats->latency_poll_reply);
	shell_virtio_latency_range_ms(reply_irq, sizeof(reply_irq),
		&stats->latency_reply_irq);
	shell_virtio_latency_range_ms(total, sizeof(total), &stats->latency_total);

	shell_item_begin("%s vm%hu:%hu", shell_virtio_device_to_str(stats->device_id),
		stats->vm_id, stats->index);
	/* queues/pending are ready/total and outstanding/limit; kick counters classify
	 * notification work; throughput is bytes per second; latency is min/avg/max
	 * reply-to-interrupt time in ms, with '-' when no samples exist.
	 */
	shell_item_line("device:%3u tag:%12s access:%s throughput:%s",
		stats->device_id, stats->tag, shell_virtio_access_to_str(stats->access),
		shell_virtio_throughput_to_str(stats->throughput));
	shell_item_line("state:%s status:0x%08x queues:%hu/%hu notify:%lu backend:%s health:%s pending:%hu/%hu",
		shell_virtio_state_to_str(stats->state), stats->status, ready,
		stats->queue_num, stats->notify_count, backend,
		stats->backend_healthy ? "ok" : "stale", stats->pending_active,
		stats->pending_limit);
	shell_item_line("kick:notify:%lu merge:%lu prefetch:%lu backend:%lu bp:%lu irq:%lu saved:%lu",
		stats->notify_count, stats->notify_coalesced_count,
		stats->notify_prefetch_count, stats->notify_backend_kick_count,
		stats->notify_backpressure_count, stats->irq_count,
		stats->batch_irq_saved_count);
	shell_item_line("throughput:req:%luB reply:%luB avg:%luB/s done:%lu",
		stats->request_bytes, stats->reply_bytes, stats->byte_rate,
		stats->completed_count);
	shell_item_line("hcall:register:%lu poll:%lu/%lu empty:%lu reply:%lu/%lu busy:%lu bp:%lu ret:%6s(%d)",
		stats->hcall_register_count, stats->hcall_poll_ok_count,
		stats->hcall_poll_count, stats->hcall_empty_poll_count,
		stats->hcall_reply_ok_count,
		stats->hcall_reply_count, stats->hcall_busy_count,
		stats->hcall_backpressure_count,
		shell_errno_to_str(stats->last_hcall_ret), stats->last_hcall_ret);
	shell_item_line("batch:poll:%lu/%lu items:%lu reply:%lu/%lu items:%lu last:%u",
		stats->hcall_batch_poll_ok_count, stats->hcall_batch_poll_count,
		stats->hcall_batch_poll_item_count,
		stats->hcall_batch_reply_ok_count, stats->hcall_batch_reply_count,
		stats->hcall_batch_reply_item_count, stats->last_batch_count);
	shell_item_line("backend:abi:%u caps:0x%x heartbeat:%lu age:%lums wait:%uus",
		stats->backend_abi_version, stats->backend_caps,
		stats->hcall_heartbeat_count, stats->heartbeat_age_ms,
		stats->last_wait_us);
	shell_item_line("latency:min/avg/max");
	shell_item_line("  notify-poll:%s", notify_poll);
	shell_item_line("  poll-reply: %s", poll_reply);
	shell_item_line("  reply-irq:  %s", reply_irq);
	shell_item_line("  total:      %s", total);
	shell_item_line("fault:timeout:%lu reset:%lu samples:%lu",
		stats->timeout_count, stats->reset_count, stats->latency_total.count);
	shell_item_line("last:poll:q:%hu reply:q:%hu len:%u",
		stats->last_poll_queue_id, stats->last_reply_queue_id, stats->last_reply_len);
	shell_item_end();
	shell_output_checkpoint();
}

static bool shell_virtiostat_print_summary_for_device(uint32_t device_id)
{
	struct virtio_proxy_stats stats;
	bool printed = false;

	for (uint16_t vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		uint16_t count = virtio_proxy_device_count(vm_id);

		for (uint16_t index = 0U; index < count; index++) {
			if (!virtio_proxy_get_stats(vm_id, index, &stats) ||
				(stats.device_id != device_id) ||
				!shell_virtio_stats_active(&stats)) {
				continue;
			}
			shell_virtiostat_print_summary_device(&stats);
			printed = true;
		}
	}

	return printed;
}

static void shell_virtiostat_print_summary_others(void)
{
	struct virtio_proxy_stats stats;

	for (uint16_t vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		uint16_t count = virtio_proxy_device_count(vm_id);

		for (uint16_t index = 0U; index < count; index++) {
			if (!virtio_proxy_get_stats(vm_id, index, &stats) ||
				shell_virtio_is_grouped_device(stats.device_id) ||
				!shell_virtio_stats_active(&stats)) {
				continue;
			}
			shell_virtiostat_print_summary_device(&stats);
		}
	}
}

static void shell_virtiostat_print_summary(void)
{
	(void)shell_virtiostat_print_summary_for_device(VIRTIO_DEVICE_ID_NET);
	(void)shell_virtiostat_print_summary_for_device(VIRTIO_DEVICE_ID_FS);
	(void)shell_virtiostat_print_summary_for_device(VIRTIO_DEVICE_ID_RNG);
	(void)shell_virtiostat_print_summary_for_device(VIRTIO_DEVICE_ID_BLOCK);
	(void)shell_virtiostat_print_summary_for_device(VIRTIO_DEVICE_ID_I2C);
	shell_virtiostat_print_summary_others();
}

int32_t shell_virtiostat(int32_t argc, __unused char **argv)
{
	if (argc != 1) {
		shell_puts("usage: virtiostat\r\n");
		return -EINVAL;
	}

	shell_virtiostat_print_summary();
	return 0;
}
