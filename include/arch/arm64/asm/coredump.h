/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_COREDUMP_H
#define ARM64_COREDUMP_H

#include <types.h>

struct arm64_coredump_context {
	uint64_t pc;
	uint64_t lr;
	uint64_t sp;
	uint64_t fp;
	uint64_t esr;
};

void arm64_coredump_log(const struct arm64_coredump_context *context, uint16_t pcpu_id);
bool arm64_coredump_print_stored(void);
void arm64_coredump_erase_stored(void);

#endif /* ARM64_COREDUMP_H */
