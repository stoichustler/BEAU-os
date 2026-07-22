/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <sprintf.h>
#include <ticks.h>
#include <trace.h>
#include <util.h>
#ifdef CONFIG_PERF
#include <asm/perf.h>
#endif

#include "shell_cmds.h"

#define TRACE_DUMP_DEFAULT_COUNT	64U
#ifdef CONFIG_PERF
#define PERF_DUMP_DEFAULT_COUNT	32U
#endif

struct shell_trace_cursor {
	struct trace_record record;
	uint32_t index;
	bool valid;
};

static uint64_t shell_trace_category_mask(const char *category)
{
	uint64_t mask = 0UL;

	if (strcmp(category, "all") == 0) {
		mask = TRACE_MASK_ALL;
	} else if (strcmp(category, "timer") == 0) {
		mask = TRACE_MASK_TIMER;
	} else if (strcmp(category, "sched") == 0) {
		mask = TRACE_MASK_SCHED;
	} else if (strcmp(category, "hcall") == 0) {
		mask = TRACE_MASK_HCALL;
	} else if (strcmp(category, "vm") == 0) {
		mask = TRACE_MASK_VM;
	}

	return mask;
}

static void shell_trace_append_category(char *buf, size_t size, const char *category)
{
	size_t len = strnlen_s(buf, size);

	if (len != 0U) {
		(void)strncat_s(buf, size, ",", 1U);
	}
	(void)strncat_s(buf, size, category, strnlen_s(category, size));
}

static void shell_trace_format_mask(uint64_t mask, char *buf, size_t size)
{
	buf[0] = '\0';
	if (mask == TRACE_MASK_ALL) {
		(void)strncpy_s(buf, size, "all", size - 1U);
		return;
	}
	if ((mask & TRACE_MASK_TIMER) != 0UL) {
		shell_trace_append_category(buf, size, "timer");
	}
	if ((mask & TRACE_MASK_SCHED) != 0UL) {
		shell_trace_append_category(buf, size, "sched");
	}
	if ((mask & TRACE_MASK_HCALL) != 0UL) {
		shell_trace_append_category(buf, size, "hcall");
	}
	if ((mask & TRACE_MASK_VM) != 0UL) {
		shell_trace_append_category(buf, size, "vm");
	}
	if (buf[0] == '\0') {
		(void)strncpy_s(buf, size, "none", size - 1U);
	}
}

static const char *shell_trace_event_name(uint32_t event_id)
{
	const char *name;

	switch (event_id) {
	case TRACE_TIMER_ACTION_ADDED:
		name = "timer-add";
		break;
	case TRACE_TIMER_ACTION_PCKUP:
		name = "timer-fire";
		break;
	case TRACE_TIMER_ACTION_UPDAT:
		name = "timer-update";
		break;
	case TRACE_TIMER_IRQ:
		name = "timer-irq";
		break;
	case TRACE_SCHED_NEXT:
		name = "sched-switch";
		break;
	case TRACE_VMEXIT_VMCALL:
		name = "hcall";
		break;
	case TRACE_VM_ENTER:
		name = "vm-enter";
		break;
	case TRACE_VM_EXIT:
		name = "vm-exit";
		break;
	default:
		name = "unknown";
		break;
	}

	return name;
}

static void shell_trace_print_record(uint32_t sequence, uint64_t base_tsc,
	const struct trace_record *record)
{
	uint32_t event_id = (uint32_t)record->id;
	uint64_t delta_us = (record->tsc >= base_tsc) ?
		ticks_to_us(record->tsc - base_tsc) : 0UL;
	const char *name = shell_trace_event_name(event_id);

	/* Every row starts with ring sequence, delta from dump baseline in us, and
	 * source CPU. The remaining fields decode the event-specific trace payload.
	 */
	switch (event_id) {
	case TRACE_TIMER_ACTION_ADDED:
	case TRACE_TIMER_ACTION_PCKUP:
	case TRACE_TIMER_ACTION_UPDAT:
	case TRACE_TIMER_IRQ:
		shell_item_line("[%04u] +%8luus cpu:%u %-12s deadline:0x%016lx data:0x%016lx",
			sequence, delta_us, record->cpu, name,
			record->payload.fields_64.e, record->payload.fields_64.f);
		break;
	case TRACE_SCHED_NEXT:
		shell_item_line("[%04u] +%8luus cpu:%u %-12s %s", sequence, delta_us,
			record->cpu, name, record->payload.str);
		break;
	case TRACE_VMEXIT_VMCALL:
		shell_item_line("[%04u] +%8luus cpu:%u %-12s vm:%lu id:0x%016lx",
			sequence, delta_us, record->cpu, name,
			record->payload.fields_64.e, record->payload.fields_64.f);
		break;
	case TRACE_VM_ENTER:
	case TRACE_VM_EXIT:
		shell_item_line("[%04u] +%8luus cpu:%u %-12s vm:%u vcpu:%u src:0x%x status:%d",
			sequence, delta_us, record->cpu, name,
			record->payload.fields_32.a, record->payload.fields_32.b,
			record->payload.fields_32.c,
			(int32_t)record->payload.fields_32.d);
		break;
	default:
		shell_item_line("[%04u] +%8luus cpu:%u %-12s id:0x%lx data:0x%016lx/0x%016lx",
			sequence, delta_us, record->cpu, name, record->id,
			record->payload.fields_64.e, record->payload.fields_64.f);
		break;
	}
}

static void shell_trace_status(void)
{
	char mask[64U];
	uint16_t pcpu_id;

	shell_trace_format_mask(trace_get_mask(), mask, sizeof(mask));
	shell_item_begin("TRACE");
	/* mask lists enabled categories; capacity is records per pCPU; records and
	 * overwritten are ring counters; writer reports active producer ownership.
	 */
	shell_item_line("state:%s mask:%s capacity:%u/pCPU record-size:%uB",
		trace_is_running() ? "running" : "stopped", mask,
		trace_get_capacity(), (uint32_t)sizeof(struct trace_record));
	shell_item_line("pCPU  records  overwritten  writer");
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		struct trace_cpu_status status;

		trace_get_cpu_status(pcpu_id, &status);
		shell_item_line("%4hu  %7u  %11lu  %-6s", pcpu_id,
			status.count, status.overwritten,
			status.writer_active ? "active" : "idle");
		shell_output_checkpoint();
	}
	shell_item_end();
}

static int32_t shell_trace_start(int32_t argc, char **argv)
{
	uint64_t mask = TRACE_MASK_ALL;
	int32_t idx;

	if (argc > 2) {
		mask = 0UL;
		for (idx = 2; idx < argc; idx++) {
			uint64_t category = shell_trace_category_mask(argv[idx]);

			if (category == 0UL) {
				shell_puts("usage: trace start [all|timer|sched|hcall|vm]...\r\n");
				return -EINVAL;
			}
			mask |= category;
		}
	}

	return trace_start(mask);
}

static int32_t shell_trace_dump(int32_t argc, char **argv)
{
	struct shell_trace_cursor cursors[MAX_PCPU_NUM];
	uint32_t total = 0U;
	uint32_t requested = TRACE_DUMP_DEFAULT_COUNT;
	uint32_t skipped;
	uint32_t consumed = 0U;
	uint32_t printed = 0U;
	uint64_t base_tsc = 0UL;
	uint16_t pcpu_id;

	if (trace_is_running()) {
		shell_puts("trace dump requires stopped capture\r\n");
		return -EBUSY;
	}
	if ((argc < 2) || (argc > 3)) {
		return -EINVAL;
	}
	if (argc == 3) {
		int64_t value = strtol_deci(argv[2]);
		uint32_t max_records = trace_get_capacity() * get_pcpu_nums();

		if ((value <= 0) || ((uint64_t)value > max_records)) {
			shell_puts("usage: trace dump [count]\r\n");
			return -EINVAL;
		}
		requested = (uint32_t)value;
	}

	(void)memset(cursors, 0U, sizeof(cursors));
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		struct trace_cpu_status status;

		trace_get_cpu_status(pcpu_id, &status);
		cursors[pcpu_id].valid = trace_get_record(pcpu_id, 0U,
			&cursors[pcpu_id].record);
		total += status.count;
	}
	if (requested > total) {
		requested = total;
	}
	skipped = total - requested;

	shell_item_begin("TRACE DUMP");
	shell_item_line("records:%u shown:%u", total, requested);
	while (consumed < total) {
		uint16_t best = INVALID_CPU_ID;

		for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
			if (cursors[pcpu_id].valid &&
				((best == INVALID_CPU_ID) ||
				(cursors[pcpu_id].record.tsc < cursors[best].record.tsc))) {
				best = pcpu_id;
			}
		}
		if (best == INVALID_CPU_ID) {
			break;
		}

		if (consumed >= skipped) {
			if (printed == 0U) {
				base_tsc = cursors[best].record.tsc;
			}
			shell_trace_print_record(printed, base_tsc, &cursors[best].record);
			printed++;
		}
		consumed++;
		cursors[best].index++;
		cursors[best].valid = trace_get_record(best, cursors[best].index,
			&cursors[best].record);
		shell_output_checkpoint();
	}
	shell_item_end();

	return 0;
}

int32_t shell_trace(int32_t argc, char **argv)
{
	int32_t ret;
	char buffer[96U];

	if (argc < 2) {
		shell_puts("usage: trace <status|start|stop|clear|dump> [category|count]\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "status") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		shell_trace_status();
		ret = 0;
	} else if (strcmp(argv[1], "start") == 0) {
		ret = shell_trace_start(argc, argv);
	} else if (strcmp(argv[1], "stop") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		ret = trace_stop();
		if (ret == 0) {
			shell_trace_status();
		}
	} else if (strcmp(argv[1], "clear") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		ret = trace_clear();
		if (ret == 0) {
			shell_trace_status();
		}
	} else if (strcmp(argv[1], "dump") == 0) {
		ret = shell_trace_dump(argc, argv);
	} else {
		shell_puts("usage: trace <status|start|stop|clear|dump> [category|count]\r\n");
		return -EINVAL;
	}

	(void)snprintf(buffer, sizeof(buffer), "TRACE %6s: %s (%d)\r\n",
		argv[1], (ret == 0) ? "ok" : "failed", ret);
	shell_puts(buffer);

	return ret;
}

#ifdef CONFIG_PERF
struct shell_perf_cursor {
	struct arm64_perf_sample sample;
	uint32_t index;
	bool valid;
};

static struct shell_perf_cursor shell_perf_cursors[MAX_PCPU_NUM];

static void shell_perf_status(void)
{
	struct arm64_perf_status perf;
	uint16_t pcpu_id;

	arm64_perf_get_status(&perf);
	shell_item_begin("PERF");
	/* session/generation identify one capture; duration/frequency/period define
	 * sampling cadence. Per-pCPU rows separate retained records from capture
	 * attempts, no-stack/missed loss, overwrite, writer state, and pending data.
	 */
	shell_item_line("state:%s readable:%s controller:%hu session:%lu generation:%lu",
		perf.running ? "running" : "stopped", perf.readable ? "Y" : "N",
		perf.controller_pcpu, perf.epoch, perf.generation);
	shell_item_line("duration:%ums frequency:%uHz period:%uus capacity:%u/pCPU",
		perf.duration_ms, perf.frequency_hz, perf.period_us,
		ARM64_PERF_RECORDS_PER_CPU);
	shell_item_line("pCPU  records  captured  attempts  no-stack  missed  overwritten  writer  pending");
	for (pcpu_id = 0U; pcpu_id < perf.pcpu_num; pcpu_id++) {
		struct arm64_perf_cpu_status status;

		arm64_perf_get_cpu_status(pcpu_id, &status);
		shell_item_line("%4hu  %7u  %8lu  %8lu  %8lu  %6lu  %11lu  %-6s  %lu",
			pcpu_id, status.count, status.captured, status.attempts,
			status.no_stack, status.missed, status.overwritten,
			status.writer_active ? "active" : "idle",
			status.pending_generation);
		shell_output_checkpoint();
	}
	shell_item_end();
}

static void shell_perf_print_sample(uint32_t sequence, uint64_t base_tsc,
	const struct arm64_perf_sample *sample)
{
	uint64_t delta_us = (sample->tsc >= base_tsc) ?
		ticks_to_us(sample->tsc - base_tsc) : 0UL;
	uint16_t frame;

	/* Rows start with sequence/delta/source/session/generation. Owner selects
	 * guest VM/vCPU attribution or host thread frames; stop explains unwind end.
	 */
	if (sample->owner == ARM64_PERF_OWNER_GUEST) {
		if ((sample->vm_id == ARM64_PERF_INVALID_ID) ||
			(sample->vcpu_id == ARM64_PERF_INVALID_ID)) {
			shell_item_line("[%04u] +%8luus cpu:%hu session:%lu gen:%lu owner:guest unknown",
				sequence, delta_us, sample->pcpu_id, sample->epoch,
				sample->generation);
		} else {
			shell_item_line("[%04u] +%8luus cpu:%hu session:%lu gen:%lu owner:vm%hu:v%hu",
				sequence, delta_us, sample->pcpu_id, sample->epoch,
				sample->generation,
				sample->vm_id, sample->vcpu_id);
		}
		return;
	}

	shell_item_line("[%04u] +%8luus cpu:%hu session:%lu gen:%lu owner:host thread:%s frames:%hu stop:%s",
		sequence, delta_us, sample->pcpu_id, sample->epoch,
		sample->generation,
		(sample->thread_name[0] != '\0') ? sample->thread_name : "unknown",
		sample->frame_count,
		arm64_perf_unwind_stop_name(sample->unwind_stop));
	for (frame = 0U; frame < sample->frame_count; frame++) {
		char symbol[96U];

		dbg_format_symbol(sample->frames[frame], symbol, sizeof(symbol));
		shell_item_line("         #%02hu 0x%016lx %s", frame,
			sample->frames[frame], symbol);
	}
}

static int32_t shell_perf_dump(int32_t argc, char **argv)
{
	struct arm64_perf_status perf;
	struct shell_perf_cursor *cursors = shell_perf_cursors;
	uint32_t requested = PERF_DUMP_DEFAULT_COUNT;
	uint32_t total = 0U;
	uint32_t skipped;
	uint32_t consumed = 0U;
	uint32_t printed = 0U;
	uint64_t base_tsc = 0UL;
	uint16_t pcpu_id;

	arm64_perf_get_status(&perf);
	if (!perf.readable) {
		shell_puts("perf dump requires stopped, quiescent capture\r\n");
		return -EBUSY;
	}
	if ((argc < 2) || (argc > 3)) {
		return -EINVAL;
	}
	if (argc == 3) {
		int64_t value = strtol_deci(argv[2]);
		uint32_t max_records = ARM64_PERF_RECORDS_PER_CPU * perf.pcpu_num;

		if ((value <= 0) || ((uint64_t)value > max_records)) {
			shell_puts("usage: perf dump [count]\r\n");
			return -EINVAL;
		}
		requested = (uint32_t)value;
	}

	(void)memset(cursors, 0U,
		MAX_PCPU_NUM * sizeof(struct shell_perf_cursor));
	for (pcpu_id = 0U; pcpu_id < perf.pcpu_num; pcpu_id++) {
		struct arm64_perf_cpu_status status;

		arm64_perf_get_cpu_status(pcpu_id, &status);
		cursors[pcpu_id].valid = arm64_perf_get_sample(pcpu_id, 0U,
			&cursors[pcpu_id].sample);
		total += status.count;
	}
	if (requested > total) {
		requested = total;
	}
	skipped = total - requested;

	shell_item_begin("PERF DUMP");
	shell_item_line("samples:%u shown:%u", total, requested);
	while (consumed < total) {
		uint16_t best = INVALID_CPU_ID;

		for (pcpu_id = 0U; pcpu_id < perf.pcpu_num; pcpu_id++) {
			if (cursors[pcpu_id].valid &&
				((best == INVALID_CPU_ID) ||
				 (cursors[pcpu_id].sample.tsc < cursors[best].sample.tsc))) {
				best = pcpu_id;
			}
		}
		if (best == INVALID_CPU_ID) {
			break;
		}
		if (consumed >= skipped) {
			if (printed == 0U) {
				base_tsc = cursors[best].sample.tsc;
			}
			shell_perf_print_sample(printed, base_tsc, &cursors[best].sample);
			printed++;
		}
		consumed++;
		cursors[best].index++;
		cursors[best].valid = arm64_perf_get_sample(best,
			cursors[best].index, &cursors[best].sample);
		shell_output_checkpoint();
	}
	shell_item_end();
	return 0;
}

int32_t shell_perf(int32_t argc, char **argv)
{
	int32_t ret;

	if (argc < 2) {
		shell_puts("usage: perf <record|status|stop|clear|dump> [duration-ms frequency-hz|count]\r\n");
		return -EINVAL;
	}
	if (strcmp(argv[1], "record") == 0) {
		int64_t duration;
		int64_t frequency;

		if (argc != 4) {
			return -EINVAL;
		}
		duration = strtol_deci(argv[2]);
		frequency = strtol_deci(argv[3]);
		if ((duration <= 0) || ((uint64_t)duration > UINT32_MAX) ||
			(frequency <= 0) || ((uint64_t)frequency > UINT32_MAX)) {
			return -EINVAL;
		}
		ret = arm64_perf_record((uint32_t)duration, (uint32_t)frequency);
		if (ret == 0) {
			shell_perf_status();
		}
	} else if (strcmp(argv[1], "status") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		shell_perf_status();
		ret = 0;
	} else if (strcmp(argv[1], "stop") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		ret = arm64_perf_stop();
		if (ret == 0) {
			shell_perf_status();
		}
	} else if (strcmp(argv[1], "clear") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		ret = arm64_perf_clear();
		if (ret == 0) {
			shell_perf_status();
		}
	} else if (strcmp(argv[1], "dump") == 0) {
		ret = shell_perf_dump(argc, argv);
	} else {
		shell_puts("usage: perf <record|status|stop|clear|dump> [duration-ms frequency-hz|count]\r\n");
		ret = -EINVAL;
	}
	return ret;
}
#endif

/* [20260709] SMMU monitor:
 *
 * smmustat is deliberately diagnostic-only. Probe puts the hardware into an
 * abort-default state; PCI passthrough assignment then replaces a selected STE
 * with a VM stage-2 descriptor and synchronizes the command queue.
 *
 *   zero STEs + SMMUEN -> abort-default ready
 *                               |
 *                               v
 *                    stream assignment -> VM stage-2 STE
 *                               |
 *                               v
 *                    shell snapshot: SMMU + ITS + streams + vSMMU instances
 *
 * Stream output principle:
 *
 *   sw-owner : VM recorded in the software stream ownership table
 *   ste-vm   : VMID decoded from the current STE word2
 *   cfg      : hardware action for DMA from this StreamID
 *              abort  - block DMA
 *              bypass - no translation; unsafe for assigned guest devices
 *              s2     - translate through the VM stage-2 root
 *
 * A healthy assigned passthrough stream should show sw-owner == ste-vm and
 * cfg == s2. A healthy unassigned stream should show sw-owner none and cfg
 * abort. Anything else is a useful signal for passthrough DMA debugging.
 */
