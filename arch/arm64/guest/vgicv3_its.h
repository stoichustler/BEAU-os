/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VGICV3_ITS_H
#define ARM64_GUEST_VGICV3_ITS_H

#include <types.h>
#include <vm.h>
#include <vcpu.h>
#include <bsp/io_req.h>
#include <asm/guest/vgicv3.h>

void arm64_vgicv3_its_init(struct arm64_vgicv3 *vgic);
int32_t arm64_vgicv3_its_inject_msi(struct acrn_vm *vm,
	struct arm64_vgicv3 *vgic, uint32_t device_id, uint32_t event_id);
int32_t arm64_vgicv3_its_mmio_access(struct acrn_vcpu *vcpu,
	struct arm64_vgicv3 *vgic, struct acrn_mmio_request *mmio);
bool arm64_vgicv3_its_mmio_access64(struct acrn_vcpu *vcpu,
	struct arm64_vgicv3 *vgic, struct acrn_mmio_request *mmio, int32_t *status);

int32_t arm64_vgicv3_inject_irq_locked(struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level);
void arm64_vgicv3_set_pending_locked(struct arm64_vgicv3 *vgic,
	uint16_t vcpu_id, struct arm64_vgic_irq *desc, bool pending);
void arm64_vgicv3_update_irq_row_lr_locked(struct acrn_vm *vm,
	uint16_t vcpu_id, const struct arm64_vgic_irq *desc);
void arm64_vgicv3_flush_vm_locked(struct acrn_vcpu *current_vcpu);

#endif /* ARM64_GUEST_VGICV3_ITS_H */
