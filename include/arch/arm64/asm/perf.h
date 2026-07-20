/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PERF_H
#define ARM64_PERF_H

#include <types.h>

#define ARM64_PERF_MAX_FRAMES		12U
#define ARM64_PERF_RECORDS_PER_CPU	128U
#define ARM64_PERF_THREAD_NAME_LEN	16U
#define ARM64_PERF_INVALID_ID		0xffffU

enum arm64_perf_owner {
	ARM64_PERF_OWNER_HOST = 0U,
	ARM64_PERF_OWNER_GUEST,
};

enum arm64_perf_unwind_stop {
	ARM64_PERF_UNWIND_COMPLETE = 0U,
	ARM64_PERF_UNWIND_NO_STACK,
	ARM64_PERF_UNWIND_FP_MISALIGNED,
	ARM64_PERF_UNWIND_FP_OUTSIDE,
	ARM64_PERF_UNWIND_FP_ORDER,
	ARM64_PERF_UNWIND_LR_OUTSIDE,
	ARM64_PERF_UNWIND_DEPTH,
};

struct arm64_perf_sample {
	uint64_t tsc;
	uint64_t epoch;
	uint64_t generation;
	uint64_t frames[ARM64_PERF_MAX_FRAMES];
	char thread_name[ARM64_PERF_THREAD_NAME_LEN];
	uint16_t pcpu_id;
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint16_t frame_count;
	uint16_t unwind_stop;
	uint16_t owner;
};

struct arm64_perf_status {
	uint64_t epoch;
	uint64_t generation;
	uint64_t stop_ticks;
	uint32_t duration_ms;
	uint32_t frequency_hz;
	uint32_t period_us;
	uint16_t controller_pcpu;
	uint16_t pcpu_num;
	bool running;
	bool readable;
};

struct arm64_perf_cpu_status {
	uint64_t attempts;
	uint64_t captured;
	uint64_t no_stack;
	uint64_t missed;
	uint64_t overwritten;
	uint64_t pending_generation;
	uint32_t count;
	bool writer_active;
};

struct intr_excp_ctx;

int32_t arm64_perf_record(uint32_t duration_ms, uint32_t frequency_hz);
int32_t arm64_perf_stop(void);
int32_t arm64_perf_clear(void);
void arm64_perf_sample_irq(const struct intr_excp_ctx *ctx, bool guest_context);
void arm64_perf_get_status(struct arm64_perf_status *status);
void arm64_perf_get_cpu_status(uint16_t pcpu_id,
	struct arm64_perf_cpu_status *status);
bool arm64_perf_get_sample(uint16_t pcpu_id, uint32_t index,
	struct arm64_perf_sample *sample);
const char *arm64_perf_unwind_stop_name(uint16_t stop);

#endif /* ARM64_PERF_H */
