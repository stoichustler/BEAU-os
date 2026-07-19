/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMMON_NOTIFY_H
#define COMMON_NOTIFY_H

#include <types.h>
#include <asm/notify.h>

typedef void (*smp_call_func_t)(void *data);

struct smp_call_info_data {
	smp_call_func_t func;
	void *data;
	uint64_t generation;
};

void init_smp_call(void);
void handle_smp_call(void);
void smp_call_function(uint64_t mask, smp_call_func_t func, void *data);
bool smp_call_function_timeout(uint64_t mask, smp_call_func_t func, void *data, uint32_t timeout_us);
int32_t smp_try_call_function_timeout(uint64_t mask, smp_call_func_t func,
	void *data, uint32_t timeout_us);
void kick_notification(__unused uint32_t irq, __unused void *data);

#endif /* COMMON_NOTIFY_H */
