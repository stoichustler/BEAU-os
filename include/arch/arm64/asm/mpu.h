/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_MPU_H
#define ARM64_MPU_H

#include <types.h>

#ifndef ASSEMBLER

uint64_t arm64_mpu_host_features(void);
bool arm64_mpu_host_feature_enabled(uint64_t feature);
const char *arm64_mpu_feature_name(uint64_t feature);

#endif /* ASSEMBLER */

#endif /* ARM64_MPU_H */
