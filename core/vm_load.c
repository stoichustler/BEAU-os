/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm.h>
#include <vboot.h>
#include <errno.h>
#include <logmsg.h>
#include <guest_memory.h>

/*
 * VM image preparation sits between VM creation and vCPU launch:
 *
 *   create_vm()
 *        -> init_vm_boot_info() records source modules and boot metadata
 *        -> prepare_os_image() copies the selected guest image into guest RAM
 *        -> start_vm() lets arch_vm_prepare_bsp() build the initial CPU state
 *
 * This file is intentionally a dispatcher. It does not parse bootloader module
 * tags and it does not know ARM64 stage-2 layout details. The boot layer fills
 * vm->sw with source/load/entry information, the selected loader performs the
 * format-specific copy and entry setup, and guest_memory helpers enforce the
 * active GPA-to-HPA contract.
 */

/**
 * @pre sw_module != NULL
 */
void load_sw_module(struct acrn_vm *vm, struct sw_module_info *sw_module)
{
	/*
	 * Firmware payloads such as ACPI or FDT are copied after the kernel loader
	 * chooses their guest load GPA. A zero-sized module or a missing load
	 * address means the current VM type does not consume that payload.
	 */
	if ((sw_module->size != 0) && (sw_module->load_addr != NULL)) {
		(void)copy_to_gpa(vm, sw_module->src_addr, (uint64_t)sw_module->load_addr, sw_module->size);
	}
}

/**
 * @pre vm != NULL
 */
int32_t prepare_os_image(struct acrn_vm *vm)
{
	int32_t ret = -EINVAL;
	struct sw_module_info *acpi_info = &(vm->sw.acpi_info);

	/*
	 * vm->sw.kernel_type is populated from VM configuration by
	 * init_vm_boot_info(). The selected loader owns format-specific placement:
	 * raw images use configured load/entry GPAs, bzImage builds Linux
	 * zeropage state, and ELF follows program headers.
	 */
	switch (vm->sw.kernel_type) {
#ifdef CONFIG_GUEST_KERNEL_BZIMAGE
	case KERNEL_BZIMAGE:
		ret = bzimage_loader(vm);
		break;
#endif
#ifdef CONFIG_GUEST_KERNEL_RAWIMAGE
	case KERNEL_RAWIMAGE:
		ret = rawimage_loader(vm);
		break;
#endif
#ifdef CONFIG_GUEST_KERNEL_ELF
	case KERNEL_ELF:
		ret = elf_loader(vm);
		break;
#endif
	default:
		ret = -EINVAL;
		break;
	}

	if (ret == 0) {
		/*
		 * Hardware description modules are copied only after the kernel loader
		 * succeeds. On ARM64 raw-image Linux, the loader may adjust FDT
		 * placement to avoid kernel/initramfs overlap before this final copy.
		 */
		load_sw_module(vm, acpi_info);
		load_sw_module(vm, &(vm->sw.fdt_info));
		LOG_DBG("vm-%hu kernel entry at 0x%016lx", vm->vm_id,
			vm->sw.kernel_info.kernel_entry_addr);
	}

	return ret;
}
