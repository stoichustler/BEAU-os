/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <bits.h>
#include <cpu.h>
#include <debug/shell.h>
#include <errno.h>
#include <per_cpu.h>
#include <notify.h>
#include <schedule.h>
#include <spinlock.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>
#include <bsp/cpufreq.h>
#include <asm/pmu.h>
#include <asm/sysreg.h>
#if CONFIG_ARM64_SPE
#include <asm/spe.h>
#endif

#include "shell_cmds.h"

static struct arm64_core_pmu_snapshot shell_pmu_snapshot;

#define HWC_SMP_TIMEOUT_US	10000U

struct shell_hwc_sample {
	uint64_t cycles;
	uint64_t generation;
	uint16_t pcpu_id;
	bool valid;
} __aligned(64);

static struct shell_hwc_sample shell_hwc_samples[MAX_PCPU_NUM];
static uint64_t shell_hwc_generation;

/* [20260812] Bounded per-pCPU hardware-cycle sample
 *
 * shell owner -> publish generation -> SMP callback -> local CNTPCT read
 *                                                |
 *                                                v
 *                                         release publication
 *
 * Key rule:
 *   - each target pCPU owns its CNTPCT_EL0 read and publishes it only after
 *     the command generation is visible;
 *   - a timed-out or stale callback cannot be reported as this command's
 *     sample, so incomplete rows remain unavailable.
 */
static void shell_hwc_capture(void *data)
{
	const uint64_t *generation = data;
	uint16_t pcpu_id = get_pcpu_id();
	struct shell_hwc_sample *sample;
	uint64_t value;

	if ((generation == NULL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	value = __atomic_load_n(generation, __ATOMIC_ACQUIRE);
	if (value == 0UL) {
		return;
	}

	sample = &shell_hwc_samples[pcpu_id];
	__atomic_store_n(&sample->valid, false, __ATOMIC_RELEASE);
	sample->cycles = cpu_ticks();
	sample->pcpu_id = pcpu_id;
	__atomic_store_n(&sample->generation, value, __ATOMIC_RELEASE);
	__atomic_store_n(&sample->valid, true, __ATOMIC_RELEASE);
}

static bool shell_hwc_get_sample(uint16_t pcpu_id, uint64_t generation,
	uint64_t *cycles)
{
	const struct shell_hwc_sample *sample = &shell_hwc_samples[pcpu_id];
	uint64_t sample_generation;
	uint64_t value;

	if ((cycles == NULL) || !__atomic_load_n(&sample->valid, __ATOMIC_ACQUIRE)) {
		return false;
	}
	value = __atomic_load_n(&sample->cycles, __ATOMIC_ACQUIRE);
	sample_generation = __atomic_load_n(&sample->generation, __ATOMIC_ACQUIRE);
	if (!__atomic_load_n(&sample->valid, __ATOMIC_ACQUIRE) ||
		(sample_generation != generation) || (sample->pcpu_id != pcpu_id)) {
		return false;
	}
	*cycles = value;
	return true;
}

#if CONFIG_ARM64_SPE
static const char *shell_spe_reason(enum arm64_spe_reason reason)
{
	static const char * const names[] = {
		"none", "config", "no-pmsver", "higher-el", "no-ppi", "buffer",
		"hardware", "buffer-full",
	};

	return reason < ARRAY_SIZE(names) ? names[reason] : "unknown";
}

int32_t shell_spestat(int32_t argc, char **argv)
{
	struct arm64_spe_snapshot snapshot;
	int32_t status;
	uint16_t pcpu_id;

	if (argc == 2) {
		if (strcmp(argv[1], "start") == 0) {
			return arm64_spe_start();
		}
		if (strcmp(argv[1], "stop") == 0) {
			return arm64_spe_stop();
		}
		if (strcmp(argv[1], "reset") == 0) {
			return arm64_spe_reset();
		}
	}
	if ((argc == 3) && (strcmp(argv[1], "dump") == 0)) {
		uint8_t bytes[ARM64_SPE_SHELL_DUMP_MAX];
		uint32_t length;
		uint32_t offset;

		pcpu_id = (uint16_t)strtol_deci(argv[2]);
		status = arm64_spe_dump(pcpu_id, bytes, &length);
		if (status != 0) {
			return status;
		}
		for (offset = 0U; offset < length; offset += 16U) {
			char line[MAX_STR_SIZE];
			uint32_t index;
			uint32_t pos = (uint32_t)snprintf(line, sizeof(line), "SPE cpu%hu +0x%04x:",
				pcpu_id, offset);

			for (index = offset; (index < length) && (index < (offset + 16U)); index++) {
				pos += (uint32_t)snprintf(&line[pos], sizeof(line) - pos,
					" %02x", bytes[index]);
			}
			shell_puts(line);
			shell_puts("\r\n");
		}
		return 0;
	}
	if (argc != 1) {
		return -EINVAL;
	}
	status = arm64_spe_take_snapshot(&snapshot);
	/* avail/running describe capture availability; PMSVer and ready are hardware
	 * capability/state; full/loss/error are cumulative capture faults; h0/h1 are
	 * retained half-buffer byte counts, and reason names the last stop cause.
	 */
	for (pcpu_id = 0U; pcpu_id < snapshot.pcpu_num; pcpu_id++) {
		char line[MAX_STR_SIZE];
		const struct arm64_spe_pcpu_snapshot *spe = &snapshot.pcpu[pcpu_id];

		snprintf(line, sizeof(line), "SPE cpu%hu avail=%u running=%u pmsver=%u ready=0x%x full=%lu loss=%lu error=%lu h0=%u h1=%u reason=%s\r\n",
			pcpu_id, spe->available, spe->running, spe->pmsver, spe->ready_mask,
			spe->buffer_full_count, spe->data_loss_count, spe->error_count,
			spe->half_bytes[0], spe->half_bytes[1], shell_spe_reason(spe->reason));
		shell_puts(line);
	}
	return status;
}
#endif
int32_t shell_cpufreq(__unused int32_t argc, __unused char **argv)
{
	cpufreq_dump();
	return 0;
}

int32_t shell_hwc(int32_t argc, __unused char **argv)
{
	uint64_t active_mask;
	uint64_t generation;
	uint16_t pcpu_id;
	int32_t status;

	if (argc != 1) {
		shell_puts("usage: hwc\r\n");
		return -EINVAL;
	}

	active_mask = get_active_pcpu_bitmap();
	if (active_mask == 0UL) {
		return -ENODEV;
	}
	generation = __atomic_add_fetch(&shell_hwc_generation, 1UL,
		__ATOMIC_SEQ_CST);
	if (generation == 0UL) {
		generation = __atomic_add_fetch(&shell_hwc_generation, 1UL,
			__ATOMIC_SEQ_CST);
	}
	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		if ((active_mask & (1UL << pcpu_id)) != 0UL) {
			__atomic_store_n(&shell_hwc_samples[pcpu_id].valid, false,
				__ATOMIC_RELEASE);
		}
	}
	status = smp_try_call_function_timeout(active_mask, shell_hwc_capture,
		&generation, HWC_SMP_TIMEOUT_US);

	shell_item_begin("HWC online:%hu smp-status:%d", bitmap_weight(active_mask), status);
	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		uint64_t cycles;
		uint64_t frequency_hz;

		if ((active_mask & (1UL << pcpu_id)) == 0UL) {
			continue;
		}
		if (!shell_hwc_get_sample(pcpu_id, generation, &cycles)) {
			shell_item_line("pcpu %hu: unavailable", pcpu_id);
			continue;
		}
		if (cpufreq_get_pcpu_frequency_hz(pcpu_id, &frequency_hz)) {
			shell_item_line("pcpu %hu: %lu hw cycles, %lu hz", pcpu_id,
				cycles, frequency_hz);
		} else {
			shell_item_line("pcpu %hu: %lu hw cycles, frequency unavailable",
				pcpu_id, cycles);
		}
	}
	shell_item_end();
	return status;
}

struct shell_pmu_aggregate {
	struct arm64_core_pmu_values values;
	uint64_t enabled_ticks;
	uint32_t event_mask;
	bool valid;
};

static uint64_t shell_pmu_saturating_add(uint64_t left, uint64_t right)
{
	return (right > (UINT64_MAX - left)) ? UINT64_MAX : left + right;
}

static void shell_pmu_add_values(struct arm64_core_pmu_values *target,
	const struct arm64_core_pmu_values *source)
{
	uint32_t event;

	for (event = 0U; event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		target->value[event] = shell_pmu_saturating_add(
			target->value[event], source->value[event]);
	}
	target->running_ticks = shell_pmu_saturating_add(
		target->running_ticks, source->running_ticks);
}

static void shell_pmu_format_ipc(char *buffer, size_t size, uint64_t instructions,
	uint64_t cycles)
{
	uint64_t whole = 0UL;
	uint64_t fraction = 0UL;

	if (cycles != 0UL) {
		uint64_t remainder;

		whole = instructions / cycles;
		remainder = instructions % cycles;
		if (remainder <= (UINT64_MAX / 1000UL)) {
			fraction = (remainder * 1000UL) / cycles;
		} else {
			fraction = remainder / ((cycles / 1000UL) + 1UL);
		}
	}
	(void)snprintf(buffer, size, "%lu.%03lu", whole, fraction);
}

static void shell_pmu_print_entity(const char *name, int32_t pcpu_id,
	uint64_t enabled_ticks, uint32_t event_mask,
	const struct arm64_core_pmu_values *values)
{
	char instructions[24U];
	char ipc[24U];

	if ((event_mask & (1U << ARM64_CORE_PMU_INSTRUCTIONS)) != 0U) {
		(void)snprintf(instructions, sizeof(instructions), "%lu",
			values->value[ARM64_CORE_PMU_INSTRUCTIONS]);
		shell_pmu_format_ipc(ipc, sizeof(ipc),
			values->value[ARM64_CORE_PMU_INSTRUCTIONS],
			values->value[ARM64_CORE_PMU_CYCLES]);
	} else {
		(void)snprintf(instructions, sizeof(instructions), "N/A");
		(void)snprintf(ipc, sizeof(ipc), "N/A");
	}
	shell_item_line("%-12s %4d %12lu %12lu %16lu %16s %9s",
		name, pcpu_id, ticks_to_us(enabled_ticks),
		ticks_to_us(values->running_ticks),
		values->value[ARM64_CORE_PMU_CYCLES],
		instructions, ipc);
	shell_output_checkpoint();
}

static void shell_pmu_format_optional(char *buffer, size_t size, uint64_t value,
	uint32_t event_mask, enum arm64_core_pmu_event event)
{
	if ((event_mask & (1U << event)) != 0U) {
		(void)snprintf(buffer, size, "%lu", value);
	} else {
		(void)snprintf(buffer, size, "N/A");
	}
}

static void shell_pmu_print_optional_entity(const char *name, uint32_t event_mask,
	const struct arm64_core_pmu_values *values)
{
	char frontend[24U];
	char backend[24U];
	char l1d[24U];
	char dtlb[24U];
	char branch[24U];

	shell_pmu_format_optional(frontend, sizeof(frontend),
		values->value[ARM64_CORE_PMU_STALL_FRONTEND], event_mask,
		ARM64_CORE_PMU_STALL_FRONTEND);
	shell_pmu_format_optional(backend, sizeof(backend),
		values->value[ARM64_CORE_PMU_STALL_BACKEND], event_mask,
		ARM64_CORE_PMU_STALL_BACKEND);
	shell_pmu_format_optional(l1d, sizeof(l1d),
		values->value[ARM64_CORE_PMU_L1D_REFILL], event_mask,
		ARM64_CORE_PMU_L1D_REFILL);
	shell_pmu_format_optional(dtlb, sizeof(dtlb),
		values->value[ARM64_CORE_PMU_DTLB_WALK], event_mask,
		ARM64_CORE_PMU_DTLB_WALK);
	shell_pmu_format_optional(branch, sizeof(branch),
		values->value[ARM64_CORE_PMU_BRANCH_MISPRED], event_mask,
		ARM64_CORE_PMU_BRANCH_MISPRED);
	shell_item_line("%-12s %14s %14s %14s %14s %14s", name,
		frontend, backend, l1d, dtlb, branch);
	shell_output_checkpoint();
}

static void shell_pmu_format_counter(char *buffer, size_t size,
	const struct arm64_core_pmu_capability *capability,
	enum arm64_core_pmu_event event)
{
	if (!capability->available ||
		((capability->event_mask & (1U << event)) == 0U)) {
		(void)snprintf(buffer, size, "-");
	} else if (event == ARM64_CORE_PMU_CYCLES) {
		(void)snprintf(buffer, size, "ccnt");
	} else {
		(void)snprintf(buffer, size, "c%u", capability->event_counter[event]);
	}
}

static void shell_pmu_print_event_map(uint16_t pcpu_id,
	const struct arm64_core_pmu_capability *capability)
{
	char counter[ARM64_CORE_PMU_EVENT_NUM][8U];
	uint32_t event;

	for (event = 0U; event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		shell_pmu_format_counter(counter[event], sizeof(counter[event]), capability,
			(enum arm64_core_pmu_event)event);
	}
	shell_item_line("pcpu%-2hu cycles:%-4s instructions:%-3s frontend:%-3s backend:%-3s l1d:%-3s dtlb:%-3s branch:%-3s",
		pcpu_id, counter[ARM64_CORE_PMU_CYCLES],
		counter[ARM64_CORE_PMU_INSTRUCTIONS],
		counter[ARM64_CORE_PMU_STALL_FRONTEND],
		counter[ARM64_CORE_PMU_STALL_BACKEND],
		counter[ARM64_CORE_PMU_L1D_REFILL],
		counter[ARM64_CORE_PMU_DTLB_WALK],
		counter[ARM64_CORE_PMU_BRANCH_MISPRED]);
	shell_output_checkpoint();
}

static void shell_pmu_add_path(struct arm64_core_pmu_path_values *target,
	const struct arm64_core_pmu_path_values *source)
{
	target->cycles = shell_pmu_saturating_add(target->cycles, source->cycles);
	target->instructions = shell_pmu_saturating_add(target->instructions,
		source->instructions);
	target->calls = shell_pmu_saturating_add(target->calls, source->calls);
	target->instruction_calls = shell_pmu_saturating_add(
		target->instruction_calls, source->instruction_calls);
	target->dropped = shell_pmu_saturating_add(target->dropped, source->dropped);
}

static void shell_pmu_print_path(const char *owner, enum arm64_core_pmu_path path,
	const struct arm64_core_pmu_path_values *values)
{
	char instructions[24U];
	char ipc[24U];

	if ((values->calls != 0UL) &&
		(values->instruction_calls == values->calls)) {
		(void)snprintf(instructions, sizeof(instructions), "%lu",
			values->instructions);
		shell_pmu_format_ipc(ipc, sizeof(ipc), values->instructions,
			values->cycles);
	} else {
		(void)snprintf(instructions, sizeof(instructions), "N/A");
		(void)snprintf(ipc, sizeof(ipc), "N/A");
	}
	shell_item_line("%-12s %-8s %10lu %16lu %16s %9s %10lu", owner,
		arm64_core_pmu_path_name(path), values->calls, values->cycles,
		instructions, ipc, values->dropped);
	shell_output_checkpoint();
}

/* [20260717] pmustat diagnostic projection
 *
 * local pCPU snapshots -> Host/pCPU/VM/vCPU aggregates -> one dump
 *
 * Key rules:
 *   - snapshot first closes every live owner interval on its owning pCPU;
 *   - unsupported events remain N/A rather than becoming misleading zeros;
 *   - shared-pCPU peer time plus scheduler wait is correlation, not causality.
 */
static int32_t shell_pmustat_dump(void)
{
	struct shell_pmu_aggregate host = { 0U };
	struct shell_pmu_aggregate vm[CONFIG_MAX_VM_NUM] = { 0U };
	struct arm64_core_pmu_path_values global_path[ARM64_CORE_PMU_PATH_NUM] = { 0U };
	struct arm64_core_pmu_path_values
		vm_path[CONFIG_MAX_VM_NUM][ARM64_CORE_PMU_PATH_NUM] = { 0U };
	uint16_t pcpu_id;
	uint16_t vm_id;
	uint32_t path;
	int32_t status = arm64_core_pmu_take_snapshot(&shell_pmu_snapshot);

	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];

		if (!pcpu->valid) {
			continue;
		}
		if (!host.valid) {
			host.event_mask = pcpu->capability.available ?
				pcpu->capability.event_mask : 0U;
		} else {
			host.event_mask &= (pcpu->capability.available ?
				pcpu->capability.event_mask : 0U);
		}
		host.valid = true;
		host.enabled_ticks = shell_pmu_saturating_add(host.enabled_ticks,
			pcpu->enabled_ticks);
		shell_pmu_add_values(&host.values, &pcpu->host);
		for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
			const struct arm64_core_pmu_vcpu_snapshot *vcpu = &pcpu->vcpu[vm_id];

			if (!vcpu->valid) {
				continue;
			}
			if (!vm[vm_id].valid) {
				vm[vm_id].event_mask = pcpu->capability.available ?
					pcpu->capability.event_mask : 0U;
			} else {
				vm[vm_id].event_mask &= (pcpu->capability.available ?
					pcpu->capability.event_mask : 0U);
			}
			vm[vm_id].valid = true;
			vm[vm_id].enabled_ticks = shell_pmu_saturating_add(
				vm[vm_id].enabled_ticks, vcpu->enabled_ticks);
			shell_pmu_add_values(&vm[vm_id].values, &vcpu->total);
			for (path = 0U; path < ARM64_CORE_PMU_PATH_NUM; path++) {
				shell_pmu_add_path(&global_path[path], &vcpu->path[path]);
				shell_pmu_add_path(&vm_path[vm_id][path], &vcpu->path[path]);
			}
		}
	}

	shell_item_begin("PMUSTAT");
	/* state/epoch describe collection control; poll.us is the sampling period.
	 * Capability is hardware support; counts/pressure/paths are snapshots. IPC is
	 * instructions per cycle, and '-' means the event is unsupported.
	 */
	shell_item_line("state:%s epoch:%lu poll.us:%u snapshot:%s",
		shell_pmu_snapshot.requested_running ? "running" : "stopped",
		shell_pmu_snapshot.epoch, ARM64_CORE_PMU_POLL_US,
		shell_pmu_snapshot.complete ? "complete" : "partial");
#if defined(CONFIG_PLATFORM_QEMU)
	shell_item_line("warning:QEMU validates PMU control/isolation/attribution only; values are not SoC performance data");
#endif
	shell_item_line("capability:");
	shell_item_line("pcpu state      PMUVer counters width(c/e) event-mask overflow PMCEID0           PMCEID1");
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];

		if (!pcpu->valid) {
			shell_item_line("%-4hu stale", pcpu_id);
			continue;
		}
		shell_item_line("%-4hu %-10s %-6u %-8u %2u/%-2u      0x%02x %8lu 0x%016lx 0x%016lx",
			pcpu_id, pcpu->capability.available ?
				(pcpu->running ? "running" : "stopped") : "disabled",
			pcpu->capability.pmuver, pcpu->capability.counter_num,
			pcpu->capability.cycle_width, pcpu->capability.event_width,
			pcpu->capability.event_mask, pcpu->overflow_count,
			pcpu->capability.pmceid0,
			pcpu->capability.pmceid1);
		shell_output_checkpoint();
	}
	shell_item_line("event-map ('-' means unsupported):");
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];

		if (pcpu->valid) {
			shell_pmu_print_event_map(pcpu_id, &pcpu->capability);
		}
	}

	shell_item_line("counts:");
	shell_item_line("owner        pcpu   enabled.us   running.us           cycles     instructions       IPC");
	shell_pmu_print_entity("host", -1, host.enabled_ticks, host.event_mask,
		&host.values);
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];
		char owner[16U];

		if (!pcpu->valid) {
			continue;
		}
		(void)snprintf(owner, sizeof(owner), "pcpu%hu", pcpu_id);
		shell_pmu_print_entity(owner, pcpu_id, pcpu->enabled_ticks,
			pcpu->capability.event_mask, &pcpu->total);
	}
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		char owner[16U];

		if (!vm[vm_id].valid) {
			continue;
		}
		(void)snprintf(owner, sizeof(owner), "vm%hu", vm_id);
		shell_pmu_print_entity(owner, -1, vm[vm_id].enabled_ticks,
			vm[vm_id].event_mask, &vm[vm_id].values);
	}
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];

		for (vm_id = 0U; pcpu->valid && (vm_id < CONFIG_MAX_VM_NUM); vm_id++) {
			const struct arm64_core_pmu_vcpu_snapshot *vcpu = &pcpu->vcpu[vm_id];
			char owner[16U];

			if (!vcpu->valid) {
				continue;
			}
			(void)snprintf(owner, sizeof(owner), "vm%hu:v%hu", vm_id,
				vcpu->vcpu_id);
			shell_pmu_print_entity(owner, pcpu_id, vcpu->enabled_ticks,
				pcpu->capability.event_mask, &vcpu->total);
		}
	}

	shell_item_line("pressure-events:");
	shell_item_line("owner        stall.frontend  stall.backend     l1d.refill      dtlb.walk branch.mispred");
	shell_pmu_print_optional_entity("host", host.event_mask, &host.values);
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];
		char owner[16U];

		if (!pcpu->valid) {
			continue;
		}
		(void)snprintf(owner, sizeof(owner), "pcpu%hu", pcpu_id);
		shell_pmu_print_optional_entity(owner, pcpu->capability.event_mask,
			&pcpu->total);
	}
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		char owner[16U];

		if (vm[vm_id].valid) {
			(void)snprintf(owner, sizeof(owner), "vm%hu", vm_id);
			shell_pmu_print_optional_entity(owner, vm[vm_id].event_mask,
				&vm[vm_id].values);
		}
	}
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];

		for (vm_id = 0U; pcpu->valid && (vm_id < CONFIG_MAX_VM_NUM); vm_id++) {
			const struct arm64_core_pmu_vcpu_snapshot *vcpu = &pcpu->vcpu[vm_id];
			char owner[16U];

			if (!vcpu->valid) {
				continue;
			}
			(void)snprintf(owner, sizeof(owner), "vm%hu:v%hu", vm_id,
				vcpu->vcpu_id);
			shell_pmu_print_optional_entity(owner,
				pcpu->capability.event_mask, &vcpu->total);
		}
	}

	shell_item_line("paths (inclusive; nested rows overlap):");
	shell_item_line("owner        path          calls           cycles     instructions       IPC    dropped");
	for (path = 0U; path < ARM64_CORE_PMU_PATH_NUM; path++) {
		shell_pmu_print_path("all", (enum arm64_core_pmu_path)path,
			&global_path[path]);
	}
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		char owner[16U];

		if (!vm[vm_id].valid) {
			continue;
		}
		(void)snprintf(owner, sizeof(owner), "vm%hu", vm_id);
		for (path = 0U; path < ARM64_CORE_PMU_PATH_NUM; path++) {
			if ((vm_path[vm_id][path].calls != 0UL) ||
				(vm_path[vm_id][path].dropped != 0UL)) {
				shell_pmu_print_path(owner,
					(enum arm64_core_pmu_path)path,
					&vm_path[vm_id][path]);
			}
		}
	}
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];

		for (vm_id = 0U; pcpu->valid && (vm_id < CONFIG_MAX_VM_NUM); vm_id++) {
			const struct arm64_core_pmu_vcpu_snapshot *vcpu = &pcpu->vcpu[vm_id];
			char owner[16U];

			if (!vcpu->valid) {
				continue;
			}
			(void)snprintf(owner, sizeof(owner), "vm%hu:v%hu", vm_id,
				vcpu->vcpu_id);
			for (path = 0U; path < ARM64_CORE_PMU_PATH_NUM; path++) {
				if ((vcpu->path[path].calls != 0UL) ||
					(vcpu->path[path].dropped != 0UL)) {
					shell_pmu_print_path(owner,
						(enum arm64_core_pmu_path)path,
						&vcpu->path[path]);
				}
			}
		}
	}

	shell_item_line("shared-pcpu correlation (scheduler max-wait is lifetime data):");
	shell_item_line("vcpu         pcpu target.run.us peer.run.us wait.max.us denied-pmu");
	for (pcpu_id = 0U; pcpu_id < shell_pmu_snapshot.pcpu_num; pcpu_id++) {
		const struct arm64_core_pmu_pcpu_snapshot *pcpu =
			&shell_pmu_snapshot.pcpu[pcpu_id];
		uint64_t all_vcpu_ticks =
			(pcpu->total.running_ticks > pcpu->host.running_ticks) ?
			(pcpu->total.running_ticks - pcpu->host.running_ticks) : 0UL;

		for (vm_id = 0U; pcpu->valid && (vm_id < CONFIG_MAX_VM_NUM); vm_id++) {
			const struct arm64_core_pmu_vcpu_snapshot *vcpu = &pcpu->vcpu[vm_id];
			struct sched_latency_stats latency = { 0U };
			struct acrn_vm *target_vm;
			uint64_t peer_ticks;
			char owner[16U];

			if (!vcpu->valid) {
				continue;
			}
			target_vm = get_vm_from_vmid(vm_id);
			if (is_created_vm(target_vm) &&
				(vcpu->vcpu_id < target_vm->hw.created_vcpus)) {
				sched_get_latency(&vcpu_from_vid(target_vm,
					vcpu->vcpu_id)->thread_obj,
					&latency);
			}
			peer_ticks = (all_vcpu_ticks > vcpu->total.running_ticks) ?
				(all_vcpu_ticks - vcpu->total.running_ticks) : 0UL;
			(void)snprintf(owner, sizeof(owner), "vm%hu:v%hu", vm_id,
				vcpu->vcpu_id);
			shell_item_line("%-12s %-4hu %13lu %11lu %11lu %10lu", owner,
				pcpu_id, ticks_to_us(vcpu->total.running_ticks),
				ticks_to_us(peer_ticks), ticks_to_us(latency.max_wait_ticks),
				vcpu->denied_accesses);
			shell_output_checkpoint();
		}
	}
	shell_item_end();
	return status;
}

int32_t shell_pmustat(int32_t argc, char **argv)
{
	int32_t status;
	char buffer[96U];

	if (argc != 2) {
		shell_puts("usage: pmustat <start|stop|reset|dump>\r\n");
		return -EINVAL;
	}
	if (strcmp(argv[1], "start") == 0) {
		status = arm64_core_pmu_start();
	} else if (strcmp(argv[1], "stop") == 0) {
		status = arm64_core_pmu_stop();
	} else if (strcmp(argv[1], "reset") == 0) {
		status = arm64_core_pmu_reset();
	} else if (strcmp(argv[1], "dump") == 0) {
		return shell_pmustat_dump();
	} else {
		shell_puts("usage: pmustat <start|stop|reset|dump>\r\n");
		return -EINVAL;
	}

	(void)snprintf(buffer, sizeof(buffer), "PMUSTAT %5s: %s (%d)\r\n",
		argv[1], (status == 0) ? "ok" : "failed", status);
	shell_puts(buffer);
	return status;
}
