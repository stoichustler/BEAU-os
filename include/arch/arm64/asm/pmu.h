/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_CORE_PMU_H
#define ARM64_CORE_PMU_H

#include <types.h>
#include <vm_config.h>

/* EL2 owns one physical PMU instance per pCPU. This interface exposes
 * capability-aware diagnostic snapshots and scoped hot-path measurements; it
 * does not virtualize a programmable PMU for guests.
 */
#define ARM64_CORE_PMU_POLL_US		10000U
#define ARM64_CORE_PMU_MAX_COUNTERS	6U

enum arm64_core_pmu_event {
	ARM64_CORE_PMU_CYCLES = 0U,
	ARM64_CORE_PMU_INSTRUCTIONS,
	ARM64_CORE_PMU_STALL_FRONTEND,
	ARM64_CORE_PMU_STALL_BACKEND,
	ARM64_CORE_PMU_L1D_REFILL,
	ARM64_CORE_PMU_DTLB_WALK,
	ARM64_CORE_PMU_BRANCH_MISPRED,
	ARM64_CORE_PMU_EVENT_NUM,
};

enum arm64_core_pmu_path {
	ARM64_CORE_PMU_PATH_MMIO = 0U,
	ARM64_CORE_PMU_PATH_VGIC,
	ARM64_CORE_PMU_PATH_VIRTIO,
	ARM64_CORE_PMU_PATH_NUM,
};

struct arm64_core_pmu_values {
	uint64_t value[ARM64_CORE_PMU_EVENT_NUM];
	uint64_t running_ticks;
};

/* Path values are inclusive. Nested MMIO, vGIC, or virtio scopes can overlap,
 * so callers must not sum path rows as mutually exclusive execution time.
 */
struct arm64_core_pmu_path_values {
	uint64_t cycles;
	uint64_t instructions;
	uint64_t calls;
	uint64_t instruction_calls;
	uint64_t dropped;
};

/* Capability is probed independently on every pCPU. event_mask is the contract
 * for consumers: an unsupported event is unavailable, not a measured zero.
 */
struct arm64_core_pmu_capability {
	uint64_t pmceid0;
	uint64_t pmceid1;
	uint32_t event_mask;
	uint8_t event_counter[ARM64_CORE_PMU_EVENT_NUM];
	uint8_t pmuver;
	uint8_t counter_num;
	uint8_t cycle_width;
	uint8_t event_width;
	bool available;
};

/* Snapshot structures are caller-owned diagnostic copies. A valid pCPU entry
 * was captured by that CPU; snapshot.complete states whether every requested
 * pCPU responded within the bounded cross-CPU call.
 */
struct arm64_core_pmu_vcpu_snapshot {
	struct arm64_core_pmu_values total;
	struct arm64_core_pmu_path_values path[ARM64_CORE_PMU_PATH_NUM];
	uint64_t enabled_ticks;
	uint64_t denied_accesses;
	uint16_t vm_id;
	uint16_t vcpu_id;
	bool valid;
};

struct arm64_core_pmu_pcpu_snapshot {
	struct arm64_core_pmu_capability capability;
	struct arm64_core_pmu_values total;
	struct arm64_core_pmu_values host;
	struct arm64_core_pmu_vcpu_snapshot vcpu[CONFIG_MAX_VM_NUM];
	uint64_t enabled_ticks;
	uint64_t overflow_count;
	uint16_t pcpu_id;
	bool valid;
	bool running;
	bool suspended;
};

struct arm64_core_pmu_snapshot {
	struct arm64_core_pmu_pcpu_snapshot pcpu[MAX_PCPU_NUM];
	uint64_t epoch;
	uint16_t pcpu_num;
	bool requested_running;
	bool complete;
};

/* A path token is valid only within one uninterrupted accounting interval on
 * the recorded pCPU and VM/vCPU owner. Epoch and owner_generation reject stale
 * samples after reset, suspend, or a scheduler ownership transition.
 */
struct arm64_core_pmu_path_token {
	uint64_t cycles;
	uint64_t instructions;
	uint64_t epoch;
	uint64_t owner_generation;
	uint16_t pcpu_id;
	uint16_t vm_id;
	uint16_t vcpu_id;
	bool instructions_valid;
	bool valid;
};

struct acrn_vcpu;

void arm64_core_pmu_init_pcpu(void);
void arm64_core_pmu_init_workers(void);
void arm64_core_pmu_vcpu_load(const struct acrn_vcpu *vcpu);
void arm64_core_pmu_vcpu_unload(const struct acrn_vcpu *vcpu);

int32_t arm64_core_pmu_start(void);
int32_t arm64_core_pmu_stop(void);
int32_t arm64_core_pmu_reset(void);
int32_t arm64_core_pmu_take_snapshot(struct arm64_core_pmu_snapshot *snapshot);

void arm64_core_pmu_suspend_cpu(uint64_t epoch);
void arm64_core_pmu_resume_cpu(uint64_t epoch);

bool arm64_core_pmu_guest_sysreg(uint64_t sysreg);
void arm64_core_pmu_record_guest_access(const struct acrn_vcpu *vcpu);

void arm64_core_pmu_path_begin(struct arm64_core_pmu_path_token *token);
void arm64_core_pmu_path_end(enum arm64_core_pmu_path path,
	struct arm64_core_pmu_path_token *token);

const char *arm64_core_pmu_event_name(enum arm64_core_pmu_event event);
const char *arm64_core_pmu_path_name(enum arm64_core_pmu_path path);

#endif /* ARM64_CORE_PMU_H */
