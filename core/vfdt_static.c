/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm.h>
#include <reloc.h>
#include <vm_config.h>
#if CONFIG_STATIC_VFDT
#include <libfdt.h>
#include <vfdt.h>
#include <fdt_api.h>
#include <logmsg.h>
#endif

void init_service_vm_vfdt(struct acrn_vm *vm)
{
#if CONFIG_STATIC_VFDT
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	void *fdt = vm_get_vfdt(vm);

	arch_init_service_vm_vfdt(vm);

	if (vm_config->os_config.bootargs[0] != '\0') {
		(void)fdt_set_kernel_bootargs(fdt, vm_config->os_config.bootargs);
	}

	vm->sw.fdt_info.src_addr = fdt;
	vm->sw.fdt_info.size = fdt_totalsize(fdt);
	LOG_INF("vm-%hu static vFDT size: 0x%08x", vm->vm_id, vm->sw.fdt_info.size);
#else
	(void)vm;
#endif
}
