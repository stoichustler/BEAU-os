/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <rtl.h>

#include <asm/ras.h>
#include <asm/sysreg.h>

#define ID_AA64PFR0_RAS_SHIFT		28U
#define ID_AA64PFR0_RAS_MASK		(0xfUL << ID_AA64PFR0_RAS_SHIFT)
#define ERRIDR_NUM_MASK		0xffffUL
#define ERRSELR_SEL_MASK		0xffffUL
#define ERXSTATUS_AV			(1UL << 31U)
#define ERXSTATUS_V			(1UL << 30U)

static bool arm64_ras_supported(void)
{
	return (read_id_aa64pfr0_el1() & ID_AA64PFR0_RAS_MASK) != 0UL;
}

static void arm64_ras_select_record(uint16_t index)
{
	arm64_sysreg_write(errselr_el1, (uint64_t)index & ERRSELR_SEL_MASK);
	arm64_isb();
}

/* [20260721] Local RAS record snapshot
 *
 * SError entry -> save ERRSELR -> select one ERR<n> -> copy state
 *       |                                      |
 *       |                                      +--> never clear ERXSTATUS
 *       v
 * restore ERRSELR before publishing the software snapshot
 *
 * Key rule:
 *   - ERRSELR is PE-local hardware selection state and is restored before the
 *     caller can resume or terminate the interrupted context;
 *   - capture uses a fixed record bound and performs no allocation or locking,
 *     preventing an asynchronous RAS path from blocking on failed software.
 */
bool arm64_ras_capture(struct arm64_ras_snapshot *snapshot)
{
	uint64_t saved_selector;
	uint32_t reported_count;
	uint32_t capture_count;
	uint32_t index;

	if (snapshot == NULL) {
		return false;
	}
	(void)memset(snapshot, 0U, sizeof(*snapshot));
	if (!arm64_ras_supported()) {
		return false;
	}

	snapshot->flags = ARM64_RAS_SNAPSHOT_SUPPORTED;
	snapshot->erridr = arm64_sysreg_read(erridr_el1);
	snapshot->disr = arm64_sysreg_read(disr_el1);
	reported_count = (uint32_t)((snapshot->erridr & ERRIDR_NUM_MASK) + 1UL);
	capture_count = (reported_count < ARM64_RAS_MAX_RECORDS) ?
		reported_count : ARM64_RAS_MAX_RECORDS;
	snapshot->record_count = reported_count;
	if (reported_count > ARM64_RAS_MAX_RECORDS) {
		snapshot->flags |= ARM64_RAS_SNAPSHOT_TRUNCATED;
	}

	saved_selector = arm64_sysreg_read(errselr_el1);
	for (index = 0U; index < capture_count; index++) {
		struct arm64_ras_record record = { 0U };

		arm64_ras_select_record((uint16_t)index);
		record.status = arm64_sysreg_read(erxstatus_el1);
		record.address = arm64_sysreg_read(erxaddr_el1);
		record.misc0 = arm64_sysreg_read(erxmisc0_el1);
		record.index = (uint16_t)index;
		if ((record.status & ERXSTATUS_V) == 0UL) {
			continue;
		}
		record.flags = ARM64_RAS_RECORD_VALID;
		if ((record.status & ERXSTATUS_AV) != 0UL) {
			record.flags |= ARM64_RAS_RECORD_ADDRESS_VALID;
		}
		snapshot->record[snapshot->valid_count] = record;
		snapshot->valid_count++;
	}
	arm64_sysreg_write(errselr_el1, saved_selector & ERRSELR_SEL_MASK);
	arm64_isb();

	if (snapshot->valid_count == 0U) {
		snapshot->flags |= ARM64_RAS_SNAPSHOT_NO_VALID;
	}
	return true;
}
