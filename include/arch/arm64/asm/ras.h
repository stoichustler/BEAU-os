/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_RAS_H
#define ARM64_RAS_H

#include <types.h>

#define ARM64_RAS_MAX_RECORDS		8U

#define ARM64_RAS_SNAPSHOT_SUPPORTED	(1U << 0U)
#define ARM64_RAS_SNAPSHOT_TRUNCATED	(1U << 1U)
#define ARM64_RAS_SNAPSHOT_NO_VALID	(1U << 2U)

#define ARM64_RAS_RECORD_VALID		(1U << 0U)
#define ARM64_RAS_RECORD_ADDRESS_VALID	(1U << 1U)

struct arm64_ras_record {
	uint64_t status;
	uint64_t address;
	uint64_t misc0;
	uint16_t index;
	uint16_t flags;
};

struct arm64_ras_snapshot {
	uint64_t erridr;
	uint64_t disr;
	uint32_t flags;
	uint32_t record_count;
	uint16_t valid_count;
	struct arm64_ras_record record[ARM64_RAS_MAX_RECORDS];
};

/*
 * Captures the local PE's RAS error records without clearing hardware state.
 * Returns false when FEAT_RAS is unavailable; a true return can still carry
 * zero valid records when firmware has already consumed the reported error.
 */
bool arm64_ras_capture(struct arm64_ras_snapshot *snapshot);


#endif /* ARM64_RAS_H */
