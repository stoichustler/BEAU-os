/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vcpu.h>
#include <vm.h>
#include <vconfig.h>
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
bool arm64_guest_gpa_range_valid(const struct acrn_vm *vm, uint64_t gpa,
	uint64_t size)
{
	const struct arch_vm_config *arch_config;
	uint64_t ram_start;
	uint64_t ram_size;
	uint64_t ram_end;
	uint64_t gpa_end;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM) || (size == 0UL)) {
		return false;
	}
	arch_config = &get_vm_config(vm->vm_id)->arch;
	ram_start = arch_config->guest_ram_start;
	ram_size = arch_config->guest_ram_size;
	if ((ram_size == 0UL) || (ram_start > (UINT64_MAX - ram_size)) ||
		(gpa > (UINT64_MAX - size))) {
		return false;
	}
	ram_end = ram_start + ram_size;
	gpa_end = gpa + size;

	return (gpa >= ram_start) && (gpa_end <= ram_end);
}

/* [20260721] Static guest PA-to-GPA attribution
 *
 * RAS PA -> configured guest RAM HPA window -> configured guest GPA window
 *    |                       |
 *    +--> outside/overflow ---+--> leave GPA unavailable
 *
 * Key rule:
 *   - platform VM configuration owns this immutable RAM correspondence;
 *   - only an exactly reversible byte mapping is reported, preventing a host
 *     PA or dynamic device mapping from being misattributed to a guest GPA.
 */
bool arm64_guest_hpa_to_gpa(const struct acrn_vm *vm, uint64_t hpa,
	uint64_t *gpa)
{
	const struct arch_vm_config *arch_config;
	uint64_t hpa_start;
	uint64_t ram_size;
	uint64_t offset;
	uint64_t candidate;

	if ((vm == NULL) || (gpa == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM)) {
		return false;
	}
	arch_config = &get_vm_config(vm->vm_id)->arch;
	hpa_start = arch_config->guest_ram_hpa;
	ram_size = arch_config->guest_ram_size;
	if ((ram_size == 0UL) || (hpa_start > (UINT64_MAX - ram_size)) ||
		(hpa < hpa_start) || (hpa >= (hpa_start + ram_size))) {
		return false;
	}
	offset = hpa - hpa_start;
	if (arch_config->guest_ram_start > (UINT64_MAX - offset)) {
		return false;
	}
	candidate = arch_config->guest_ram_start + offset;
	if (!arm64_guest_gpa_range_valid(vm, candidate, 1UL) ||
		(gpa2hpa((struct acrn_vm *)vm, candidate) != hpa)) {
		return false;
	}
	*gpa = candidate;
	return true;
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
	uint64_t hpa = INVALID_HPA;

	if (arm64_guest_gpa_range_valid(vm, gpa, 1UL)) {
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
	} else if (arm64_guest_gpa_range_valid(vm, gpa, size)) {
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
	} else if (arm64_guest_gpa_range_valid(vm, gpa, size)) {
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
