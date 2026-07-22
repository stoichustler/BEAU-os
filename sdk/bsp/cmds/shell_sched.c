/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <per_cpu.h>
#include <schedule.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>
#include <vconfig.h>
#include <vcpu.h>

#include "shell_cmds.h"

#define SHELL_THREAD_SAMPLE_MAX \
	((CONFIG_MAX_VM_NUM * MAX_VCPUS_PER_VM) + MAX_PCPU_NUM + 8U)
#define SHELL_CPU_PERCENT_SCALE	1000UL

static const char *thread_state_str(enum thread_object_state state);
static const char *thread_lifecycle_str(const struct thread_object *thread);
struct shell_schedstat_thread_sample {
	const struct thread_object *thread;
	uint64_t max_wait_ticks;
	uint64_t wait_hist[SCHED_LATENCY_HIST_BUCKETS];
};

struct shell_schedstat_snapshot {
	bool valid;
	bool overflow;
	uint64_t sample_ticks;
	uint64_t idle_runtime_ticks[MAX_PCPU_NUM];
	bool idle_seen[MAX_PCPU_NUM];
	uint32_t thread_count;
	struct shell_schedstat_thread_sample thread[SHELL_THREAD_SAMPLE_MAX];
};

struct shell_ps_thread_sample {
	const struct thread_object *thread;
	uint64_t runtime_ticks;
};

struct shell_ps_snapshot {
	bool valid;
	bool overflow;
	uint64_t sample_ticks;
	uint32_t thread_count;
	struct shell_ps_thread_sample thread[SHELL_THREAD_SAMPLE_MAX];
};

static struct shell_schedstat_snapshot shell_schedstat_last;
static struct shell_schedstat_snapshot shell_schedstat_sample;
static struct shell_ps_snapshot shell_ps_last;
static struct shell_ps_snapshot shell_ps_sample;
static bool pcpu_is_shared_by_vcpus(uint16_t pcpu_id)
{
	struct acrn_vm *vm;
	struct acrn_vcpu *vcpu;
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint16_t count = 0U;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm = get_vm_from_vmid(vm_id);
		if (is_poweroff_vm(vm)) {
			continue;
		}

		foreach_vcpu(vcpu_id, vm, vcpu) {
			if (pcpuid_from_vcpu(vcpu) == pcpu_id) {
				count++;
				if (count > 1U) {
					return true;
				}
			}
		}
	}

	return false;
}

/* [20260630] vcpus monitor:
 *
 * This command reports vCPU scheduler ownership, not guest CPU topology.
 * It answers which pCPU owns each vCPU thread, whether that pCPU is shared by
 * multiple vCPUs, and how long the vCPU has waited in its current state.
 *
 *   VM/vCPU -> scheduler thread -> pCPU binding -> latency snapshot
 */
int32_t shell_list_vcpu(__unused int32_t argc, __unused char **argv)
{
	struct acrn_vm *vm;
	struct acrn_vcpu *vcpu;
	uint16_t i;
	uint16_t idx;

	shell_item_begin("vcpus");
	/* lifecycle is VM/vCPU lifetime, thread is scheduler state; switches and
	 * wait values are scheduler counters/durations in us, and since is current
	 * runnable-wait age or '-'.
	 */
	shell_item_line("vcpu       pcpu  pcpu_mode  lifecycle  thread    switches  lastwait.us  maxwait.us  since.us");
	shell_item_line("─────────  ────  ─────────  ─────────  ────────  ────────  ───────────  ──────────  ────────");

	for (idx = 0U; idx < CONFIG_MAX_VM_NUM; idx++) {
		vm = get_vm_from_vmid(idx);
		if (is_poweroff_vm(vm)) {
			continue;
		}
		foreach_vcpu(i, vm, vcpu) {
			struct sched_latency_stats stats = { 0U };
			char since_us[24U];
			uint64_t since_ticks;
			uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);
			bool shared_pcpu = pcpu_is_shared_by_vcpus(pcpu_id);

			sched_get_latency(&vcpu->thread_obj, &stats);
			if (shared_pcpu) {
				since_ticks = (stats.state_since != 0UL) ? (cpu_ticks() - stats.state_since) : 0UL;
				snprintf(since_us, sizeof(since_us), "%lu", ticks_to_us(since_ticks));
			} else {
				snprintf(since_us, sizeof(since_us), "-");
			}
			shell_item_line("%-9s  %-4hu  %-9s  %-9s  %-8s  %-8lu  %-11lu  %-10lu  %-8s",
				vcpu->thread_obj.name,
				pcpu_id,
				shared_pcpu ? "shared" : "exclusive",
				vcpu_state_to_str(vcpu_get_state(vcpu)),
				thread_state_str(vcpu->thread_obj.status),
				stats.switches,
				ticks_to_us(stats.last_wait_ticks),
				ticks_to_us(stats.max_wait_ticks),
				since_us);
			shell_output_checkpoint();
		}
	}
	shell_item_end();

	return 0;
}

static const char *thread_state_str(enum thread_object_state state)
{
	const char *str;

	switch (state) {
	case THREAD_STS_RUNNING:
		str = "running";
		break;
	case THREAD_STS_RUNNABLE:
		str = "runnable";
		break;
	case THREAD_STS_BLOCKED:
		str = "blocked";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *thread_lifecycle_str(const struct thread_object *thread)
{
	const char *state = "-";

	if ((thread != NULL) && thread->is_vcpu &&
		(thread->vm_id < CONFIG_MAX_VM_NUM)) {
		struct acrn_vm *vm = get_vm_from_vmid(thread->vm_id);

		if ((vm != NULL) && (thread->vcpu_id < vm->hw.created_vcpus)) {
			struct acrn_vcpu *vcpu = vcpu_from_vid(vm, thread->vcpu_id);

			state = vcpu_state_to_str(vcpu_get_state(vcpu));
		}
	}

	return state;
}

static uint64_t shell_counter_delta(uint64_t current, uint64_t previous)
{
	return (current >= previous) ? (current - previous) : 0UL;
}

static void shell_format_cpu_percent(char *buf, size_t size,
	uint64_t used_ticks, uint64_t window_ticks)
{
	uint64_t permille;

	if ((used_ticks >= window_ticks) ||
		(used_ticks > (UINT64_MAX / SHELL_CPU_PERCENT_SCALE))) {
		permille = SHELL_CPU_PERCENT_SCALE;
	} else {
		permille = (used_ticks * SHELL_CPU_PERCENT_SCALE) / window_ticks;
	}

	(void)snprintf(buf, size, "%lu.%01lu", permille / 10UL,
		permille % 10UL);
}

static const struct shell_ps_thread_sample *shell_ps_find_thread_sample(
	const struct shell_ps_snapshot *snapshot, const struct thread_object *thread)
{
	uint32_t idx;

	for (idx = 0U; idx < snapshot->thread_count; idx++) {
		if (snapshot->thread[idx].thread == thread) {
			return &snapshot->thread[idx];
		}
	}

	return NULL;
}

static void shell_ps_take_snapshot(struct shell_ps_snapshot *snapshot)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;

	(void)memset(snapshot, 0U, sizeof(*snapshot));
	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		struct sched_latency_stats stats = { 0U };

		sched_get_latency(thread, &stats);
		if (snapshot->thread_count < SHELL_THREAD_SAMPLE_MAX) {
			snapshot->thread[snapshot->thread_count].thread = thread;
			snapshot->thread[snapshot->thread_count].runtime_ticks =
				stats.runtime_ticks;
			snapshot->thread_count++;
		} else {
			snapshot->overflow = true;
		}
	}
	snapshot->sample_ticks = cpu_ticks();
	snapshot->valid = true;
}

/* [20260719] threads and on-demand CPU usage monitor
 *
 * previous ps runtime[N]
 *           |
 *           v
 * current scheduler runtime[N] -> bounded delta/window -> ps CPU columns
 *           |
 *           +--> missing/rollback/overflow -> "--"
 *
 * Key rule:
 *   - the shell thread owns ps history independently from schedstat;
 *   - newly observed or invalid samples never masquerade as zero utilization;
 *   - sampling reads existing scheduler accounting without changing its hot path.
 */
int32_t shell_list_threads(__unused int32_t argc, __unused char **argv)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;
	uint64_t window_ticks;
	bool has_window;

	shell_ps_take_snapshot(&shell_ps_sample);
	has_window = shell_ps_last.valid &&
		(shell_ps_sample.sample_ticks > shell_ps_last.sample_ticks);
	window_ticks = has_window ?
		shell_ps_sample.sample_ticks - shell_ps_last.sample_ticks : 0UL;
	if (has_window) {
		shell_item_begin("ps threads:%u window:%lums", sched_get_thread_count(),
			ticks_to_ms(window_ticks));
	} else {
		shell_item_begin("ps threads:%u window:baseline", sched_get_thread_count());
	}
	/* cpu%% and run.us are deltas over the printed window; baseline, new threads,
	 * counter rollback, and incomplete samples display '--'.
	 */
	shell_item_line("name             pcpu  lifecycle  thread    current  cpu%%   run.us");
	shell_item_line("───────────────  ────  ─────────  ────────  ───────  ─────  ─────────");

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		const struct shell_ps_thread_sample *current_sample =
			shell_ps_find_thread_sample(&shell_ps_sample, thread);
		const struct shell_ps_thread_sample *previous_sample =
			shell_ps_find_thread_sample(&shell_ps_last, thread);
		struct thread_object *current = sched_get_current(thread->pcpu_id);
		char percent[16U];
		char runtime_us[24U];

		if (has_window && (current_sample != NULL) &&
			(previous_sample != NULL) &&
			(current_sample->runtime_ticks >= previous_sample->runtime_ticks)) {
			uint64_t run_delta = current_sample->runtime_ticks -
				previous_sample->runtime_ticks;

			shell_format_cpu_percent(percent, sizeof(percent), run_delta,
				window_ticks);
			(void)snprintf(runtime_us, sizeof(runtime_us), "%lu",
				ticks_to_us(run_delta));
		} else {
			(void)snprintf(percent, sizeof(percent), "--");
			(void)snprintf(runtime_us, sizeof(runtime_us), "--");
		}

		shell_item_line("%-15s  %-4hu  %-9s  %-8s  %-7s  %-5s  %-9s",
			thread->name,
			thread->pcpu_id,
			thread_lifecycle_str(thread),
			thread_state_str(thread->status),
			(current == thread) ? "Y" : "N",
			percent,
			runtime_us);
		shell_output_checkpoint();
	}
	if (shell_ps_sample.overflow || shell_ps_last.overflow) {
		shell_item_line("warning: ps thread sample overflow; cpu%% may omit some threads.");
	}
	shell_ps_last = shell_ps_sample;
	shell_item_end();

	return 0;
}

static uint32_t shell_sched_runqueue_count(uint16_t pcpu_id)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;
	uint32_t count = 0U;

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);

		if ((thread->pcpu_id == pcpu_id) && (thread->status == THREAD_STS_RUNNABLE)) {
			count++;
		}
	}

	return count;
}

static const struct shell_schedstat_thread_sample *shell_schedstat_find_thread_sample(
	const struct shell_schedstat_snapshot *snapshot, const struct thread_object *thread)
{
	uint32_t idx;

	for (idx = 0U; idx < snapshot->thread_count; idx++) {
		if (snapshot->thread[idx].thread == thread) {
			return &snapshot->thread[idx];
		}
	}

	return NULL;
}

/* [20260719] schedstat monitor
 *
 * previous schedstat idle runtime / wait histogram
 *                       |
 *                       v
 * current scheduler snapshot -> pCPU busy + CBS latency deltas
 *
 * Key rule:
 *   - schedstat owns policy and pCPU diagnostics, while ps owns per-thread CPU
 *     usage history;
 *   - the two command histories remain independent;
 *   - snapshot overflow remains visible instead of silently dropping evidence.
 */
static void shell_schedstat_take_snapshot(struct shell_schedstat_snapshot *snapshot)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;

	(void)memset(snapshot, 0U, sizeof(*snapshot));

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		struct sched_latency_stats stats = { 0U };
		uint16_t pcpu_id = thread->pcpu_id;

		sched_get_latency(thread, &stats);
		if (snapshot->thread_count < SHELL_THREAD_SAMPLE_MAX) {
			snapshot->thread[snapshot->thread_count].thread = thread;
			snapshot->thread[snapshot->thread_count].max_wait_ticks = stats.max_wait_ticks;
			memcpy(snapshot->thread[snapshot->thread_count].wait_hist, stats.wait_hist,
				sizeof(stats.wait_hist));
			snapshot->thread_count++;
		} else {
			snapshot->overflow = true;
		}

		if ((pcpu_id < MAX_PCPU_NUM) && is_idle_thread(thread)) {
			snapshot->idle_runtime_ticks[pcpu_id] = stats.runtime_ticks;
			snapshot->idle_seen[pcpu_id] = true;
		}
	}

	snapshot->sample_ticks = cpu_ticks();
	snapshot->valid = true;
}

static void shell_schedstat_format_pcpu_busy(char *buf, size_t size, uint16_t pcpu_id,
	uint64_t window_ticks)
{
	uint64_t idle_delta;
	uint64_t idle_permille;
	uint64_t busy_permille;

	if (!shell_schedstat_last.valid ||
		!shell_schedstat_sample.idle_seen[pcpu_id] ||
		!shell_schedstat_last.idle_seen[pcpu_id] ||
		(window_ticks == 0UL)) {
		snprintf(buf, size, "0.0");
		return;
	}

	idle_delta = shell_counter_delta(shell_schedstat_sample.idle_runtime_ticks[pcpu_id],
		shell_schedstat_last.idle_runtime_ticks[pcpu_id]);
	if ((idle_delta >= window_ticks) || (idle_delta > (UINT64_MAX / SHELL_CPU_PERCENT_SCALE))) {
		idle_permille = SHELL_CPU_PERCENT_SCALE;
	} else {
		idle_permille = (idle_delta * SHELL_CPU_PERCENT_SCALE) / window_ticks;
		if (idle_permille > SHELL_CPU_PERCENT_SCALE) {
			idle_permille = SHELL_CPU_PERCENT_SCALE;
		}
	}

	busy_permille = SHELL_CPU_PERCENT_SCALE - idle_permille;
	snprintf(buf, size, "%lu.%01lu", busy_permille / 10UL, busy_permille % 10UL);
}

static const char *shell_schedstat_pcpu_role(uint16_t pcpu_id)
{
	const struct sched_platform_config *config = sched_get_platform_config();
	uint64_t pcpu_mask = 1UL << pcpu_id;

	if (config->configured) {
		if ((config->exclusive.pcpu_mask & pcpu_mask) != 0UL) {
			return "exclusive";
		}
		if ((config->shared.pcpu_mask & pcpu_mask) != 0UL) {
			return "shared";
		}
	}

	return pcpu_is_shared_by_vcpus(pcpu_id) ? "shared" : "exclusive";
}

static void shell_schedstat_print_cbs_latency_hist(const struct list_head *head)
{
	struct list_head *pos;
	bool printed_header = false;
	bool has_previous = shell_schedstat_last.valid;

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		const struct shell_schedstat_thread_sample *current;
		const struct shell_schedstat_thread_sample *previous;
		struct sched_cbs_stats cbs;
		uint64_t hist[SCHED_LATENCY_HIST_BUCKETS];
		uint32_t bucket;

		if (!sched_get_cbs_stats(thread, &cbs)) {
			continue;
		}
		current = shell_schedstat_find_thread_sample(&shell_schedstat_sample, thread);
		if (current == NULL) {
			continue;
		}
		previous = shell_schedstat_find_thread_sample(&shell_schedstat_last, thread);
		for (bucket = 0U; bucket < SCHED_LATENCY_HIST_BUCKETS; bucket++) {
			hist[bucket] = (has_previous && (previous != NULL)) ?
				shell_counter_delta(current->wait_hist[bucket],
					previous->wait_hist[bucket]) :
				current->wait_hist[bucket];
		}

		if (!printed_header) {
			shell_item_section("CBS latency histogram %s (runnable -> running):",
				has_previous ? "delta" : "cumulative");
			shell_item_line("name             pcpu  %-7s  %-7s  %-7s  %-7s  %-7s  %-7s  %-7s  %-7s  max.us (TTL)",
				sched_latency_hist_bucket_name(0U),
				sched_latency_hist_bucket_name(1U),
				sched_latency_hist_bucket_name(2U),
				sched_latency_hist_bucket_name(3U),
				sched_latency_hist_bucket_name(4U),
				sched_latency_hist_bucket_name(5U),
				sched_latency_hist_bucket_name(6U),
				sched_latency_hist_bucket_name(7U));
			shell_item_line("───────────────  ────  ───────  ───────  ───────  ───────  ───────  ───────  ───────  ───────  ──────");
			printed_header = true;
		}

		shell_item_line("%-15s  %-4hu  %-7lu  %-7lu  %-7lu  %-7lu  %-7lu  %-7lu  %-7lu  %-7lu  %-6lu",
			thread->name,
			thread->pcpu_id,
			hist[0U],
			hist[1U],
			hist[2U],
			hist[3U],
			hist[4U],
			hist[5U],
			hist[6U],
			hist[7U],
			ticks_to_us(current->max_wait_ticks));
		shell_output_checkpoint();
	}
}

int32_t shell_schedstat(__unused int32_t argc, __unused char **argv)
{
	const struct list_head *head = sched_get_thread_list();
	struct list_head *pos;
	uint16_t pcpu_id;
	uint16_t pcpu_num = get_pcpu_nums();
	bool has_bvt_stats = false;
	bool has_rtds_stats = false;
	bool has_cbs_stats = false;
	bool printed_cbs_pcpu_header = false;
	uint64_t window_ticks;

	shell_schedstat_take_snapshot(&shell_schedstat_sample);
	window_ticks = shell_schedstat_last.valid ?
		shell_counter_delta(shell_schedstat_sample.sample_ticks,
			shell_schedstat_last.sample_ticks) : 0UL;

	shell_item_begin("schedstat pcpus:%hu", pcpu_num);

	/*
	 * Per-pCPU counters answer whether the scheduler is ticking, whether
	 * context switches are happening, and which thread currently owns a CPU.
	 */
	shell_item_section("Per-pCPU hybrid scheduler counters:");
	shell_item_line("pcpu  role       scheduler    busy%%  timer   switches  resched  runqueue  current");
	shell_item_line("────  ─────────  ───────────  ─────  ──────  ────────  ───────  ────────  ─────────────────");

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		struct thread_object *current = sched_get_current(pcpu_id);
		const char *name = (current != NULL) ? current->name : "-";
		char busy[16U];

		shell_schedstat_format_pcpu_busy(busy, sizeof(busy), pcpu_id, window_ticks);

		shell_item_line("%-5hu %-10s %-12s %-6s %-7lu %-9lu %-8lu %-9u %s",
			pcpu_id,
			shell_schedstat_pcpu_role(pcpu_id),
			sched_get_scheduler_name(pcpu_id),
			busy,
			sched_get_ticks(pcpu_id),
			sched_get_context_switches(pcpu_id),
			sched_get_reschedule_requests(pcpu_id),
			shell_sched_runqueue_count(pcpu_id),
			name);
		shell_output_checkpoint();
	}

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		struct sched_cbs_pcpu_stats cbs_pcpu;

		if (sched_get_cbs_pcpu_stats(pcpu_id, &cbs_pcpu)) {
			if (!printed_cbs_pcpu_header) {
				shell_item_section("CBS pCPU stats:");
				shell_item_line("pcpu  admission.ppm  runqueue");
				shell_item_line("────  ─────────────  ────────");
				printed_cbs_pcpu_header = true;
			}
			shell_item_line("%-5hu %-14lu %-8u",
				pcpu_id, cbs_pcpu.admission_utilization,
				cbs_pcpu.runqueue_count);
			shell_output_checkpoint();
		}
	}

	list_for_each(pos, head) {
		struct thread_object *thread = container_of(pos, struct thread_object, node);
		struct sched_bvt_stats bvt;
		struct sched_rtds_stats rtds;
		struct sched_cbs_stats cbs;

		if (sched_get_bvt_stats(thread, &bvt)) {
			has_bvt_stats = true;
		}
		if (sched_get_rtds_stats(thread, &rtds)) {
			has_rtds_stats = true;
		}
		if (sched_get_cbs_stats(thread, &cbs)) {
			has_cbs_stats = true;
		}
		if (has_bvt_stats && has_rtds_stats && has_cbs_stats) {
			break;
		}
	}

	if (has_bvt_stats) {
		/*
		 * BVT stats expose virtual-time ordering. Lower avt/evt is more
		 * eligible; weight controls how quickly virtual time advances.
		 */
		shell_item_section("BVT stats:");
		shell_item_line("name             pcpu  state     weight  avt       evt");
		shell_item_line("───────────────  ────  ────────  ──────  ────────  ────────");

		list_for_each(pos, head) {
			struct thread_object *thread = container_of(pos, struct thread_object, node);
			struct sched_bvt_stats bvt;

			if (sched_get_bvt_stats(thread, &bvt)) {
				shell_item_line("%-15s  %-4hu  %-8s  %-6u  %-8ld  %-8ld",
					thread->name,
					thread->pcpu_id,
					thread_state_str(thread->status),
					(uint32_t)bvt.weight,
					bvt.avt,
					bvt.evt);
				shell_output_checkpoint();
			}
		}
	}

	if (has_rtds_stats) {
		uint64_t now = cpu_ticks();

		/*
		 * RTDS stats show fixed-period budget accounting and the time
		 * left before the next scheduling deadline.
		 */
		shell_item_section("RTDS stats:");
		shell_item_line("name             pcpu  state     period.us  budget.us  remain.us  deadline-in.us");
		shell_item_line("───────────────  ────  ────────  ─────────  ─────────  ─────────  ──────────────");

		list_for_each(pos, head) {
			struct thread_object *thread = container_of(pos, struct thread_object, node);
			struct sched_rtds_stats rtds;

			if (sched_get_rtds_stats(thread, &rtds)) {
				shell_item_line("%-15s  %-4hu  %-8s  %-9lu  %-9lu  %-9lu  %-11lu",
					thread->name,
					thread->pcpu_id,
					thread_state_str(thread->status),
					ticks_to_us(rtds.period_ticks),
					ticks_to_us(rtds.budget_ticks),
					ticks_to_us(rtds.remaining_ticks),
					(rtds.deadline_ticks > now) ?
						ticks_to_us(rtds.deadline_ticks - now) : 0UL);
				shell_output_checkpoint();
			}
		}
	}

	if (has_cbs_stats) {
		uint64_t now = cpu_ticks();

		/*
		 * CBS stats show the active reservation server state. Deadline moves
		 * forward when budget is replenished after depletion or wake admission.
		 */
		shell_item_section("CBS stats:");
		shell_item_line("name             pcpu  state     period.us  budget.us  remain.us  deadline-in.us  dep       repl      wake      late");
		shell_item_line("───────────────  ────  ────────  ─────────  ─────────  ─────────  ──────────────  ────────  ────────  ────────  ────────");

		list_for_each(pos, head) {
			struct thread_object *thread = container_of(pos, struct thread_object, node);
			struct sched_cbs_stats cbs;

			if (sched_get_cbs_stats(thread, &cbs)) {
				shell_item_line("%-15s  %-4hu  %-8s  %-9lu  %-9lu  %-9lu  %-14lu  %-8lu  %-8lu  %-8lu  %-8lu",
					thread->name,
					thread->pcpu_id,
					thread_state_str(thread->status),
					ticks_to_us(cbs.period_ticks),
					ticks_to_us(cbs.budget_ticks),
					ticks_to_us(cbs.remaining_ticks),
					(cbs.deadline_ticks > now) ?
						ticks_to_us(cbs.deadline_ticks - now) : 0UL,
					cbs.depleted_count,
					cbs.replenish_count,
					cbs.wake_replenish_count,
					cbs.late_account_count);
				shell_output_checkpoint();
			}
		}
		shell_schedstat_print_cbs_latency_hist(head);
	}

	shell_schedstat_last = shell_schedstat_sample;
	shell_item_end();

	return 0;
}

int32_t shell_schedai(int32_t argc, char **argv)
{
	uint16_t pcpu_id;
	bool found = false;

	if ((argc == 2) && (strcmp(argv[1], "snapshot") == 0)) {
		char line[MAX_STR_SIZE];

		for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
			const struct sched_cpupool_config *pool = sched_get_pcpu_pool_config(pcpu_id);
			struct sched_cbs_pcpu_stats cbs;
			uint64_t active_mask = 0UL;
			uint16_t vmid;

			if ((pool == NULL) || !pool->ai_assist || !sched_get_cbs_pcpu_stats(pcpu_id, &cbs)) {
				continue;
			}
			for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
				if ((get_vm_config(vmid)->cpu_affinity & AFFINITY_CPU(pcpu_id)) != 0UL) {
					active_mask |= 1UL << vmid;
				}
			}
			snprintf(line, sizeof(line), "AI_SCHED ticks=%lu pcpu=%hu period_us=%u admission_ppm=%lu runqueue=%u active_mask=0x%lx pool_budget_us=%u cs=%lu resched=%lu\r\n",
				cpu_ticks(), pcpu_id, pool->period_us, cbs.admission_utilization,
				cbs.runqueue_count, active_mask, pool->budget_us, sched_get_context_switches(pcpu_id),
				sched_get_reschedule_requests(pcpu_id));
			shell_puts(line);
			found = true;
		}
		return found ? 0 : -ENOTSUP;
	}
	return -EINVAL;
}
