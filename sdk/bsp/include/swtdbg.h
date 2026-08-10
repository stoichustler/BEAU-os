/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SWTDBG_H
#define SWTDBG_H

#include <types.h>
#include <vm_wdt.h>
#include <asm/ras.h>

struct acrn_vcpu;
struct cpu_regs;

enum swtdbg_timeout_kind {
	SWTDBG_TIMEOUT_FIRST_KICK = 0U,
	SWTDBG_TIMEOUT_RUNTIME,
};

enum swtdbg_recovery_result {
	SWTDBG_RECOVERY_NOT_REQUESTED = 0U,
	SWTDBG_RECOVERY_QUIESCING,
	SWTDBG_RECOVERY_RESETTING,
	SWTDBG_RECOVERY_LAUNCHED,
	SWTDBG_RECOVERY_VERIFIED,
	SWTDBG_RECOVERY_LATE_KICK,
	SWTDBG_RECOVERY_EXHAUSTED,
	SWTDBG_RECOVERY_INVALID_STATE,
	SWTDBG_RECOVERY_RESET_FAILED,
	SWTDBG_RECOVERY_QUIESCE_TIMEOUT,
	SWTDBG_RECOVERY_VERIFY_TIMEOUT,
};

enum swtdbg_guest_fault_reason {
	SWTDBG_GUEST_FAULT_IABT = 0U,
	SWTDBG_GUEST_FAULT_SERROR,
	SWTDBG_GUEST_FAULT_UNHANDLED_EXIT,
};

struct swtdbg_timeout_context {
	enum swtdbg_timeout_kind kind;
	enum vm_wdt_cause cause;
	uint64_t detected_tsc;
	uint64_t heartbeat_tsc;
	uint64_t age_ms;
	uint64_t timeout_count;
	uint64_t restart_count;
	uint64_t restart_fail_count;
	uint64_t last_token;
	uint64_t irq_total;
	uint64_t irq_delta;
	uint64_t expected_vcpu_mask;
	uint64_t started_vcpu_mask;
	uint64_t stalled_vcpu_mask;
	uint64_t vcpu_age_ms[MAX_VCPUS_PER_VM];
	uint64_t vcpu_last_token[MAX_VCPUS_PER_VM];
	uint32_t timeout_ms;
	bool restart_enabled;
	bool per_vcpu_mode;
};

uint64_t swtdbg_capture_timeout(uint16_t vm_id,
	const struct swtdbg_timeout_context *timeout);
void swtdbg_capture_ras(struct acrn_vcpu *vcpu, const struct cpu_regs *regs,
	const struct arm64_ras_snapshot *snapshot);
void swtdbg_capture_guest_fault(struct acrn_vcpu *vcpu,
	enum swtdbg_guest_fault_reason reason, int32_t exit_ret);
void swtdbg_update_recovery(uint16_t vm_id, uint64_t sequence,
	enum swtdbg_recovery_result result, uint64_t attempt,
	uint64_t wait_vcpus, int32_t reset_ret);
int32_t shell_swtdbg(int32_t argc, char **argv);
int32_t shell_crash(int32_t argc, char **argv);

#endif /* SWTDBG_H */
