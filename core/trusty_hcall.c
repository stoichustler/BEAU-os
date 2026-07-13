/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm.h>
#include <hcall.h>
#include <errno.h>
#include <logmsg.h>

/* [20260713] Trusty hcall world-state principle
 *
 * normal world HVC
 *     |
 *     v
 * validate sworld_control
 *     |
 *     +--> initialize Trusty
 *     |       -> copy boot params from caller GPA -> activate secure world
 *     |
 *     +--> world switch
 *     |       -> current context decides next world -> switch_world()
 *     |
 *     +--> BSP save/restore
 *             -> save secure context -> later restore into same VM
 *
 * Key rule:
 *   - sworld_control is owned by the VM and gates every transition;
 *   - only the BSP saves/restores VM-wide secure-world context;
 *   - invalid ordering fails closed so normal-world guests cannot enter an
 *     uninitialized or already-owned secure context.
 */

int32_t hcall_world_switch(struct acrn_vcpu *vcpu, __unused struct acrn_vm *target_vm,
		__unused uint64_t param1, __unused uint64_t param2)
{
	int32_t next_world_id = !(vcpu->arch.cur_context);
	int32_t ret = -EPERM;

	if ((vcpu->vm->arch_vm.sworld_control.flag.supported != 0UL) && (next_world_id < NR_WORLD)
		&& (vcpu->vm->arch_vm.sworld_control.flag.active != 0UL)) {
		switch_world(vcpu, next_world_id);
		ret = 0;
	}
	return ret;
}

int32_t hcall_initialize_trusty(struct acrn_vcpu *vcpu, __unused struct acrn_vm *target_vm,
		uint64_t param1, __unused uint64_t param2)
{
	int32_t ret = -EFAULT;

	if ((vcpu->vm->arch_vm.sworld_control.flag.supported != 0UL)
		&& (vcpu->vm->arch_vm.sworld_control.flag.active == 0UL)
		&& (vcpu->arch.cur_context == NORMAL_WORLD)) {
		struct trusty_boot_param boot_param;

		if (copy_from_gpa(vcpu->vm, &boot_param, param1, sizeof(boot_param)) == 0) {
			if (initialize_trusty(vcpu, &boot_param)) {
				vcpu->vm->arch_vm.sworld_control.flag.active = 1UL;
				ret = 0;
			}
		}
	} else {
		ret = -EPERM;
		LOG_ERR("%s, context mismatch when initialize trusty.\n", __func__);
	}
	return ret;
}

int32_t hcall_save_restore_sworld_ctx(struct acrn_vcpu *vcpu, __unused struct acrn_vm *target_vm,
		__unused uint64_t param1, __unused uint64_t param2)
{
	struct acrn_vm *vm = vcpu->vm;
	int32_t ret = -EINVAL;

	if (is_vcpu_bsp(vcpu) && (vm->arch_vm.sworld_control.flag.supported != 0UL)) {
		if (vm->arch_vm.sworld_control.flag.active != 0UL) {
			save_sworld_context(vcpu);
			vm->arch_vm.sworld_control.flag.ctx_saved = 1UL;
			ret = 0;
		} else {
			if (vm->arch_vm.sworld_control.flag.ctx_saved != 0UL) {
				restore_sworld_context(vcpu);
				vm->arch_vm.sworld_control.flag.ctx_saved = 0UL;
				vm->arch_vm.sworld_control.flag.active = 1UL;
				ret = 0;
			}
		}
	} else {
		ret = -EPERM;
		LOG_ERR("%s, states mismatch when save restore sworld context.\n", __func__);
	}

	return ret;
}
