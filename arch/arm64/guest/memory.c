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
#include <cpu.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>
#include <asm/guest/stage2.h>

#define ARM64_GVA_PAGE_SHIFT		12U
#define ARM64_GVA_PAGE_SIZE		(1UL << ARM64_GVA_PAGE_SHIFT)
#define ARM64_GVA_PAGE_MASK		(ARM64_GVA_PAGE_SIZE - 1UL)
#define ARM64_GVA_TCR_T0SZ_MASK	0x3fUL
#define ARM64_GVA_TCR_T1SZ_SHIFT	16U
#define ARM64_GVA_TCR_TG0_SHIFT	14U
#define ARM64_GVA_TCR_TG1_SHIFT	30U
#define ARM64_GVA_TCR_TG_MASK		0x3UL
#define ARM64_GVA_TCR_EPD0		(1UL << 7U)
#define ARM64_GVA_TCR_EPD1		(1UL << 23U)
#define ARM64_GVA_TCR_TBI0		(1UL << 37U)
#define ARM64_GVA_TCR_TBI1		(1UL << 38U)
#define ARM64_GVA_TCR_TG0_4K		0UL
#define ARM64_GVA_TCR_TG1_4K		2UL
#define ARM64_GVA_SCTLR_M		(1UL << 0U)
#define ARM64_GVA_PTE_VALID		(1UL << 0U)
#define ARM64_GVA_PTE_TABLE		(1UL << 1U)
#define ARM64_GVA_PTE_AF		(1UL << 10U)
#define ARM64_GVA_PTE_AP_SHIFT		6U
#define ARM64_GVA_PTE_AP_MASK		0x3UL
#define ARM64_GVA_PTE_APTABLE_SHIFT	61U
#define ARM64_GVA_PTE_APTABLE_MASK	0x3UL
#define ARM64_GVA_LEVELS		4U

enum arm64_guest_memory_access {
	ARM64_GUEST_MEMORY_READ = 0U,
	ARM64_GUEST_MEMORY_WRITE,
};

struct arm64_guest_gva_context {
	uint64_t sctlr;
	uint64_t tcr;
	uint64_t ttbr0;
	uint64_t ttbr1;
	uint64_t generation;
	bool live;
};

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

/* [20260806] Guest virtual-memory copy boundary
 *
 * vCPU EL1 translation snapshot
 *        |
 *        v
 * bounded Stage-1 walk through guest GPA copies
 *        |
 *        +--> invalid descriptor, permission, or S2 mismatch -> fail closed
 *        |
 *        v
 * page-sized GPA copy and lifecycle-generation recheck
 *
 * Key rule:
 *   - a running vCPU owns live EL1 registers; a paused vCPU owns gctx;
 *   - every guest page-table entry and final GPA is validated through the
 *     VM's static RAM and Stage-2 mapping before EL2 dereferences it;
 *   - no result may survive a VM lifecycle transition, preventing reset or
 *     teardown from turning a valid translation into a stale host access.
 */
static void arm64_guest_memory_report(uint32_t *err_code, uint64_t *fault_addr,
	enum guest_memory_error error, uint64_t gva)
{
	if (err_code != NULL) {
		*err_code = (uint32_t)error;
	}
	if (fault_addr != NULL) {
		*fault_addr = (error == GUEST_MEMORY_ERR_NONE) ? 0UL : gva;
	}
}

static uint64_t arm64_guest_memory_generation(const struct acrn_vm *vm)
{
	return __atomic_load_n(&vm->lifecycle.generation, __ATOMIC_ACQUIRE);
}

static int32_t arm64_guest_gva_context_snapshot(struct acrn_vcpu *vcpu,
	struct arm64_guest_gva_context *context)
{
	const struct arm64_vcpu_guest_ctx *gctx;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (context == NULL)) {
		return -EINVAL;
	}
	(void)memset(context, 0U, sizeof(*context));
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		context->sctlr = read_sctlr_el1();
		context->tcr = read_tcr_el1();
		context->ttbr0 = read_ttbr0_el1();
		context->ttbr1 = read_ttbr1_el1();
		context->live = true;
	} else if (is_vcpu_paused(vcpu)) {
		gctx = &vcpu->arch.gctx;
		context->sctlr = gctx->sctlr_el1;
		context->tcr = gctx->tcr_el1;
		context->ttbr0 = gctx->ttbr0_el1;
		context->ttbr1 = gctx->ttbr1_el1;
	} else {
		return -EBUSY;
	}
	context->generation = arm64_guest_memory_generation(vcpu->vm);

	return (context->generation != 0UL) ? 0 : -EAGAIN;
}

static bool arm64_guest_gva_is_low(uint64_t gva, uint32_t t0sz)
{
	return (t0sz > 0U) && (t0sz < 64U) && ((gva >> (64U - t0sz)) == 0UL);
}

static bool arm64_guest_gva_is_high(uint64_t gva, uint32_t t1sz)
{
	return (t1sz > 0U) && (t1sz < 64U) &&
		((gva >> (64U - t1sz)) == ((1UL << t1sz) - 1UL));
}

static int32_t arm64_guest_gva_select_ttbr(const struct arm64_guest_gva_context *context,
	uint64_t input_gva, uint64_t *gva, uint64_t *ttbr)
{
	uint32_t t0sz;
	uint32_t t1sz;
	uint32_t tg0;
	uint32_t tg1;
	uint64_t untagged_gva;

	if ((context == NULL) || (gva == NULL) || (ttbr == NULL)) {
		return -EINVAL;
	}
	t0sz = (uint32_t)(context->tcr & ARM64_GVA_TCR_T0SZ_MASK);
	t1sz = (uint32_t)((context->tcr >> ARM64_GVA_TCR_T1SZ_SHIFT) &
		ARM64_GVA_TCR_T0SZ_MASK);
	tg0 = (uint32_t)((context->tcr >> ARM64_GVA_TCR_TG0_SHIFT) &
		ARM64_GVA_TCR_TG_MASK);
	tg1 = (uint32_t)((context->tcr >> ARM64_GVA_TCR_TG1_SHIFT) &
		ARM64_GVA_TCR_TG_MASK);
	if ((tg0 != ARM64_GVA_TCR_TG0_4K) || (tg1 != ARM64_GVA_TCR_TG1_4K) ||
		(t0sz != 16U) || (t1sz != 16U)) {
		return -ENOTSUP;
	}

	untagged_gva = input_gva & 0x00ffffffffffffffUL;
	if (((context->tcr & ARM64_GVA_TCR_TBI0) != 0UL) &&
		arm64_guest_gva_is_low(untagged_gva, t0sz)) {
		if ((context->tcr & ARM64_GVA_TCR_EPD0) != 0UL) {
			return -EFAULT;
		}
		*gva = untagged_gva;
		*ttbr = context->ttbr0;
		return 0;
	}
	if (((context->tcr & ARM64_GVA_TCR_TBI1) != 0UL) &&
		arm64_guest_gva_is_high(untagged_gva | 0xff00000000000000UL, t1sz)) {
		if ((context->tcr & ARM64_GVA_TCR_EPD1) != 0UL) {
			return -EFAULT;
		}
		*gva = untagged_gva | 0xff00000000000000UL;
		*ttbr = context->ttbr1;
		return 0;
	}
	if (arm64_guest_gva_is_low(input_gva, t0sz)) {
		if ((context->tcr & ARM64_GVA_TCR_EPD0) != 0UL) {
			return -EFAULT;
		}
		*gva = input_gva;
		*ttbr = context->ttbr0;
		return 0;
	}
	if (arm64_guest_gva_is_high(input_gva, t1sz)) {
		if ((context->tcr & ARM64_GVA_TCR_EPD1) != 0UL) {
			return -EFAULT;
		}
		*gva = input_gva;
		*ttbr = context->ttbr1;
		return 0;
	}

	return -EFAULT;
}

static int32_t arm64_guest_gva_validate_gpa(struct acrn_vm *vm, uint64_t gpa,
	enum arm64_guest_memory_access access)
{
	struct arm64_stage2_walk walk;
	uint64_t descriptor;
	uint64_t expected_hpa;
	int32_t ret;

	if (!arm64_guest_gpa_range_valid(vm, gpa, 1UL)) {
		return -EFAULT;
	}
	ret = arm64_stage2_walk(vm, gpa, &walk);
	if ((ret != 0) || (walk.result != ARM64_STAGE2_WALK_MAPPED) ||
		(walk.step_count == 0U)) {
		return (ret != 0) ? ret : -EFAULT;
	}
	descriptor = walk.step[walk.step_count - 1U].descriptor;
	expected_hpa = gpa2hpa(vm, gpa);
	if (((descriptor & PAGE_S2_MEMATTR_MASK) != PAGE_S2_MEMATTR_NORMAL) ||
		((descriptor & PAGE_S2_S2AP_READ) == 0UL) ||
		((access == ARM64_GUEST_MEMORY_WRITE) &&
		 ((descriptor & PAGE_S2_S2AP_WRITE) == 0UL)) ||
		(expected_hpa == INVALID_HPA) || (walk.hpa != expected_hpa)) {
		return -EACCES;
	}

	return 0;
}

static int32_t arm64_guest_gva_read_pte(struct acrn_vm *vm, uint64_t gpa,
	uint64_t *descriptor)
{
	int32_t ret;

	if (descriptor == NULL) {
		return -EINVAL;
	}
	ret = arm64_guest_gva_validate_gpa(vm, gpa, ARM64_GUEST_MEMORY_READ);
	if (ret == 0) {
		ret = copy_from_gpa(vm, descriptor, gpa, sizeof(*descriptor));
	}

	return ret;
}

static int32_t arm64_guest_gva_translate(const struct acrn_vcpu *vcpu,
	const struct arm64_guest_gva_context *context, uint64_t input_gva,
	enum arm64_guest_memory_access access, uint64_t *gpa)
{
	uint64_t gva;
	uint64_t table_gpa;
	uint64_t descriptor;
	uint64_t pte_gpa;
	uint64_t page_size;
	uint64_t output_base;
	uint32_t level;
	int32_t ret;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (context == NULL) ||
		(gpa == NULL)) {
		return -EINVAL;
	}
	if ((context->sctlr & ARM64_GVA_SCTLR_M) == 0UL) {
		if (!arm64_guest_gpa_range_valid(vcpu->vm, input_gva, 1UL)) {
			return -EFAULT;
		}
		ret = arm64_guest_gva_validate_gpa(vcpu->vm, input_gva, access);
		if (ret == 0) {
			*gpa = input_gva;
		}
		return ret;
	}
	ret = arm64_guest_gva_select_ttbr(context, input_gva, &gva, &table_gpa);
	if (ret != 0) {
		return ret;
	}
	table_gpa &= PAGE_PFN_MASK;
	for (level = 0U; level < ARM64_GVA_LEVELS; level++) {
		uint64_t shift = 39U - ((uint64_t)level * 9UL);
		uint64_t index = (gva >> shift) & (PG_TABLE_ENTRIES - 1UL);
		uint64_t ap_table;

		if ((table_gpa > (UINT64_MAX - (index * sizeof(descriptor))))) {
			return -EFAULT;
		}
		pte_gpa = table_gpa + (index * sizeof(descriptor));
		ret = arm64_guest_gva_read_pte(vcpu->vm, pte_gpa, &descriptor);
		if (ret != 0) {
			return ret;
		}
		if ((descriptor & ARM64_GVA_PTE_VALID) == 0UL) {
			return -EFAULT;
		}
		if (level == (ARM64_GVA_LEVELS - 1U)) {
			if ((descriptor & ARM64_GVA_PTE_TABLE) == 0UL) {
				return -EFAULT;
			}
			page_size = ARM64_GVA_PAGE_SIZE;
		} else if ((descriptor & ARM64_GVA_PTE_TABLE) == 0UL) {
			if (level == 0U) {
				return -EFAULT;
			}
			page_size = 1UL << shift;
		} else {
			ap_table = (descriptor >> ARM64_GVA_PTE_APTABLE_SHIFT) &
				ARM64_GVA_PTE_APTABLE_MASK;
			if ((access == ARM64_GUEST_MEMORY_WRITE) && (ap_table >= 2UL)) {
				return -EACCES;
			}
			table_gpa = descriptor & PAGE_PFN_MASK;
			continue;
		}
		if (((descriptor & ARM64_GVA_PTE_AF) == 0UL) ||
			((access == ARM64_GUEST_MEMORY_WRITE) &&
			 (((descriptor >> ARM64_GVA_PTE_AP_SHIFT) & ARM64_GVA_PTE_AP_MASK) >= 2UL))) {
			return -EACCES;
		}
		output_base = (descriptor & PAGE_PFN_MASK) & ~(page_size - 1UL);
		if ((gva & (page_size - 1UL)) > (UINT64_MAX - output_base)) {
			return -EFAULT;
		}
		*gpa = output_base + (gva & (page_size - 1UL));

		return arm64_guest_gva_validate_gpa(vcpu->vm, *gpa, access);
	}

	return -EFAULT;
}

static enum guest_memory_error arm64_guest_gva_error(int32_t ret)
{
	if (ret == -EBUSY) {
		return GUEST_MEMORY_ERR_VCPU_BUSY;
	}
	if (ret == -EAGAIN) {
		return GUEST_MEMORY_ERR_RETRY;
	}
	if (ret == -EACCES) {
		return GUEST_MEMORY_ERR_PERMISSION;
	}
	if ((ret == -ENODEV) || (ret == -EIO)) {
		return GUEST_MEMORY_ERR_STAGE2;
	}
	return GUEST_MEMORY_ERR_TRANSLATION;
}

int32_t gva2gpa(struct acrn_vcpu *vcpu, uint64_t gva, uint64_t *gpa,
	uint32_t *err_code)
{
	struct arm64_guest_gva_context context;
	int32_t ret;

	if (gpa == NULL) {
		arm64_guest_memory_report(err_code, NULL, GUEST_MEMORY_ERR_ARGUMENT, gva);
		return -EINVAL;
	}
	ret = arm64_guest_gva_context_snapshot(vcpu, &context);
	if (ret == 0) {
		ret = arm64_guest_gva_translate(vcpu, &context, gva,
			ARM64_GUEST_MEMORY_READ, gpa);
		if ((ret == 0) &&
			(arm64_guest_memory_generation(vcpu->vm) != context.generation)) {
			ret = -EAGAIN;
		}
	}
	arm64_guest_memory_report(err_code, NULL,
		(ret == 0) ? GUEST_MEMORY_ERR_NONE : arm64_guest_gva_error(ret), gva);

	return ret;
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
	} else if (h_ptr == NULL) {
		ret = -EINVAL;
	} else if (arm64_guest_gpa_range_valid(vm, gpa, size)) {
		hva = gpa2hva(vm, gpa);
		if (hva != NULL) {
			memcpy(h_ptr, hva, size);
			ret = 0;
		}
	}

	return ret;
}

int32_t copy_to_gpa(struct acrn_vm *vm, void *h_ptr, uint64_t gpa, uint32_t size)
{
	void *hva = NULL;
	int32_t ret = -EFAULT;

	if (size == 0U) {
		ret = 0;
	} else if (h_ptr == NULL) {
		ret = -EINVAL;
	} else if (arm64_guest_gpa_range_valid(vm, gpa, size)) {
		hva = gpa2hva(vm, gpa);
		if (hva != NULL) {
			memcpy(hva, h_ptr, size);
			ret = 0;
		}
	}

	return ret;
}

int32_t copy_from_gva(struct acrn_vcpu *vcpu, void *h_ptr, uint64_t gva,
	uint32_t size, uint32_t *err_code, uint64_t *fault_addr)
{
	struct arm64_guest_gva_context context;
	uint8_t *destination = h_ptr;
	uint64_t current_gva = gva;
	uint32_t copied = 0U;
	int32_t ret = 0;

	if ((size != 0U) && ((h_ptr == NULL) || (gva > (UINT64_MAX - size)))) {
		arm64_guest_memory_report(err_code, fault_addr, GUEST_MEMORY_ERR_ARGUMENT, gva);
		return -EINVAL;
	}
	if (size != 0U) {
		ret = arm64_guest_gva_context_snapshot(vcpu, &context);
	}
	while ((ret == 0) && (copied < size)) {
		uint64_t gpa;
		uint32_t remaining = size - copied;
		uint32_t page_remaining = (uint32_t)(ARM64_GVA_PAGE_SIZE -
			(current_gva & ARM64_GVA_PAGE_MASK));
		uint32_t chunk = (remaining < page_remaining) ? remaining : page_remaining;

		ret = arm64_guest_gva_translate(vcpu, &context, current_gva,
			ARM64_GUEST_MEMORY_READ, &gpa);
		if (ret == 0) {
			ret = copy_from_gpa(vcpu->vm, &destination[copied], gpa, chunk);
		}
		if ((ret == 0) &&
			(arm64_guest_memory_generation(vcpu->vm) != context.generation)) {
			ret = -EAGAIN;
		}
		if (ret == 0) {
			copied += chunk;
			current_gva += chunk;
		}
	}
	arm64_guest_memory_report(err_code, fault_addr,
		(ret == 0) ? GUEST_MEMORY_ERR_NONE : arm64_guest_gva_error(ret), current_gva);

	return ret;
}

int32_t copy_to_gva(struct acrn_vcpu *vcpu, void *h_ptr, uint64_t gva,
	uint32_t size, uint32_t *err_code, uint64_t *fault_addr)
{
	struct arm64_guest_gva_context context;
	const uint8_t *source = h_ptr;
	uint64_t current_gva = gva;
	uint32_t copied = 0U;
	int32_t ret = 0;

	if ((size != 0U) && ((h_ptr == NULL) || (gva > (UINT64_MAX - size)))) {
		arm64_guest_memory_report(err_code, fault_addr, GUEST_MEMORY_ERR_ARGUMENT, gva);
		return -EINVAL;
	}
	if (size != 0U) {
		ret = arm64_guest_gva_context_snapshot(vcpu, &context);
	}
	while ((ret == 0) && (copied < size)) {
		uint64_t gpa;
		uint32_t remaining = size - copied;
		uint32_t page_remaining = (uint32_t)(ARM64_GVA_PAGE_SIZE -
			(current_gva & ARM64_GVA_PAGE_MASK));
		uint32_t chunk = (remaining < page_remaining) ? remaining : page_remaining;

		ret = arm64_guest_gva_translate(vcpu, &context, current_gva,
			ARM64_GUEST_MEMORY_WRITE, &gpa);
		if (ret == 0) {
			ret = copy_to_gpa(vcpu->vm, (void *)&source[copied], gpa, chunk);
		}
		if ((ret == 0) &&
			(arm64_guest_memory_generation(vcpu->vm) != context.generation)) {
			ret = -EAGAIN;
		}
		if (ret == 0) {
			copied += chunk;
			current_gva += chunk;
		}
	}
	arm64_guest_memory_report(err_code, fault_addr,
		(ret == 0) ? GUEST_MEMORY_ERR_NONE : arm64_guest_gva_error(ret), current_gva);

	return ret;
}
