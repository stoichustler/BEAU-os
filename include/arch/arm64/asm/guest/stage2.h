/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_STAGE2_H
#define ARM64_GUEST_STAGE2_H

#include <types.h>

struct acrn_vm;

#define ARM64_STAGE2_VMID_SHIFT		48U
#define ARM64_STAGE2_VMID_MASK		0xffUL

#define ARM64_STAGE2_MAP_READ		(1U << 0U)
#define ARM64_STAGE2_MAP_WRITE		(1U << 1U)
#define ARM64_STAGE2_MAP_DEVICE		(1U << 2U)
#define ARM64_STAGE2_MAP_NORMAL		(1U << 3U)

uint64_t arm64_stage2_vttbr(const struct acrn_vm *vm);
void arm64_stage2_map(struct acrn_vm *vm, uint64_t hpa, uint64_t ipa,
	uint64_t size, uint32_t flags);
void arm64_stage2_unmap(struct acrn_vm *vm, uint64_t ipa, uint64_t size);

static inline void arm64_stage2_map_device(struct acrn_vm *vm, uint64_t hpa,
	uint64_t ipa, uint64_t size, bool writable)
{
	uint32_t flags = ARM64_STAGE2_MAP_READ | ARM64_STAGE2_MAP_DEVICE;

	if (writable) {
		flags |= ARM64_STAGE2_MAP_WRITE;
	}
	arm64_stage2_map(vm, hpa, ipa, size, flags);
}

static inline void arm64_stage2_map_normal(struct acrn_vm *vm, uint64_t hpa,
	uint64_t ipa, uint64_t size, bool writable)
{
	uint32_t flags = ARM64_STAGE2_MAP_READ | ARM64_STAGE2_MAP_NORMAL;

	if (writable) {
		flags |= ARM64_STAGE2_MAP_WRITE;
	}
	arm64_stage2_map(vm, hpa, ipa, size, flags);
}

#endif /* ARM64_GUEST_STAGE2_H */
