/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SPE_H
#define ARM64_SPE_H

#include <types.h>
#include <vconfig.h>

#define ARM64_SPE_HALF_NUM 2U
#define ARM64_SPE_SHELL_DUMP_MAX 256U

enum arm64_spe_reason {
	ARM64_SPE_REASON_NONE = 0U,
	ARM64_SPE_REASON_CONFIG_DISABLED,
	ARM64_SPE_REASON_NO_PMSVER,
	ARM64_SPE_REASON_HIGHER_EL_OWNER,
	ARM64_SPE_REASON_NO_PPI,
	ARM64_SPE_REASON_BUFFER,
	ARM64_SPE_REASON_HW_EVENT,
	ARM64_SPE_REASON_BUFFER_EXHAUSTED,
};

struct arm64_spe_pcpu_snapshot {
	uint64_t pmbidr;
	uint64_t pmsidr;
	uint64_t buffer_full_count;
	uint64_t error_count;
	uint64_t data_loss_count;
	uint32_t half_bytes[ARM64_SPE_HALF_NUM];
	uint16_t pcpu_id;
	uint8_t pmsver;
	uint8_t ready_mask;
	enum arm64_spe_reason reason;
	bool available;
	bool running;
};

struct arm64_spe_snapshot {
	struct arm64_spe_pcpu_snapshot pcpu[MAX_PCPU_NUM];
	uint16_t pcpu_num;
	bool complete;
};

void arm64_spe_init_pcpu(void);
void arm64_spe_suspend_cpu(uint64_t epoch);
void arm64_spe_resume_cpu(uint64_t epoch);
int32_t arm64_spe_start(void);
int32_t arm64_spe_stop(void);
int32_t arm64_spe_reset(void);
int32_t arm64_spe_take_snapshot(struct arm64_spe_snapshot *snapshot);
int32_t arm64_spe_dump(uint16_t pcpu_id, uint8_t *buffer, uint32_t *length);
bool arm64_spe_guest_sysreg(uint64_t sysreg);
void arm64_spe_record_guest_access(void);

#endif /* ARM64_SPE_H */
