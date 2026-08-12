/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <swtdbg.h>

#include "shell_cmds.h"

/*
 * This file owns the fixed command-to-handler association. shell.c only reads
 * these tables after shell_init() publishes them to the shell control block.
 */
#define CMD(_name, _param, _help, _fn, _completion) \
	{ .str = (_name), .cmd_param = (_param), .help_str = (_help), .fcn = (_fn), \
	  .completion = (_completion) }
#define SUBCMD(_name) \
	{ .str = (_name) }
#define SUBCMD_CHILDREN(_name, _children, _flags) \
	{ .str = (_name), .children = &(_children), \
	  .flags = (_flags) }
#define VALUE_CHILDREN(_children) \
	{ .children = &(_children), \
	  .flags = SHELL_COMPLETION_FLAG_VALUE }
#define COMPLETION_SET(_name, ...) \
	static const struct shell_completion _name##_entries[] = { __VA_ARGS__ }; \
	static const struct shell_completion_set _name = { \
		.entries = _name##_entries, .count = ARRAY_SIZE(_name##_entries) }

COMPLETION_SET(shell_schedai_completions,
	SUBCMD("snapshot"),
);

#if CONFIG_ARM64_SPE
COMPLETION_SET(shell_spestat_completions,
	SUBCMD("start"),
	SUBCMD("stop"),
	SUBCMD("reset"),
	SUBCMD("dump"),
);
#endif

COMPLETION_SET(shell_gen_completions,
	SUBCMD("panic"),
	SUBCMD("brk"),
	SUBCMD("esr"),
);

COMPLETION_SET(shell_crash_vmid_completions,
	SUBCMD("erase"),
);

COMPLETION_SET(shell_crash_completions,
	VALUE_CHILDREN(shell_crash_vmid_completions),
);

COMPLETION_SET(shell_coredump_completions,
	SUBCMD("print"),
	SUBCMD("erase"),
);

COMPLETION_SET(shell_trace_category_completions,
	SUBCMD("all"),
	SUBCMD("timer"),
	SUBCMD("sched"),
	SUBCMD("hcall"),
	SUBCMD("vm"),
	SUBCMD("virq"),
);

COMPLETION_SET(shell_trace_completions,
	SUBCMD("status"),
	SUBCMD_CHILDREN("start", shell_trace_category_completions,
		SHELL_COMPLETION_FLAG_REPEAT),
	SUBCMD("stop"),
	SUBCMD("clear"),
	SUBCMD("dump"),
);

#ifdef CONFIG_PERF
COMPLETION_SET(shell_perf_completions,
	SUBCMD("record"),
	SUBCMD("status"),
	SUBCMD("stop"),
	SUBCMD("clear"),
	SUBCMD("dump"),
);
#endif

COMPLETION_SET(shell_pm_completions,
	SUBCMD("suspend"),
	SUBCMD("resume"),
	SUBCMD("reboot"),
	SUBCMD("status"),
);

COMPLETION_SET(shell_pmustat_completions,
	SUBCMD("start"),
	SUBCMD("stop"),
	SUBCMD("reset"),
	SUBCMD("dump"),
);

COMPLETION_SET(shell_aes_completions,
	SUBCMD("enc"),
	SUBCMD("dec"),
);

COMPLETION_SET(shell_tee_completions,
	SUBCMD("version"),
	SUBCMD("dump"),
);

struct shell_cmd shell_common_cmds[] = {
	CMD("version", NULL, "display the hv version information", shell_version, NULL),
	CMD("clear", NULL, "clear the BEAU console screen", shell_clear, NULL),
	CMD("symtab", NULL, "list debug symbol offsets and names", shell_symtab, NULL),
	CMD("loglevel", "[<console_loglevel> [<mem_loglevel> [npk_loglevel]]]",
		"loglevel {0-6}", shell_loglevel, NULL),
	CMD("dmesg", "[count]", "dump retained host log records", shell_dmesg, NULL),
	CMD("vcpus", NULL, "list all vcpus in all vms", shell_list_vcpu, NULL),
	CMD("ps", NULL, "list scheduler threads, state, and CPU usage", shell_list_threads, NULL),
	CMD("schedstat", NULL, "list per-pcpu scheduler statistics", shell_schedstat, NULL),
	CMD("schedai", "snapshot", "emit configured AI scheduler telemetry",
		shell_schedai, &shell_schedai_completions),
#if CONFIG_ARM64_SPE
	CMD("spestat", "[start|stop|reset|dump <pcpu>]",
		"show or control EL2-owned Arm SPE capture", shell_spestat,
		&shell_spestat_completions),
#endif
	CMD("irqstat", NULL, "list host IRQ counts and ARM64 guest vIRQ latency", shell_irqstat, NULL),
	CMD("vsh", "<vm id>", "switch to the vm console. type `CTRL-D` switch to BEAU",
		shell_to_vm_console, NULL),
	CMD("ramlog", "<vm id>", "dump retained VM boot and watchdog log", shell_ramlog, NULL),
};
uint32_t shell_common_cmds_sz = ARRAY_SIZE(shell_common_cmds);

struct shell_cmd shell_arch_cmds[] = {
	CMD("devmap", NULL, "list arm64 host stage-1 and vm stage-2 memory mappings", shell_list_mem, NULL),
	CMD("memstat", NULL, "list ARM64 page-table pool and stage-2 ownership statistics", shell_memstat, NULL),
	CMD("walkpt", "<vmid> <ipa>", "walk one VM stage-2 translation without modifying it", shell_s2walk, NULL),
	CMD("kusg", NULL, "list BEAU static memory usage in KB", shell_kusg, NULL),
	CMD("gen", "<panic|brk|esr <hex>>", "generate controlled ARM64 diagnostic faults",
		shell_gen, &shell_gen_completions),
	CMD("swtdbg", "<vmid>", "show retained VM watchdog minidump", shell_swtdbg, NULL),
	CMD("crash", "<vmid> [erase]", "show or erase retained VM crash history",
		shell_crash, &shell_crash_completions),
	CMD("coredump", "<print|erase>", "print or erase the latest ARM64 coredump",
		shell_coredump, &shell_coredump_completions),
	CMD("vmstat", NULL, "list arm64 vm state", shell_vmstat, NULL),
	CMD("vmexitstat", NULL, "list cumulative synchronous vm-exit handler timing", shell_vmexitstat, NULL),
	CMD("cachestat", NULL, "list arm64 host cache and LLC domains", shell_cachestat, NULL),
	CMD("ipcstat", NULL, "list HVC and remoteproc IPC channels", shell_ipcstat, NULL),
	CMD("virtiostat", NULL, "list active virtio-proxy devices", shell_virtiostat, NULL),
	CMD("smmustat", NULL, "list ARM and guest-visible synthetic SMMUv3 state", shell_smmustat, NULL),
	CMD("pcistat", NULL, "list PCI passthrough and SMMU stream state", shell_pcistat, NULL),
	CMD("cpufreq", NULL, "list host CPU frequency policy state", shell_cpufreq, NULL),
	CMD("hwc", NULL, "read current pCPU hardware cycle counter", shell_hwc, NULL),
	CMD("rttest", NULL, "run local EL2 timer latency tests on every pCPU", shell_rttest, NULL),
	CMD("oslat", "[duration-ms]", "measure bounded EL2 polling gaps on every pCPU", shell_oslat, NULL),
	CMD("ipilat", "[samples]", "measure bounded EL2 IPI latency from BSP", shell_ipilat, NULL),
#ifdef CONFIG_COREMARK
	CMD("coremark", "[iterations]", "run asynchronous CoreMark contexts on every pCPU", shell_coremark, NULL),
#endif
#ifdef CONFIG_DHRYSTONE
	CMD("dhrystone", "[initial-runs]", "run serialized Dhrystone diagnostics on every pCPU", shell_dhrystone, NULL),
#endif
	CMD("trace", "<status|start|stop|clear|dump> [category|count]",
		"capture and dump per-pCPU EL2 trace events", shell_trace,
		&shell_trace_completions),
#ifdef CONFIG_PERF
	CMD("perf", "<record|status|stop|clear|dump> [duration-ms frequency-hz|count]",
		"sample and dump bounded ARM64 EL2 call stacks", shell_perf,
		&shell_perf_completions),
#endif
	CMD("reboot", NULL, "trigger a system reboot (immediately)", shell_reboot, NULL),
	CMD("pm", "<suspend|resume|reboot> <vmid> | status",
		"control VM power state and inspect PM state", shell_pm, &shell_pm_completions),
	CMD("pmstat", NULL, "list coordinated guest suspend transaction statistics", shell_pmstat, NULL),
	CMD("pmustat", "<start|stop|reset|dump>",
		"control and dump EL2-owned core PMU statistics", shell_pmustat,
		&shell_pmustat_completions),
	CMD("aes", "<enc|dec> <text|hex>",
		"verify ARMv8 AES text encryption and decryption", shell_aes,
		&shell_aes_completions),
	CMD("tee", "<version|dump>", "display Trusty TEE build or system information",
		shell_tee, &shell_tee_completions),
	{ .str = "ddb", .cmd_param = "<passwd>", .help_str = "enter the ARM64 kernel debugger",
		.fcn = shell_ddb, .flags = SHELL_CMD_FLAG_SENSITIVE_ARGS, .completion = NULL },
};
uint32_t shell_arch_cmds_sz = ARRAY_SIZE(shell_arch_cmds);
