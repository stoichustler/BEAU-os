/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VM_WDT_H
#define VM_WDT_H

#include <types.h>

#ifndef CONFIG_VM_WDT_MONITOR_VM_NUM
#define CONFIG_VM_WDT_MONITOR_VM_NUM	3U
#endif

#ifndef CONFIG_VM_WDT_PRINT_PERIOD_MS
#define CONFIG_VM_WDT_PRINT_PERIOD_MS	5000U
#endif

#ifndef CONFIG_VM_WDT_TIMEOUT_MS
#define CONFIG_VM_WDT_TIMEOUT_MS	15000U
#endif

/*
 * A bit set for VM n permits watchdog recovery for that VM.  A VM must opt in
 * only when its guest image includes a periodic HC_VM_WDT_KICK source.
 */
#ifndef CONFIG_VM_WDT_RESTART_VM_MASK
#define CONFIG_VM_WDT_RESTART_VM_MASK	0UL
#endif

#ifndef CONFIG_VM_WDT_RESTART_MAX
#define CONFIG_VM_WDT_RESTART_MAX	3U
#endif

#ifndef CONFIG_VM_WDT_RESTART_QUIESCE_TIMEOUT_MS
#define CONFIG_VM_WDT_RESTART_QUIESCE_TIMEOUT_MS	1000U
#endif

#ifndef CONFIG_VM_WDT_RESTART_POLL_MS
#define CONFIG_VM_WDT_RESTART_POLL_MS	20U
#endif

struct acrn_vm;

enum vm_wdt_status {
	VM_WDT_STATUS_UNUSED = 0U,
	VM_WDT_STATUS_OFFLINE,
	VM_WDT_STATUS_UNKNOWN,
	VM_WDT_STATUS_ALIVE,
	VM_WDT_STATUS_STUCK,
};

enum vm_wdt_cause {
	VM_WDT_CAUSE_NONE = 0U,
	VM_WDT_CAUSE_HEARTBEAT,
	VM_WDT_CAUSE_TIMEOUT,
	VM_WDT_CAUSE_VCPU_STALL,
	VM_WDT_CAUSE_IRQ_STORM,
	VM_WDT_CAUSE_CONSOLE_STUCK,
	VM_WDT_CAUSE_VIRTIO_STUCK,
};

enum vm_wdt_recovery_state {
	VM_WDT_RECOVERY_IDLE = 0U,
	VM_WDT_RECOVERY_QUIESCING,
	VM_WDT_RECOVERY_RESETTING,
	VM_WDT_RECOVERY_VERIFYING,
};

struct vm_wdt_snapshot {
	enum vm_wdt_status status;
	enum vm_wdt_cause cause;
	enum vm_wdt_recovery_state recovery_state;
	uint64_t last_ms;
	uint64_t timeout_count;
	uint64_t restart_count;
	uint64_t restart_fail_count;
	uint64_t last_token;
	uint64_t recovery_wait_vcpus;
	uint64_t observed_tsc;
	uint64_t start_tsc;
	uint64_t last_kick_tsc;
	uint64_t irq_total;
	uint64_t irq_delta;
	uint64_t daemon_merged;
	uint64_t daemon_dropped;
	uint32_t timeout_ms;
	bool restart_pending;
	bool heartbeat_started;
	bool timeout_active;
	bool restart_enabled;
	bool daemon_pending;
};

void vm_wdt_start(void);
void vm_wdt_reset(const struct acrn_vm *vm);
void vm_wdt_restart_complete(uint16_t vm_id, int32_t reset_ret);
void vm_wdt_kick(const struct acrn_vm *vm, uint64_t token);
int32_t vm_wdt_get_snapshot(uint16_t vm_id, struct vm_wdt_snapshot *snapshot);
int32_t vm_wdt_pm_suspend(uint64_t epoch);
int32_t vm_wdt_pm_resume(uint64_t epoch);
int32_t vm_wdt_pm_suspend_vm(uint16_t vm_id, uint64_t epoch);
int32_t vm_wdt_pm_resume_vm(uint16_t vm_id, uint64_t epoch);

#endif /* VM_WDT_H */
