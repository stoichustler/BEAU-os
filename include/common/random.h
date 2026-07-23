/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMMON_RANDOM_H
#define COMMON_RANDOM_H

uint64_t arch_get_random_value(void);
void arch_random_bootstrap_init(void);
void arch_random_publish_strong(void);
bool arch_random_strong_ready(void);
bool arch_get_random_strong_value(uint64_t *value);

static inline uint64_t get_random_value(void)
{
	return arch_get_random_value();
}

#endif /* COMMON_RANDOM_H */
