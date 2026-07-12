/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm.h>
#include <hypercall.h>
#include <errno.h>
#include <logmsg.h>


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
