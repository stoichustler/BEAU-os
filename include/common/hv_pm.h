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

enum beau_vm_pm_state {
	VM_PM_RUNNING = 0U,
	VM_PM_PREPARE_SENT,
	VM_PM_SUSPEND_PENDING,
	VM_PM_SUSPENDED,
	VM_PM_RESUMING,
	VM_PM_FAILED,
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
	struct beau_vm_pm_record vm[CONFIG_MAX_VM_NUM];
	struct beau_pm_phase_error last_error;
	uint32_t state;
	uint32_t last_state;
	int32_t last_status;
	uint16_t initiator_vmid;
	uint16_t controller_vmid;
	uint8_t io_gated;
	uint8_t reserved[7U];
};

struct beau_pm_transaction {
	spinlock_t lock;
	struct beau_pm_snapshot data;
};

const char *hv_pm_state_to_str(enum beau_pm_system_state state);
int32_t hv_pm_request_suspend(uint16_t initiator_vmid);
int32_t hv_pm_abort(uint64_t epoch, int32_t reason);
void hv_pm_get_snapshot(struct beau_pm_snapshot *snapshot);
bool hv_pm_io_is_gated(void);

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
