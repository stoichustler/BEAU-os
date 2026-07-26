/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <rtl.h>
#include <ticks.h>
#include <util.h>

#include "coremark_engine.h"

#define COREMARK_DEFAULT_SEED3 0x66U
#define COREMARK_CALIBRATION_MIN_US 1000000UL
#define COREMARK_CALIBRATION_TARGET_SECONDS 10U
#define COREMARK_CALIBRATION_MAX_STEPS 10U

/* [20260726] Worker-owned CoreMark context preparation
 *
 * persistent result/data -> initialize algorithms -> publish to one worker
 *
 * Key rule:
 *   - the owning worker's context is never shared with another generation;
 *   - all four algorithm memory pointers remain within the fixed 2000-byte area;
 *   - preparation completes before the request generation is published.
 */
int32_t coremark_engine_prepare(struct coremark_engine_context *context,
	uint32_t iterations)
{
	core_results *result;
	uint32_t algorithm_size;

	if ((context == NULL) || (iterations == 0U)) {
		return -EINVAL;
	}
	result = &context->result;
	(void)memset(context, 0U, sizeof(*context));
	algorithm_size = COREMARK_TOTAL_DATA_SIZE / NUM_ALGORITHMS;
	if (algorithm_size == 0U) {
		return -EINVAL;
	}
	result->seed1 = 0;
	result->seed2 = 0;
	result->seed3 = (ee_s16)COREMARK_DEFAULT_SEED3;
	result->size = algorithm_size;
	result->iterations = iterations;
	result->execs = ALL_ALGORITHMS_MASK;
	result->memblock[0] = context->data;
	result->memblock[1] = context->data;
	result->memblock[2] = context->data + algorithm_size;
	result->memblock[3] = context->data + (algorithm_size * 2U);
	result->list = core_list_init(result->size, (list_head *)result->memblock[1],
		result->seed1);
	core_init_matrix(result->size, result->memblock[2],
		(ee_s32)result->seed1 | ((ee_s32)result->seed2 << 16), &result->mat);
	core_init_state(result->size, result->seed1, (ee_u8 *)result->memblock[3]);
	result->port.portable_id = 1U;
	return 0;
}

int32_t coremark_engine_calibrate(struct coremark_engine_context *context,
	uint32_t *iterations)
{
	uint32_t candidate = 1U;
	uint32_t step;
	uint64_t elapsed_us = 0UL;
	uint64_t elapsed_ticks;

	if ((context == NULL) || (iterations == NULL)) {
		return -EINVAL;
	}
	for (step = 0U; step < COREMARK_CALIBRATION_MAX_STEPS; step++) {
		if (coremark_engine_prepare(context, candidate) < 0) {
			return -EINVAL;
		}
		elapsed_ticks = cpu_ticks();
		(void)iterate(&context->result);
		elapsed_us = ticks_to_us(cpu_ticks() - elapsed_ticks);
		if (elapsed_us >= COREMARK_CALIBRATION_MIN_US) {
			break;
		}
		if (candidate > (UINT32_MAX / 10U)) {
			return -EINVAL;
		}
		candidate *= 10U;
	}
	if (elapsed_us < COREMARK_CALIBRATION_MIN_US) {
		return -ETIMEDOUT;
	}
	step = 1U + (COREMARK_CALIBRATION_TARGET_SECONDS /
		(uint32_t)(elapsed_us / COREMARK_CALIBRATION_MIN_US));
	if ((step == 0U) || (candidate > (UINT32_MAX / step))) {
		return -EINVAL;
	}
	candidate *= step;
	*iterations = candidate;
	return 0;
}
