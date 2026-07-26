/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BEAU_DHRYSTONE_PORT_H
#define BEAU_DHRYSTONE_PORT_H

#include <errno.h>
#include <rtl.h>
#include <types.h>

struct dhrystone_result {
	uint32_t runs;
	uint64_t elapsed_ticks;
	bool valid;
};

bool dhrystone_port_acquire(void);
void dhrystone_port_release(void);
uint64_t dhrystone_port_ticks(void);
uint64_t dhrystone_port_ticks_per_second(void);
int32_t dhrystone_run(uint32_t initial_runs, struct dhrystone_result *result);

#endif /* BEAU_DHRYSTONE_PORT_H */
