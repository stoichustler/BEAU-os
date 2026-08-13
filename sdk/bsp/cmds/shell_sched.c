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

enum shell_schedstat_algorithm {
	SHELL_SCHEDSTAT_ALGORITHM_NONE,
	SHELL_SCHEDSTAT_ALGORITHM_BVT,
	SHELL_SCHEDSTAT_ALGORITHM_CBS,
	SHELL_SCHEDSTAT_ALGORITHM_RTDS,
};

struct shell_schedstat_thread_sample {
	const struct thread_object *thread;
	enum shell_schedstat_algorithm algorithm;
	struct sched_bvt_stats bvt;
	struct sched_cbs_stats cbs;
	struct sched_rtds_stats rtds;
};

struct shell_schedstat_snapshot {
	bool overflow;
	uint32_t bvt_count;
	uint32_t cbs_count;
	uint32_t rtds_count;
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

static struct shell_schedstat_snapshot shell_schedstat_sample;
static struct shell_ps_snapshot shell_ps_last;
static struct shell_ps_snapshot shell_ps_sample;

static uint32_t shell_snapshot_threads(struct thread_object **threads, bool *overflow)
{
	return sched_snapshot_threads(threads, SHELL_THREAD_SAMPLE_MAX, overflow);
}
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
	struct thread_object *threads[SHELL_THREAD_SAMPLE_MAX];
	uint32_t idx;
	uint32_t count;

	(void)memset(snapshot, 0U, sizeof(*snapshot));
	count = shell_snapshot_threads(threads, &snapshot->overflow);
	for (idx = 0U; idx < count; idx++) {
		struct thread_object *thread = threads[idx];
		struct sched_latency_stats stats = { 0U };

		sched_get_latency(thread, &stats);
		snapshot->thread[idx].thread = thread;
		snapshot->thread[idx].runtime_ticks = stats.runtime_ticks;
		snapshot->thread_count++;
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
	uint32_t idx;
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

	for (idx = 0U; idx < shell_ps_sample.thread_count; idx++) {
		const struct thread_object *thread = shell_ps_sample.thread[idx].thread;
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
	struct thread_object *threads[SHELL_THREAD_SAMPLE_MAX];
	uint32_t idx;
	uint32_t thread_count;
	uint32_t count = 0U;

	thread_count = shell_snapshot_threads(threads, NULL);
	for (idx = 0U; idx < thread_count; idx++) {
		struct thread_object *thread = threads[idx];

		if ((thread->pcpu_id == pcpu_id) && (thread->status == THREAD_STS_RUNNABLE)) {
			count++;
		}
	}

	return count;
}

/* [20260813] schedstat algorithm snapshot
 *
 * pCPU scheduler lock -> algorithm snapshot -> shell-owned report -> BVT/CBS/RTDS tables
 *
 * Key rule:
 *   - scheduler state remains pCPU-owned and every algorithm helper snapshots
 *     it under that pCPU's scheduler lock;
 *   - the shell owns the copied report and never holds a scheduler lock while
 *     formatting output;
 *   - snapshot overflow remains visible instead of silently dropping evidence.
 */
static void shell_schedstat_take_snapshot(struct shell_schedstat_snapshot *snapshot)
{
	struct thread_object *threads[SHELL_THREAD_SAMPLE_MAX];
	uint32_t idx;
	uint32_t count;

	(void)memset(snapshot, 0U, sizeof(*snapshot));
	count = shell_snapshot_threads(threads, &snapshot->overflow);
	for (idx = 0U; idx < count; idx++) {
		struct thread_object *thread = threads[idx];
		struct shell_schedstat_thread_sample *sample =
			&snapshot->thread[snapshot->thread_count];

		sample->thread = thread;
		if (sched_get_bvt_stats(thread, &sample->bvt)) {
			sample->algorithm = SHELL_SCHEDSTAT_ALGORITHM_BVT;
			snapshot->bvt_count++;
		} else if (sched_get_cbs_stats(thread, &sample->cbs)) {
			sample->algorithm = SHELL_SCHEDSTAT_ALGORITHM_CBS;
			snapshot->cbs_count++;
		} else if (sched_get_rtds_stats(thread, &sample->rtds)) {
			sample->algorithm = SHELL_SCHEDSTAT_ALGORITHM_RTDS;
			snapshot->rtds_count++;
		}
		snapshot->thread_count++;
	}
}

static void shell_schedstat_print_bvt(const struct shell_schedstat_snapshot *snapshot)
{
	uint32_t idx;

	if (snapshot->bvt_count == 0U) {
		return;
	}

	shell_item_section("BVT threads:");
	shell_item_line("thread           pcpu  weight  vt.ratio   avt        evt        warp.value  warp.left  cooldown.left.us");
	shell_item_line("───────────────  ────  ──────  ─────────  ─────────  ─────────  ──────────  ─────────  ────────────────");
	for (idx = 0U; idx < snapshot->thread_count; idx++) {
		const struct shell_schedstat_thread_sample *sample = &snapshot->thread[idx];

		if (sample->algorithm != SHELL_SCHEDSTAT_ALGORITHM_BVT) {
			continue;
		}
		shell_item_line("%-15s  %-4hu  %-6u  %-9lu  %-9ld  %-9ld  %-10ld  %-9u  %-16lu", sample->thread->name,
			sample->thread->pcpu_id, (uint32_t)sample->bvt.weight,
			sample->bvt.vt_ratio, sample->bvt.avt, sample->bvt.evt,
			(int64_t)sample->bvt.warp_value, sample->bvt.warp_left,
			ticks_to_us(sample->bvt.cooldown_left_ticks));
		shell_output_checkpoint();
	}
}

static void shell_schedstat_format_utilization(char *buf, size_t size,
	uint64_t budget_ticks, uint64_t period_ticks)
{
	if (period_ticks == 0UL) {
		(void)snprintf(buf, size, "--");
		return;
	}

	shell_format_cpu_percent(buf, size, budget_ticks, period_ticks);
}

static void shell_schedstat_print_cbs(const struct shell_schedstat_snapshot *snapshot)
{
	uint32_t idx;

	if (snapshot->cbs_count == 0U) {
		return;
	}

	shell_item_section("CBS threads:");
	shell_item_line("thread           pcpu  util%%   period.us  budget.us  remain.us  deadline.us  depleted  replenish  late");
	shell_item_line("───────────────  ────  ─────   ─────────  ─────────  ─────────  ───────────  ────────  ─────────  ────────");
	for (idx = 0U; idx < snapshot->thread_count; idx++) {
		const struct shell_schedstat_thread_sample *sample = &snapshot->thread[idx];
		uint64_t now;
		uint64_t deadline;
		char utilization[16U];

		if (sample->algorithm != SHELL_SCHEDSTAT_ALGORITHM_CBS) {
			continue;
		}
		now = cpu_ticks();
		deadline = (sample->cbs.deadline_ticks > now) ?
			ticks_to_us(sample->cbs.deadline_ticks - now) : 0UL;
		shell_schedstat_format_utilization(utilization, sizeof(utilization),
			sample->cbs.budget_ticks, sample->cbs.period_ticks);
		shell_item_line("%-15s  %-4hu  %-6s  %-9lu  %-9lu  %-9lu  %-11lu  %-8lu  %-9lu  %-8lu",
			sample->thread->name, sample->thread->pcpu_id,
			utilization,
			ticks_to_us(sample->cbs.period_ticks), ticks_to_us(sample->cbs.budget_ticks),
			ticks_to_us(sample->cbs.remaining_ticks), deadline, sample->cbs.depleted_count,
			sample->cbs.replenish_count, sample->cbs.late_account_count);
		shell_output_checkpoint();
	}
}

static void shell_schedstat_print_rtds(const struct shell_schedstat_snapshot *snapshot)
{
	uint32_t idx;

	if (snapshot->rtds_count == 0U) {
		return;
	}

	shell_item_section("RTDS threads:");
	shell_item_line("thread           pcpu  util%%   period.us  budget.us  remain.us  deadline.us");
	shell_item_line("───────────────  ────  ──────   ─────────  ─────────  ─────────  ───────────");
	for (idx = 0U; idx < snapshot->thread_count; idx++) {
		const struct shell_schedstat_thread_sample *sample = &snapshot->thread[idx];
		uint64_t now;
		uint64_t deadline;
		char utilization[16U];

		if (sample->algorithm != SHELL_SCHEDSTAT_ALGORITHM_RTDS) {
			continue;
		}
		now = cpu_ticks();
		deadline = (sample->rtds.deadline_ticks > now) ?
			ticks_to_us(sample->rtds.deadline_ticks - now) : 0UL;
		shell_schedstat_format_utilization(utilization, sizeof(utilization),
			sample->rtds.budget_ticks, sample->rtds.period_ticks);
		shell_item_line("%-15s  %-4hu  %-6s  %-9lu  %-9lu  %-9lu  %-11lu",
			sample->thread->name, sample->thread->pcpu_id,
			utilization,
			ticks_to_us(sample->rtds.period_ticks), ticks_to_us(sample->rtds.budget_ticks),
			ticks_to_us(sample->rtds.remaining_ticks), deadline);
		shell_output_checkpoint();
	}
}

int32_t shell_schedstat(__unused int32_t argc, __unused char **argv)
{
	uint16_t pcpu_id;
	uint16_t pcpu_num = get_pcpu_nums();
	const char *policy_names[MAX_PCPU_NUM];
	uint16_t policy_pcpus[MAX_PCPU_NUM];
	uint16_t policy_count = 0U;

	shell_schedstat_take_snapshot(&shell_schedstat_sample);
	shell_item_begin("schedstat pcpus:%hu", pcpu_num);
	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		const char *name = sched_get_scheduler_name(pcpu_id);
		uint16_t policy_idx;

		for (policy_idx = 0U; policy_idx < policy_count; policy_idx++) {
			if (strcmp(policy_names[policy_idx], name) == 0) {
				break;
			}
		}
		if (policy_idx == policy_count) {
			policy_names[policy_count] = name;
			policy_pcpus[policy_count] = pcpu_id;
			policy_count++;
		}
	}
	for (pcpu_id = 0U; pcpu_id < policy_count; pcpu_id++) {
		shell_item_line("scheduler policy: %s=%s", policy_names[pcpu_id],
			sched_get_scheduler_stat_desc(policy_pcpus[pcpu_id]));
	}
	shell_item_section("pCPU schedulers:");
    shell_item_line("pcpu  scheduler    ticks      ctx-swi    resched    runqueue  current");
	shell_item_line("────  ───────────  ─────────  ─────────  ─────────  ────────  ───────────────");
	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		struct thread_object *current = sched_get_current(pcpu_id);

		shell_item_line("%-5hu %-12s %-10lu %-10lu %-10lu %-9u %s", pcpu_id,
			sched_get_scheduler_name(pcpu_id), sched_get_ticks(pcpu_id),
			sched_get_context_switches(pcpu_id), sched_get_reschedule_requests(pcpu_id),
			shell_sched_runqueue_count(pcpu_id), (current != NULL) ? current->name : "-");
		shell_output_checkpoint();
	}

	shell_item_section("scheduler summary: BVT:%u CBS:%u RTDS:%u",
		shell_schedstat_sample.bvt_count, shell_schedstat_sample.cbs_count,
		shell_schedstat_sample.rtds_count);
	shell_schedstat_print_bvt(&shell_schedstat_sample);
	shell_schedstat_print_cbs(&shell_schedstat_sample);
	shell_schedstat_print_rtds(&shell_schedstat_sample);
	if (shell_schedstat_sample.overflow) {
		shell_item_line("warning: schedstat thread sample overflow; algorithm rows may be incomplete.");
	}
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
