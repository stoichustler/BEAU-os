/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <hwtdbg.h>

#include "shell_cmds.h"

/*
 * This file owns the fixed command-to-handler association. shell.c only reads
 * these tables after shell_init() publishes them to the shell control block.
 */
#define CMD(_name, _param, _help, _fn) \
	{ .str = (_name), .cmd_param = (_param), .help_str = (_help), .fcn = (_fn) }

struct shell_cmd shell_common_cmds[] = {
	CMD("version", NULL, "display the hv version information", shell_version),
	CMD("clear", NULL, "clear the BEAU console screen", shell_clear),
	CMD("symtab", NULL, "list debug symbol offsets and names", shell_symtab),
	CMD("loglevel", "[<console_loglevel> [<mem_loglevel> [npk_loglevel]]]",
		"loglevel {0-6}", shell_loglevel),
	CMD("vcpus", NULL, "list all vcpus in all vms", shell_list_vcpu),
	CMD("ps", NULL, "list scheduler threads, state, and CPU usage", shell_list_threads),
	CMD("schedstat", NULL, "list per-pcpu scheduler statistics", shell_schedstat),
	CMD("schedai", "snapshot", "emit configured AI scheduler telemetry", shell_schedai),
#if CONFIG_ARM64_SPE
	CMD("spestat", "[start|stop|reset|dump <pcpu>]",
		"show or control EL2-owned Arm SPE capture", shell_spestat),
#endif
	CMD("irqstat", NULL, "list host IRQ counts and ARM64 guest vIRQ latency", shell_irqstat),
	CMD("vsh", "<vm id>", "switch to the vm console. type `CTRL-D` switch to BEAU",
		shell_to_vm_console),
	CMD("ramlog", "<vm id>", "dump retained VM boot and watchdog log", shell_ramlog),
};
uint32_t shell_common_cmds_sz = ARRAY_SIZE(shell_common_cmds);

struct shell_cmd shell_arch_cmds[] = {
	CMD("devmap", NULL, "list arm64 host stage-1 and vm stage-2 memory mappings", shell_list_mem),
	CMD("memstat", NULL, "list ARM64 page-table pool and stage-2 ownership statistics", shell_memstat),
	CMD("s2walk", "<vmid> <ipa>", "walk one VM stage-2 translation without modifying it", shell_s2walk),
	CMD("kusg", NULL, "list BEAU static memory usage in KB", shell_kusg),
	CMD("health", NULL, "summarize current host and VM operational health", shell_health),
	CMD("hwtdbg", "<vmid>", "show retained VM watchdog minidump", shell_hwtdbg),
	CMD("coredump", "<print|erase>", "print or erase the latest ARM64 coredump", shell_coredump),
	CMD("vmstat", NULL, "list arm64 vm state", shell_vmstat),
	CMD("cachestat", NULL, "list arm64 host cache and LLC domains", shell_cachestat),
	CMD("ipcstat", NULL, "list HVC and remoteproc IPC channels", shell_ipcstat),
	CMD("virtiostat", NULL, "list active virtio-proxy devices", shell_virtiostat),
	CMD("smmustat", NULL, "list ARM and guest-visible synthetic SMMUv3 state", shell_smmustat),
	CMD("pcistat", NULL, "list PCI passthrough and SMMU stream state", shell_pcistat),
	CMD("cpufreq", NULL, "list host CPU frequency policy state", shell_cpufreq),
	CMD("rttest", NULL, "run local EL2 timer latency tests on every pCPU", shell_rttest),
	CMD("trace", "<status|start|stop|clear|dump> [category|count]",
		"capture and dump per-pCPU EL2 trace events", shell_trace),
#ifdef CONFIG_PERF
	CMD("perf", "<record|status|stop|clear|dump> [duration-ms frequency-hz|count]",
		"sample and dump bounded ARM64 EL2 call stacks", shell_perf),
#endif
	CMD("reboot", NULL, "trigger a system reboot (immediately)", shell_reboot),
	CMD("pm", "<suspend|resume|reboot> <vmid> | status",
		"control VM power state and inspect PM state", shell_pm),
	CMD("pmstat", NULL, "list coordinated guest suspend transaction statistics", shell_pmstat),
	CMD("pmustat", "<start|stop|reset|dump>", "control and dump EL2-owned core PMU statistics", shell_pmustat),
	CMD("aes", "<enc|dec> <text|hex>", "verify ARMv8 AES text encryption and decryption", shell_aes),
	{ .str = "ddb", .cmd_param = "<passwd>", .help_str = "enter the ARM64 kernel debugger",
		.fcn = shell_ddb, .flags = SHELL_CMD_FLAG_SENSITIVE_ARGS },
};
uint32_t shell_arch_cmds_sz = ARRAY_SIZE(shell_arch_cmds);
