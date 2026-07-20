/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_DDB_H
#define ARM64_DDB_H

#include <types.h>

#define ARM64_DDB_BRK_IMM		0x0DDBU
#define ARM64_DDB_PANIC_BRK_IMM		0x0DDCU

struct intr_excp_ctx;

void arm64_ddb_break(void);
void arm64_ddb_panic_break(void);
bool arm64_ddb_handle_trap(struct intr_excp_ctx *ctx, uint64_t trap_type);

#endif /* ARM64_DDB_H */
