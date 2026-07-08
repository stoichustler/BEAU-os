/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BSP_PASSTHROUGH_H
#define BSP_PASSTHROUGH_H

#include <types.h>

struct acrn_vm;

struct passthrough_spi_mapping {
	uint32_t phys_spi;
	uint32_t virt_irq;
	bool level;
};

int32_t passthrough_register_device(uint32_t stream_id, const char *name,
	bool writable);
int32_t passthrough_register_spi(uint32_t stream_id,
	const struct passthrough_spi_mapping *mapping);
int32_t passthrough_assign_device(struct acrn_vm *vm, uint32_t stream_id,
	bool writable);
int32_t passthrough_deassign_device(struct acrn_vm *vm, uint32_t stream_id);
void passthrough_deassign_vm(struct acrn_vm *vm);

#endif /* BSP_PASSTHROUGH_H */
