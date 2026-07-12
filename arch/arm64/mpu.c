/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <asm/mpu.h>
#include <asm/sve.h>
#include <asm/vm_config.h>

struct arm64_mpu_feature_desc {
	uint64_t feature;
	const char *name;
	bool (*host_supported)(void);
};

static const struct arm64_mpu_feature_desc arm64_mpu_features[] = {
	{
		.feature = ARM64_VM_FEATURE_SVE,
		.name = "sve",
		.host_supported = arm64_sve_host_supported,
	},
};

static bool host_features_valid;
static uint64_t host_features;

static uint64_t arm64_mpu_collect_host_features(void)
{
	uint64_t features = 0UL;
	uint32_t idx;

	for (idx = 0U; idx < ARRAY_SIZE(arm64_mpu_features); idx++) {
		const struct arm64_mpu_feature_desc *desc = &arm64_mpu_features[idx];

		if ((desc->host_supported != NULL) && desc->host_supported()) {
			features |= desc->feature;
		}
	}

	return features;
}

uint64_t arm64_mpu_host_features(void)
{
	if (!host_features_valid) {
		host_features = arm64_mpu_collect_host_features();
		host_features_valid = true;
	}

	return host_features;
}

bool arm64_mpu_host_feature_enabled(uint64_t feature)
{
	return (arm64_mpu_host_features() & feature) != 0UL;
}

const char *arm64_mpu_feature_name(uint64_t feature)
{
	uint32_t idx;

	for (idx = 0U; idx < ARRAY_SIZE(arm64_mpu_features); idx++) {
		if (arm64_mpu_features[idx].feature == feature) {
			return arm64_mpu_features[idx].name;
		}
	}

	return "unknown";
}
