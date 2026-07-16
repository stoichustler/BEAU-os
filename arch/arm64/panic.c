/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <vcpu.h>
#include <vm.h>
#include <logmsg.h>
#include <asm/coredump.h>
#include <asm/sysreg.h>

void panic_dump_context(void)
{
	struct arm64_coredump_context context;
	uint16_t pcpu_id;
	struct acrn_vcpu *vcpu = NULL;

	__asm__ volatile(
		"mov %0, x30\n"
		"mov %1, sp\n"
		"mov %2, x29\n"
		: "=r" (context.pc), "=r" (context.sp), "=r" (context.fp));
	context.lr = context.pc;
	pcpu_id = get_pcpu_id();

	if (pcpu_id < MAX_PCPU_NUM) {
		vcpu = get_running_vcpu(pcpu_id);
	}

	LOG_FTL("context: pcpu:%hu active:0x%lx mpidr:0x%lx daif:0x%lx",
		pcpu_id, get_active_pcpu_bitmap(), read_mpidr_el1(), read_daif());
	LOG_FTL("el2:  elr:0x%08lx  spsr:0x%08lx   esr:0x%08lx far:0x%08lx hpfar:0x%08lx",
		read_elr_el2(), read_spsr_el2(), read_esr_el2(), read_far_el2(),
		read_hpfar_el2());
	LOG_FTL("virt: hcr:0x%08lx vttbr:0x%08lx sctlr:0x%08lx",
		read_hcr_el2(), read_vttbr_el2(), read_sctlr_el2());

	if ((vcpu != NULL) && (vcpu->vm != NULL)) {
		LOG_FTL("vcpu: vm:%hu vcpu:%hu state:%u pcpu:%hu pending:0x%lx",
			vcpu->vm->vm_id, vcpu->vcpu_id, vcpu_get_state(vcpu),
			pcpuid_from_vcpu(vcpu), vcpu->pending_req);
	} else {
		LOG_FTL("vcpu: none");
	}
	arm64_coredump_log(&context, pcpu_id, LOG_FATAL);
}
