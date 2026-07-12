/*
 * Copyright (C) 2021-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm.h>
#include <vm_config.h>
#include <vboot.h>
#include <guest_memory.h>
#include <pgtable.h>
#include <errno.h>
#include <logmsg.h>
#include <util.h>
#if defined(CONFIG_ARM64)
#include <mmu.h>
#endif

/* [20260710] raw-image loader service principle:
 *
 * The raw-image loader is the BSP service that turns boot metadata into bytes
 * inside a VM RAM window. It does not create stage-2 mappings and does not
 * interpret Linux headers; it trusts vm_config for load and entry GPAs, then
 * enforces that every copied payload fits the configured guest RAM contract.
 *
 *   vm->sw kernel/ramdisk/FDT metadata
 *          |
 *          v
 *   range_fits() / range_overlaps()
 *          |
 *          v
 *   copy_to_gpa()
 *      |
 *      +-- kernel Image
 *      +-- optional initramfs
 *      +-- optional FDT
 *          |
 *          v
 *   vm->sw.kernel_info.kernel_entry_addr
 *
 * The loader reasons in guest GPAs even when current ARM64 platforms are
 * identity-mapped. That keeps the service boundary compatible with future
 * non-identity guest RAM placement.
 */

static bool range_overlaps(uint64_t start_a, uint64_t size_a, uint64_t start_b, uint64_t size_b)
{
	uint64_t end_a = start_a + size_a;
	uint64_t end_b = start_b + size_b;

	return (end_a > start_a) && (end_b > start_b) && (start_a < end_b) && (start_b < end_a);
}

static bool range_fits(uint64_t addr, uint64_t size, uint64_t window_start, uint64_t window_size)
{
	uint64_t addr_end = addr + size;
	uint64_t window_end = window_start + window_size;

	return (addr_end > addr) && (window_end > window_start) &&
		(addr >= window_start) && (addr_end <= window_end);
}

static uint64_t rawimage_range_end(uint64_t base, uint64_t size)
{
	if (size == 0UL) {
		return base;
	}
	if ((size - 1UL) > (UINT64_MAX - base)) {
		return UINT64_MAX;
	}
	return base + size - 1UL;
}

static void rawimage_log_load(uint16_t vm_id, const char *name, uint64_t base, uint64_t size)
{
	if (vm_id <= 3U) {
		LOG_INF("VM%u: load %-7s [0x%016lx-0x%016lx] (0x%08lx)",
			vm_id, name, base, rawimage_range_end(base, size), size);
	}
}

#if defined(CONFIG_ARM64)
#define ARM64_LINUX_IMAGE_ALIGN	MEM_2M

static int32_t arm64_rawimage_check_linux_alignment(struct acrn_vm *vm,
	const struct acrn_vm_config *vm_config, uint64_t kernel_load_gpa)
{
	uint64_t kernel_entry_gpa = vm_config->os_config.kernel_entry_addr;

	if (vm_config->os_config.os_family != VM_OS_LINUX) {
		return 0;
	}

	if (((kernel_load_gpa & (ARM64_LINUX_IMAGE_ALIGN - 1UL)) != 0UL) ||
		((kernel_entry_gpa & (ARM64_LINUX_IMAGE_ALIGN - 1UL)) != 0UL)) {
		LOG_ERR("vm-%u:%-9s arm64 Linux Image must be 2MiB aligned load:0x%016lx entry:0x%016lx",
			vm->vm_id, vm_config->os_config.kernel_mod_tag, kernel_load_gpa,
			kernel_entry_gpa);
		return -EINVAL;
	}

	if (kernel_entry_gpa != kernel_load_gpa) {
		LOG_ERR("vm-%u:%-9s arm64 Linux raw Image entry must match load address load:0x%016lx entry:0x%016lx",
			vm->vm_id, vm_config->os_config.kernel_mod_tag, kernel_load_gpa,
			kernel_entry_gpa);
		return -EINVAL;
	}

	return 0;
}

static uint64_t arm64_rawimage_fdt_load_gpa(struct acrn_vm *vm, uint64_t kernel_load_gpa,
	uint32_t kernel_size, uint64_t ramdisk_load_gpa, uint32_t ramdisk_size)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t ram_start = arch_config->guest_ram_start;
	uint64_t ram_size = arch_config->guest_ram_size;
	uint64_t fdt_size = roundup((uint64_t)vm->sw.fdt_info.size, MEM_4K);
	uint64_t fdt_load_gpa;

	if (fdt_size == 0UL) {
		fdt_size = MEM_4K;
	}
	if ((ram_size <= fdt_size) || ((ram_start + ram_size) <= ram_start)) {
		return 0UL;
	}
	fdt_load_gpa = (ram_start + ram_size - fdt_size) & ~(uint64_t)(MEM_4K - 1U);

	/*
	 * Static RTOS images do not have a Linux bootloader to choose safe module
	 * addresses. Prefer the end of the VM RAM window for the FDT so it stays
	 * away from the raw image text offset and from low-memory guest boot data.
	 * Returning 0 makes overlap explicit instead of silently corrupting the
	 * kernel image.
	 */
	if (!range_fits(fdt_load_gpa, fdt_size, ram_start, ram_size) ||
		range_overlaps(fdt_load_gpa, fdt_size, kernel_load_gpa, kernel_size) ||
		range_overlaps(fdt_load_gpa, fdt_size, ramdisk_load_gpa, ramdisk_size)) {
		return 0UL;
	}

	return fdt_load_gpa;
}
#endif

/**
 * @pre vm != NULL
 */
static int32_t load_rawimage(struct acrn_vm *vm)
{
	struct sw_kernel_info *sw_kernel = &(vm->sw.kernel_info);
	struct sw_module_info *ramdisk_info = &(vm->sw.ramdisk_info);
	const struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	uint64_t kernel_load_gpa;
	uint64_t ram_start;
	uint64_t ram_size;
	uint64_t ramdisk_load_gpa = 0UL;
	uint32_t ramdisk_size = ramdisk_info->size;
	int32_t ret;

	/*
	 * Raw-image startup is the active ARM64 path:
	 *
	 *   vm_config load addresses
	 *        -> range checks against the configured guest RAM window
	 *        -> optional FDT placement inside the same RAM window
	 *        -> copy kernel/initramfs from boot modules to guest GPAs
	 *        -> record kernel_entry_addr for arch_vm_prepare_bsp()
	 *
	 * Stage-2 mappings are already created before this loader runs, so
	 * copy_to_gpa() validates that every destination GPA is backed by the VM's
	 * configured RAM. On current ARM64 platforms that RAM is identity-mapped,
	 * but this loader still reasons in guest GPAs.
	 */

	kernel_load_gpa = vm_config->os_config.kernel_load_addr;
#if defined(CONFIG_ARM64)
	ram_start = vm_config->arch.guest_ram_start;
	ram_size = vm_config->arch.guest_ram_size;
#else
	ram_start = 0UL;
	ram_size = UINT64_MAX;
#endif

	if (!range_fits(kernel_load_gpa, sw_kernel->kernel_size, ram_start, ram_size)) {
		LOG_ERR("vm-%u:%-9s does not fit ram gpa [0x%016lx-0x%016lx] (0x%08lx)",
			vm->vm_id, vm_config->os_config.kernel_mod_tag, kernel_load_gpa,
			rawimage_range_end(kernel_load_gpa, sw_kernel->kernel_size),
			(uint64_t)sw_kernel->kernel_size);
		return -EFAULT;
	}

#if defined(CONFIG_ARM64)
	ret = arm64_rawimage_check_linux_alignment(vm, vm_config, kernel_load_gpa);
	if (ret != 0) {
		return ret;
	}
#endif

	if (ramdisk_size > 0U) {
		ramdisk_load_gpa = vm_config->os_config.kernel_ramdisk_addr;
		if (ramdisk_load_gpa == 0UL) {
			LOG_ERR("vm-%u:%-9s has no load address",
				vm->vm_id, vm_config->os_config.ramdisk_mod_tag);
			return -EFAULT;
		}
		if (!range_fits(ramdisk_load_gpa, ramdisk_size, ram_start, ram_size) ||
			range_overlaps(ramdisk_load_gpa, ramdisk_size, kernel_load_gpa,
				sw_kernel->kernel_size)) {
			LOG_ERR("vm-%u:%-9s does not fit ram gpa [0x%016lx-0x%016lx] (0x%08lx)",
				vm->vm_id, vm_config->os_config.ramdisk_mod_tag, ramdisk_load_gpa,
				rawimage_range_end(ramdisk_load_gpa, ramdisk_size),
				(uint64_t)ramdisk_size);
			return -EFAULT;
		}
		ramdisk_info->load_addr = (void *)ramdisk_load_gpa;
	}

	if (vm->sw.fdt_info.src_addr != NULL) {
#if defined(CONFIG_ARM64)
		uint64_t fdt_load_gpa = arm64_rawimage_fdt_load_gpa(vm, kernel_load_gpa,
			sw_kernel->kernel_size, ramdisk_load_gpa, ramdisk_size);

		/*
		 * The FDT is part of the guest boot ABI, not a separate mapped device.
		 * Keep it in normal guest RAM and away from raw image payloads so the
		 * initial ARM64 register state can pass its GPA directly to EL1.
		 */
		if (fdt_load_gpa == 0UL) {
			LOG_ERR("vm-%u fdt does not fit guest ram without overlapping raw image",
				vm->vm_id);
			return -EFAULT;
		}
		vm->sw.fdt_info.load_addr = (void *)fdt_load_gpa;
#else
		vm->sw.fdt_info.load_addr = (void *)0x40000000UL;
#endif
	}

	/* Copy the guest kernel image to its run-time location */
	ret = copy_to_gpa(vm, sw_kernel->kernel_src_addr, kernel_load_gpa, sw_kernel->kernel_size);
	if (ret != 0) {
		LOG_ERR("vm-%u:%-9s does not fit 1:1 ram gpa [0x%016lx-0x%016lx] (0x%08lx)",
			vm->vm_id, vm_config->os_config.kernel_mod_tag, kernel_load_gpa,
			rawimage_range_end(kernel_load_gpa, sw_kernel->kernel_size),
			(uint64_t)sw_kernel->kernel_size);
		return -EFAULT;
	}

	if (ramdisk_size > 0U) {
		ret = copy_to_gpa(vm, ramdisk_info->src_addr, ramdisk_load_gpa, ramdisk_size);
		if (ret != 0) {
			LOG_ERR("vm-%u:%-9s does not fit 1:1 ram gpa [0x%016lx-0x%016lx] (0x%08lx)",
				vm->vm_id, vm_config->os_config.ramdisk_mod_tag, ramdisk_load_gpa,
				rawimage_range_end(ramdisk_load_gpa, ramdisk_size),
				(uint64_t)ramdisk_size);
			return -EFAULT;
		}
	}

	if ((vm->sw.fdt_info.src_addr != NULL) && (vm->sw.fdt_info.size != 0U) &&
		(vm->sw.fdt_info.load_addr != NULL)) {
		uint64_t fdt_load_gpa = (uint64_t)vm->sw.fdt_info.load_addr;

		ret = copy_to_gpa(vm, vm->sw.fdt_info.src_addr, fdt_load_gpa,
			vm->sw.fdt_info.size);
		if (ret != 0) {
			LOG_ERR("vm-%u:DTB does not fit 1:1 ram gpa [0x%016lx-0x%016lx] (0x%08lx)",
				vm->vm_id, fdt_load_gpa,
				rawimage_range_end(fdt_load_gpa, vm->sw.fdt_info.size),
				(uint64_t)vm->sw.fdt_info.size);
			return -EFAULT;
		}
	}

	sw_kernel->kernel_entry_addr = (void *)vm_config->os_config.kernel_entry_addr;
	rawimage_log_load(vm->vm_id, "KERNEL", kernel_load_gpa, sw_kernel->kernel_size);
	if ((vm->sw.fdt_info.src_addr != NULL) && (vm->sw.fdt_info.size != 0U) &&
		(vm->sw.fdt_info.load_addr != NULL)) {
		rawimage_log_load(vm->vm_id, "DTB", (uint64_t)vm->sw.fdt_info.load_addr,
			vm->sw.fdt_info.size);
	}
	if (ramdisk_size > 0U) {
		rawimage_log_load(vm->vm_id, "RAMDISK", ramdisk_load_gpa, ramdisk_size);
	}
	return 0;
}

int32_t rawimage_loader(struct acrn_vm *vm)
{
	return load_rawimage(vm);
}
