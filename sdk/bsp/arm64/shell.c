/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <guest_memory.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <sprintf.h>
#include <util.h>
#include <reloc.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <vm_wdt.h>
#include <hv_pm.h>
#include <schedule.h>
#include <spinlock.h>
#include <ticks.h>
#include <timer.h>
#include <trace.h>
#include <console.h>
#include <debug/shell.h>
#include <acrn_hv_defs.h>
#include <virtio_console.h>
#include <virtio_proxy.h>
#include <bsp/cpufreq.h>
#include <bsp/pm.h>
#include <bsp/pci.h>
#include <bsp/vpci.h>
#include <bsp/vuart.h>
#include <debug/symbol.h>
#include <asm/mmu.h>
#include <asm/coredump.h>
#include <asm/irq.h>
#include <asm/cache.h>
#include <asm/platform.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vcpu.h>
#include <asm/guest/vmpu.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vipc.h>
#include <asm/guest/vsmmu.h>
#include <asm/sysreg.h>
#include <asm/vtd.h>
#include "../shell_priv.h"

#define SHELL_CMD_MEM_MAP		"devmap"
#define SHELL_CMD_MEM_MAP_PARAM		NULL
#define SHELL_CMD_MEM_MAP_HELP		"list arm64 host stage-1 and vm stage-2 memory mappings"
#define SHELL_CMD_MEM_STAT		"memstat"
#define SHELL_CMD_MEM_STAT_PARAM	NULL
#define SHELL_CMD_MEM_STAT_HELP		"list ARM64 page-table pool and stage-2 ownership statistics"
#define SHELL_CMD_HEALTH		"health"
#define SHELL_CMD_HEALTH_PARAM		NULL
#define SHELL_CMD_HEALTH_HELP		"summarize current host and VM operational health"
#define SHELL_CMD_DUMPSTAT		"dumpstat"
#define SHELL_CMD_DUMPSTAT_PARAM	"[vm id]"
#define SHELL_CMD_DUMPSTAT_HELP		"dump arm64 vcpu stats and vgic/vtimer diagnostics"
#define SHELL_CMD_COREDUMP		"coredump"
#define SHELL_CMD_COREDUMP_PARAM	"<print|erase>"
#define SHELL_CMD_COREDUMP_HELP		"print or erase the latest ARM64 coredump"
#define SHELL_CMD_VMSTAT		"vmstat"
#define SHELL_CMD_VMSTAT_PARAM		NULL
#define SHELL_CMD_VMSTAT_HELP		"list arm64 vm state"
#define SHELL_CMD_CACHESTAT		"cachestat"
#define SHELL_CMD_CACHESTAT_PARAM	NULL
#define SHELL_CMD_CACHESTAT_HELP	"list arm64 host cache and LLC domains"
#define SHELL_CMD_IPCSTAT		"ipcstat"
#define SHELL_CMD_IPCSTAT_PARAM		NULL
#define SHELL_CMD_IPCSTAT_HELP		"list static VM IPC channels"
#define SHELL_CMD_VIRTIOSTAT		"virtiostat"
#define SHELL_CMD_VIRTIOSTAT_PARAM	NULL
#define SHELL_CMD_VIRTIOSTAT_HELP	"list active virtio-proxy devices"
#define SHELL_CMD_SMMUSTAT		"smmustat"
#define SHELL_CMD_SMMUSTAT_PARAM	NULL
#define SHELL_CMD_SMMUSTAT_HELP		"list ARM SMMUv3 and ITS passthrough state"
#define SHELL_CMD_VSMMUSTAT		"vsmmustat"
#define SHELL_CMD_VSMMUSTAT_PARAM	"[vm id]"
#define SHELL_CMD_VSMMUSTAT_HELP	"list guest-visible synthetic SMMUv3 state"
#define SHELL_CMD_PCISTAT		"pcistat"
#define SHELL_CMD_PCISTAT_PARAM		NULL
#define SHELL_CMD_PCISTAT_HELP		"list PCI passthrough and SMMU stream state"
#define SHELL_CMD_CPUFREQ		"cpufreq"
#define SHELL_CMD_CPUFREQ_PARAM		NULL
#define SHELL_CMD_CPUFREQ_HELP		"list host CPU frequency policy state"
#define SHELL_CMD_RTTEST		"rttest"
#define SHELL_CMD_RTTEST_PARAM		NULL
#define SHELL_CMD_RTTEST_HELP		"run local EL2 timer latency tests on every pCPU"
#define SHELL_CMD_TRACE			"trace"
#define SHELL_CMD_TRACE_PARAM		"<status|start|stop|clear|dump> [category|count]"
#define SHELL_CMD_TRACE_HELP		"capture and dump per-pCPU EL2 trace events"
#define SHELL_CMD_REBOOT		"reboot"
#define SHELL_CMD_REBOOT_PARAM		NULL
#define SHELL_CMD_REBOOT_HELP		"trigger a system reboot (immediately)"
#define SHELL_CMD_PM			"pm"
#define SHELL_CMD_PM_PARAM		"<suspend|resume|reboot> <vmid> | status"
#define SHELL_CMD_PM_HELP		"control VM power state and inspect PM state"
#define SHELL_CMD_PMSTAT		"pmstat"
#define SHELL_CMD_PMSTAT_PARAM		NULL
#define SHELL_CMD_PMSTAT_HELP		"list coordinated guest suspend transaction statistics"
#define DUMPSTAT_SMP_CALL_TIMEOUT_US	1000U
#define DUMPSTAT_STACK_DEPTH		16U
#define DUMPSTAT_REG_KEY_FMT		"%5s:0x%016lx"
#define DUMPSTAT_REGS_PER_LINE_MAX	4U
#define VMSTAT_CPU_WAIT_WARN_US		20000UL
#define PCISTAT_MAX_STREAMS		64U
#define RTTEST_INTERVAL_US		1000U
#define RTTEST_SAMPLE_COUNT		1000U
#define RTTEST_CBS_PERIOD_US		10000U
#define RTTEST_CBS_BUDGET_US		500U
#define RTTEST_LINE_SIZE		112U
#define RTTEST_OUTPUT_SIZE		((MAX_PCPU_NUM * RTTEST_LINE_SIZE) + 1U)
#define TRACE_DUMP_DEFAULT_COUNT	64U
#define SHELL_MEM_KB_BYTES		1024UL
#define SHELL_MEM_MB_BYTES		(SHELL_MEM_KB_BYTES * 1024UL)
#define SHELL_MEM_GB_BYTES		(SHELL_MEM_MB_BYTES * 1024UL)

static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv);
static int32_t shell_memstat(int32_t argc, __unused char **argv);
static int32_t shell_health(int32_t argc, __unused char **argv);
static int32_t shell_dumpstat(int32_t argc, char **argv);
static int32_t shell_coredump(int32_t argc, char **argv);
static int32_t shell_vmstat(int32_t argc, __unused char **argv);
static int32_t shell_cachestat(int32_t argc, __unused char **argv);
static int32_t shell_ipcstat(int32_t argc, __unused char **argv);
static int32_t shell_virtiostat(int32_t argc, char **argv);
static int32_t shell_smmustat(int32_t argc, __unused char **argv);
static int32_t shell_vsmmustat(int32_t argc, char **argv);
static int32_t shell_pcistat(int32_t argc, char **argv);
static int32_t shell_cpufreq(int32_t argc, __unused char **argv);
static int32_t shell_rttest(int32_t argc, __unused char **argv);
static int32_t shell_trace(int32_t argc, char **argv);
static int32_t shell_reboot(__unused int32_t argc, __unused char **argv);
static int32_t shell_pm(int32_t argc, char **argv);
static int32_t shell_pmstat(int32_t argc, __unused char **argv);
static const char *shell_yes_no(bool value);
static const char *shell_vm_state_to_str(enum vm_state state);
static struct arm_smmu_stream_config shell_smmu_streams[PCISTAT_MAX_STREAMS];

struct shell_cmd arch_shell_cmds[] = {
	{
		.str		= SHELL_CMD_MEM_MAP,
		.cmd_param	= SHELL_CMD_MEM_MAP_PARAM,
		.help_str	= SHELL_CMD_MEM_MAP_HELP,
		.fcn		= shell_list_mem,
	},
	{
		.str		= SHELL_CMD_MEM_STAT,
		.cmd_param	= SHELL_CMD_MEM_STAT_PARAM,
		.help_str	= SHELL_CMD_MEM_STAT_HELP,
		.fcn		= shell_memstat,
	},
	{
		.str		= SHELL_CMD_HEALTH,
		.cmd_param	= SHELL_CMD_HEALTH_PARAM,
		.help_str	= SHELL_CMD_HEALTH_HELP,
		.fcn		= shell_health,
	},
	{
		.str		= SHELL_CMD_DUMPSTAT,
		.cmd_param	= SHELL_CMD_DUMPSTAT_PARAM,
		.help_str	= SHELL_CMD_DUMPSTAT_HELP,
		.fcn		= shell_dumpstat,
	},
	{
		.str		= SHELL_CMD_COREDUMP,
		.cmd_param	= SHELL_CMD_COREDUMP_PARAM,
		.help_str	= SHELL_CMD_COREDUMP_HELP,
		.fcn		= shell_coredump,
	},
	{
		.str		= SHELL_CMD_VMSTAT,
		.cmd_param	= SHELL_CMD_VMSTAT_PARAM,
		.help_str	= SHELL_CMD_VMSTAT_HELP,
		.fcn		= shell_vmstat,
	},
	{
		.str		= SHELL_CMD_CACHESTAT,
		.cmd_param	= SHELL_CMD_CACHESTAT_PARAM,
		.help_str	= SHELL_CMD_CACHESTAT_HELP,
		.fcn		= shell_cachestat,
	},
	{
		.str		= SHELL_CMD_IPCSTAT,
		.cmd_param	= SHELL_CMD_IPCSTAT_PARAM,
		.help_str	= SHELL_CMD_IPCSTAT_HELP,
		.fcn		= shell_ipcstat,
	},
	{
		.str		= SHELL_CMD_VIRTIOSTAT,
		.cmd_param	= SHELL_CMD_VIRTIOSTAT_PARAM,
		.help_str	= SHELL_CMD_VIRTIOSTAT_HELP,
		.fcn		= shell_virtiostat,
	},
	{
		.str		= SHELL_CMD_SMMUSTAT,
		.cmd_param	= SHELL_CMD_SMMUSTAT_PARAM,
		.help_str	= SHELL_CMD_SMMUSTAT_HELP,
		.fcn		= shell_smmustat,
	},
	{
		.str		= SHELL_CMD_VSMMUSTAT,
		.cmd_param	= SHELL_CMD_VSMMUSTAT_PARAM,
		.help_str	= SHELL_CMD_VSMMUSTAT_HELP,
		.fcn		= shell_vsmmustat,
	},
	{
		.str		= SHELL_CMD_PCISTAT,
		.cmd_param	= SHELL_CMD_PCISTAT_PARAM,
		.help_str	= SHELL_CMD_PCISTAT_HELP,
		.fcn		= shell_pcistat,
	},
	{
		.str		= SHELL_CMD_CPUFREQ,
		.cmd_param	= SHELL_CMD_CPUFREQ_PARAM,
		.help_str	= SHELL_CMD_CPUFREQ_HELP,
		.fcn		= shell_cpufreq,
	},
	{
		.str		= SHELL_CMD_RTTEST,
		.cmd_param	= SHELL_CMD_RTTEST_PARAM,
		.help_str	= SHELL_CMD_RTTEST_HELP,
		.fcn		= shell_rttest,
	},
	{
		.str		= SHELL_CMD_TRACE,
		.cmd_param	= SHELL_CMD_TRACE_PARAM,
		.help_str	= SHELL_CMD_TRACE_HELP,
		.fcn		= shell_trace,
	},
	{
		.str		= SHELL_CMD_REBOOT,
		.cmd_param	= SHELL_CMD_REBOOT_PARAM,
		.help_str	= SHELL_CMD_REBOOT_HELP,
		.fcn		= shell_reboot,
	},
	{
		.str		= SHELL_CMD_PM,
		.cmd_param	= SHELL_CMD_PM_PARAM,
		.help_str	= SHELL_CMD_PM_HELP,
		.fcn		= shell_pm,
	},
	{
		.str		= SHELL_CMD_PMSTAT,
		.cmd_param	= SHELL_CMD_PMSTAT_PARAM,
		.help_str	= SHELL_CMD_PMSTAT_HELP,
		.fcn		= shell_pmstat,
	},
};
uint32_t arch_shell_cmds_sz = ARRAY_SIZE(arch_shell_cmds);

static void shell_print_mem_header(void)
{
	shell_puts("domain      type       attr                    address range (size)\r\n");
	shell_puts("──────────  ─────────  ──────────────────────  ─────────────────────────────────────────────────────\r\n");
}

static uint64_t shell_mem_range_end(uint64_t start, uint64_t size)
{
	return (size == 0UL) ? start : (start + size - 1UL);
}

static void shell_print_mem_map(const char *domain, const char *type,
	const char *attr, uint64_t addr, uint64_t size)
{
	char temp_str[MAX_STR_SIZE];
	uint64_t unit_bytes;
	uint64_t size_whole;
	uint64_t size_fraction;
	const char *unit;

	if (size >= SHELL_MEM_GB_BYTES) {
		unit_bytes = SHELL_MEM_GB_BYTES;
		unit = "GB";
	} else if (size >= SHELL_MEM_MB_BYTES) {
		unit_bytes = SHELL_MEM_MB_BYTES;
		unit = "MB";
	} else {
		unit_bytes = SHELL_MEM_KB_BYTES;
		unit = "KB";
	}
	size_whole = size / unit_bytes;
	size_fraction = ((size % unit_bytes) * 1000UL) / unit_bytes;

	snprintf(temp_str, MAX_STR_SIZE,
		"%-10s  %-9s  %-22s  [0x%016lx,0x%016lx] (%04lu.%03lu %s)\r\n",
		domain, type, attr, addr, shell_mem_range_end(addr, size),
		size_whole, size_fraction, unit);
	shell_puts(temp_str);
}

static const char *shell_memory_type_name(enum arm64_memory_type type)
{
	const char *name;

	switch (type) {
	case ARM64_MEMORY_NORMAL:
		name = "Normal Memory";
		break;
	case ARM64_MEMORY_DEVICE_GRE:
		name = "Device-GRE";
		break;
	case ARM64_MEMORY_DEVICE_nGRE:
		name = "Device-nGRE";
		break;
	case ARM64_MEMORY_DEVICE_nGnRE:
		name = "Device-nGnRE";
		break;
	case ARM64_MEMORY_DEVICE_nGnRnE:
		name = "Device-nGnRnE";
		break;
	case ARM64_MEMORY_UNMAPPED:
		name = "Unmapped";
		break;
	case ARM64_MEMORY_UNKNOWN:
	default:
		name = "Unknown";
		break;
	}

	return name;
}

static void shell_format_memory_attr(char *buf, size_t size,
	const struct arm64_memory_attr *attr, bool stage2)
{
	const char *name;
	uint8_t encoding;

	if ((buf == NULL) || (size == 0U) || (attr == NULL)) {
		return;
	}
	if (attr->type == ARM64_MEMORY_UNMAPPED) {
		(void)snprintf(buf, size, "Unmapped (%s)",
			stage2 ? "IPA->HPA" : "VA->HPA");
		return;
	}

	name = shell_memory_type_name(attr->type);
	encoding = attr->encoding & 0x0fU;
	(void)snprintf(buf, size, "%-13s [0b%u%u%u%u]", name,
		(encoding >> 3U) & 1U, (encoding >> 2U) & 1U,
		(encoding >> 1U) & 1U, encoding & 1U);
}

static void shell_print_host_mem_map(const char *type, uint64_t addr,
	uint64_t size)
{
	struct arm64_memory_attr attr = { 0U };
	char attr_text[32U];

	if (!arm64_get_hv_s1_memory_attr(addr, &attr)) {
		attr.type = ARM64_MEMORY_UNKNOWN;
	}
	(void)memset(attr_text, 0U, sizeof(attr_text));
	shell_format_memory_attr(attr_text, sizeof(attr_text), &attr, false);
	shell_print_mem_map("host s1", type, attr_text, addr, size);
}

static void shell_print_stage2_mem_map(struct acrn_vm *vm, const char *domain,
	const char *type, uint64_t addr, uint64_t size)
{
	struct arm64_memory_attr attr = { 0U };
	char attr_text[32U];

	if (!arm64_get_stage2_memory_attr(vm, addr, &attr)) {
		attr.type = ARM64_MEMORY_UNKNOWN;
	}
	(void)memset(attr_text, 0U, sizeof(attr_text));
	shell_format_memory_attr(attr_text, sizeof(attr_text), &attr, true);
	shell_print_mem_map(domain, type, attr_text, addr, size);
}

static void shell_print_host_maps(void)
{
	const struct arm64_mem_region *mmio_regions;
	const struct mem_region *rsvd_regions;
	uint32_t mmio_count;
	uint32_t rsvd_count;
	uint32_t idx;
	uint64_t hv_base = get_hv_image_base();

	mmio_regions = arm64_get_platform_mmio_regions(&mmio_count);
	for (idx = 0U; idx < mmio_count; idx++) {
		shell_print_host_mem_map("MMIO",
			mmio_regions[idx].base, mmio_regions[idx].size);
	}

	shell_print_host_mem_map("RAM",
		arm64_get_phys_mem_start(), arm64_get_phys_mem_size());
	shell_print_host_mem_map("HV", hv_base, get_hv_image_size());

	rsvd_regions = arm64_get_reserved_mem_regions(&rsvd_count);
	for (idx = 0U; idx < rsvd_count; idx++) {
		shell_print_host_mem_map("RSVD",
			rsvd_regions[idx].addr, rsvd_regions[idx].size);
	}
}

static void shell_print_vm_stage2_maps(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char domain[16];

	snprintf(domain, sizeof(domain), "vm-%u s2", vm->vm_id);
	shell_print_stage2_mem_map(vm, domain, "RAM",
		arch_config->guest_ram_start,
		arch_config->guest_ram_size);
	shell_print_stage2_mem_map(vm, domain, "vGICD",
		arch_config->guest_gicd_base,
		arch_config->guest_gicd_size);
	shell_print_stage2_mem_map(vm, domain, "vGICR",
		arch_config->guest_gicr_base,
		arch_config->guest_gicr_size);
	shell_print_stage2_mem_map(vm, domain, "vPL011",
		arch_config->guest_uart_base,
		arch_config->guest_uart_size);
}

/* [20260717] devmap memory attributes:
 *
 *   host VA -> stage-1 leaf -> AttrIdx -> MAIR_EL2 -> type + low 4 bits
 *   guest IPA -> locked stage-2 leaf -> MemAttr[3:0] -> type + 4 bits
 *
 * The command prints the EL2 stage-1 host view first, then each VM's stage-2
 * view. VIO rows intentionally have no stage-2 leaf and report Unmapped.
 *
 * Key rule:
 *   - names and encodings come from live descriptors instead of range labels;
 *   - Stage-2 reads serialize with dynamic map/unmap in the architecture layer;
 *   - unmapped VIO is distinguished from unknown or reserved attributes.
 */
static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv)
{
	uint16_t vm_id;

	shell_puts("\r\narm64 memory mappings:\r\n");
	shell_print_mem_header();
	shell_print_host_maps();

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (vm->root_stg2ptp != NULL) {
			shell_print_vm_stage2_maps(vm);
		}
	}

	return 0;
}

static void shell_memstat_format_usage(char *buf, size_t size,
	const struct page_pool_stats *stats)
{
	uint64_t permille = 0UL;

	if ((stats != NULL) && (stats->total_pages != 0UL)) {
		permille = (stats->used_pages * 1000UL) / stats->total_pages;
	}
	(void)snprintf(buf, size, "%lu.%01lu%%", permille / 10UL, permille % 10UL);
}

static void shell_memstat_print_pool(const char *name,
	const struct page_pool_stats *stats)
{
	char usage[16U];

	shell_memstat_format_usage(usage, sizeof(usage), stats);
	shell_item_line("%-6s  %-5lu  %-5lu  %-5lu  %s",
		name, stats->total_pages, stats->used_pages, stats->free_pages, usage);
}

static bool shell_memstat_print_vm(uint16_t vm_id, uint64_t *accounted,
	uint64_t *malformed)
{
	struct acrn_vm *vm = get_vm_from_vmid(vm_id);
	struct arm64_stage2_vm_stats stats;

	if (!arm64_get_stage2_vm_stats(vm, &stats)) {
		return false;
	}

	shell_item_line("vm%-2hu  %-9s  0x%016lx  %-3lu  %-3lu  %-3lu  %-3lu  %-5lu  %-9lu",
		vm_id, shell_vm_state_to_str(vm->state), stats.root_address,
		stats.level3_pages, stats.level2_pages, stats.level1_pages,
		stats.level0_pages, stats.total_pages, stats.malformed_entries);
	*accounted += stats.total_pages;
	*malformed += stats.malformed_entries;
	return true;
}

/*
 * memstat reports fixed page-table memory, not a general heap. hv-s1 is the
 * EL2 translation-table pool; vm-s2 is shared by all VM stage-2 roots. The
 * ownership walk counts only table pages reachable from each VM root, making
 * the all-VM difference useful for spotting orphaned allocations.
 */
static int32_t shell_memstat(int32_t argc, __unused char **argv)
{
	struct page_pool_stats hv_s1 = { 0U };
	struct page_pool_stats vm_s2 = { 0U };
	uint64_t accounted = 0UL;
	uint64_t malformed = 0UL;
	uint64_t unowned;
	uint64_t overaccounted;
	bool found = false;

	if (argc != 1) {
		shell_puts("usage: memstat\r\n");
		return -EINVAL;
	}

	arm64_get_hv_s1_page_pool_stats(&hv_s1);
	arm64_get_stage2_page_pool_stats(&vm_s2);

	shell_item_begin("memstat");
	shell_item_line("page-size:%uB hv-image:0x%016lx+0x%016lx",
		PAGE_SIZE, get_hv_image_base(), get_hv_image_size());
	shell_item_section("Page-table pools");
	shell_item_line("pool    total  used   free   usage");
	shell_item_line("──────  ─────  ─────  ─────  ──────");
	shell_memstat_print_pool("hv-s1", &hv_s1);
	shell_memstat_print_pool("vm-s2", &vm_s2);

	shell_item_section("Stage-2 ownership");
	shell_item_line("vm    state      root                L3   L2   L1   L0   total  malformed");
	shell_item_line("────  ─────────  ──────────────────  ───  ───  ───  ───  ─────  ─────────");
	for (uint16_t vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		found |= shell_memstat_print_vm(vm_id, &accounted, &malformed);
	}

	if (!found) {
		shell_item_line("no stage-2 roots");
		shell_item_end();
		return 0;
	}

	unowned = (vm_s2.used_pages > accounted) ?
		(vm_s2.used_pages - accounted) : 0UL;
	overaccounted = (accounted > vm_s2.used_pages) ?
		(accounted - vm_s2.used_pages) : 0UL;

	shell_item_line("accounted:%lu unowned:%lu overaccounted:%lu malformed:%lu",
		accounted, unowned, overaccounted, malformed);
	shell_item_end();

	return 0;
}

static int32_t shell_cpufreq(__unused int32_t argc, __unused char **argv)
{
	cpufreq_dump();
	return 0;
}

struct rttest_cpu_context {
	struct hv_timer timer;
	struct thread_object thread;
	uint8_t stack[CONFIG_STACK_SIZE] __aligned(16);
	uint16_t pcpu_id;
	volatile bool pending;
	volatile bool wait_ready;
	volatile bool complete;
	volatile bool failed;
	uint32_t count;
	uint64_t min_ticks;
	uint64_t act_ticks;
	uint64_t sum_ticks;
	uint64_t max_ticks;
};

struct rttest_run_context {
	spinlock_t lock;
	bool initialized;
	bool running;
	uint16_t pcpu_num;
	uint32_t completed;
};

static struct rttest_cpu_context rttest_cpus[MAX_PCPU_NUM];
static struct rttest_run_context rttest_run = { .lock = { .head = 0U, .tail = 0U } };
static char rttest_output[RTTEST_OUTPUT_SIZE];

/*
 * rttest measures each pCPU's local EL2 CNTHP deadline to SOFTIRQ_TIMER
 * callback path while that CPU retains its configured partition scheduler.
 * It does not migrate vCPUs, change BVT/CBS policy, or measure a Linux
 * user-thread wakeup. The rt-tests-shaped summary fields are:
 * T = test index, (...) = pCPU, P = EL2 priority placeholder, I = interval in
 * microseconds, C = completed samples, and Min/Act/Avg/Max = minimum, last,
 * mean, and worst positive lateness in microseconds.
 *
 * C must reach RTTEST_SAMPLE_COUNT for a valid result. Lower Avg and Max
 * are better; a run passes only when Max is within the platform workload's
 * latency budget. The budget is product-specific and is not hard-coded here.
 */

static void rttest_append_result(const struct rttest_cpu_context *ctx, size_t *offset)
{
	uint64_t min_ticks = ctx->min_ticks == UINT64_MAX ? 0UL : ctx->min_ticks;
	uint64_t avg_ticks = ctx->count == 0U ? 0UL : ctx->sum_ticks / ctx->count;
	size_t remaining = RTTEST_OUTPUT_SIZE - *offset;

	if (remaining > 1U) {
		(void)snprintf(&rttest_output[*offset], remaining,
			"T:%2hu (%5hu) P:%2u I:%4u C:%7u Min:%7lu Act:%7lu Avg:%7lu Max:%7lu\r\n",
			ctx->pcpu_id, ctx->pcpu_id, 0U, RTTEST_INTERVAL_US, ctx->count,
			ticks_to_us(min_ticks), ticks_to_us(ctx->act_ticks), ticks_to_us(avg_ticks),
			ticks_to_us(ctx->max_ticks));
		*offset += strnlen_s(&rttest_output[*offset], remaining);
	}
}

static void rttest_print_results(void)
{
	size_t offset = 0U;
	uint16_t pcpu_id;

	rttest_output[0] = '\0';
	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		rttest_append_result(&rttest_cpus[pcpu_id], &offset);
	}
	(void)shell_async_puts(rttest_output);
}

static void rttest_complete(struct rttest_cpu_context *ctx)
{
	uint64_t rflags;
	bool print_results = false;

	spinlock_irqsave_obtain(&rttest_run.lock, &rflags);
	if (!ctx->complete) {
		ctx->complete = true;
		rttest_run.completed++;
		print_results = rttest_run.completed == rttest_run.pcpu_num;
	}
	spinlock_irqrestore_release(&rttest_run.lock, rflags);

	if (print_results) {
		rttest_print_results();
		spinlock_irqsave_obtain(&rttest_run.lock, &rflags);
		rttest_run.running = false;
		spinlock_irqrestore_release(&rttest_run.lock, rflags);
	}
}

static void rttest_timer(void *data)
{
	struct rttest_cpu_context *ctx = (struct rttest_cpu_context *)data;
	uint64_t now = cpu_ticks();
	uint64_t deadline = ctx->timer.timeout;
	uint64_t latency = now > deadline ? now - deadline : 0UL;

	if (latency < ctx->min_ticks) {
		ctx->min_ticks = latency;
	}
	if (latency > ctx->max_ticks) {
		ctx->max_ticks = latency;
	}
	ctx->act_ticks = latency;
	if (ctx->sum_ticks <= (UINT64_MAX - latency)) {
		ctx->sum_ticks += latency;
	} else {
		ctx->sum_ticks = UINT64_MAX;
	}
	ctx->count++;

	if (ctx->count == RTTEST_SAMPLE_COUNT) {
		/* timer_softirq() sees this after the callback and does not reinsert it. */
		ctx->timer.mode = TICK_MODE_ONESHOT;
		rttest_complete(ctx);
		if (ctx->wait_ready) {
			wake_thread(&ctx->thread);
		}
	}
}

static bool rttest_start_local(struct rttest_cpu_context *ctx)
{
	uint64_t now = cpu_ticks();
	int32_t ret;

	initialize_timer(&ctx->timer, rttest_timer, ctx,
		now + us_to_ticks(RTTEST_INTERVAL_US), us_to_ticks(RTTEST_INTERVAL_US));
	ret = add_timer(&ctx->timer);
	if (ret != 0) {
		ctx->failed = true;
		rttest_complete(ctx);
	}

	return ret == 0;
}

static void rttest_worker(struct thread_object *thread)
{
	struct rttest_cpu_context *ctx = &rttest_cpus[get_pcpu_id()];

	ASSERT(thread == &ctx->thread, "rttest worker on wrong pCPU\n");
	while (true) {
		if (ctx->pending) {
			ctx->pending = false;
			if (rttest_start_local(ctx)) {
				sleep_thread(thread);
				ctx->wait_ready = true;
				if (ctx->complete) {
					wake_thread(thread);
				}
				schedule();
			}
		}
		sleep_thread(thread);
		schedule();
	}
}

void arm64_rttest_init(void)
{
	struct sched_params params = {
		.prio = PRIO_LOW,
		.bvt_weight = 1U,
		.cbs_period_us = RTTEST_CBS_PERIOD_US,
		.cbs_budget_us = RTTEST_CBS_BUDGET_US,
	};
	uint16_t pcpu_id;

	if (rttest_run.initialized) {
		return;
	}

	rttest_run.pcpu_num = get_pcpu_nums();
	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		struct rttest_cpu_context *ctx = &rttest_cpus[pcpu_id];

		ctx->pcpu_id = pcpu_id;
		(void)snprintf(ctx->thread.name, sizeof(ctx->thread.name), "rttest-%02hu", pcpu_id);
		ctx->thread.pcpu_id = pcpu_id;
		ctx->thread.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
		ctx->thread.thread_entry = rttest_worker;
		ctx->thread.switch_out = NULL;
		ctx->thread.switch_in = NULL;
		ctx->thread.host_sp = arch_setup_thread_stack(&ctx->thread, ctx->stack,
			CONFIG_STACK_SIZE);
		init_thread_data(&ctx->thread, &params);
	}
	rttest_run.initialized = true;
}

static int32_t shell_rttest(int32_t argc, __unused char **argv)
{
	uint64_t rflags;
	uint16_t pcpu_id;

	if (argc != 1) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&rttest_run.lock, &rflags);
	if (!rttest_run.initialized || rttest_run.running) {
		spinlock_irqrestore_release(&rttest_run.lock, rflags);
		return -EBUSY;
	}
	rttest_run.running = true;
	rttest_run.completed = 0U;
	spinlock_irqrestore_release(&rttest_run.lock, rflags);

	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		struct rttest_cpu_context *ctx = &rttest_cpus[pcpu_id];

		ctx->pending = true;
		ctx->wait_ready = false;
		ctx->complete = false;
		ctx->failed = false;
		ctx->count = 0U;
		ctx->min_ticks = UINT64_MAX;
		ctx->act_ticks = 0UL;
		ctx->sum_ticks = 0UL;
		ctx->max_ticks = 0UL;
	}
	for (pcpu_id = 0U; pcpu_id < rttest_run.pcpu_num; pcpu_id++) {
		wake_thread(&rttest_cpus[pcpu_id].thread);
	}

	/* Console input may execute from a timer softirq, so completion is asynchronous. */
	return 0;
}

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
	shell_item_begin("trace");
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

	shell_item_begin("trace dump");
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
	}
	shell_item_end();

	return 0;
}

static int32_t shell_trace(int32_t argc, char **argv)
{
	int32_t ret;

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
		ret = -EINVAL;
	}

	return ret;
}

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
 *                    shell snapshot: SMMU + ITS + streams
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
static void shell_format_vmid(char *buf, size_t size, uint16_t vmid)
{
	if (vmid == ACRN_INVALID_VMID) {
		snprintf(buf, size, "none");
	} else {
		snprintf(buf, size, "vm%hu", vmid);
	}
}

static const char *shell_smmu_ste_cfg_to_str(uint32_t cfg)
{
	const char *str;

	switch (cfg) {
	case 0U:
		str = "abort";
		break;
	case 4U:
		str = "bypass";
		break;
	case 6U:
		str = "s2";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *shell_smmu_state_to_str(enum arm_smmu_state state)
{
	const char *str;

	switch (state) {
	case ARM_SMMU_STATE_ABORT:
		str = "abort";
		break;
	case ARM_SMMU_STATE_READY:
		str = "ready";
		break;
	case ARM_SMMU_STATE_DEGRADED:
		str = "degraded";
		break;
	case ARM_SMMU_STATE_FAILED:
		str = "failed";
		break;
	case ARM_SMMU_STATE_UNDISCOVERED:
	default:
		str = "undiscovered";
		break;
	}

	return str;
}

static int32_t shell_smmustat(int32_t argc, __unused char **argv)
{
	struct arm_smmu_hw_info info;
	struct arm64_gicv3_its_stats its;
	uint32_t stream_count;
	uint32_t idx;

	if (argc > 1) {
		shell_puts("usage: smmustat\r\n");
		return -EINVAL;
	}

	(void)memset(&info, 0U, sizeof(info));
	(void)memset(&its, 0U, sizeof(its));
	arm_smmu_poll_events();
	arm_smmu_get_hw_info(&info);
	arm64_gicv3_its_get_stats(&its);
	stream_count = arm_smmu_get_stream_configs(shell_smmu_streams,
		ARRAY_SIZE(shell_smmu_streams));

	shell_puts("\r\nsmmustat:\r\n");
	shell_item_begin("smmu");
	shell_item_line("discovered:%s probed:%s abort:%s ready:%s",
		shell_yes_no(info.discovered), shell_yes_no(info.probed),
		shell_yes_no(info.aborted), shell_yes_no(info.ready));
	shell_item_line("strict:%s caps.valid:%s state:%s",
		shell_yes_no(info.strict), shell_yes_no(info.caps_valid),
		shell_smmu_state_to_str(info.state));
	shell_item_line("init.status:%d", info.init_status);
	if (!info.discovered) {
		shell_item_line("hardware:none");
		shell_item_end();
		return 0;
	}

	shell_item_line("mmio:base:0x%016lx size:0x%016lx", info.base, info.size);
	shell_item_line("caps:sid.bits:%u oas.bits:%u streams:%u s2p:%s",
		info.sid_bits, info.oas_bits, 1U << info.strtab_log2_entries,
		shell_yes_no((info.idr0 & 0x1U) != 0U));
	shell_item_line("caps:vmid.bits:%u oas.effective:%u required:%u fail:0x%016lx",
		info.vmid_bits, info.effective_oas_bits, info.required_oas_bits,
		info.cap_fail);
	shell_item_line("policy:sid.present:%s max:0x%x",
		shell_yes_no(info.policy_present), info.policy_max_sid);
	shell_item_line("regs:cr0:0x%08x cr1:0x%08x cr2:0x%08x gbpa:0x%08x",
		info.cr0, info.cr1, info.cr2, info.gbpa);
	shell_item_line("strtab:0x%016lx cmdq:0x%016lx evtq:0x%016lx",
		info.strtab_base, info.cmdq_base, info.evtq_base);
	shell_item_line("queue.entries:cmd:%u evt:%u cmdq.en:%s evtq.en:%s",
		info.cmdq_entries, info.evtq_entries, shell_yes_no(info.cmdq_enabled),
		shell_yes_no(info.evtq_enabled));
	shell_item_line("cmdq:prod:0x%08x cons:0x%08x last-cons:0x%08x last-ret:%d",
		info.cmdq_prod, info.cmdq_cons, info.cmdq_last_cons, info.cmdq_last_ret);
	shell_item_line("cmdq.ops:issued:%u sync:%u err:%u full:%u timeout:%u",
		info.cmdq_issued, info.cmdq_syncs, info.cmdq_errors,
		info.cmdq_full, info.cmdq_timeouts);
	shell_item_line("evtq:prod:0x%08x cons:0x%08x last-prod:0x%08x last-cons:0x%08x",
		info.evtq_prod, info.evtq_cons, info.evtq_last_prod,
		info.evtq_last_cons);
	shell_item_line("evtq.ops:poll:%u events:%u err:%u overflow:%u quarantine:%u",
		info.evtq_polled, info.evtq_events, info.evtq_errors,
		info.evtq_overflow, info.evtq_quarantined);
	shell_item_line("evtq.last:w0:0x%016lx w1:0x%016lx w2:0x%016lx w3:0x%016lx",
		info.evtq_last_word0, info.evtq_last_word1,
		info.evtq_last_word2, info.evtq_last_word3);
	shell_item_line("idr0:0x%08x idr1:0x%08x idr5:0x%08x",
		info.idr0, info.idr1, info.idr5);
	shell_item_line("iidr:0x%08x aidr:0x%08x", info.iidr, info.aidr);
	shell_item_line("ready.scope:abort-default + vm-stage2 STE");
	shell_item_line("assignment:%s streams:%u ok:%u fail:%u unassign.ok:%u unassign.fail:%u",
		shell_yes_no(arm_smmu_assignment_ready()), stream_count,
		info.assign_ok, info.assign_fail, info.unassign_ok, info.unassign_fail);
	shell_item_line("its:ready:%s base:0x%016lx size:0x%016lx target:0x%016lx",
		shell_yes_no(its.ready), its.base, its.size, its.target);
	shell_item_line("its:typer:0x%016lx cmd.writer:0x%08x vectors:%u/%u programmed:%u devs:%u",
		its.typer, its.cmdq_writer, its.vectors_used, its.vector_capacity,
		its.vectors_programmed, its.devices_used);
	shell_item_line("its.ops:msi:%u/%u msix:%u/%u rel:%u/%u map:%u/%u unmap:%u/%u",
		its.alloc_msi_ok, its.alloc_msi_fail,
		its.alloc_msix_ok, its.alloc_msix_fail,
		its.release_msi, its.release_msix,
		its.map_event_ok, its.map_event_fail,
		its.unmap_event_ok, its.unmap_event_fail);
	shell_item_line("its.cmd:issued:%u err:%u timeout:%u stall:%u last-ret:%d",
		its.cmd_issued, its.cmd_errors, its.cmd_timeouts,
		its.cmd_stalls, its.last_ret);
	for (idx = 0U; idx < stream_count; idx++) {
		char owner[16U];
		char domain[16U];

		shell_format_vmid(owner, sizeof(owner), shell_smmu_streams[idx].owner_vmid);
		shell_format_vmid(domain, sizeof(domain), shell_smmu_streams[idx].domain_vmid);
		shell_item_line("stream[0x%04x] sw-owner:%s ste-vm:%s assigned:%s quarantine:%s strtab:%s idx:0x%04x",
			shell_smmu_streams[idx].stream_id, owner, domain,
			shell_yes_no(shell_smmu_streams[idx].assigned),
			shell_yes_no(shell_smmu_streams[idx].quarantined),
			shell_yes_no(shell_smmu_streams[idx].in_strtab),
			shell_smmu_streams[idx].strtab_index);
		shell_item_line("     s2:ipa:%u root:0x%016lx",
			shell_smmu_streams[idx].ipa_width,
			shell_smmu_streams[idx].root_table_hpa);
		if (shell_smmu_streams[idx].in_strtab) {
			shell_item_line("     ste:valid:%s cfg:%s(%u) w0:0x%016lx w1:0x%016lx",
				shell_yes_no(shell_smmu_streams[idx].ste_valid),
				shell_smmu_ste_cfg_to_str(shell_smmu_streams[idx].ste_cfg),
				shell_smmu_streams[idx].ste_cfg, shell_smmu_streams[idx].ste[0],
				shell_smmu_streams[idx].ste[1]);
			shell_item_line("     ste:w2:0x%016lx w3:0x%016lx",
				shell_smmu_streams[idx].ste[2], shell_smmu_streams[idx].ste[3]);
		}
		if (shell_smmu_streams[idx].fault_count != 0U) {
			shell_item_line("     fault:count:%u code:0x%02x iova:0x%016lx",
				shell_smmu_streams[idx].fault_count,
				shell_smmu_streams[idx].last_fault_code,
				shell_smmu_streams[idx].last_fault_iova);
		}
	}
	shell_item_end();

	return 0;
}

static void shell_vsmmustat_one(uint16_t vm_id,
	const struct arm64_vsmmu_debug *debug)
{
	shell_item_begin("vm%hu vsmmu", vm_id);
	shell_item_line("configured:%s available:%s mmio:0x%016lx+0x%016lx",
		shell_yes_no(debug->size != 0UL), shell_yes_no(debug->available),
		debug->base, debug->size);
	if (!debug->available) {
		shell_item_line("state:hidden");
		shell_item_end();
		return;
	}
	shell_item_line("regs:cr0:0x%08x irq.ctrl:0x%08x gerror:0x%08x/%08x",
		debug->cr0, debug->irq_ctrl, debug->gerror, debug->gerrorn);
	shell_item_line("tables:strtab:0x%016lx cmdq:0x%016lx evtq:0x%016lx",
		debug->strtab_base, debug->cmdq_base, debug->evtq_base);
	shell_item_line("cmdq:prod:0x%08x cons:0x%08x processed:%lu rejected:%lu",
		debug->cmdq_prod, debug->cmdq_cons, debug->commands_processed,
		debug->commands_rejected);
	shell_item_line("evtq:prod:0x%08x cons:0x%08x", debug->evtq_prod,
		debug->evtq_cons);
	shell_item_line("worker:cpu%hu pending:%s budget:%lu generation:%lu irq:%s",
		debug->worker_pcpu, shell_yes_no(debug->worker_pending),
		debug->budget_exhausted, debug->generation,
		shell_yes_no(debug->irq_asserted));
	shell_item_line("sid-map:none broker:S1+S2-pending");
	shell_item_end();
}

static int32_t shell_vsmmustat(int32_t argc, char **argv)
{
	struct arm64_vsmmu_debug debug;
	int64_t param;
	uint16_t first = 0U;
	uint16_t last = CONFIG_MAX_VM_NUM;
	uint16_t vm_id;
	bool found = false;

	if (argc > 2) {
		shell_puts("usage: vsmmustat [vm id]\r\n");
		return -EINVAL;
	}
	if (argc == 2) {
		param = strtol_deci(argv[1]);
		if ((param < 0L) || (param >= CONFIG_MAX_VM_NUM)) {
			shell_puts("invalid vm id\r\n");
			return -EINVAL;
		}
		first = (uint16_t)param;
		last = first + 1U;
	}

	shell_puts("\r\nvsmmustat:\r\n");
	for (vm_id = first; vm_id < last; vm_id++) {
		if (arm64_vsmmu_get_debug(vm_id, &debug)) {
			shell_vsmmustat_one(vm_id, &debug);
			found = true;
		}
	}
	if (!found) {
		shell_item_begin("vsmmu");
		shell_item_line("instances:none");
		shell_item_end();
	}

	return 0;
}

static void shell_format_bdf(char *buf, size_t size, union pci_bdf bdf)
{
	snprintf(buf, size, "%02x:%02x.%x", bdf.bits.b, bdf.bits.d, bdf.bits.f);
}

static uint32_t shell_pci_vcfg_read(const struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes)
{
	uint32_t val = 0U;
	uint32_t idx;

	if ((vdev == NULL) || (bytes == 0U) || (bytes > 4U) ||
		((offset + bytes) > PCIE_CONFIG_SPACE_SIZE)) {
		return 0U;
	}

	for (idx = 0U; idx < bytes; idx++) {
		val |= (uint32_t)vdev->cfgdata.data_8[offset + idx] << (idx * 8U);
	}

	return val;
}

static void shell_pcistat_print_bars(const struct pci_vdev *vdev)
{
	uint32_t bar_idx;

	for (bar_idx = 0U; bar_idx < vdev->nr_bars; bar_idx++) {
		const struct pci_vbar *vbar = &vdev->vbars[bar_idx];
		const char *type;

		if (vbar->is_mem64hi || (vbar->size == 0UL)) {
			continue;
		}
		type = ((vbar->bar_type.io_space.indicator == 1U) && !vbar->is_mem64hi) ?
			"io" : "mem";
		shell_item_line("     bar%u:%s gpa:0x%016lx hpa:0x%016lx size:0x%016lx",
			bar_idx, type, vbar->base_gpa, vbar->base_hpa, vbar->size);
	}
}

static void shell_pcistat_print_msi(const struct pci_vdev *vdev)
{
	uint32_t ctrl;
	uint32_t mask = 0U;
	bool pvm;

	if (vdev->msi.capoff == 0U) {
		return;
	}

	ctrl = shell_pci_vcfg_read(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U);
	pvm = ((ctrl & PCIM_MSICTRL_PVMC) != 0U);
	if (pvm) {
		uint32_t mask_offset = vdev->msi.is_64bit ? PCIR_MSI_MASK : (PCIR_MSI_MASK - 4U);

		mask = shell_pci_vcfg_read(vdev, vdev->msi.capoff + mask_offset, 4U);
	}

	shell_item_line("     msi:cap:0x%02x len:%u vectors:%u 64:%s pvm:%s enabled:%s ctrl:0x%04x mask:0x%08x",
		vdev->msi.capoff, vdev->msi.caplen, vdev->msi.vector_count,
		shell_yes_no(vdev->msi.is_64bit),
		shell_yes_no(pvm), shell_yes_no((ctrl & PCIM_MSICTRL_MSI_ENABLE) != 0U),
		ctrl, mask);
}

static void shell_pcistat_print_msix(const struct pci_vdev *vdev)
{
	uint32_t ctrl;
	uint64_t hole_gpa = 0UL;
	uint64_t hole_hpa = 0UL;
	uint64_t hole_size = 0UL;

	if (vdev->msix.capoff == 0U) {
		return;
	}

	ctrl = shell_pci_vcfg_read(vdev, vdev->msix.capoff + PCIR_MSIX_CTRL, 2U);
	if ((vdev->msix.mmio_gpa != 0UL) && (vdev->msix.table_count != 0U)) {
		uint64_t table_gpa = vdev->msix.mmio_gpa + vdev->msix.table_offset;
		uint64_t table_hpa = vdev->msix.mmio_hpa + vdev->msix.table_offset;
		uint64_t table_size = (uint64_t)vdev->msix.table_count * MSIX_TABLE_ENTRY_SIZE;

		hole_gpa = table_gpa & PAGE_MASK;
		hole_hpa = table_hpa & PAGE_MASK;
		hole_size = ((table_gpa + table_size + PAGE_SIZE - 1UL) & PAGE_MASK) - hole_gpa;
	}

	shell_item_line("     msix:cap:0x%02x len:%u table:bar%u off:0x%08x count:%u enabled:%s masked:%s on-msi:%s programmed:%s",
		vdev->msix.capoff, vdev->msix.caplen, vdev->msix.table_bar,
		vdev->msix.table_offset, vdev->msix.table_count,
		shell_yes_no((ctrl & PCIM_MSIXCTRL_MSIX_ENABLE) != 0U),
		shell_yes_no((ctrl & PCIM_MSIXCTRL_FUNCTION_MASK) != 0U),
		shell_yes_no(vdev->msix.is_vmsix_on_msi),
		shell_yes_no(vdev->msix.is_vmsix_on_msi_programmed));
	if (hole_size != 0UL) {
		shell_item_line("     msix-hole:gpa:0x%016lx hpa:0x%016lx size:0x%016lx",
			hole_gpa, hole_hpa, hole_size);
	}
}

static const struct arm_smmu_stream_config *shell_find_stream_config(
	const struct arm_smmu_stream_config *streams, uint32_t stream_count,
	uint32_t stream_id)
{
	const struct arm_smmu_stream_config *found = NULL;
	uint32_t idx;

	for (idx = 0U; idx < stream_count; idx++) {
		if (streams[idx].stream_id == stream_id) {
			found = &streams[idx];
			break;
		}
	}

	return found;
}

static const char *shell_pcistat_stream_state(const struct arm_smmu_stream_config *stream, uint16_t vm_id)
{
	const char *state;

	if (stream == NULL) {
		state = "missing";
	} else if (stream->quarantined) {
		state = "quarantine";
	} else if (!stream->assigned) {
		state = "free";
	} else if (stream->owner_vmid == vm_id) {
		state = "owned";
	} else {
		state = "wrong-vm";
	}

	return state;
}

static void shell_pcistat_host(void)
{
	struct pci_mmcfg_region *mmcfg = get_mmcfg_region();
	uint32_t pdev_count = get_pci_pdev_num();
	uint32_t idx;

	shell_item_begin("host-pci");
	shell_item_line("ecam:0x%016lx bus:%u-%u pdevs:%u",
		mmcfg->address, mmcfg->start_bus, mmcfg->end_bus, pdev_count);
	for (idx = 0U; idx < pdev_count; idx++) {
		const struct pci_pdev *pdev = get_pci_pdev(idx);
		char bdf[16U];

		if (pdev == NULL) {
			continue;
		}
		shell_format_bdf(bdf, sizeof(bdf), pdev->bdf);
		shell_item_line("[%02u] %s class:%02x:%02x hdr:0x%02x bars:%u drhd:%u",
			idx, bdf, pdev->base_class, pdev->sub_class, pdev->hdr_type,
			pdev->nr_bars, pdev->drhd_index);
	}
	shell_item_end();
}

static void shell_pcistat_vm(uint16_t vm_id,
	const struct arm_smmu_stream_config *streams, uint32_t stream_count)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm_id);
	struct acrn_vm *vm = get_vm_from_vmid(vm_id);
	uint16_t idx;

	if ((vm_config->pci_devs == NULL) || (vm_config->pci_dev_num == 0U)) {
		shell_item_begin("vm%hu pci", vm_id);
		shell_item_line("devices:none");
		shell_item_end();
		return;
	}

	shell_item_begin("vm%hu pci:%s", vm_id, vm_config->name);
	shell_item_line("state:%s configured:%hu vpci:%s",
		is_poweroff_vm(vm) ? "poweroff" : "created",
		vm_config->pci_dev_num,
		(vm->vpci.pci_mmcfg.address != 0UL) ? "Y" : "N");
	for (idx = 0U; idx < vm_config->pci_dev_num; idx++) {
		const struct acrn_vm_pci_dev_config *dev_config = &vm_config->pci_devs[idx];
		const struct arm_smmu_stream_config *stream;
		struct pci_vdev *vdev = NULL;
		char pbdf[16U];
		char vbdf[16U];

		if (!is_poweroff_vm(vm) && (vm->vpci.pci_mmcfg.address != 0UL)) {
			vdev = pci_find_vdev(&vm->vpci, dev_config->vbdf);
		}
		stream = shell_find_stream_config(streams, stream_count,
			(uint32_t)dev_config->pbdf.value);
		shell_format_bdf(pbdf, sizeof(pbdf), dev_config->pbdf);
		shell_format_bdf(vbdf, sizeof(vbdf), dev_config->vbdf);
		shell_item_line("[%02hu] p:%s v:%s pdev:%s vdev:%s stream:0x%04x smmu:%s",
			idx, pbdf, vbdf,
			shell_yes_no(dev_config->pdev != NULL),
			shell_yes_no(vdev != NULL),
			dev_config->pbdf.value,
			shell_pcistat_stream_state(stream, vm_id));
		if (stream != NULL) {
			shell_item_line("     owner:vm%hu ipa:%u root:0x%016lx quarantine:%s",
				stream->owner_vmid, stream->ipa_width, stream->root_table_hpa,
				shell_yes_no(stream->quarantined));
			if (stream->fault_count != 0U) {
				shell_item_line("     fault:count:%u code:0x%02x iova:0x%016lx",
					stream->fault_count, stream->last_fault_code,
					stream->last_fault_iova);
			}
		}
		if (vdev != NULL) {
			shell_pcistat_print_bars(vdev);
			shell_pcistat_print_msi(vdev);
			shell_pcistat_print_msix(vdev);
		}
	}
	shell_item_end();
}

static int32_t shell_pcistat(int32_t argc, __unused char **argv)
{
	uint32_t stream_count;
	uint16_t vm_id;

	if (argc > 1) {
		shell_puts("usage: pcistat\r\n");
		return -EINVAL;
	}

	arm_smmu_poll_events();
	stream_count = arm_smmu_get_stream_configs(shell_smmu_streams,
		ARRAY_SIZE(shell_smmu_streams));
	shell_puts("\r\npcistat:\r\n");
	shell_pcistat_host();

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);

		if ((vm_config->pci_devs != NULL) && (vm_config->pci_dev_num != 0U)) {
			shell_pcistat_vm(vm_id, shell_smmu_streams, stream_count);
		}
	}

	return 0;
}

static const char *thread_state_to_str(enum thread_object_state state)
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

static const char *vcpu_sched_state_to_str(const struct acrn_vcpu *vcpu)
{
	return (vcpu_get_state(vcpu) == VCPU_OFFLINE) ?
		"offline" : thread_state_to_str(vcpu->thread_obj.status);
}

static void shell_dumpstat_format_reg(char *buf, size_t size, const char *name,
	uint64_t value)
{
	snprintf(buf, size, DUMPSTAT_REG_KEY_FMT, name, value);
}

static void shell_dumpstat_reg_line(uint32_t count, ...)
{
	char reg[DUMPSTAT_REGS_PER_LINE_MAX][32U];
	va_list args;
	uint32_t idx;

	if ((count == 0U) || (count > DUMPSTAT_REGS_PER_LINE_MAX)) {
		return;
	}

	va_start(args, count);
	for (idx = 0U; idx < count; idx++) {
		const char *name = __builtin_va_arg(args, const char *);
		uint64_t value = __builtin_va_arg(args, uint64_t);

		shell_dumpstat_format_reg(reg[idx], sizeof(reg[idx]), name, value);
	}
	va_end(args);

	switch (count) {
	case 1U:
		shell_item_line("%s", reg[0U]);
		break;
	case 2U:
		shell_item_line("%s %s", reg[0U], reg[1U]);
		break;
	case 3U:
		shell_item_line("%s %s %s", reg[0U], reg[1U], reg[2U]);
		break;
	default:
		shell_item_line("%s %s %s %s", reg[0U], reg[1U], reg[2U], reg[3U]);
		break;
	}
}

static void shell_dumpstat_regs(const struct cpu_regs *regs)
{
	shell_dumpstat_reg_line(3U, "elr", regs->elr, "spsr", regs->spsr, "esr", regs->esr);
	shell_dumpstat_reg_line(2U, "far", regs->far, "hpfar", regs->hpfar);
	shell_dumpstat_reg_line(4U, "x00", regs->x0, "x01", regs->x1, "x02", regs->x2, "x03", regs->x3);
	shell_dumpstat_reg_line(4U, "x04", regs->x4, "x05", regs->x5, "x06", regs->x6, "x07", regs->x7);
	shell_dumpstat_reg_line(4U, "x08", regs->x8, "x09", regs->x9, "x10", regs->x10, "x11", regs->x11);
	shell_dumpstat_reg_line(4U, "x12", regs->x12, "x13", regs->x13, "x14", regs->x14, "x15", regs->x15);
	shell_dumpstat_reg_line(4U, "x16", regs->x16, "x17", regs->x17, "x18", regs->x18, "x19", regs->x19);
	shell_dumpstat_reg_line(4U, "x20", regs->x20, "x21", regs->x21, "x22", regs->x22, "x23", regs->x23);
	shell_dumpstat_reg_line(4U, "x24", regs->x24, "x25", regs->x25, "x26", regs->x26, "x27", regs->x27);
	shell_dumpstat_reg_line(4U, "x28", regs->x28, "x29", regs->x29, "lr", regs->lr, "sp", regs->sp);
}

static const char *shell_yes_no(bool value)
{
	return value ? "Y" : "N";
}

static uint32_t shell_lr_state(uint64_t lr)
{
	return (uint32_t)((lr >> ICH_LR_STATE_SHIFT) & 0x3UL);
}

static const char *shell_vtimer_trace_event_to_str(uint32_t event)
{
	const char *str;

	/*
	 * dumpstat prints short event names in the vtimer ring:
	 * load/unload : vCPU switch boundary saved or restored timer state.
	 * sysreg      : guest timer sysreg access updated the EL2 shadow.
	 * ppi         : host generic-timer PPI reached EL2.
	 * poll        : EL2 sampled an expired timer at a bounded sync point.
	 * update      : timer state was synchronized into the vGIC line.
	 * inject      : an expired timer became a guest-visible PPI.
	 * eoi         : guest completed the virtual timer interrupt.
	 * requeue     : timer line was queued again after vGIC/timer sync.
	 * backup      : offline backup timer fired for an unloaded vCPU.
	 * lr-pending  : pending-only timer PPI already resides in a guest LR.
	 * lr-noeoi    : old pending timer LR is absent and no EOI was reported.
	 * mask        : EL2 live CNTV IMASK ownership changed.
	 * stall       : stale pending LR/host handoff stall was detected.
	 */
	switch (event) {
	case ARM64_VTIMER_TRACE_LOAD:
		str = "load";
		break;
	case ARM64_VTIMER_TRACE_UNLOAD:
		str = "unload";
		break;
	case ARM64_VTIMER_TRACE_SYSREG:
		str = "sysreg";
		break;
	case ARM64_VTIMER_TRACE_PPI:
		str = "ppi";
		break;
	case ARM64_VTIMER_TRACE_POLL:
		str = "poll";
		break;
	case ARM64_VTIMER_TRACE_UPDATE:
		str = "update";
		break;
	case ARM64_VTIMER_TRACE_INJECT:
		str = "inject";
		break;
	case ARM64_VTIMER_TRACE_EOI:
		str = "eoi";
		break;
	case ARM64_VTIMER_TRACE_REQUEUE:
		str = "requeue";
		break;
	case ARM64_VTIMER_TRACE_BACKUP:
		str = "backup";
		break;
	case ARM64_VTIMER_TRACE_PENDING_LR:
		str = "lr-pending";
		break;
	case ARM64_VTIMER_TRACE_LOST_LR:
		str = "lr-noeoi";
		break;
	case ARM64_VTIMER_TRACE_MASK:
		str = "mask";
		break;
	case ARM64_VTIMER_TRACE_STALL:
		str = "stall";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *shell_guest_trace_event_to_str(uint8_t event)
{
	const char *str;

	/*
	 * Guest trace event names describe EL1/EL2 control boundaries:
	 * enter  : vCPU thread is about to enter guest EL1.
	 * exit   : guest returned to EL2 because of a trap or IRQ.
	 * resume : EL2 handled the exit and is about to return to EL1.
	 */
	switch (event) {
	case ARM64_VCPU_GUEST_TRACE_ENTER:
		str = "enter";
		break;
	case ARM64_VCPU_GUEST_TRACE_EXIT:
		str = "exit";
		break;
	case ARM64_VCPU_GUEST_TRACE_RESUME:
		str = "resume";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static void shell_dumpstat_guest_trace(const struct arm64_vcpu_guest_trace *trace)
{
	uint32_t count = trace->count;
	uint32_t start;
	uint32_t idx;
	uint64_t prev_tsc = 0UL;

	if (count > ARM64_VCPU_GUEST_TRACE_NUM) {
		count = ARM64_VCPU_GUEST_TRACE_NUM;
	}
	if (count == 0U) {
		shell_item_line("guest-trace:none");
		return;
	}

	start = (trace->head + ARM64_VCPU_GUEST_TRACE_NUM - count) %
		ARM64_VCPU_GUEST_TRACE_NUM;
	/*
	 * src identifies sync versus IRQ exits, ec is the ESR exception class
	 * when available, and delta.us is time since the previous boundary row.
	 */
	for (idx = 0U; idx < count; idx++) {
		uint32_t ring_idx = (start + idx) % ARM64_VCPU_GUEST_TRACE_NUM;
		const struct arm64_vcpu_guest_trace_entry *entry = &trace->entry[ring_idx];
		uint64_t delta_us = ((prev_tsc == 0UL) || (entry->tsc < prev_tsc)) ?
			0UL : ticks_to_us(entry->tsc - prev_tsc);

		if (entry->ec == ARM64_VCPU_DEBUG_EXIT_EC_INVALID) {
			shell_item_line("gt[%02u] %-6s pcpu:%hu src:0x%02x ec:N/A  status:%3d delta.us:%8lu",
				idx, shell_guest_trace_event_to_str(entry->event),
				entry->pcpu_id, entry->source, entry->status,
				delta_us);
		} else {
			shell_item_line("gt[%02u] %-6s pcpu:%hu src:0x%02x ec:0x%02x status:%3d delta.us:%8lu",
				idx, shell_guest_trace_event_to_str(entry->event),
				entry->pcpu_id, entry->source, entry->ec, entry->status,
				delta_us);
		}
		shell_item_line("       elr:0x%016lx esr:0x%016lx far:0x%016lx hpfar:0x%016lx",
			entry->elr, entry->esr, entry->far, entry->hpfar);
		prev_tsc = entry->tsc;
	}
}

static void shell_dumpstat_vtimer_trace(const struct arm64_vcpu_vtimer_trace *trace)
{
	uint32_t count = trace->count;
	uint32_t start;
	uint32_t idx;

	if (count > ARM64_VCPU_VTIMER_TRACE_NUM) {
		count = ARM64_VCPU_VTIMER_TRACE_NUM;
	}
	if (count == 0U) {
		shell_item_line("trace:none");
		return;
	}

	start = (trace->head + ARM64_VCPU_VTIMER_TRACE_NUM - count) %
		ARM64_VCPU_VTIMER_TRACE_NUM;
	for (idx = 0U; idx < count; idx++) {
		uint32_t ring_idx = (start + idx) % ARM64_VCPU_VTIMER_TRACE_NUM;
		const struct arm64_vcpu_vtimer_trace_entry *entry = &trace->entry[ring_idx];
		int64_t delta = (int64_t)(entry->cval - entry->cntvct);

		shell_item_line("vt[%02u] %-10s pcpu:%hu virq:%u ctl:0x%08x exp:%s mask:%s p/a/l:%s/%s/%s wr:%s inj:%s delta:%ld",
			idx, shell_vtimer_trace_event_to_str(entry->event),
			entry->pcpu_id, entry->virq, entry->ctl,
			shell_yes_no(entry->expired), shell_yes_no(entry->masked),
			shell_yes_no(entry->pending), shell_yes_no(entry->active),
			shell_yes_no(entry->level), shell_yes_no(entry->write),
			shell_yes_no(entry->injected), delta);
		shell_item_line("       cval:0x%016lx cntvct:0x%016lx lr0:0x%016lx hcr:0x%016lx",
			entry->cval, entry->cntvct, entry->lr0, entry->hcr);
	}
}

static bool shell_stack_contains(uint64_t start, uint64_t end, uint64_t addr, uint64_t bytes)
{
	uint64_t left;

	if ((addr < start) || (addr >= end)) {
		return false;
	}

	left = end - addr;
	return bytes <= left;
}

static void shell_dumpstat_print_frame(uint32_t idx, uint64_t fp, uint64_t lr, bool symbolize)
{
	char sym[96U];

	if (symbolize) {
		dbg_format_symbol(lr, sym, sizeof(sym));
		shell_item_line("[%02u] fp:0x%016lx  lr:0x%016lx  %s",
			idx, fp, lr, sym);
	} else {
		shell_item_line("[%02u] fp:0x%016lx  lr:0x%016lx",
			idx, fp, lr);
	}
}

static void shell_dumpstat_unwind_host_stack(uint64_t sp, uint64_t fp, uint64_t lr,
	uint64_t stack_start, uint64_t stack_end)
{
	uint32_t idx;

	if (!shell_stack_contains(stack_start, stack_end, sp, sizeof(uint64_t))) {
		shell_item_line("trace unavailable: sp is outside the stack");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_STACK_DEPTH; idx++) {
		uint64_t next_fp = 0UL;

		shell_dumpstat_print_frame(idx, fp, lr, true);

		if (!shell_stack_contains(stack_start, stack_end, fp, sizeof(uint64_t) * 2UL)) {
			break;
		}

		next_fp = *((const uint64_t *)fp);
		lr = *(((const uint64_t *)fp) + 1UL);
		if ((next_fp == SP_BOTTOM_MAGIC) || (next_fp <= fp)) {
			break;
		}

		fp = next_fp;
	}
}

struct dumpstat_guest_frame {
	uint64_t fp;
	uint64_t lr;
};

static void shell_dumpstat_vm_stack(struct acrn_vcpu *vcpu, const struct cpu_regs *regs)
{
	struct dumpstat_guest_frame frame;
	uint64_t fp = regs->x29;
	uint64_t lr = regs->lr;
	uint32_t idx;

	if ((fp == 0UL) && (lr == 0UL)) {
		shell_item_line("trace unavailable: empty frame registers");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_STACK_DEPTH; idx++) {
		shell_dumpstat_print_frame(idx, fp, lr, false);

		if (fp == 0UL) {
			break;
		}

		if (copy_from_gpa(vcpu->vm, &frame, fp, sizeof(frame)) != 0) {
			break;
		}
		if ((frame.fp == 0UL) || (frame.fp <= fp)) {
			break;
		}

		fp = frame.fp;
		lr = frame.lr;
	}
}

static void shell_dumpstat_host_stack(const struct acrn_vcpu *vcpu)
{
	const struct stack_frame *saved_frame = (const struct stack_frame *)vcpu->thread_obj.host_sp;
	uint64_t stack_start = (uint64_t)&vcpu->stack[0];
	uint64_t stack_end = (uint64_t)&vcpu->stack[CONFIG_STACK_SIZE];

	if (!shell_stack_contains(stack_start, stack_end, vcpu->thread_obj.host_sp, sizeof(*saved_frame))) {
		shell_item_line("trace unavailable: saved sp is outside the vcpu thread stack");
		return;
	}

	shell_dumpstat_unwind_host_stack(vcpu->thread_obj.host_sp,
		saved_frame->x29, saved_frame->lr, stack_start, stack_end);
}

struct dumpstat_snapshot {
	struct acrn_vcpu *vcpu;
	struct cpu_regs regs;
	struct arm64_vcpu_debug_info debug;
	struct arm64_vcpu_guest_ctx gctx;
	struct arm64_vgicv3_vcpu_ctx vgic_ctx;
	struct arm64_vgic_irq timer_irq;
	uint64_t live_cntvct_el0;
	uint64_t live_cntv_cval_el0;
	uint64_t live_ich_hcr_el2;
	uint64_t live_ich_vmcr_el2;
	uint64_t live_ich_lr[4U];
	uint64_t pending_req;
	uint64_t irqs_pending;
	uint64_t irqs_pending_mask;
	uint32_t timer_pending_word;
	uint32_t live_cntv_ctl_el0;
	bool has_timer_irq;
	bool has_live_timer;
	bool captured;
};

static void shell_dumpstat_snapshot_timer_irq(struct dumpstat_snapshot *snapshot)
{
	const struct acrn_vcpu *vcpu = snapshot->vcpu;
	uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized &&
		(vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS) && (virq < ARM64_VGIC_IRQ_NUM)) {
		const struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint32_t word = virq / 32U;

		snapshot->timer_irq = vgic->irq[vcpu->vcpu_id][virq];
		snapshot->timer_pending_word = vgic->pending_bitmap[vcpu->vcpu_id][word];
		snapshot->has_timer_irq = true;
	}
}

static void shell_dumpstat_capture(void *data)
{
	struct dumpstat_snapshot *snapshot = (struct dumpstat_snapshot *)data;

	if (get_running_vcpu(get_pcpu_id()) == snapshot->vcpu) {
		(void)memcpy_s(&snapshot->regs, sizeof(snapshot->regs),
			&snapshot->vcpu->arch.regs, sizeof(snapshot->regs));
		(void)memcpy_s(&snapshot->debug, sizeof(snapshot->debug),
			&snapshot->vcpu->arch.debug, sizeof(snapshot->debug));
		(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
			&snapshot->vcpu->arch.gctx, sizeof(snapshot->gctx));
		(void)memcpy_s(&snapshot->vgic_ctx, sizeof(snapshot->vgic_ctx),
			&snapshot->vcpu->arch.vgic, sizeof(snapshot->vgic_ctx));
		snapshot->pending_req = snapshot->vcpu->pending_req;
		snapshot->irqs_pending = snapshot->vcpu->arch.irqs_pending;
		snapshot->irqs_pending_mask = snapshot->vcpu->arch.irqs_pending_mask;
		shell_dumpstat_snapshot_timer_irq(snapshot);
		snapshot->live_cntvct_el0 = read_cntvct_el0();
		snapshot->live_cntv_cval_el0 = read_cntv_cval_el0();
		snapshot->live_cntv_ctl_el0 = read_cntv_ctl_el0();
		snapshot->live_ich_hcr_el2 = read_ich_hcr_el2();
		snapshot->live_ich_vmcr_el2 = read_ich_vmcr_el2();
		/*
		 * Live LRs can differ from the saved software copy while the vCPU
		 * is running. Capturing both views identifies save/restore drift
		 * versus hardware state that is genuinely stuck at the CPU interface.
		 */
		snapshot->live_ich_lr[0U] = read_ich_lr_el2(0U);
		snapshot->live_ich_lr[1U] = read_ich_lr_el2(1U);
		snapshot->live_ich_lr[2U] = read_ich_lr_el2(2U);
		snapshot->live_ich_lr[3U] = read_ich_lr_el2(3U);
		snapshot->has_live_timer = true;
		snapshot->captured = true;
	}
}

static const struct cpu_regs *shell_dumpstat_get_regs(struct acrn_vcpu *vcpu,
	struct dumpstat_snapshot *snapshot)
{
	uint16_t pcpu_id = vcpu->thread_obj.pcpu_id;

	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->vcpu = vcpu;
	(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
		&vcpu->arch.gctx, sizeof(snapshot->gctx));
	(void)memcpy_s(&snapshot->vgic_ctx, sizeof(snapshot->vgic_ctx),
		&vcpu->arch.vgic, sizeof(snapshot->vgic_ctx));
	snapshot->pending_req = vcpu->pending_req;
	snapshot->irqs_pending = vcpu->arch.irqs_pending;
	snapshot->irqs_pending_mask = vcpu->arch.irqs_pending_mask;
	shell_dumpstat_snapshot_timer_irq(snapshot);
	snapshot->has_live_timer = false;
	snapshot->captured = false;

	if (is_vcpu_running(vcpu) &&
		(sched_get_current(pcpu_id) == &vcpu->thread_obj) &&
		(pcpu_id != get_pcpu_id())) {
		(void)smp_call_function_timeout(1UL << pcpu_id, shell_dumpstat_capture,
			snapshot, DUMPSTAT_SMP_CALL_TIMEOUT_US);
	}

	return snapshot->captured ? &snapshot->regs : &vcpu->arch.regs;
}

/* [20260630] dumpstat monitor:
 *
 * dumpstat is a per-vCPU deep snapshot. If the target vCPU is current on a
 * remote pCPU, an IPI samples live EL2 timer and vGIC state; otherwise the
 * command falls back to the durable vCPU state saved in memory. Printing both
 * saved and live fields separates context-save bugs from guest-visible stalls.
 *
 *   target vCPU current on remote pCPU
 *        -> IPI live capture -> regs/vGIC/vtimer + stacks
 *      otherwise
 *        -> saved vCPU image -> regs/vGIC/vtimer + stacks
 */
static int32_t shell_find_valid_lr_for_virq(const uint64_t *lrs, uint32_t count, uint32_t virq)
{
	uint32_t idx;

	for (idx = 0U; idx < count; idx++) {
		uint64_t lr = lrs[idx];

		if ((shell_lr_state(lr) != ICH_LR_STATE_INVALID) &&
			((uint32_t)(lr & ICH_LR_VINTID_MASK) == virq)) {
			return (int32_t)idx;
		}
	}

	return -1;
}

static void shell_dumpstat_timer_irq_state(const struct dumpstat_snapshot *snapshot)
{
	uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	uint32_t bit = 1U << (virq % 32U);
	uint64_t vmcr = snapshot->has_live_timer ?
		snapshot->live_ich_vmcr_el2 : snapshot->vgic_ctx.vmcr;
	int32_t saved_lr = shell_find_valid_lr_for_virq(snapshot->vgic_ctx.lr,
		snapshot->vgic_ctx.used_lrs, virq);
	int32_t live_lr = snapshot->has_live_timer ?
		shell_find_valid_lr_for_virq(snapshot->live_ich_lr,
			ARRAY_SIZE(snapshot->live_ich_lr), virq) : -1;
	bool gicd_g1 = ((snapshot->vcpu->vm->arch_vm.vgic.gicd_ctlr & (1U << 1U)) != 0U);
	bool vmcr_g1 = ((vmcr & ICH_VMCR_VENG1) != 0UL);
	bool bitmap = ((snapshot->timer_pending_word & bit) != 0U);

	if (!snapshot->has_timer_irq) {
		shell_item_line("      vgic:desc:none");
		return;
	}

	shell_item_line("      vgic:en:%s pend:%s act:%s level:%s bitmap:%s deliverable:%s",
		shell_yes_no(snapshot->timer_irq.enabled),
		shell_yes_no(snapshot->timer_irq.pending),
		shell_yes_no(snapshot->timer_irq.active),
		shell_yes_no(snapshot->timer_irq.level),
		shell_yes_no(bitmap),
		shell_yes_no(gicd_g1 && vmcr_g1 && snapshot->timer_irq.enabled));
	shell_item_line("      route:saved-lr:%d live-lr:%d hcr:0x%016lx live-hcr:0x%016lx",
		saved_lr, live_lr, snapshot->vgic_ctx.hcr, snapshot->live_ich_hcr_el2);
}

static void shell_dumpstat_timer_state(const struct dumpstat_snapshot *snapshot)
{
	const struct arm64_vcpu_guest_ctx *gctx = &snapshot->gctx;

	if (snapshot->has_live_timer) {
		uint64_t guest_now = snapshot->live_cntvct_el0;

		shell_item_line("PPI%u cntv_ctl:0x%08x guest_ctl:0x%08x cntv_cval:0x%08lx cntvct:0x%08lx delta:%ld el2_mask:%s",
			gctx->timer_virq, snapshot->live_cntv_ctl_el0,
			gctx->cntv_ctl_el0, snapshot->live_cntv_cval_el0,
			snapshot->live_cntvct_el0,
			(int64_t)(snapshot->live_cntv_cval_el0 - guest_now),
			shell_yes_no(gctx->cntv_el2_masked));
	} else {
		shell_item_line("PPI%u live:none el2_mask:%s",
			gctx->timer_virq, shell_yes_no(gctx->cntv_el2_masked));
	}
	shell_dumpstat_timer_irq_state(snapshot);
}

static void shell_dumpstat_vtimer_diag(const struct arm64_vcpu_vtimer_diag *diag)
{
	uint64_t mask_ticks = diag->max_el2_mask_ticks;

	if (diag->el2_mask_since_ticks != 0UL) {
		uint64_t now = cpu_ticks();
		uint64_t active_ticks = (now > diag->el2_mask_since_ticks) ?
			(now - diag->el2_mask_since_ticks) : 0UL;

		if (active_ticks > mask_ticks) {
			mask_ticks = active_ticks;
		}
	}

	/*
	 * This section deliberately avoids repeating raw CNTV/LR fields from
	 * "timer/vgic state" and "vtimer trace". It gives cumulative counters for
	 * the four transitions that matter when Linux reports timer-softirq stalls:
	 * WFI wakeup, pending-only LR preservation, EL2 host-timer masking, and the
	 * last-chance flush before ERET.
	 */
	shell_item_line("wfi:trap:%lu irq-masked:%lu pending-irq:%lu",
		diag->wfi_trap, diag->wfi_irq_masked, diag->wfi_pending_irq);
	shell_item_line("lr-pending-only:seen:%lu preserve:%lu drop:%lu missing-no-eoi:%lu",
		diag->pending_only_lr_seen, diag->pending_only_lr_preserve,
		diag->pending_only_lr_drop, diag->lost_pending_lr);
	shell_item_line("el2-mask:set:%lu clear:%lu max-age.us:%lu active:%s",
		diag->el2_mask_set, diag->el2_mask_clear,
		ticks_to_us(mask_ticks),
		shell_yes_no(diag->el2_mask_since_ticks != 0UL));
	shell_item_line("pre-eret-flush:run:%lu expired:%lu",
		diag->pre_eret_flush,
		diag->pre_eret_flush_expired);
}

static int32_t shell_dumpstat_vcpu(struct acrn_vcpu *vcpu)
{
	struct thread_object *current;
	struct dumpstat_snapshot snapshot;
	const struct cpu_regs *regs;
	const struct arm64_vcpu_debug_info *debug;

	current = (vcpu_get_state(vcpu) == VCPU_OFFLINE) ?
		NULL : sched_get_current(vcpu->thread_obj.pcpu_id);
	regs = shell_dumpstat_get_regs(vcpu, &snapshot);
	debug = snapshot.captured ? &snapshot.debug : &vcpu->arch.debug;

	shell_item_begin("vm%hu/vcpu%hu", vcpu->vm->vm_id, vcpu->vcpu_id);
	/*
	 * Header fields identify CPU binding, scheduler state, whether this vCPU
	 * is the current thread, and whether live EL2 state was sampled by IPI.
	 */
	shell_item_line("pcpu:%hu sched:%s current:%s live:%s", vcpu->thread_obj.pcpu_id,
		vcpu_sched_state_to_str(vcpu),
		shell_yes_no(current == &vcpu->thread_obj),
		shell_yes_no(snapshot.captured));
	shell_item_line("lifecycle:%s thread:%s",
		vcpu_state_to_str(vcpu_get_state(vcpu)),
		thread_state_to_str(vcpu->thread_obj.status));
	shell_item_line("requests:pending:0x%016lx arch-irqs:0x%016lx mask:0x%016lx",
		snapshot.pending_req, snapshot.irqs_pending,
		snapshot.irqs_pending_mask);
	shell_item_section("vcpu stats");
	/*
	 * vcpu stats keeps the execution context compact. Timer/vGIC-specific
	 * evidence is printed below without the older shadow-register block.
	 */
	shell_item_line("guest regs:");
	shell_dumpstat_regs(regs);
	shell_dumpstat_guest_trace(&debug->guest_trace);
	if (vcpu_get_state(vcpu) != VCPU_OFFLINE) {
		shell_item_line("vcpu stack:");
		shell_dumpstat_vm_stack(vcpu, regs);
		shell_item_line("pcpu stack:");
		shell_dumpstat_host_stack(vcpu);
	}
	shell_item_section("vgic/vtimer");
	shell_dumpstat_timer_state(&snapshot);
	shell_dumpstat_vtimer_diag(&vcpu->arch.debug.vtimer_diag);
	shell_dumpstat_vtimer_trace(&debug->vtimer_trace);
	shell_item_end();

	return 0;
}

static int32_t shell_dumpstat(int32_t argc, char **argv)
{
	struct acrn_vm *vm;
	int64_t param;
	uint16_t vm_id = 0U;
	uint16_t vcpu_id;

	if (argc > 2) {
		return -EINVAL;
	}

	if (argc == 2) {
		param = strtol_deci(argv[1]);
		if ((param < 0) || (param >= CONFIG_MAX_VM_NUM)) {
			return -EINVAL;
		}
		vm_id = (uint16_t)param;
	}

	vm = get_vm_from_vmid(vm_id);
	if (is_poweroff_vm(vm)) {
		return -EINVAL;
	}

	shell_item_begin("dumpstat vm%hu:%s", vm_id, vm->name);
	shell_item_line("vcpus:%hu", vm->hw.created_vcpus);
	shell_item_end();

	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		(void)shell_dumpstat_vcpu(vcpu_from_vid(vm, vcpu_id));
	}

	return 0;
}

static int32_t shell_coredump(int32_t argc, char **argv)
{
	if (argc != 2) {
		shell_puts("usage: coredump <print|erase>\r\n");
		return -EINVAL;
	}

	if (strncmp(argv[1], "print", sizeof("print")) == 0) {
		if (!arm64_coredump_print_stored(LOG_INFO)) {
			shell_puts("coredump: no valid stored snapshot\r\n");
		}
		return 0;
	}
	if (strncmp(argv[1], "erase", sizeof("erase")) == 0) {
		arm64_coredump_erase_stored();
		shell_puts("coredump: erased\r\n");
		return 0;
	}

	shell_puts("usage: coredump <print|erase>\r\n");
	return -EINVAL;
}

static const char *shell_vm_state_to_str(enum vm_state state)
{
	const char *str;

	switch (state) {
	case VM_POWERED_OFF:
		str = "poweroff";
		break;
	case VM_CREATED:
		str = "created";
		break;
	case VM_RUNNING:
		str = "running";
		break;
	case VM_READY_TO_POWEROFF:
		str = "ready-off";
		break;
	case VM_PAUSED:
		str = "paused";
		break;
	default:
		str = "N/A";
		break;
	}

	return str;
}

/* [20260630] vmstat monitor:
 *
 * vmstat is the broad health summary before using dumpstat. It keeps one VM
 * visible at a time: configured resources, runtime state, watchdog and console
 * status, scheduler diagnostics, and the guest timer/vGIC delivery summary.
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
			shell_item_line("vm%hu:%s vcpu%hu pcpu:- llc:-", vm_id, name, vcpu_id);
		} else {
			shell_item_line("vm%hu:%s vcpu%hu pcpu:%hu llc:%u",
				vm_id, name, vcpu_id, pcpu_id, llc_id);
		}
	}
}

static int32_t shell_cachestat(int32_t argc, __unused char **argv)
{
	struct arm64_cache_info info;
	uint32_t idx;
	uint16_t vm_id;

	if (argc != 1) {
		return -EINVAL;
	}

	arm64_cache_get_info(&info);
	shell_item_begin("cachestat");
	shell_item_line("valid:%s ctr:0x%016lx clidr:0x%016lx line:d:%u i:%u",
		shell_yes_no(info.valid), info.ctr_el0, info.clidr_el1,
		info.dcache_line_size, info.icache_line_size);
	shell_item_line("llc:domains:%u level:%u type:%s size:%luKB mask:0x%016lx",
		info.llc_domain_count, info.llc_level, arm64_cache_type_str(info.llc_type),
		info.llc_size / 1024UL, info.llc_pcpu_mask);

	if (info.leaf_count == 0U) {
		shell_item_line("cache:none");
	} else {
		shell_item_line("cache leaves:");
		shell_item_line("level  type     line  sets   ways   sizeKB  shared-pcpu-mask");
		shell_item_line("─────  ───────  ────  ─────  ─────  ──────  ────────────────");
		for (idx = 0U; idx < info.leaf_count; idx++) {
			const struct arm64_cache_leaf *leaf = &info.leaves[idx];

			shell_item_line("%-5u  %-7s  %-4u  %-5u  %-5u  %-6lu  0x%016lx",
				leaf->level, arm64_cache_type_str(leaf->type), leaf->line_size,
				leaf->sets, leaf->ways, leaf->size / 1024UL,
				leaf->shared_pcpu_mask);
		}
	}

	shell_item_line("vm llc placement:");
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (!shell_vm_config_present(vm_config) &&
			(vm->hw.created_vcpus == 0U) && is_poweroff_vm(vm)) {
			continue;
		}
		shell_cachestat_print_vm_affinity(vm_id, vm_config, vm);
	}
	shell_item_end();
	return 0;
}

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

static int32_t shell_ipcstat(int32_t argc, __unused char **argv)
{
	struct arm64_vipc_channel_stats stats[ARM64_VIPC_MAX_STATIC_CHANNELS];
	uint32_t count;
	uint32_t idx;
	uint64_t now;

	if (argc != 1) {
		return -EINVAL;
	}

	count = arm64_vipc_get_stats(stats, ARRAY_SIZE(stats));
	now = cpu_ticks();

	shell_item_begin("ipcstat");
	if (count == 0U) {
		shell_item_line("channels:none");
		shell_item_end();
		return 0;
	}

	for (idx = 0U; idx < count; idx++) {
		shell_item_line("ch%u ep0:vm%hu ep1:vm%hu gpa:0x%016lx ring:%u count:%u mapped:0x%08x virq:%u bad:%lu",
			stats[idx].channel_id, stats[idx].endpoint_vmid[0],
			stats[idx].endpoint_vmid[1], stats[idx].gpa_base,
			stats[idx].ring_size, stats[idx].ring_count,
			stats[idx].mapped_mask, stats[idx].notify_virq,
			stats[idx].bad_hcall_count);
		shell_ipcstat_print_dir(&stats[idx], ACRN_IPC_DIR_EP0_TO_EP1, now);
		shell_ipcstat_print_dir(&stats[idx], ACRN_IPC_DIR_EP1_TO_EP0, now);
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
		(void)snprintf(console, sizeof(console), "%u/%u d:%lu",
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

	shell_item_line("vm%-2hu %-10s %-9s %2hu/%-2hu %-7s %-10s %-20s %-6s result:%s",
		health->vm_id, health->name, shell_vm_state_to_str(health->state),
		health->created_vcpus, health->configured_vcpus,
		wdt_status, wdt_age, console, virtio,
		shell_health_level_to_str(health->level));
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
		shell_item_line("host: hv-s1 page-table pool exhausted");
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_VM_S2_FULL) != 0UL) {
		shell_item_line("host: vm-s2 page-table pool exhausted");
		printed = true;
	}
	if ((host->reasons & SHELL_HEALTH_HOST_S2_OWNERSHIP) != 0UL) {
		shell_item_line("host: vm-s2 ownership mismatch unowned:%lu over:%lu",
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
			shell_item_line("vm%hu:%s watchdog stuck cause:%s age:%lums", vm_id,
				health->name, shell_vmstat_wdt_cause_to_str(health->wdt.cause),
				health->wdt.last_ms);
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
	}

	if (!printed) {
		shell_item_line("none");
	}
}

static int32_t shell_health(int32_t argc, __unused char **argv)
{
	struct shell_health_host host;
	struct shell_health_vm vms[CONFIG_MAX_VM_NUM];
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

	shell_item_begin("health");
	shell_item_line("overall:%s uptime:%lums",
		shell_health_level_to_str(overall), ticks_to_ms(cpu_ticks()));
	shell_item_section("Host");
	shell_item_line("pcpus:active:%hu/%hu current:%hu/%hu result:%s",
		host.pcpu_active, host.pcpu_total, host.pcpu_current, host.pcpu_total,
		shell_health_level_to_str(host.level));
	shell_item_line("pages:hv-s1:%lu/%lu vm-s2:%lu/%lu accounted:%lu unowned:%lu over:%lu malformed:%lu",
		host.hv_s1.used_pages, host.hv_s1.total_pages,
		host.vm_s2.used_pages, host.vm_s2.total_pages,
		host.stage2_accounted, host.stage2_unowned,
		host.stage2_overaccounted, host.stage2_malformed);
	shell_item_section("Virtual machines");
	shell_item_line("vm   name       state     vcpus wdt     age        console              virtio result");
	shell_item_line("──── ────────── ───────── ───── ─────── ────────── ──────────────────── ────── ───────────");
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (vms[vm_id].present) {
			shell_health_print_vm(&vms[vm_id]);
		}
	}
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
			const struct arm64_vcpu_vtimer_diag *diag = &vcpu->arch.debug.vtimer_diag;

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
	struct virtio_console_stats vcon = { 0U };
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
		shell_item_line("vits:q ctlr:%s cbaser:%s writer:0x%016lx reader:0x%016lx cmds:%lu invalid:%lu unsupported:%lu qerr:%lu copy-fail:%lu budget:%lu",
			shell_yes_no(vits.ctlr_enabled), shell_yes_no(vits.cbaser_valid),
			vits.cwriter, vits.creadr, vits.cmd_processed,
			vits.cmd_invalid, vits.cmd_unsupported,
			vits.cmd_queue_errors, vits.cmd_copy_fail,
			vits.cmd_budget_exhausted);
		shell_item_line("vits:tables dev:%u evt:%u col:%u cfg:%lu/%lu mmio:%lu/%lu trans:%lu inject:%lu/%lu no-event:%lu bad-target:%lu",
			vits.devices, vits.events, vits.collections,
			vits.config_update_ok, vits.config_update_fail,
			vits.mmio_read, vits.mmio_write, vits.translater_write,
			vits.inject_ok, vits.inject_fail,
			vits.inject_no_event, vits.inject_bad_target);
		shell_item_line("vits:last op:0x%02x dev:%u event:%u lpi:%u col:%hu target:%hu ret:%d",
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

		shell_item_line("HWT:status:%7s cause:%12s (%02lu.%03lu) kick:%08lu timeout:%02lu restart:%02lu fail:%02lu pending:%s recovery:%10s wait:0x%02lx token:0x%016lx",
			shell_vmstat_wdt_status_to_str(wdt.status),
			shell_vmstat_wdt_cause_to_str(wdt.cause), last_sec, last_msec,
			wdt.kick_count, wdt.timeout_count, wdt.restart_count,
			wdt.restart_fail_count, shell_yes_no(wdt.restart_pending),
			shell_vmstat_wdt_recovery_to_str(wdt.recovery_state),
			wdt.recovery_wait_vcpus, wdt.last_token);
	}
	shell_item_line("console:selected:%s bound:%s ring:%u/%u drain:%u pending:%s",
		shell_yes_no(console_vmid == vm_id), shell_yes_no(ring.vuart_bound),
		ring.queued, ring.capacity, ring.drain_budget,
		shell_yes_no(ring.pending));

	if (vu != NULL) {
		shell_item_line("        vuart:active:%s irq:%u rx:%u tx:%u ier:0x%02x lsr:0x%02x",
			shell_yes_no(vu->active), vu->irq, vuart_rx_numchars(vu),
			vu->txfifo.num, vu->ier, vu->lsr);
	}
	if (virtio_console_get_stats(vm_id, &vcon)) {
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
		for (uint16_t qid = 0U; qid < VIRTIO_CONSOLE_STAT_QUEUE_NUM; qid++) {
			const struct virtio_console_queue_stats *queue = &vcon.queues[qid];

			shell_item_line("        vcon.q%hu ready:%s num:%hu idx:%hu desc:0x%016lx avail:0x%016lx used:0x%016lx",
				qid, shell_yes_no(queue->ready), queue->num,
				queue->last_avail_idx, queue->desc, queue->avail,
				queue->used);
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
	const struct arm64_vcpu_vtimer_diag *diag = &vcpu->arch.debug.vtimer_diag;
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
			thread_state_to_str(vcpu->thread_obj.status),
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
	}
}

static int32_t shell_vmstat(int32_t argc, __unused char **argv)
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
	}

	return 0;
}

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

static int32_t shell_virtiostat(int32_t argc, __unused char **argv)
{
	if (argc != 1) {
		shell_puts("usage: virtiostat\r\n");
		return -EINVAL;
	}

	shell_virtiostat_print_summary();
	return 0;
}

static const char *shell_pm_mode_to_str(uint8_t mode)
{
	const char *name;

	switch (mode) {
	case HV_PM_PLATFORM_SIMULATED:
		name = "simulated";
		break;
	case HV_PM_PLATFORM_STRICT:
		name = "strict";
		break;
	default:
		name = "disabled";
		break;
	}

	return name;
}

static uint64_t shell_pm_phase_ticks(const struct beau_pm_snapshot *snapshot,
	uint32_t phase)
{
	uint64_t duration = 0UL;

	if (phase < HV_PM_PHASE_COUNT) {
		duration = snapshot->phase_duration_ticks[phase];
		if ((duration == 0UL) && (snapshot->state == phase) &&
			(snapshot->phase_start_ticks[phase] != 0UL)) {
			duration = cpu_ticks() - snapshot->phase_start_ticks[phase];
		}
	}

	return duration;
}

static void shell_pm_print_snapshot(const struct beau_pm_snapshot *snapshot,
	bool verbose)
{
	uint64_t visible_vm_mask;
	uint16_t vmid;
	uint32_t phase;

	shell_item_begin("pm epoch:%lu", snapshot->epoch);
	if (snapshot->target_vmid < CONFIG_MAX_VM_NUM) {
		shell_item_line("phase:%s scope:%s owner:vm%hu target:vm%hu controller:vm%hu enabled:%s mode:%s",
			hv_pm_state_to_str((enum beau_pm_system_state)snapshot->state),
			hv_pm_scope_to_str((enum beau_pm_scope)snapshot->scope),
			snapshot->initiator_vmid, snapshot->target_vmid,
			snapshot->controller_vmid,
			shell_yes_no(snapshot->enabled != 0U),
			shell_pm_mode_to_str(snapshot->platform_mode));
	} else {
		shell_item_line("phase:%s scope:%s owner:vm%hu target:N/A controller:vm%hu enabled:%s mode:%s",
			hv_pm_state_to_str((enum beau_pm_system_state)snapshot->state),
			hv_pm_scope_to_str((enum beau_pm_scope)snapshot->scope),
			snapshot->initiator_vmid, snapshot->controller_vmid,
			shell_yes_no(snapshot->enabled != 0U),
			shell_pm_mode_to_str(snapshot->platform_mode));
	}
	shell_item_line("masks:policy:0x%016lx required:0x%016lx io-gated:0x%016lx topology:0x%016lx hooks:0x%016lx",
		snapshot->policy_required_vm_mask, snapshot->required_vm_mask,
		snapshot->io_gated_vm_mask, snapshot->topology_change_vm_mask,
		snapshot->completed_hook_mask);
	shell_item_line("timeouts:prepare:%ums resume:%ums io-gated:%s",
		snapshot->prepare_timeout_ms, snapshot->resume_timeout_ms,
		shell_yes_no(snapshot->io_gated != 0U));
	shell_item_line("platform-caps:0x%08x", platform_pm_capabilities());
	shell_item_line("wake:reason:%lu bitmap:0x%016lx",
		snapshot->wake_reason, snapshot->wake_bitmap);
	shell_item_line("last:epoch:%lu phase:%s status:%d error:epoch:%lu phase:%s vm:%hu status:%d",
		snapshot->last_epoch,
		hv_pm_state_to_str((enum beau_pm_system_state)snapshot->last_state),
		snapshot->last_status, snapshot->last_error.epoch,
		hv_pm_state_to_str((enum beau_pm_system_state)snapshot->last_error.phase),
		snapshot->last_error.vmid, snapshot->last_error.status);
	shell_item_line("current-duration.us:%lu",
		ticks_to_us(shell_pm_phase_ticks(snapshot, snapshot->state)));

	if (verbose) {
		visible_vm_mask = snapshot->policy_required_vm_mask |
			snapshot->required_vm_mask | snapshot->io_gated_vm_mask;
		for (phase = 0U; phase < HV_PM_PHASE_COUNT; phase++) {
			uint64_t duration = shell_pm_phase_ticks(snapshot, phase);

			if ((duration != 0UL) || (snapshot->state == phase)) {
				shell_item_line("phase:%-16s duration.us:%lu",
					hv_pm_state_to_str((enum beau_pm_system_state)phase),
					ticks_to_us(duration));
			}
		}
		for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
			const struct beau_vm_pm_record *record = &snapshot->vm[vmid];

			if ((visible_vm_mask & (1UL << vmid)) != 0UL) {
				shell_item_line("vm%hu epoch:%lu state:%u prior:%u required:%u status:%d gated:0x%016lx active:0x%016lx frozen:0x%016lx wake-owned:0x%016lx",
					vmid, record->epoch, record->state,
					record->prior_vm_state, record->required, record->status,
					record->gated_vcpu_mask, record->active_vcpu_mask,
					record->frozen_vcpu_mask,
					record->wake_owned_vcpu_mask);
			}
		}
	}
	shell_item_end();
}

static bool shell_pm_parse_vmid(const char *arg, uint16_t *vmid)
{
	uint32_t value = 0U;
	uint32_t idx;

	if ((arg == NULL) || (arg[0] == '\0') || (vmid == NULL)) {
		return false;
	}
	for (idx = 0U; arg[idx] != '\0'; idx++) {
		if ((arg[idx] < '0') || (arg[idx] > '9')) {
			return false;
		}
		value = (value * 10U) + (uint32_t)(arg[idx] - '0');
		if (value >= CONFIG_MAX_VM_NUM) {
			return false;
		}
	}
	*vmid = (uint16_t)value;

	return true;
}

static int32_t shell_pm_reboot_vm(uint16_t vmid)
{
	struct acrn_vm *vm = get_vm_from_vmid(vmid);

	if (is_service_vm(vm)) {
		shell_puts("refuse to reboot service VM from shell\r\n");
		return -EPERM;
	}
	if (is_poweroff_vm(vm)) {
		shell_puts("vm is powered off\r\n");
		return -EINVAL;
	}

	return request_vm_cold_restart(vm);
}

static int32_t shell_pm(int32_t argc, char **argv)
{
	struct beau_pm_snapshot snapshot;
	uint16_t vmid;

	if ((argc < 2) || (argc > 3)) {
		shell_puts("usage: pm <suspend|resume|reboot> <vmid> | status\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "status") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		hv_pm_get_snapshot(&snapshot);
		shell_pm_print_snapshot(&snapshot, false);
		return 0;
	}
	if (strcmp(argv[1], "suspend") == 0) {
		if ((argc != 3) || !shell_pm_parse_vmid(argv[2], &vmid)) {
			return -EINVAL;
		}
		return bsp_pm_suspend_vm(vmid);
	}
	if (strcmp(argv[1], "resume") == 0) {
		if ((argc != 3) || !shell_pm_parse_vmid(argv[2], &vmid)) {
			return -EINVAL;
		}
		return bsp_pm_resume_vm(vmid);
	}
	if (strcmp(argv[1], "reboot") == 0) {
		if ((argc != 3) || !shell_pm_parse_vmid(argv[2], &vmid)) {
			return -EINVAL;
		}
		return shell_pm_reboot_vm(vmid);
	}

	return -EINVAL;
}

static int32_t shell_pmstat(int32_t argc, __unused char **argv)
{
	struct beau_pm_snapshot snapshot;

	if (argc != 1) {
		return -EINVAL;
	}
	hv_pm_get_snapshot(&snapshot);
	shell_pm_print_snapshot(&snapshot, true);

	return 0;
}

static int32_t shell_reboot(__unused int32_t argc, __unused char **argv)
{
	reset_host(false);
	return 0;
}
