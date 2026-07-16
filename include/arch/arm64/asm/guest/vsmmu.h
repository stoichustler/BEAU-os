/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VSMMU_H
#define ARM64_GUEST_VSMMU_H

#include <types.h>

struct acrn_vm;
struct io_request;

struct arm64_vsmmu_debug {
	uint64_t base;
	uint64_t size;
	uint64_t strtab_base;
	uint64_t cmdq_base;
	uint64_t evtq_base;
	uint64_t generation;
	uint64_t commands_processed;
	uint64_t commands_rejected;
	uint64_t budget_exhausted;
	uint32_t cr0;
	uint32_t irq_ctrl;
	uint32_t gerror;
	uint32_t gerrorn;
	uint32_t cmdq_prod;
	uint32_t cmdq_cons;
	uint32_t evtq_prod;
	uint32_t evtq_cons;
	uint16_t worker_pcpu;
	bool worker_pending;
	bool irq_asserted;
	bool available;
};

void arm64_vsmmu_init_vm(struct acrn_vm *vm);
void arm64_vsmmu_reset_vm(struct acrn_vm *vm);
void arm64_vsmmu_deinit_vm(struct acrn_vm *vm);
bool arm64_vsmmu_available(const struct acrn_vm *vm);
bool arm64_vsmmu_get_debug(uint16_t vm_id, struct arm64_vsmmu_debug *debug);
int32_t arm64_vsmmu_mmio_handler(struct io_request *io_req,
	void *handler_private_data);

#endif /* ARM64_GUEST_VSMMU_H */
