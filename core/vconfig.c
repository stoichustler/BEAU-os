/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vconfig.h>
#include <util.h>
#include <rtl.h>

struct acrn_vm_config *get_vm_config(uint16_t vm_id)
{
	return &vm_configs[vm_id];
}

uint8_t get_vm_severity(uint16_t vm_id)
{
	return vm_configs[vm_id].severity;
}

bool vm_has_matched_name(uint16_t vmid, const char *name)
{
	struct acrn_vm_config *vm_config = get_vm_config(vmid);

	return (strncmp(vm_config->name, name, MAX_VM_NAME_LEN) == 0);
}
