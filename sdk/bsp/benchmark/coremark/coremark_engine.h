/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BEAU_COREMARK_ENGINE_H
#define BEAU_COREMARK_ENGINE_H

#include <types.h>

#include "coremark.h"

struct coremark_engine_context {
	core_results result;
	uint8_t data[COREMARK_TOTAL_DATA_SIZE] __aligned(16);
};

int32_t coremark_engine_prepare(struct coremark_engine_context *context,
	uint32_t iterations);
int32_t coremark_engine_calibrate(struct coremark_engine_context *context,
	uint32_t *iterations);

#endif /* BEAU_COREMARK_ENGINE_H */
