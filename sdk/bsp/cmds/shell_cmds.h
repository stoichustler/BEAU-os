/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BSP_CMDS_SHELL_CMDS_H
#define BSP_CMDS_SHELL_CMDS_H

#include <types.h>
#include "../shell_priv.h"

/*
 * Registry descriptions are owned by shell_registry.c. shell.c publishes their
 * addresses during initialization; command handlers only consume this interface.
 */
extern struct shell_cmd shell_common_cmds[];
extern uint32_t shell_common_cmds_sz;
extern struct shell_cmd shell_arch_cmds[];
extern uint32_t shell_arch_cmds_sz;
extern uint16_t mem_loglevel;
extern uint16_t console_loglevel;
extern uint16_t npk_loglevel;

/* Console ownership transitions use this one shell-state operation. */
void shell_set_input_active(bool active);

/* Command handlers registered by shell_registry.c. */
int32_t shell_version(int32_t argc, char **argv);
int32_t shell_clear(int32_t argc, char **argv);
int32_t shell_symtab(int32_t argc, char **argv);
int32_t shell_loglevel(int32_t argc, char **argv);
int32_t shell_list_vcpu(int32_t argc, char **argv);
int32_t shell_list_threads(int32_t argc, char **argv);
int32_t shell_schedstat(int32_t argc, char **argv);
int32_t shell_schedai(int32_t argc, char **argv);
int32_t shell_spestat(int32_t argc, char **argv);
int32_t shell_irqstat(int32_t argc, char **argv);
int32_t shell_to_vm_console(int32_t argc, char **argv);
int32_t shell_ramlog(int32_t argc, char **argv);
int32_t shell_list_mem(int32_t argc, char **argv);
int32_t shell_memstat(int32_t argc, char **argv);
int32_t shell_s2walk(int32_t argc, char **argv);
int32_t shell_kusg(int32_t argc, char **argv);
int32_t shell_health(int32_t argc, char **argv);
int32_t shell_coredump(int32_t argc, char **argv);
int32_t shell_vmstat(int32_t argc, char **argv);
int32_t shell_cachestat(int32_t argc, char **argv);
int32_t shell_ipcstat(int32_t argc, char **argv);
int32_t shell_virtiostat(int32_t argc, char **argv);
int32_t shell_smmustat(int32_t argc, char **argv);
int32_t shell_pcistat(int32_t argc, char **argv);
int32_t shell_cpufreq(int32_t argc, char **argv);
int32_t shell_rttest(int32_t argc, char **argv);
int32_t shell_trace(int32_t argc, char **argv);
#ifdef CONFIG_PERF
int32_t shell_perf(int32_t argc, char **argv);
#endif
int32_t shell_reboot(int32_t argc, char **argv);
int32_t shell_pm(int32_t argc, char **argv);
int32_t shell_pmstat(int32_t argc, char **argv);
int32_t shell_pmustat(int32_t argc, char **argv);
int32_t shell_aes(int32_t argc, char **argv);
int32_t shell_ddb(int32_t argc, char **argv);

/* Stateless strings used only while rendering command output. */
const char *shell_yes_no(bool value);
const char *shell_thread_state_to_str(uint32_t state);
const char *shell_vm_state_to_str(uint32_t state);

#endif /* BSP_CMDS_SHELL_CMDS_H */
