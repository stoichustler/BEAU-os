/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_TRUSTY_H
#define ARM64_TRUSTY_H

#include <types.h>

struct acrn_vcpu;

#define ARM64_TRUSTY_SMC_UNK	UINT64_MAX

struct arm64_trusty_system_info {
	uint32_t smp_max_cpus;
	uint32_t api_version;
	bool api_version_valid;
};

int32_t arm64_trusty_get_version(char *version, size_t version_size);
int32_t arm64_trusty_get_system_info(struct arm64_trusty_system_info *info);
void arm64_trusty_heartbeat_start(void);
uint64_t arm64_trusty_handle_guest_smc(struct acrn_vcpu *vcpu);

#endif /* ARM64_TRUSTY_H */
