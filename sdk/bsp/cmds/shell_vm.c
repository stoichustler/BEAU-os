/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <console.h>
#include <errno.h>
#include <schedule.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>
#include <vcpu.h>
#include <vconfig.h>
#include <vm.h>
#include <vm_wdt.h>
#include <vhost_console.h>
#include <virtio_proxy.h>
#include <bsp/vuart.h>
#include <asm/cache.h>
#include <asm/coredump.h>
#include <asm/guest/vcpu.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vmpu.h>
#include <asm/guest/stage2.h>
#include <asm/mmu.h>
#include <asm/pmu.h>
#include <asm/platform.h>
#include <asm/sysreg.h>

#include "shell_cmds.h"

#define VMSTAT_CPU_WAIT_WARN_US	20000UL
#define SHELL_HEALTH_CPU_PERCENT_SCALE	1000UL

/* VM-scoped diagnostics share one read-only snapshot/reporting module. */
int32_t shell_coredump(int32_t argc, char **argv)
{
	if (argc != 2) {
		shell_puts("usage: coredump <print|erase>\r\n");
		return -EINVAL;
	}

	if (strncmp(argv[1], "print", sizeof("print")) == 0) {
		if (!arm64_coredump_print_stored()) {
			shell_puts("COREDUMP: no valid stored snapshot\r\n");
		}
		return 0;
	}
	if (strncmp(argv[1], "erase", sizeof("erase")) == 0) {
		arm64_coredump_erase_stored();
		shell_puts("COREDUMP: erased\r\n");
		return 0;
	}

	shell_puts("usage: coredump <print|erase>\r\n");
	return -EINVAL;
}

/* [20260630] vmstat monitor:
 *
 * vmstat is the broad health summary before using retained hwtdbg evidence. It
 * keeps one VM visible at a time: configured resources, runtime state,
 * watchdog and console status, scheduler diagnostics, and the guest timer/vGIC
 * delivery summary.
 *
 *   VM config + runtime VM object
 *        -> VM resource/state rows
 *        -> per-vCPU scheduler rows
 *        -> per-vCPU timer/vGIC rows
 */
static bool shell_vm_config_present(const struct acrn_vm_config *vm_config)
{
	return (vm_config->name[0] != '\0') || (vm_config->cpu_affinity != 0UL) ||
		((vm_config->guest_flags & GUEST_FLAG_STATIC_VM) != 0UL);
}

static void shell_print_cpu_bitmap(uint64_t bitmap)
{
	char temp_str[MAX_STR_SIZE];
	bool first = true;
	uint16_t cpu_id;

	for (cpu_id = 0U; cpu_id < MAX_PCPU_NUM; cpu_id++) {
		if ((bitmap & (1UL << cpu_id)) != 0UL) {
			(void)snprintf(temp_str, MAX_STR_SIZE, "%spcpu%hu", first ? "" : ",", cpu_id);
			shell_puts(temp_str);
			first = false;
		}
	}

	if (first) {
		shell_puts("-");
	}
}

static void shell_cachestat_print_vm_affinity(uint16_t vm_id,
	const struct acrn_vm_config *vm_config, const struct acrn_vm *vm)
{
	const char *name = (vm->name[0] != '\0') ? vm->name : vm_config->name;
	uint16_t count = vm_config->cpu_affinity_num;
	uint16_t vcpu_id;

	if (count == 0U) {
		count = vm->hw.created_vcpus;
	}
	if (count == 0U) {
		shell_item_line("vm%hu:%s cache:affinity:none", vm_id, name);
		return;
	}

	for (vcpu_id = 0U; vcpu_id < count; vcpu_id++) {
		uint16_t pcpu_id = INVALID_CPU_ID;
		uint32_t llc_id;

		if (vcpu_id < vm_config->cpu_affinity_num) {
			pcpu_id = vm_config->cpu_affinity_order[vcpu_id];
		} else if (vcpu_id < vm->hw.created_vcpus) {
			const struct acrn_vcpu *vcpu =
				vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);

			if (vcpu != NULL) {
				pcpu_id = vcpu->thread_obj.pcpu_id;
			}
		}

		llc_id = arm64_cache_llc_id_for_pcpu(pcpu_id);
		if (llc_id == UINT32_MAX) {
			shell_item_line("vm%hu:%9s vcpu%hu pcpu:- llc:-", vm_id, name, vcpu_id);
		} else {
			shell_item_line("vm%hu:%9s vcpu%hu pcpu:%hu llc:%u",
				vm_id, name, vcpu_id, pcpu_id, llc_id);
		}
		shell_output_checkpoint();
	}
}

static const char *shell_cachestat_domain_source(uint8_t source)
{
	return (source == ARM64_PLATFORM_LLC_SOURCE_DTS) ? "dts" : "mpidr";
}

int32_t shell_cachestat(int32_t argc, __unused char **argv)
{
	struct arm64_cache_info info;
	uint32_t idx;
	uint16_t vm_id;

	if (argc != 1) {
		return -EINVAL;
	}

	arm64_cache_get_info(&info);
	shell_item_begin("cachestat");
	/* Cache leaves are BSP-local register samples; LLC rows are DTS topology. */
	shell_item_line("valid:%s ctr:0x%016lx clidr:0x%016lx line:D:%u I:%u",
		shell_yes_no(info.valid), info.ctr_el0, info.clidr_el1,
		info.dcache_line_size, info.icache_line_size);
	shell_item_line("BSP cache: level:%u type:%s size:%luKB mask:0x%016lx",
		info.llc_level, arm64_cache_type_str(info.llc_type),
		info.llc_size / 1024UL, info.llc_pcpu_mask);
	shell_item_line("LLC domains:");
	for (idx = 0U; idx < info.llc_domain_count; idx++) {
		const struct arm64_cache_domain *domain = &info.domains[idx];

		shell_item_line("llc%u source:%s pcpus:0x%016lx", domain->id,
			shell_cachestat_domain_source(domain->source), domain->pcpu_mask);
	}
	shell_item_line("pCPU LLC map:");
	for (idx = 0U; idx < MAX_PCPU_NUM; idx++) {
		shell_item_line("pcpu%u llc:%u", idx, arm64_cache_llc_id_for_pcpu((uint16_t)idx));
	}

	if (info.leaf_count == 0U) {
		shell_item_line("cache:none");
	} else {
		shell_item_line("cache leaves:");
		shell_item_line("level  type     line  sets   ways   size    shared-pcpu-mask");
		shell_item_line("─────  ───────  ────  ─────  ─────  ──────  ──────────────────");
		for (idx = 0U; idx < info.leaf_count; idx++) {
			const struct arm64_cache_leaf *leaf = &info.leaves[idx];

			shell_item_line("%-5u  %-7s  %-4u  %-5u  %-5u  %-6lu  0x%016lx",
				leaf->level, arm64_cache_type_str(leaf->type), leaf->line_size,
				leaf->sets, leaf->ways, leaf->size / 1024UL,
				leaf->shared_pcpu_mask);
			shell_output_checkpoint();
		}
	}

	shell_item_line("vm LLC placement:");
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (!shell_vm_config_present(vm_config) &&
			(vm->hw.created_vcpus == 0U) && is_poweroff_vm(vm)) {
			continue;
		}
		shell_cachestat_print_vm_affinity(vm_id, vm_config, vm);
		shell_output_checkpoint();
	}
	shell_item_end();
	return 0;
}

static void shell_print_vm_affinity(const struct acrn_vm_config *vm_config,
	const struct acrn_vm *vm)
{
	char temp_str[MAX_STR_SIZE];
	bool first = true;
	uint16_t vcpu_id;

	if (vm->hw.created_vcpus == 0U) {
		shell_print_cpu_bitmap(vm_config->cpu_affinity);
		return;
	}

	/* [20260711] vCPU affinity display:
	 * cpu_affinity is a bitmap and loses DTS order. Runtime vCPU objects keep
	 * the real vCPU->pCPU binding, so print that order to make vCPU0 placement
	 * explicit in vmstat.
	 */
	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		const struct acrn_vcpu *vcpu = vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);

		if (vcpu == NULL) {
			continue;
		}
		(void)snprintf(temp_str, MAX_STR_SIZE, "%svcpu%hu:pcpu%hu",
			first ? "" : ",", vcpu_id, vcpu->thread_obj.pcpu_id);
		shell_puts(temp_str);
		first = false;
	}

	if (first) {
		shell_puts("-");
	}
}

static uint32_t shell_cpu_bitmap_weight(uint64_t bitmap)
{
	uint32_t weight = 0U;
	uint16_t cpu_id;

	for (cpu_id = 0U; cpu_id < MAX_PCPU_NUM; cpu_id++) {
		if ((bitmap & (1UL << cpu_id)) != 0UL) {
			weight++;
		}
	}

	return weight;
}

struct shell_vmstat_timer_summary {
	uint64_t cntv_ppi;
	uint64_t cntv_backup;
	uint64_t cntv_poll;
	uint64_t pre_eret_flush;
	uint64_t pre_eret_flush_expired;
	uint64_t lost_pending_lr;
};

static const char *shell_vmstat_wdt_status_to_str(enum vm_wdt_status status)
{
	const char *str;

	switch (status) {
	case VM_WDT_STATUS_OFFLINE:
		str = "offline";
		break;
	case VM_WDT_STATUS_UNKNOWN:
		str = "none";
		break;
	case VM_WDT_STATUS_ALIVE:
		str = "alive";
		break;
	case VM_WDT_STATUS_STUCK:
		str = "stuck";
		break;
	default:
		str = "unused";
		break;
	}

	return str;
}

static const char *shell_vmstat_wdt_cause_to_str(enum vm_wdt_cause cause)
{
	const char *str;

	switch (cause) {
	case VM_WDT_CAUSE_HEARTBEAT:
		str = "heartbeat";
		break;
	case VM_WDT_CAUSE_TIMEOUT:
		str = "timeout";
		break;
	case VM_WDT_CAUSE_VCPU_STALL:
		str = "vcpustall";
		break;
	case VM_WDT_CAUSE_IRQ_STORM:
		str = "irqstorm";
		break;
	case VM_WDT_CAUSE_CONSOLE_STUCK:
		str = "console";
		break;
	case VM_WDT_CAUSE_VIRTIO_STUCK:
		str = "virtio";
		break;
	case VM_WDT_CAUSE_GUEST_VCPU_STALL:
		str = "guestvcpu";
		break;
	case VM_WDT_CAUSE_NONE:
	default:
		str = "N/A";
		break;
	}

	return str;
}

static const char *shell_vmstat_wdt_recovery_to_str(enum vm_wdt_recovery_state state)
{
	const char *str;

	switch (state) {
	case VM_WDT_RECOVERY_QUIESCING:
		str = "quiescing";
		break;
	case VM_WDT_RECOVERY_RESETTING:
		str = "resetting";
		break;
	case VM_WDT_RECOVERY_VERIFYING:
		str = "verifying";
		break;
	case VM_WDT_RECOVERY_IDLE:
	default:
		str = "idle";
		break;
	}

	return str;
}

enum shell_health_level {
	SHELL_HEALTH_PASS = 0U,
	SHELL_HEALTH_WARN,
	SHELL_HEALTH_FAIL,
};

#define SHELL_HEALTH_HOST_PCPU_INACTIVE		(1UL << 0U)
#define SHELL_HEALTH_HOST_NO_CURRENT		(1UL << 1U)
#define SHELL_HEALTH_HOST_HV_S1_FULL		(1UL << 2U)
#define SHELL_HEALTH_HOST_VM_S2_FULL		(1UL << 3U)
#define SHELL_HEALTH_HOST_S2_OWNERSHIP		(1UL << 4U)
#define SHELL_HEALTH_HOST_S2_MALFORMED		(1UL << 5U)

#define SHELL_HEALTH_VM_STATE			(1UL << 0U)
#define SHELL_HEALTH_VM_VCPU_COUNT		(1UL << 1U)
#define SHELL_HEALTH_VM_VCPU_STATE		(1UL << 2U)
#define SHELL_HEALTH_VM_WDT_UNKNOWN		(1UL << 3U)
#define SHELL_HEALTH_VM_WDT_STUCK		(1UL << 4U)
#define SHELL_HEALTH_VM_WDT_RECOVERY		(1UL << 5U)
#define SHELL_HEALTH_VM_WDT_RESTART_FAIL	(1UL << 6U)
#define SHELL_HEALTH_VM_VIRTIO_NOT_READY	(1UL << 7U)
#define SHELL_HEALTH_VM_VIRTIO_LOST		(1UL << 8U)
#define SHELL_HEALTH_VM_VIRTIO_TIMEOUT		(1UL << 9U)

struct shell_health_host {
	enum shell_health_level level;
	uint64_t reasons;
	uint16_t pcpu_total;
	uint16_t pcpu_active;
	uint16_t pcpu_current;
	struct page_pool_stats hv_s1;
	struct page_pool_stats vm_s2;
	uint64_t stage2_accounted;
	uint64_t stage2_unowned;
	uint64_t stage2_overaccounted;
	uint64_t stage2_malformed;
};

struct shell_health_vm {
	enum shell_health_level level;
	uint64_t reasons;
	const char *name;
	enum vm_state state;
	uint16_t lifecycle_phase;
	uint16_t vm_id;
	uint16_t configured_vcpus;
	uint16_t created_vcpus;
	bool present;
	bool wdt_valid;
	struct vm_wdt_snapshot wdt;
	bool console_valid;
	uint32_t console_queued;
	uint32_t console_capacity;
	uint64_t console_dropped;
	uint16_t virtio_total;
	uint16_t virtio_ready;
	uint16_t virtio_lost;
	uint64_t virtio_timeouts;
};

/* [20260718] Bounded health snapshot storage
 *
 * shell command parser stack
 *             |
 *             v
 * static VM snapshots -> collect every slot -> print -> reuse on next command
 *
 * Key rule:
 *   - the single shell thread owns the snapshots, so no lock is required;
 *   - each collector fully initializes its slot before the output reads it;
 *   - keeping the VM array out of the 8 KiB shell stack prevents nested WDT
 *     and output snapshots from exhausting the command stack.
 */
static struct shell_health_vm shell_health_vms[CONFIG_MAX_VM_NUM];

/* [20260718] On-demand vCPU utilization
 *
 * scheduler runtime[N] at previous health
 *                  |
 *                  v
 * scheduler runtime[N] now -> bounded delta/window -> display percentage
 *                  |
 *                  +--> no baseline or counter rollback -> "--"
 *
 * Key rule:
 *   - the single shell thread owns the history, so no periodic worker or lock
 *     is needed;
 *   - lifecycle state gates presentation before a percentage is emitted;
 *   - scheduler accounting remains unchanged and the command is read-only.
 */
struct shell_health_cpu_history {
	uint64_t sample_ticks;
	uint64_t runtime_ticks[CONFIG_MAX_VM_NUM][MAX_VCPUS_PER_VM];
	bool runtime_valid[CONFIG_MAX_VM_NUM][MAX_VCPUS_PER_VM];
	bool valid;
};

static struct shell_health_cpu_history shell_health_cpu_history;

static enum shell_health_level shell_health_max(enum shell_health_level left,
	enum shell_health_level right)
{
	return left > right ? left : right;
}

static void shell_health_raise(enum shell_health_level *level,
	enum shell_health_level requested)
{
	if ((level != NULL) && (requested > *level)) {
		*level = requested;
	}
}

static const char *shell_health_level_to_str(enum shell_health_level level)
{
	const char *str;

	switch (level) {
	case SHELL_HEALTH_FAIL:
		str = "FAIL";
		break;
	case SHELL_HEALTH_WARN:
		str = "WARN";
		break;
	case SHELL_HEALTH_PASS:
	default:
		str = "PASS";
		break;
	}

	return str;
}

static uint64_t shell_health_cpu_permille(uint64_t runtime_ticks,
	uint64_t window_ticks)
{
	uint64_t permille;

	if ((window_ticks == 0UL) || (runtime_ticks >= window_ticks) ||
		(runtime_ticks > (UINT64_MAX / SHELL_HEALTH_CPU_PERCENT_SCALE))) {
		permille = (window_ticks == 0UL) ? 0UL :
			SHELL_HEALTH_CPU_PERCENT_SCALE;
	} else {
		permille = (runtime_ticks * SHELL_HEALTH_CPU_PERCENT_SCALE) /
			window_ticks;
	}

	return permille;
}

static void shell_health_cpu_format(char *buf, size_t size, uint64_t permille)
{
	(void)snprintf(buf, size, "%lu.%01lu%%", permille / 10UL,
		permille % 10UL);
}

static void shell_health_cpu_append(char *line, size_t size, const char *cell)
{
	size_t offset = strnlen_s(line, size);

	if (offset < size) {
		(void)snprintf(&line[offset], size - offset, "%-8s", cell);
	}
}

static uint16_t shell_health_max_vcpus(const struct shell_health_vm *vms)
{
	uint16_t max_vcpus = 0U;
	uint16_t vm_id;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (vms[vm_id].present &&
			(vms[vm_id].configured_vcpus > max_vcpus)) {
			max_vcpus = vms[vm_id].configured_vcpus;
		}
	}

	return (max_vcpus <= MAX_VCPUS_PER_VM) ? max_vcpus :
		MAX_VCPUS_PER_VM;
}

static void shell_health_print_cpu_usage(const struct shell_health_vm *vms)
{
	uint64_t sample_ticks = cpu_ticks();
	uint64_t window_ticks = 0UL;
	uint16_t max_vcpus = shell_health_max_vcpus(vms);
	bool has_window = shell_health_cpu_history.valid &&
		(sample_ticks > shell_health_cpu_history.sample_ticks);
	char line[MAX_STR_SIZE];
	uint16_t vm_id;
	uint16_t vcpu_id;

	if (has_window) {
		window_ticks = sample_ticks - shell_health_cpu_history.sample_ticks;
		shell_item_section("vCPU utilization window:%lums", ticks_to_ms(window_ticks));
	} else {
		shell_item_section("vCPU utilization window:baseline");
	}
	if (max_vcpus == 0U) {
		shell_item_line("no configured vCPUs");
		(void)memset(shell_health_cpu_history.runtime_valid, 0U,
			sizeof(shell_health_cpu_history.runtime_valid));
		shell_health_cpu_history.sample_ticks = sample_ticks;
		shell_health_cpu_history.valid = true;
		return;
	}

	(void)snprintf(line, sizeof(line), "vmid  ");
	for (vcpu_id = 0U; vcpu_id < max_vcpus; vcpu_id++) {
		char cell[16U];

		(void)snprintf(cell, sizeof(cell), "vcpu%hu", vcpu_id);
		shell_health_cpu_append(line, sizeof(line), cell);
	}
	shell_health_cpu_append(line, sizeof(line), "total");
	shell_item_line("%s", line);

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		const struct shell_health_vm *health = &vms[vm_id];
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);
		uint64_t total_permille = 0UL;
		uint16_t running_vcpus = 0U;
		bool total_complete = true;

		if (!health->present) {
			(void)memset(shell_health_cpu_history.runtime_valid[vm_id], 0U,
				sizeof(shell_health_cpu_history.runtime_valid[vm_id]));
			continue;
		}

		(void)snprintf(line, sizeof(line), "%-6hu", vm_id);
		for (vcpu_id = 0U; vcpu_id < max_vcpus; vcpu_id++) {
			struct acrn_vcpu *vcpu = NULL;
			struct sched_latency_stats latency = { 0U };
			uint64_t permille = 0UL;
			bool runtime_valid = false;
			bool running = false;
			char cell[16U];

			if (vcpu_id >= health->configured_vcpus) {
				(void)snprintf(cell, sizeof(cell), "NC");
			} else if (vcpu_id >= health->created_vcpus) {
				(void)snprintf(cell, sizeof(cell), "NA");
			} else {
				vcpu = vcpu_from_vid(vm, vcpu_id);
				if (vcpu != NULL) {
					sched_get_latency(&vcpu->thread_obj, &latency);
					runtime_valid = true;
					running = (health->state == VM_RUNNING) &&
						(vcpu_get_state(vcpu) == VCPU_RUNNING);
				}

				if (!running) {
					(void)snprintf(cell, sizeof(cell), "NA");
				} else {
					running_vcpus++;
					if (has_window &&
						shell_health_cpu_history.runtime_valid[vm_id][vcpu_id] &&
						(latency.runtime_ticks >=
						 shell_health_cpu_history.runtime_ticks[vm_id][vcpu_id])) {
						permille = shell_health_cpu_permille(
							latency.runtime_ticks -
							shell_health_cpu_history.runtime_ticks[vm_id][vcpu_id],
							window_ticks);
						shell_health_cpu_format(cell, sizeof(cell), permille);
						total_permille += permille;
					} else {
						(void)snprintf(cell, sizeof(cell), "--");
						total_complete = false;
					}
				}
			}

			shell_health_cpu_history.runtime_ticks[vm_id][vcpu_id] =
				latency.runtime_ticks;
			shell_health_cpu_history.runtime_valid[vm_id][vcpu_id] =
				runtime_valid;
			shell_health_cpu_append(line, sizeof(line), cell);
		}
		for (; vcpu_id < MAX_VCPUS_PER_VM; vcpu_id++) {
			shell_health_cpu_history.runtime_valid[vm_id][vcpu_id] = false;
		}

		if (running_vcpus == 0U) {
			shell_health_cpu_append(line, sizeof(line), "NA");
		} else if (!total_complete) {
			shell_health_cpu_append(line, sizeof(line), "--");
		} else {
			char total[16U];

			shell_health_cpu_format(total, sizeof(total), total_permille);
			shell_health_cpu_append(line, sizeof(line), total);
		}
		shell_item_line("%s", line);
		shell_output_checkpoint();
	}
	shell_item_line("[TIP] %%:running  NC:not configured  NA:not active  --:baseline pending");
	shell_health_cpu_history.sample_ticks = sample_ticks;
	shell_health_cpu_history.valid = true;
}

static void shell_health_collect_host(struct shell_health_host *health)
{
	uint16_t pcpu_id;
	uint16_t vm_id;

	(void)memset(health, 0U, sizeof(*health));
	health->level = SHELL_HEALTH_PASS;
	health->pcpu_total = get_pcpu_nums();
	for (pcpu_id = 0U; pcpu_id < health->pcpu_total; pcpu_id++) {
		if (is_pcpu_active(pcpu_id)) {
			health->pcpu_active++;
		} else {
			health->reasons |= SHELL_HEALTH_HOST_PCPU_INACTIVE;
			shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
		}
		if (sched_get_current(pcpu_id) != NULL) {
			health->pcpu_current++;
		} else {
			health->reasons |= SHELL_HEALTH_HOST_NO_CURRENT;
			shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
		}
	}

	arm64_get_hv_s1_page_pool_stats(&health->hv_s1);
	arm64_get_stage2_page_pool_stats(&health->vm_s2);
	if ((health->hv_s1.total_pages != 0UL) && (health->hv_s1.free_pages == 0UL)) {
		health->reasons |= SHELL_HEALTH_HOST_HV_S1_FULL;
		shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
	}
	if ((health->vm_s2.total_pages != 0UL) && (health->vm_s2.free_pages == 0UL)) {
		health->reasons |= SHELL_HEALTH_HOST_VM_S2_FULL;
		shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
	}

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct arm64_stage2_vm_stats stats;

		if (arm64_get_stage2_vm_stats(get_vm_from_vmid(vm_id), &stats)) {
			health->stage2_accounted += stats.total_pages;
			health->stage2_malformed += stats.malformed_entries;
		}
	}
	health->stage2_unowned = (health->vm_s2.used_pages > health->stage2_accounted) ?
		(health->vm_s2.used_pages - health->stage2_accounted) : 0UL;
	health->stage2_overaccounted =
		(health->stage2_accounted > health->vm_s2.used_pages) ?
		(health->stage2_accounted - health->vm_s2.used_pages) : 0UL;
	if ((health->stage2_unowned != 0UL) ||
		(health->stage2_overaccounted != 0UL)) {
		health->reasons |= SHELL_HEALTH_HOST_S2_OWNERSHIP;
		shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
	}
	if (health->stage2_malformed != 0UL) {
		health->reasons |= SHELL_HEALTH_HOST_S2_MALFORMED;
		shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
	}
}

static void shell_health_collect_virtio(struct shell_health_vm *health, bool grade)
{
	uint16_t count = virtio_proxy_device_count(health->vm_id);
	uint16_t index;

	for (index = 0U; index < count; index++) {
		struct virtio_proxy_stats stats;

		if (!virtio_proxy_get_stats(health->vm_id, index, &stats)) {
			continue;
		}
		health->virtio_total++;
		health->virtio_timeouts += stats.timeout_count;
		if ((stats.state == VIRTIO_PROXY_STATE_RUNNING) && stats.backend_healthy) {
			health->virtio_ready++;
		} else if ((stats.state == VIRTIO_PROXY_STATE_BACKEND_LOST) ||
			(stats.state == VIRTIO_PROXY_STATE_BACKEND_STALE)) {
			health->virtio_lost++;
			if (grade) {
				health->reasons |= SHELL_HEALTH_VM_VIRTIO_LOST;
				shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
			}
		} else if (grade) {
			health->reasons |= SHELL_HEALTH_VM_VIRTIO_NOT_READY;
			shell_health_raise(&health->level, SHELL_HEALTH_WARN);
		}
	}
	if (grade && (health->virtio_timeouts != 0UL)) {
		health->reasons |= SHELL_HEALTH_VM_VIRTIO_TIMEOUT;
		shell_health_raise(&health->level, SHELL_HEALTH_WARN);
	}
}

static void shell_health_collect_vm(uint16_t vm_id, struct shell_health_vm *health)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm_id);
	struct acrn_vm *vm = get_vm_from_vmid(vm_id);
	struct console_vm_ring_stats console = { 0U };
	uint16_t vcpu_id;
	bool must_run;

	(void)memset(health, 0U, sizeof(*health));
	health->level = SHELL_HEALTH_PASS;
	health->vm_id = vm_id;
	health->present = shell_vm_config_present(vm_config) ||
		(vm->hw.created_vcpus != 0U) || (vm->root_stg2ptp != NULL) ||
		!is_poweroff_vm(vm);
	if (!health->present) {
		return;
	}

	health->name = (vm->name[0] != '\0') ? vm->name : vm_config->name;
	health->state = vm->state;
	health->lifecycle_phase = vm->lifecycle.phase;
	health->configured_vcpus = (uint16_t)shell_cpu_bitmap_weight(vm_config->cpu_affinity);
	health->created_vcpus = vm->hw.created_vcpus;
	/* A created post-launched VM is valid until its device model starts it. */
	must_run = is_static_configured_vm(vm) ||
		((vm->state != VM_POWERED_OFF) && (vm->state != VM_CREATED));
	if (must_run && (vm->state != VM_RUNNING)) {
		health->reasons |= SHELL_HEALTH_VM_STATE;
		shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
	}
	if (must_run && (health->configured_vcpus != health->created_vcpus)) {
		health->reasons |= SHELL_HEALTH_VM_VCPU_COUNT;
		shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
	}
	if (vm->state == VM_RUNNING) {
		for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
			struct acrn_vcpu *vcpu = vcpu_from_vid(vm, vcpu_id);
			enum vcpu_state state = (vcpu != NULL) ?
				vcpu_get_state(vcpu) : VCPU_OFFLINE;

			if ((vcpu == NULL) ||
				((vcpu_id == BSP_CPU_ID) && !is_vcpu_running(vcpu)) ||
				((vcpu_id != BSP_CPU_ID) && !is_vcpu_running(vcpu) &&
				 (state != VCPU_INIT) && (state != VCPU_POWERED_OFF))) {
				health->reasons |= SHELL_HEALTH_VM_VCPU_STATE;
				shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
				break;
			}
		}
	}

	health->wdt_valid = vm_wdt_get_snapshot(vm_id, &health->wdt) == 0;
	if (health->wdt_valid && must_run) {
		if (health->wdt.status == VM_WDT_STATUS_STUCK) {
			health->reasons |= SHELL_HEALTH_VM_WDT_STUCK;
			shell_health_raise(&health->level, SHELL_HEALTH_FAIL);
		} else if ((vm->state == VM_RUNNING) &&
			((health->wdt.status == VM_WDT_STATUS_UNKNOWN) ||
			(health->wdt.status == VM_WDT_STATUS_OFFLINE))) {
			health->reasons |= SHELL_HEALTH_VM_WDT_UNKNOWN;
			shell_health_raise(&health->level, SHELL_HEALTH_WARN);
		}
		if (health->wdt.restart_pending ||
			(health->wdt.recovery_state != VM_WDT_RECOVERY_IDLE)) {
			health->reasons |= SHELL_HEALTH_VM_WDT_RECOVERY;
			shell_health_raise(&health->level, SHELL_HEALTH_WARN);
		}
		if (health->wdt.restart_fail_count != 0UL) {
			health->reasons |= SHELL_HEALTH_VM_WDT_RESTART_FAIL;
			shell_health_raise(&health->level, SHELL_HEALTH_WARN);
		}
	}

	health->console_valid = console_vm_ring_get_stats(vm_id, &console);
	if (health->console_valid) {
		health->console_queued = console.queued;
		health->console_capacity = console.capacity;
		health->console_dropped = console.dropped_bytes;
	}
	shell_health_collect_virtio(health, vm->state == VM_RUNNING);
}

static void shell_health_print_vm(const struct shell_health_vm *health)
{
	char wdt_age[16U];
	char console[32U];
	char virtio[16U];
	const char *wdt_status = "-";

	if (health->wdt_valid) {
		wdt_status = shell_vmstat_wdt_status_to_str(health->wdt.status);
		(void)snprintf(wdt_age, sizeof(wdt_age), "%lums", health->wdt.last_ms);
	} else {
		(void)snprintf(wdt_age, sizeof(wdt_age), "-");
	}
	if (health->console_valid) {
		(void)snprintf(console, sizeof(console), "%5u/%5u d:%lu",
			health->console_queued, health->console_capacity,
			health->console_dropped);
	} else {
		(void)snprintf(console, sizeof(console), "-");
	}
	if (health->virtio_total != 0U) {
		(void)snprintf(virtio, sizeof(virtio), "%hu/%hu",
			health->virtio_ready, health->virtio_total);
	} else {
		(void)snprintf(virtio, sizeof(virtio), "-");
	}

	shell_item_line("vm%-2hu %-10s %-9s %2hu/%-2hu %-7s %-10s %-20s %-6s %s",
		health->vm_id, health->name, shell_vm_state_to_str(health->state),
		health->created_vcpus, health->configured_vcpus,
		wdt_status, wdt_age, console, virtio,
		vm_lifecycle_phase_name(health->lifecycle_phase));
	shell_output_checkpoint();
}

static void shell_health_print_findings(const struct shell_health_host *host,
	const struct shell_health_vm *vms)
{
	bool printed = false;
	uint16_t vm_id;

	if ((host->reasons & SHELL_HEALTH_HOST_PCPU_INACTIVE) != 0UL) {
		shell_item_line("host: inactive pCPU detected");
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_NO_CURRENT) != 0UL) {
		shell_item_line("host: pCPU without current scheduler thread");
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_HV_S1_FULL) != 0UL) {
		shell_item_line("host: HV-s1 page-table pool exhausted");
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_VM_S2_FULL) != 0UL) {
		shell_item_line("host: VM-s2 page-table pool exhausted");
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_S2_OWNERSHIP) != 0UL) {
		shell_item_line("host: VM-s2 ownership mismatch unowned:%lu over:%lu",
			host->stage2_unowned, host->stage2_overaccounted);
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_S2_MALFORMED) != 0UL) {
		shell_item_line("host: malformed stage-2 links:%lu", host->stage2_malformed);
		printed = true;
	}

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		const struct shell_health_vm *health = &vms[vm_id];

		if (!health->present || (health->reasons == 0UL)) {
			continue;
		}
		if ((health->reasons & SHELL_HEALTH_VM_STATE) != 0UL) {
			shell_item_line("vm%hu:%s expected running, state:%s", vm_id,
				health->name, shell_vm_state_to_str(health->state));
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_VCPU_COUNT) != 0UL) {
			shell_item_line("vm%hu:%s vCPU count created:%hu configured:%hu", vm_id,
				health->name, health->created_vcpus, health->configured_vcpus);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_VCPU_STATE) != 0UL) {
			shell_item_line("vm%hu:%s has non-running vCPU", vm_id, health->name);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_WDT_UNKNOWN) != 0UL) {
			shell_item_line("vm%hu:%s watchdog has no live heartbeat", vm_id, health->name);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_WDT_STUCK) != 0UL) {
			shell_item_line("vm%hu:%s watchdog stuck cause:%s age:%lums stalled:0x%lx", vm_id,
				health->name, shell_vmstat_wdt_cause_to_str(health->wdt.cause),
				health->wdt.last_ms, health->wdt.stalled_vcpu_mask);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_WDT_RECOVERY) != 0UL) {
			shell_item_line("vm%hu:%s watchdog recovery:%s pending:%s", vm_id,
				health->name,
				shell_vmstat_wdt_recovery_to_str(health->wdt.recovery_state),
				shell_yes_no(health->wdt.restart_pending));
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_WDT_RESTART_FAIL) != 0UL) {
			shell_item_line("vm%hu:%s watchdog restart failures:%lu", vm_id,
				health->name, health->wdt.restart_fail_count);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_VIRTIO_NOT_READY) != 0UL) {
			shell_item_line("vm%hu:%s virtio ready:%hu/%hu", vm_id, health->name,
				health->virtio_ready, health->virtio_total);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_VIRTIO_LOST) != 0UL) {
			shell_item_line("vm%hu:%s virtio backend lost/stale:%hu", vm_id,
				health->name, health->virtio_lost);
			printed = true;
		}
		if ((health->reasons & SHELL_HEALTH_VM_VIRTIO_TIMEOUT) != 0UL) {
			shell_item_line("vm%hu:%s virtio historical timeouts:%lu", vm_id,
				health->name, health->virtio_timeouts);
			printed = true;
		}
		shell_output_checkpoint();
	}

	if (!printed) {
		shell_item_line("none");
	}
}

int32_t shell_health(int32_t argc, __unused char **argv)
{
	struct shell_health_host host;
	struct shell_health_vm *vms = shell_health_vms;
	enum shell_health_level overall;
	uint16_t vm_id;

	if (argc != 1) {
		shell_puts("usage: health\r\n");
		return -EINVAL;
	}

	shell_health_collect_host(&host);
	overall = host.level;
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		shell_health_collect_vm(vm_id, &vms[vm_id]);
		if (vms[vm_id].present) {
			overall = shell_health_max(overall, vms[vm_id].level);
		}
	}

	shell_item_begin("HEALTH");
	/* Host page values are used/total pools plus ownership anomalies. VM rows
	 * combine lifecycle, vCPU count, watchdog age, console backlog, and virtio
	 * state; Findings retains the derived PASS/WARN/FAIL diagnostic rules.
	 */
	shell_item_line("overall:%s uptime:%lums",
		shell_health_level_to_str(overall), ticks_to_ms(cpu_ticks()));
	shell_item_section("Host");
	shell_item_line("pcpus:active:%hu/%hu current:%hu/%hu result:%s",
		host.pcpu_active, host.pcpu_total, host.pcpu_current, host.pcpu_total,
		shell_health_level_to_str(host.level));
	shell_item_line("pages:HV-s1:%lu/%lu VM-s2:%lu/%lu accounted:%lu unowned:%lu over:%lu malformed:%lu",
		host.hv_s1.used_pages, host.hv_s1.total_pages,
		host.vm_s2.used_pages, host.vm_s2.total_pages,
		host.stage2_accounted, host.stage2_unowned,
		host.stage2_overaccounted, host.stage2_malformed);
	shell_item_section("Virtual machines");
	shell_item_line("vm   name       state     vcpus wdt     age        console              virtio lifecycle");
	shell_item_line("──── ────────── ───────── ───── ─────── ────────── ──────────────────── ────── ───────────");
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (vms[vm_id].present) {
			shell_health_print_vm(&vms[vm_id]);
		}
	}
	shell_health_print_cpu_usage(vms);
	shell_item_section("Findings");
	shell_health_print_findings(&host, vms);
	shell_item_end();

	return 0;
}

static int64_t shell_vmstat_ticks_delta_us(int64_t delta)
{
	if (delta < 0L) {
		uint64_t magnitude = (uint64_t)(-(delta + 1L)) + 1UL;

		return -(int64_t)ticks_to_us(magnitude);
	}

	return (int64_t)ticks_to_us((uint64_t)delta);
}

static void shell_vmstat_collect_timer_summary(const struct acrn_vm *vm,
	struct shell_vmstat_timer_summary *summary)
{
	uint16_t vcpu_id;

	if ((vm == NULL) || (summary == NULL)) {
		return;
	}

	summary->cntv_ppi = 0UL;
	summary->cntv_backup = 0UL;
	summary->cntv_poll = 0UL;
	summary->pre_eret_flush = 0UL;
	summary->pre_eret_flush_expired = 0UL;
	summary->lost_pending_lr = 0UL;
	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		const struct acrn_vcpu *vcpu = vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);

		if (vcpu != NULL) {
			const struct arm64_vcpu_vtimer_diag *diag = &vcpu->arch.vtimer_diag;

			summary->cntv_ppi += diag->cntv_ppi;
			summary->cntv_backup += diag->cntv_backup;
			summary->cntv_poll += diag->cntv_poll;
			summary->pre_eret_flush += diag->pre_eret_flush;
			summary->pre_eret_flush_expired += diag->pre_eret_flush_expired;
			summary->lost_pending_lr += diag->lost_pending_lr;
		}
	}
}

static void shell_vmstat_vm_config(uint16_t vm_id, const struct acrn_vm_config *vm_config,
	const struct acrn_vm *vm)
{
	const struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	struct shell_vmstat_timer_summary timer = { 0U };
	struct vm_wdt_snapshot wdt = { 0U };
	struct console_vm_ring_stats ring = { 0U };
	struct vhost_console_stats vcon = { 0U };
	struct acrn_vuart *vu = NULL;
	struct arm64_vm_mpu_sve_status sve_status = { 0U };
	struct arm64_vits_stats vits = { 0U };
	bool has_vits;
	char temp_str[MAX_STR_SIZE];

	arm64_vm_mpu_get_sve_status(vm, &sve_status);
	has_vits = arm64_vgicv3_get_its_stats((struct acrn_vm *)vm, &vits);
	(void)console_vm_ring_get_stats(vm_id, &ring);
	if (!is_poweroff_vm(vm)) {
		vu = vm_console_vuart((struct acrn_vm *)vm);
	}

	/*
	 * VM-level fields describe configured resources versus runtime state:
	 * vCPU count, affinity, scheduler policy, guest memory, interrupt
	 * topology, console backlog, and boot image placement.
	 */
	/* vcpus compares configuration with runtime objects; GIC/ITS/vITS expose
	 * interrupt state; timer/HWT are status and counters; vcon/virtio are
	 * diagnostic snapshots. Rendering does not modify VM state.
	 */
	shell_item_line("vcpus:configured:%u created:%hu state:%s flags:0x%08lx load:%u",
		shell_cpu_bitmap_weight(vm_config->cpu_affinity), vm->hw.created_vcpus,
		shell_vm_state_to_str(vm->state), vm_config->guest_flags,
		(uint32_t)vm_config->load_order);
	shell_puts("│   affinity:");
	shell_print_vm_affinity(vm_config, vm);
	shell_puts("\r\n");

	shell_item_line("gic:initialized:%s vcpus:%hu rdist:%hu lr-count:%u vmcr:0x%08x ctlr:0x%08x",
		shell_yes_no(vgic->initialized), vgic->vcpu_count,
		vgic->rdist_count, vgic->lr_count, vgic->vmcr, vgic->gicd_ctlr);
	shell_item_line("its:enabled:%s typer:0x%08lx ctlr:0x%08x",
		shell_yes_no(vgic->its_enabled), vgic->its.typer, vgic->its.ctlr);
	if (has_vits) {
		shell_item_line("vITS:q ctlr:%s cbaser:%s writer:0x%016lx reader:0x%016lx cmds:%lu invalid:%lu unsupported:%lu qerr:%lu copy-fail:%lu budget:%lu",
			shell_yes_no(vits.ctlr_enabled), shell_yes_no(vits.cbaser_valid),
			vits.cwriter, vits.creadr, vits.cmd_processed,
			vits.cmd_invalid, vits.cmd_unsupported,
			vits.cmd_queue_errors, vits.cmd_copy_fail,
			vits.cmd_budget_exhausted);
		shell_item_line("vITS:tables dev:%u evt:%u col:%u cfg:%lu/%lu mmio:%lu/%lu trans:%lu inject:%lu/%lu no-event:%lu bad-target:%lu",
			vits.devices, vits.events, vits.collections,
			vits.config_update_ok, vits.config_update_fail,
			vits.mmio_read, vits.mmio_write, vits.translater_write,
			vits.inject_ok, vits.inject_fail,
			vits.inject_no_event, vits.inject_bad_target);
		shell_item_line("vITS:last op:0x%02x dev:%u event:%u lpi:%u col:%hu target:%hu ret:%d",
			vits.last_opcode, vits.last_device, vits.last_event,
			vits.last_lpi, vits.last_collection,
			vits.last_target, vits.last_status);
	}
	shell_item_line("mpu:sve:cfg:%s active:%s host:%s vl:%u host-vl:%u reason:%s",
		shell_yes_no(sve_status.configured),
		shell_yes_no(sve_status.active),
		shell_yes_no(sve_status.host_supported),
		sve_status.vl_bits, sve_status.host_vl_bits,
		arm64_vm_mpu_sve_reason_str(sve_status.reason));
	shell_vmstat_collect_timer_summary(vm, &timer);
	shell_item_line("timer:cntv:Y ppi:%lu backup:%lu poll:%lu pre-eret:%lu/%lu lr-miss:%lu cnthp:Y cntp-emul:Y maintenance:Y time-delta:%ld",
		timer.cntv_ppi, timer.cntv_backup, timer.cntv_poll,
		timer.pre_eret_flush_expired, timer.pre_eret_flush,
		timer.lost_pending_lr, vm->arch_vm.time_delta);
	if (vm_wdt_get_snapshot(vm_id, &wdt) == 0) {
		uint64_t last_sec = wdt.last_ms / 1000UL;
		uint64_t last_msec = wdt.last_ms % 1000UL;
		uint16_t vcpu_id;

		shell_item_line("HWT:status:%7s cause:%12s (%02lu.%03lu) timeout:%02lu restart:%02lu fail:%02lu recovery:%10s wait:0x%02lx token:0x%016lx daemon:%s merge:%lu drop:%lu",
			shell_vmstat_wdt_status_to_str(wdt.status),
			shell_vmstat_wdt_cause_to_str(wdt.cause), last_sec, last_msec,
			wdt.timeout_count, wdt.restart_count, wdt.restart_fail_count,
			shell_vmstat_wdt_recovery_to_str(wdt.recovery_state),
			wdt.recovery_wait_vcpus, wdt.last_token,
			shell_yes_no(wdt.daemon_pending), wdt.daemon_merged,
			wdt.daemon_dropped);
		shell_item_line("HWT:heartbeat mode:%s expected:0x%02lx started:0x%02lx stalled:0x%02lx",
			wdt.per_vcpu_mode ? "per-vcpu" : "legacy",
			wdt.expected_vcpu_mask, wdt.started_vcpu_mask,
			wdt.stalled_vcpu_mask);
		if (wdt.per_vcpu_mode) {
			for (vcpu_id = 0U; vcpu_id < MAX_VCPUS_PER_VM; vcpu_id++) {
				if ((wdt.expected_vcpu_mask & (1UL << vcpu_id)) != 0UL) {
					shell_item_line("        vcpu%hu age:%lums token:0x%016lx%s",
						vcpu_id, wdt.vcpu_age_ms[vcpu_id],
						wdt.vcpu_last_token[vcpu_id],
						(wdt.stalled_vcpu_mask & (1UL << vcpu_id)) != 0UL ?
						" STALLED" : "");
				}
			}
		}
	}
	shell_item_line("vcon:selected:%s bound:%s ramlog-pending:%u drain:%u skipped:%lu",
		shell_yes_no(console_vmid == vm_id), shell_yes_no(ring.vuart_bound),
		ring.queued, ring.drain_budget, ring.dropped_bytes);

	if (vu != NULL) {
		shell_item_line("        vuart:active:%s irq:%u rx:%u tx:%u ier:0x%02x lsr:0x%02x",
			shell_yes_no(vu->active), vu->irq, vuart_rx_numchars(vu),
			vu->txfifo.num, vu->ier, vu->lsr);
	}
	if (vhost_console_get_stats(vm_id, &vcon)) {
		shell_item_line("        virtio-console:active:%s irq:%u status:0x%02x isr:0x%02x tx:%lu rx:%lu",
			shell_yes_no(vcon.active), vcon.irq, vcon.status,
			vcon.interrupt_status, vcon.tx_count, vcon.rx_count);
		shell_item_line("        vcon.stat tx:%luB/s rx:%luB/s notify:%lu/%lu irq:%lu/%lu",
			vcon.tx_byte_rate, vcon.rx_byte_rate,
			vcon.tx_notify_count, vcon.rx_notify_count,
			vcon.tx_irq_count, vcon.rx_irq_count);
		shell_item_line("        vcon.lat tx:%lu/%lu/%luus rx:%lu/%lu/%luus samples:%lu/%lu",
			vcon.tx_latency.min_us, vcon.tx_latency.avg_us,
			vcon.tx_latency.max_us, vcon.rx_latency.min_us,
			vcon.rx_latency.avg_us, vcon.rx_latency.max_us,
			vcon.tx_latency.count, vcon.rx_latency.count);
		for (uint16_t qid = 0U; qid < VHOST_CONSOLE_STAT_QUEUE_NUM; qid++) {
			const struct vhost_console_queue_stats *queue = &vcon.queues[qid];

			shell_item_line("        vcon.q%hu ready:%s num:%hu idx:%hu desc:0x%016lx avail:0x%016lx used:0x%016lx",
				qid, shell_yes_no(queue->ready), queue->num,
				queue->last_avail_idx, queue->desc, queue->avail,
				queue->used);
			shell_output_checkpoint();
		}
	}

	(void)snprintf(temp_str, MAX_STR_SIZE, "boot:kernel:%s entry:0x%016lx load:0x%016lx",
		vm_config->os_config.name, vm_config->os_config.kernel_entry_addr,
		vm_config->os_config.kernel_load_addr);
	shell_item_line("%s", temp_str);
}

static void shell_vmstat_append_flag(char *flags, size_t flags_len, const char *flag)
{
	size_t len = strnlen_s(flags, flags_len);

	if (len != 0U) {
		(void)strncat_s(flags, flags_len, ",", 1U);
	}
	(void)strncat_s(flags, flags_len, flag, strnlen_s(flag, flags_len));
}

static void shell_vmstat_vcpu_diag(const struct acrn_vcpu *vcpu,
	const struct thread_object *current, const struct sched_latency_stats *latency,
	const struct sched_rtds_stats *rtds, bool has_rtds,
	const struct sched_cbs_stats *cbs, bool has_cbs,
	char *flags, size_t flags_len)
{
	uint64_t now = cpu_ticks();
	bool cpu_wait = false;

	flags[0] = '\0';

	/*
	 * These flags are quick "why might this VM look stuck?" hints from the
	 * current vmstat snapshot. They do not prove a permanent hang by themselves;
	 * they point to the next subsystem to inspect.
	 *
	 * cpu-wait:
	 *   The vCPU is runnable but is not the current thread on its pCPU for at
	 *   least two RTDS periods. A runnable thread wants CPU time; a long wait
	 *   usually means another vCPU/thread is occupying that pCPU or the shared
	 *   core is overloaded.
	 *
	 * rtds-depleted / cbs-depleted:
	 *   The vCPU runs under a budget scheduler, is not currently executing, and
	 *   its current budget is already zero. A non-current vCPU must wait for
	 *   replenishment unless spare CPU time is available through work-conserving
	 *   slack. A current vCPU with zero budget is probably using that slack, so
	 *   it is not flagged here.
	 *
	 * rtds-overrun / cbs-overrun:
	 *   The vCPU is already in cpu-wait and the replenishment/deadline boundary
	 *   is also due. This combines "wanted CPU for a while" with "the current
	 *   budget window should have rolled", which is a stronger signal that the
	 *   shared core is behind schedule.
	 */
	if ((vcpu->thread_obj.status == THREAD_STS_RUNNABLE) &&
		(current != &vcpu->thread_obj) &&
		(latency->runnable_since != 0UL) &&
		(ticks_to_us(now - latency->runnable_since) >= VMSTAT_CPU_WAIT_WARN_US)) {
		cpu_wait = true;
		shell_vmstat_append_flag(flags, flags_len, "cpu-wait");
	}
	if (has_rtds && (current != &vcpu->thread_obj) &&
		(vcpu->thread_obj.status == THREAD_STS_RUNNABLE) &&
		(rtds->remaining_ticks == 0UL)) {
		shell_vmstat_append_flag(flags, flags_len, "rtds-depleted");
	}
	if (has_rtds && cpu_wait && (rtds->deadline_ticks <= now)) {
		shell_vmstat_append_flag(flags, flags_len, "rtds-overrun");
	}
	if (has_cbs && (current != &vcpu->thread_obj) &&
		(vcpu->thread_obj.status == THREAD_STS_RUNNABLE) &&
		(cbs->remaining_ticks == 0UL)) {
		shell_vmstat_append_flag(flags, flags_len, "cbs-depleted");
	}
	if (has_cbs && cpu_wait && (cbs->deadline_ticks <= now)) {
		shell_vmstat_append_flag(flags, flags_len, "cbs-overrun");
	}
	if (flags[0] == '\0') {
		(void)snprintf(flags, flags_len, "ok");
	}
}

static const char *shell_vmstat_scheduler_label(bool has_bvt, bool has_rtds,
	bool has_cbs)
{
	if (has_rtds) {
		return "rtds";
	}
	if (has_cbs) {
		return "cbs";
	}
	if (has_bvt) {
		return "bvt";
	}

	return "-";
}

static void shell_vmstat_vcpu_timer(const struct acrn_vcpu *vcpu)
{
	const struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	const struct arm64_vcpu_vtimer_diag *diag = &vcpu->arch.vtimer_diag;
	const struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	const struct arm64_vgic_irq *timer_irq = NULL;
	uint64_t now = cpu_ticks() - gctx->cntvoff_el2;
	int64_t delta = (int64_t)(gctx->cntv_cval_el0 - now);
	int64_t delta_us = shell_vmstat_ticks_delta_us(delta);
	bool expired = ((gctx->cntv_ctl_el0 & CNTV_CTL_ENABLE) != 0U) &&
		((gctx->cntv_ctl_el0 & CNTV_CTL_IMASK) == 0U) && (delta <= 0L);
	bool bitmap = false;
	bool deliverable = false;

	if (vgic->initialized && (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
		uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		uint32_t word = virq / 32U;
		uint32_t bit = 1U << (virq % 32U);

		timer_irq = &vgic->irq[vcpu->vcpu_id][virq];
		bitmap = ((vgic->pending_bitmap[vcpu->vcpu_id][word] & bit) != 0U);
		deliverable = ((vgic->gicd_ctlr & (1U << 1U)) != 0U) &&
			((vcpu->arch.vgic.vmcr & ICH_VMCR_VENG1) != 0UL) &&
			timer_irq->enabled;
	}

	if (timer_irq != NULL) {
		shell_item_line("      timer:PPI%u ctl:0x%08x cval:0x%016lx delta.us:%ld exp:%s mask:%s ppi:%lu backup:%lu poll:%lu pre-eret:%lu/%lu",
			ARM64_GIC_PPI_VIRTUAL_TIMER, gctx->cntv_ctl_el0,
			gctx->cntv_cval_el0, delta_us, shell_yes_no(expired),
			shell_yes_no(gctx->cntv_el2_masked), diag->cntv_ppi,
			diag->cntv_backup, diag->cntv_poll,
			diag->pre_eret_flush_expired, diag->pre_eret_flush);
		shell_item_line("      vgic:PPI%u en:%s pend:%s act:%s level:%s bitmap:%s deliverable:%s lr:%u hcr:0x%016lx vmcr:0x%016lx",
			ARM64_GIC_PPI_VIRTUAL_TIMER,
			shell_yes_no(timer_irq->enabled),
			shell_yes_no(timer_irq->pending),
			shell_yes_no(timer_irq->active),
			shell_yes_no(timer_irq->level),
			shell_yes_no(bitmap), shell_yes_no(deliverable),
			vcpu->arch.vgic.used_lrs, vcpu->arch.vgic.hcr,
			vcpu->arch.vgic.vmcr);
	} else {
		shell_item_line("      timer:PPI%u ctl:0x%08x cval:0x%016lx delta.us:%ld exp:%s mask:%s ppi:%lu backup:%lu poll:%lu pre-eret:%lu/%lu",
			ARM64_GIC_PPI_VIRTUAL_TIMER, gctx->cntv_ctl_el0,
			gctx->cntv_cval_el0, delta_us, shell_yes_no(expired),
			shell_yes_no(gctx->cntv_el2_masked), diag->cntv_ppi,
			diag->cntv_backup, diag->cntv_poll,
			diag->pre_eret_flush_expired, diag->pre_eret_flush);
		shell_item_line("      vgic:PPI%u desc:none lr:%u hcr:0x%016lx vmcr:0x%016lx",
			ARM64_GIC_PPI_VIRTUAL_TIMER, vcpu->arch.vgic.used_lrs,
			vcpu->arch.vgic.hcr, vcpu->arch.vgic.vmcr);
	}
}

static void shell_vmstat_vcpus(const struct acrn_vm *vm)
{
	uint16_t vcpu_id;

	if (vm->hw.created_vcpus == 0U) {
		shell_item_line("vcpu:none");
		return;
	}

	shell_item_section("vcpu state");
	/*
	 * The compact vCPU table is the first pass for "is this vCPU runnable,
	 * currently selected, or waiting with pending work?" questions.
	 */
	shell_item_line("vcpu       pcpu  sched  vcpu     thread    cur  req-mask            diag");
	shell_item_line("─────────  ────  ─────  ───────  ────────  ───  ──────────────────  ────────────");
	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		struct acrn_vcpu *vcpu = vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);
		struct sched_latency_stats latency = { 0U };
		struct sched_bvt_stats bvt = { 0U };
		struct sched_rtds_stats rtds = { 0U };
		struct sched_cbs_stats cbs = { 0U };
		struct thread_object *current;
		char diag[96U];
		bool has_bvt;
		bool has_rtds;
		bool has_cbs;
		const char *sched_label;

		current = sched_get_current(vcpu->thread_obj.pcpu_id);
		sched_get_latency(&vcpu->thread_obj, &latency);
		has_bvt = sched_get_bvt_stats(&vcpu->thread_obj, &bvt);
		has_rtds = sched_get_rtds_stats(&vcpu->thread_obj, &rtds);
		has_cbs = sched_get_cbs_stats(&vcpu->thread_obj, &cbs);
		sched_label = shell_vmstat_scheduler_label(has_bvt, has_rtds, has_cbs);
		shell_vmstat_vcpu_diag(vcpu, current, &latency, &rtds, has_rtds,
			&cbs, has_cbs,
			diag, sizeof(diag));
		shell_item_line("%-9s  %-4hu  %-5s  %-7s  %-8s  %-3s  0x%016lx  %s",
			vcpu->thread_obj.name, vcpu->thread_obj.pcpu_id,
			sched_label,
			vcpu_state_to_str(vcpu_get_state(vcpu)),
		shell_thread_state_to_str(vcpu->thread_obj.status),
			shell_yes_no(current == &vcpu->thread_obj),
			vcpu->pending_req, diag);
		if (has_bvt) {
			/* BVT fields expose weight and virtual-time scheduling order. */
			shell_item_line("      bvt:weight:%u avt:%ld evt:%ld", bvt.weight,
				bvt.avt, bvt.evt);
		}
		if (has_rtds) {
			uint64_t now = cpu_ticks();

			/* RTDS fields expose period budget and time to deadline. */
			shell_item_line("      rtds:period-us:%lu budget-us:%lu remain-us:%lu deadline-in-us:%lu",
				ticks_to_us(rtds.period_ticks), ticks_to_us(rtds.budget_ticks),
				ticks_to_us(rtds.remaining_ticks),
				(rtds.deadline_ticks > now) ?
					ticks_to_us(rtds.deadline_ticks - now) : 0UL);
		}
		if (has_cbs) {
			uint64_t now = cpu_ticks();

			/* CBS fields expose the reservation server window and budget. */
			shell_item_line("      %s:period-us:%lu budget-us:%lu remain-us:%lu "
				"deadline-in-us:%lu dep:%lu repl:%lu wake:%lu late:%lu",
				sched_label,
				ticks_to_us(cbs.period_ticks), ticks_to_us(cbs.budget_ticks),
				ticks_to_us(cbs.remaining_ticks),
				(cbs.deadline_ticks > now) ?
					ticks_to_us(cbs.deadline_ticks - now) : 0UL,
				cbs.depleted_count, cbs.replenish_count,
				cbs.wake_replenish_count, cbs.late_account_count);
		}
		shell_vmstat_vcpu_timer(vcpu);
		shell_output_checkpoint();
	}
}

int32_t shell_vmstat(int32_t argc, __unused char **argv)
{
	uint16_t vm_id;

	if (argc != 1) {
		return -EINVAL;
	}

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (!shell_vm_config_present(vm_config) &&
			(vm->hw.created_vcpus == 0U) && is_poweroff_vm(vm)) {
			continue;
		}

		shell_item_begin("vmstat vm%hu:%s", vm_id,
			(vm->name[0] != '\0') ? vm->name : vm_config->name);
		shell_vmstat_vm_config(vm_id, vm_config, vm);
		shell_vmstat_vcpus(vm);
		shell_item_end();
		shell_output_checkpoint();
	}

	return 0;
}
