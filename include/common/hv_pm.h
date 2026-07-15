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
	PM_GUESTS_QUIESCED,
	PM_FREEZING_HOST,
	PM_SUSPENDED,
	PM_RESTORING_HOST,
	PM_RESUMING_GUESTS,
	PM_ABORTING,
	PM_FAILED,
};

#define HV_PM_PHASE_COUNT		((uint32_t)PM_FAILED + 1U)

enum beau_vm_pm_state {
	VM_PM_RUNNING = 0U,
	VM_PM_PREPARE_SENT,
	VM_PM_SUSPEND_PENDING,
	VM_PM_SUSPENDED,
	VM_PM_RESUMING,
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
	uint64_t resume_entry;
	uint64_t context_id;
	int32_t status;
	uint32_t state;
	uint16_t vmid;
	uint8_t required;
	uint8_t reserved;
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
	uint64_t ready_vm_mask;
	uint64_t resume_pending_vm_mask;
	uint64_t completed_hook_mask;
	uint64_t wake_reason;
	uint64_t wake_bitmap;
	uint64_t phase_start_ticks[HV_PM_PHASE_COUNT];
	uint64_t phase_duration_ticks[HV_PM_PHASE_COUNT];
	struct beau_vm_pm_record vm[CONFIG_MAX_VM_NUM];
	struct beau_pm_phase_error last_error;
	uint32_t state;
	uint32_t last_state;
	int32_t last_status;
	uint16_t initiator_vmid;
	uint16_t controller_vmid;
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
int32_t hv_pm_request_suspend(uint16_t initiator_vmid);
int32_t hv_pm_abort(uint64_t epoch, int32_t reason);
void hv_pm_get_snapshot(struct beau_pm_snapshot *snapshot);
bool hv_pm_io_is_gated(void);
int32_t hv_pm_set_policy(const struct beau_pm_policy *policy);
int32_t hv_pm_record_wake(uint32_t wake_source, uint16_t source_index);
int32_t hv_pm_mark_vm_suspended(uint16_t vmid, uint64_t epoch,
	uint64_t resume_entry, uint64_t resume_context);
int32_t hv_pm_resume_vm(uint16_t vmid, uint64_t epoch);
int32_t hv_pm_register_hook(const struct beau_pm_ops *ops);
void hv_pm_finalize_hooks(void);
int32_t hv_pm_run_prepare(uint64_t epoch);
int32_t hv_pm_run_suspend(uint64_t epoch);
int32_t hv_pm_run_resume(uint64_t epoch);
int32_t hv_pm_run_abort(uint64_t epoch);

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
