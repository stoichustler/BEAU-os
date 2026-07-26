/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BEAU_COREMARK_PORT_H
#define BEAU_COREMARK_PORT_H

#include <types.h>

#define COREMARK_TOTAL_DATA_SIZE 2000U
#define COREMARK_CONTEXTS MAX_PCPU_NUM

int32_t coremark_printf(const char *fmt, ...);

#endif /* BEAU_COREMARK_PORT_H */
