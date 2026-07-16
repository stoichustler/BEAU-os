/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BSP_PASSTHROUGH_H
#define BSP_PASSTHROUGH_H

#include <types.h>

struct acrn_vm;

enum passthrough_irq_affinity_policy {
	PASSTHROUGH_IRQ_AFFINITY_NONE = 0U,
	PASSTHROUGH_IRQ_AFFINITY_OWNER,
	PASSTHROUGH_IRQ_AFFINITY_PCPU,
};

struct passthrough_spi_mapping {
	uint32_t phys_spi;
	uint32_t virt_irq;
	uint16_t affinity_pcpu;
	enum passthrough_irq_affinity_policy affinity_policy;
	bool level;
};

int32_t passthrough_register_device(uint32_t stream_id, const char *name,
	bool writable);
int32_t passthrough_register_device_owner(uint32_t stream_id, const char *name,
	bool writable, uint16_t owner_vmid);
int32_t passthrough_register_spi(uint32_t stream_id,
	const struct passthrough_spi_mapping *mapping);
int32_t passthrough_assign_device(struct acrn_vm *vm, uint32_t stream_id,
	bool writable);
int32_t passthrough_deassign_device(struct acrn_vm *vm, uint32_t stream_id);
void passthrough_deassign_vm(struct acrn_vm *vm);
bool passthrough_irq_affinity(uint16_t vm_id, uint32_t phys_spi,
	uint16_t *pcpu_id);
bool passthrough_vm_has_owned_devices(uint16_t vm_id);
bool passthrough_get_max_stream_id(uint32_t *max_stream_id);
int32_t passthrough_pm_suspend(uint64_t epoch, uint64_t required_vm_mask);
int32_t passthrough_pm_resume(uint64_t epoch);

#endif /* BSP_PASSTHROUGH_H */
