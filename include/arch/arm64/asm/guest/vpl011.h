/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VPL011_H
#define ARM64_GUEST_VPL011_H

#include <types.h>

struct acrn_vm;
struct io_request;

struct arm64_vpl011_debug {
	uint64_t tx_count;
	uint64_t tx_irq_count;
	uint64_t irq_assert_count;
	uint64_t irq_deassert_count;
	uint32_t cr;
	uint32_t imsc;
	uint32_t ris;
	uint32_t pending;
	uint8_t last_tx;
	bool irq_asserted;
};

void arm64_vpl011_init_vm(struct acrn_vm *vm);
void arm64_vpl011_reset_vm(struct acrn_vm *vm);
void arm64_vpl011_get_debug(uint16_t vm_id, struct arm64_vpl011_debug *debug);
int32_t arm64_vpl011_mmio_handler(struct io_request *io_req, void *handler_private_data);

#endif /* ARM64_GUEST_VPL011_H */
