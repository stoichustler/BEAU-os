/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HWTDBG_H
#define HWTDBG_H

#include <types.h>
#include <vm_wdt.h>
#include <asm/ras.h>

struct acrn_vcpu;
struct cpu_regs;

enum hwtdbg_timeout_kind {
	HWTDBG_TIMEOUT_FIRST_KICK = 0U,
	HWTDBG_TIMEOUT_RUNTIME,
};

enum hwtdbg_recovery_result {
	HWTDBG_RECOVERY_NOT_REQUESTED = 0U,
	HWTDBG_RECOVERY_QUIESCING,
	HWTDBG_RECOVERY_RESETTING,
	HWTDBG_RECOVERY_LAUNCHED,
	HWTDBG_RECOVERY_VERIFIED,
	HWTDBG_RECOVERY_LATE_KICK,
	HWTDBG_RECOVERY_EXHAUSTED,
	HWTDBG_RECOVERY_INVALID_STATE,
	HWTDBG_RECOVERY_RESET_FAILED,
	HWTDBG_RECOVERY_QUIESCE_TIMEOUT,
	HWTDBG_RECOVERY_VERIFY_TIMEOUT,
};

enum hwtdbg_guest_fault_reason {
	HWTDBG_GUEST_FAULT_IABT = 0U,
	HWTDBG_GUEST_FAULT_SERROR,
	HWTDBG_GUEST_FAULT_UNHANDLED_EXIT,
};

struct hwtdbg_timeout_context {
	enum hwtdbg_timeout_kind kind;
	enum vm_wdt_cause cause;
	uint64_t detected_tsc;
	uint64_t heartbeat_tsc;
	uint64_t age_ms;
	uint64_t kick_count;
	uint64_t timeout_count;
	uint64_t restart_count;
	uint64_t restart_fail_count;
	uint64_t last_token;
	uint64_t irq_total;
	uint64_t irq_delta;
	uint32_t timeout_ms;
	bool restart_enabled;
};

uint64_t hwtdbg_capture_timeout(uint16_t vm_id,
	const struct hwtdbg_timeout_context *timeout);
void hwtdbg_capture_ras(struct acrn_vcpu *vcpu, const struct cpu_regs *regs,
	const struct arm64_ras_snapshot *snapshot);
void hwtdbg_capture_guest_fault(struct acrn_vcpu *vcpu,
	enum hwtdbg_guest_fault_reason reason, int32_t exit_ret);
void hwtdbg_update_recovery(uint16_t vm_id, uint64_t sequence,
	enum hwtdbg_recovery_result result, uint64_t attempt,
	uint64_t wait_vcpus, int32_t reset_ret);
int32_t shell_hwtdbg(int32_t argc, char **argv);
int32_t shell_crash(int32_t argc, char **argv);

#endif /* HWTDBG_H */
