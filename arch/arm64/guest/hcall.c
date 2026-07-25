/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vcpu.h>
#include <guest_memory.h>
#include <hcall.h>
#include <acrn_hv_defs.h>
#include <ai_sched.h>
#include <vm_wdt.h>
#include <version.h>
#include <logmsg.h>
#include <trace.h>
#include <virtio_proxy.h>
#include <asm/guest/vipc.h>

/* [20260630] ARM64 hcall dispatch principle:
 *
 * ARM64 guests use HVC for two ABIs in this bring-up tree:
 * - PSCI, identified by the standard PSCI function IDs.
 * - ACRN hypercalls, identified by HC_ID in the top ID byte.
 *
 *   HVC64 exit
 *      |
 *      +--> PSCI function ID -> vcpu_exit.c power/CPU emulation
 *      |
 *      +--> ACRN HC_ID       -> this table -> small ARM64-safe handlers
 *
 * Keep the ACRN dispatcher small and explicit. Device assignment, profiling,
 * and unsupported legacy hypercalls return -ENOTTY until their ARM64
 * dependencies exist here.
 */
/* [20260713] hcall permission and target ownership
 *
 * guest x0..x2
 *     |
 *     v
 * mask HC_ID/function
 *     |
 *     v
 * dispatch table
 *     |
 *     +--> service VM operation
 *     |       -> relative VMID -> target VM lock -> handler()
 *     |
 *     +--> static guest operation
 *     |       -> guest flag check -> handler(vcpu, own VM)
 *     |
 *     +--> unsupported / wrong owner
 *             -> fail closed with -ENOTTY
 *
 * Key rule:
 *   - the table is the ABI boundary and permission map;
 *   - service-VM calls lock the target before touching target state;
 *   - guest self-service calls never borrow another VM's ownership;
 *   - failed dispatches are observable, but noisy retry paths stay debug-only.
 */
#define ACRN_HCALL_ID_MASK	0xffffffffUL
#define ACRN_HCALL_FUNC(id)	((id) & ACRN_HCALL_ID_MASK)
#define ACRN_HCALL_ID(id)	((ACRN_HCALL_FUNC(id) >> 24U) == HC_ID)

static int32_t hcall_arm64_get_api_version(struct acrn_vcpu *vcpu,
	__unused struct acrn_vm *target_vm, uint64_t param1,
	__unused uint64_t param2)
{
	struct hc_api_version version;

	version.major_version = HV_API_MAJOR_VERSION;
	version.minor_version = HV_API_MINOR_VERSION;

	return copy_to_gpa(vcpu->vm, &version, param1, sizeof(version));
}

static int32_t hcall_arm64_gpa_to_hpa(struct acrn_vcpu *vcpu,
	struct acrn_vm *target_vm, __unused uint64_t param1, uint64_t param2)
{
	struct vm_gpa2hpa v_gpa2hpa;
	int32_t ret = -EINVAL;

	(void)memset(&v_gpa2hpa, 0U, sizeof(v_gpa2hpa));
	if (!is_poweroff_vm(target_vm) &&
		(copy_from_gpa(vcpu->vm, &v_gpa2hpa, param2, sizeof(v_gpa2hpa)) == 0)) {
		v_gpa2hpa.hpa = gpa2hpa(target_vm, v_gpa2hpa.gpa);
		if (v_gpa2hpa.hpa != INVALID_HPA) {
			ret = copy_to_gpa(vcpu->vm, &v_gpa2hpa, param2, sizeof(v_gpa2hpa));
		}
	}

	return ret;
}

static int32_t hcall_arm64_get_hw_info(struct acrn_vcpu *vcpu,
	__unused struct acrn_vm *target_vm, uint64_t param1,
	__unused uint64_t param2)
{
	struct acrn_hw_info hw_info;

	(void)memset(&hw_info, 0U, sizeof(hw_info));
	hw_info.cpu_num = get_pcpu_nums();

	return copy_to_gpa(vcpu->vm, &hw_info, param1, sizeof(hw_info));
}

static int32_t hcall_arm64_virtio_proxy_backend(struct acrn_vcpu *vcpu,
	__unused struct acrn_vm *target_vm, uint64_t param1,
	__unused uint64_t param2)
{
	return virtio_proxy_backend_hcall(vcpu, param1);
}

static int32_t hcall_arm64_ipc(struct acrn_vcpu *vcpu,
	__unused struct acrn_vm *target_vm, uint64_t param1,
	__unused uint64_t param2)
{
	return arm64_vipc_hcall(vcpu, param1);
}

static int32_t hcall_arm64_not_supported(__unused struct acrn_vcpu *vcpu,
	__unused struct acrn_vm *target_vm, __unused uint64_t param1,
	__unused uint64_t param2)
{
	return -ENOTTY;
}

static const struct hcall_dispatch_entry arm64_hc_dispatch_table[] = {
	[HC_IDX(HC_GET_API_VERSION)] = {
		.handler = hcall_arm64_get_api_version,
	},
	[HC_IDX(HC_SERVICE_VM_OFFLINE_CPU)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_SET_CALLBACK_VECTOR)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_CREATE_VM)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_DESTROY_VM)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_START_VM)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_PAUSE_VM)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_CREATE_VCPU)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_RESET_VM)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_SET_VCPU_REGS)] = {
		.handler = hcall_arm64_not_supported,
	},
	[HC_IDX(HC_VM_GPA2HPA)] = {
		.handler = hcall_arm64_gpa_to_hpa,
	},
	[HC_IDX(HC_GET_HW_INFO)] = {
		.handler = hcall_arm64_get_hw_info,
	},
	[HC_IDX(HC_VM_WDT_KICK)] = {
		.handler = hcall_vm_wdt_kick,
		.permission_flags = GUEST_FLAG_STATIC_VM,
	},
	[HC_IDX(HC_VIRTIO_PROXY_BACKEND)] = {
		.handler = hcall_arm64_virtio_proxy_backend,
		.permission_flags = GUEST_FLAG_STATIC_VM,
	},
	[HC_IDX(HC_IPC)] = {
		.handler = hcall_arm64_ipc,
		.permission_flags = GUEST_FLAG_STATIC_VM,
	},
	[HC_IDX(HC_AI_SCHED)] = {
		.handler = hcall_ai_sched,
		.permission_flags = GUEST_FLAG_STATIC_VM,
	},
};

static struct acrn_vm *arm64_hcall_target_vm(struct acrn_vm *vm, uint64_t hcall_id,
	uint64_t param1)
{
	uint16_t vm_id;

	switch (hcall_id) {
	case HC_GET_API_VERSION:
	case HC_GET_HW_INFO:
		return vm;
	default:
		vm_id = rel_vmid_2_vmid(vm->vm_id, (uint16_t)param1);
		return (vm_id < CONFIG_MAX_VM_NUM) ? get_vm_from_vmid(vm_id) : NULL;
	}
}

bool arm64_is_acrn_hypercall(uint64_t hcall_id)
{
	return ACRN_HCALL_ID(hcall_id);
}

int32_t arm64_dispatch_hypercall(struct acrn_vcpu *vcpu)
{
	struct acrn_vm *vm = vcpu->vm;
	uint64_t hcall_id = ACRN_HCALL_FUNC(vcpu->arch.regs.x0);
	uint64_t param1 = vcpu->arch.regs.x1;
	uint64_t param2 = vcpu->arch.regs.x2;
	int32_t ret = -ENOTTY;

	if (HC_IDX(hcall_id) < ARRAY_SIZE(arm64_hc_dispatch_table)) {
		const struct hcall_dispatch_entry *dispatch =
			&arm64_hc_dispatch_table[HC_IDX(hcall_id)];
		uint64_t guest_flags = get_vm_config(vm->vm_id)->guest_flags;
		uint64_t permission_flags = dispatch->permission_flags;

		if (dispatch->handler != NULL) {
			if ((permission_flags == 0UL) && is_service_vm(vm)) {
				struct acrn_vm *target_vm =
					arm64_hcall_target_vm(vm, hcall_id, param1);

				if (target_vm != NULL) {
					get_vm_lock(target_vm);
					ret = dispatch->handler(vcpu, target_vm, param1, param2);
					put_vm_lock(target_vm);
				}
			} else if ((permission_flags != 0UL) &&
				((guest_flags & permission_flags) != 0UL)) {
				ret = dispatch->handler(vcpu, vm, param1, param2);
			}
		}
	}

	vcpu->arch.regs.x0 = (uint64_t)ret;
	if ((ret < 0) && !((hcall_id == HC_VIRTIO_PROXY_BACKEND) &&
		((ret == -EBUSY) || (ret == -ENODEV) || (ret == -ENODATA)))) {
		LOG_DBG("HCALL:  0x%lx ret:%d", hcall_id, ret);
	}
	TRACE_2L(TRACE_VMEXIT_VMCALL, vm->vm_id, hcall_id);

	return 0;
}
