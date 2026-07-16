/*
 * Copyright (C) 2018-2026 Intel Corporation.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HV_PM_H
#define HV_PM_H

#include <asm/hv_pm.h>
#include <logmsg.h>
#include <spinlock.h>
#include <vm_config.h>

#define HV_PM_DEFAULT_CONTROLLER_VM	2U
#define HV_PM_MAX_HOOKS		32U

enum beau_pm_system_state {
	PM_RUNNING = 0U,
	PM_PREPARING,
	PM_FREEZING_HOST,
	PM_SUSPENDED,
	PM_RESTORING_HOST,
	PM_ABORTING,
	PM_FAILED,
};

#define HV_PM_PHASE_COUNT		((uint32_t)PM_FAILED + 1U)

enum beau_pm_scope {
	HV_PM_SCOPE_NONE = 0U,
	HV_PM_SCOPE_SYSTEM,
	HV_PM_SCOPE_VM,
};

enum beau_vm_pm_state {
	VM_PM_RUNNING = 0U,
	VM_PM_FROZEN,
	VM_PM_FAILED,
};

enum beau_pm_platform_mode {
	HV_PM_PLATFORM_DISABLED = 0U,
	HV_PM_PLATFORM_SIMULATED,
	HV_PM_PLATFORM_STRICT,
};

struct beau_pm_policy {
	uint64_t required_vm_mask;
	uint32_t prepare_timeout_ms;
	uint32_t resume_timeout_ms;
	uint16_t controller_vmid;
	uint8_t enabled;
	uint8_t platform_mode;
};

struct beau_vm_pm_record {
	uint64_t epoch;
	uint64_t gated_vcpu_mask;
	uint64_t active_vcpu_mask;
	uint64_t frozen_vcpu_mask;
	uint64_t wake_owned_vcpu_mask;
	int32_t status;
	uint32_t state;
	uint32_t prior_vm_state;
	uint16_t vmid;
	uint8_t required;
	uint8_t reserved[5U];
};

struct beau_pm_phase_error {
	uint64_t epoch;
	int32_t status;
	uint32_t phase;
	uint16_t vmid;
	uint16_t reserved;
};

struct beau_pm_snapshot {
	uint64_t epoch;
	uint64_t last_epoch;
	uint64_t required_vm_mask;
	uint64_t completed_hook_mask;
	uint64_t wake_reason;
	uint64_t wake_bitmap;
	uint64_t topology_change_vm_mask;
	uint64_t io_gated_vm_mask;
	uint64_t phase_start_ticks[HV_PM_PHASE_COUNT];
	uint64_t phase_duration_ticks[HV_PM_PHASE_COUNT];
	struct beau_vm_pm_record vm[CONFIG_MAX_VM_NUM];
	struct beau_pm_phase_error last_error;
	uint32_t state;
	uint32_t last_state;
	int32_t last_status;
	uint16_t initiator_vmid;
	uint16_t controller_vmid;
	uint16_t target_vmid;
	uint8_t scope;
	uint8_t reserved0;
	uint32_t prepare_timeout_ms;
	uint32_t resume_timeout_ms;
	uint64_t policy_required_vm_mask;
	uint8_t io_gated;
	uint8_t enabled;
	uint8_t platform_mode;
	uint8_t reserved[5U];
};

struct beau_pm_transaction {
	spinlock_t lock;
	struct beau_pm_snapshot data;
};

typedef int32_t (*beau_pm_hook_fn)(uint64_t epoch);

struct beau_pm_ops {
	const char *name;
	uint16_t priority;
	uint16_t reserved;
	beau_pm_hook_fn prepare;
	beau_pm_hook_fn suspend;
	beau_pm_hook_fn resume;
	beau_pm_hook_fn abort;
};

const char *hv_pm_state_to_str(enum beau_pm_system_state state);
const char *hv_pm_scope_to_str(enum beau_pm_scope scope);
int32_t hv_pm_request_suspend(uint16_t initiator_vmid);
int32_t hv_pm_request_vm_suspend(uint16_t vmid);
int32_t hv_pm_request_vm_resume(uint16_t vmid);
int32_t hv_pm_abort(uint64_t epoch, int32_t reason);
void hv_pm_get_snapshot(struct beau_pm_snapshot *snapshot);
bool hv_pm_io_is_gated(void);
bool hv_pm_vm_io_is_gated(uint16_t vmid);
int32_t hv_pm_set_policy(const struct beau_pm_policy *policy);
int32_t hv_pm_record_wake(uint32_t wake_source, uint16_t source_index);
int32_t hv_pm_begin_vm_topology_change(uint16_t vmid);
void hv_pm_end_vm_topology_change(uint16_t vmid);
void make_system_suspend_request(uint16_t pcpu_id);
bool has_system_suspend_request(uint16_t pcpu_id);
bool need_system_suspend(uint16_t pcpu_id);
void hv_pm_process_from_idle(uint16_t pcpu_id);
int32_t hv_pm_register_hook(const struct beau_pm_ops *ops);
void hv_pm_finalize_hooks(void);
int32_t hv_pm_run_prepare(uint64_t epoch);
int32_t hv_pm_run_suspend(uint64_t epoch);
int32_t hv_pm_run_resume(uint64_t epoch);
int32_t hv_pm_run_abort(uint64_t epoch);
uint32_t platform_pm_capabilities(void);
int32_t platform_pm_preflight(uint8_t platform_mode);
int32_t platform_pm_enter(uint64_t epoch, bool *host_restored);

void arch_shutdown_host(void);
void arch_reset_host(bool warm);

static inline void shutdown_host(void) {
	LOG_INF("shutting down BEAU");
	arch_shutdown_host();
}

static inline void reset_host(bool warm) {
	LOG_INF("%s rebooting BEAU", warm ? "warm" : "cold");
	arch_reset_host(warm);
}

#endif /* HV_PM_H */
