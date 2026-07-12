/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vcpu.h>
#include <vm.h>
#include <vm_config.h>
#include <guest_memory.h>
#include <errno.h>

/* [20260630] guest-memory principle:
 *
 * Raw-image loading and guest-copy helpers use the same platform RAM window
 * that stage-2 maps in arch/arm64/guest/vm.c:
 *
 *   copy_to/from_gpa()
 *          |
 *          v
 *   validate GPA inside configured RAM window
 *          |
 *          v
 *   guest IPA/GPA = guest_ram_start + offset
 *   host PA       = guest_ram_hpa   + offset
 *
 * The current ARM64 static platforms set guest_ram_hpa == guest_ram_start, so
 * this offset calculation collapses to a 1:1 GPA-to-HPA mapping. Keeping the
 * formula explicit makes the ownership boundary visible and gives future
 * non-identity work one place to audit before relaxing the stage-2 identity
 * check.
 */
static bool gpa_range_is_valid(struct acrn_vm *vm, uint64_t gpa, uint32_t size)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t ram_start = arch_config->guest_ram_start;
	uint64_t ram_size = arch_config->guest_ram_size;
	uint64_t ram_end = ram_start + ram_size;
	uint64_t gpa_end = gpa + size;
	bool ret = false;

	if ((size > 0U) && (gpa_end >= gpa) && (ram_end >= ram_start) &&
		(gpa >= ram_start) && (gpa_end <= ram_end)) {
		ret = true;
	}

	return ret;
}

int32_t gva2gpa(struct acrn_vcpu *vcpu, uint64_t gva, uint64_t *gpa, uint32_t *err_code)
{
	(void)vcpu;
	(void)gva;
	(void)gpa;
	(void)err_code;
	return -ENOSYS;
}

uint64_t gpa2hpa(struct acrn_vm *vm, uint64_t gpa)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t ram_start = arch_config->guest_ram_start;
	uint64_t ram_size = arch_config->guest_ram_size;
	uint64_t hpa = INVALID_HPA;

	if ((gpa >= ram_start) && (gpa < (ram_start + ram_size))) {
		hpa = arch_config->guest_ram_hpa + (gpa - ram_start);
	}

	return hpa;
}

enum vm_paging_mode get_vcpu_paging_mode(struct acrn_vcpu *vcpu)
{
	(void)vcpu;
	return PAGING_MODE_4_LEVEL;
}

void *gpa2hva(struct acrn_vm *vm, uint64_t x)
{
	uint64_t hpa = gpa2hpa(vm, x);

	return (hpa == INVALID_HPA) ? NULL : hpa2hva(hpa);
}

int32_t copy_from_gpa(struct acrn_vm *vm, void *h_ptr, uint64_t gpa, uint32_t size)
{
	void *hva = NULL;
	int32_t ret = -EFAULT;

	if (size == 0U) {
		ret = 0;
	} else if (gpa_range_is_valid(vm, gpa, size)) {
		hva = gpa2hva(vm, gpa);
		memcpy(h_ptr, hva, size);
		ret = 0;
	}

	return ret;
}

int32_t copy_to_gpa(struct acrn_vm *vm, void *h_ptr, uint64_t gpa, uint32_t size)
{
	void *hva = NULL;
	int32_t ret = -EFAULT;

	if (size == 0U) {
		ret = 0;
	} else if (gpa_range_is_valid(vm, gpa, size)) {
		hva = gpa2hva(vm, gpa);
		memcpy(hva, h_ptr, size);
		ret = 0;
	}

	return ret;
}

int32_t copy_from_gva(struct acrn_vcpu *vcpu, void *h_ptr, uint64_t gva,
	uint32_t size, uint32_t *err_code, uint64_t *fault_addr)
{
	(void)vcpu;
	(void)h_ptr;
	(void)gva;
	(void)size;
	(void)err_code;
	(void)fault_addr;
	return -ENOSYS;
}

int32_t copy_to_gva(struct acrn_vcpu *vcpu, void *h_ptr, uint64_t gva,
	uint32_t size, uint32_t *err_code, uint64_t *fault_addr)
{
	(void)vcpu;
	(void)h_ptr;
	(void)gva;
	(void)size;
	(void)err_code;
	(void)fault_addr;
	return -ENOSYS;
}
