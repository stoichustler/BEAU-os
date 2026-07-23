/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <per_cpu.h>
#include <cpu.h>
#include <delay.h>
#include <pgtable.h>
#include <schedule.h>
#include <logmsg.h>
#include <asm/sysreg.h>
#include <asm/psci.h>
#include <asm/platform.h>

extern void _start_secondary_psci(uint64_t phy_stack_addr);

void wait_sync_change(volatile const uint64_t *sync, uint64_t wake_sync)
{
	while ((*sync) != wake_sync) {
		cpu_relax();
	}
}

uint16_t arch_get_pcpu_num(void)
{
	return MAX_PCPU_NUM;
}

void init_percpu_mpidr(uint64_t bsp_mpidr)
{
	uint16_t i;
	uint64_t bsp_aff = bsp_mpidr & MPIDR_AFFINITY_MASK;

	/* [20260724] Topology-owned MPIDR mapping
	 *
	 * embedded DTS CPU reg -> immutable topology snapshot -> PSCI CPU_ON target
	 *
	 * Key rule:
	 *   - platform DTS owns pCPU order and MPIDR identity before per-CPU state;
	 *   - BSP must be CPU0 in the authored static platform topology;
	 *   - reject a mismatch instead of renumbering APs, which would start a
	 *     wrong cluster on heterogeneous systems.
	 */
	if (!beau_config.cpu_topology_valid ||
		(beau_config.cpu_topology_count != MAX_PCPU_NUM) ||
		((beau_config.cpu_topology[BSP_CPU_ID].mpidr & MPIDR_AFFINITY_MASK) != bsp_aff)) {
		panic("invalid arm64 dts BSP MPIDR topology");
	}

	for (i = 0U; i < MAX_PCPU_NUM; i++) {
		per_cpu(arch.mpidr, i) = beau_config.cpu_topology[i].mpidr &
			MPIDR_AFFINITY_MASK;
	}
}

uint16_t get_pcpu_id_from_mpidr(uint64_t mpidr)
{
	uint16_t i;
	uint16_t pcpu_id = INVALID_CPU_ID;
	uint64_t aff = mpidr & MPIDR_AFFINITY_MASK;

	for (i = 0U; i < MAX_PCPU_NUM; i++) {
		if (per_cpu(arch.mpidr, i) == aff) {
			pcpu_id = i;
			break;
		}
	}

	return pcpu_id;
}

void arch_start_pcpu(uint16_t pcpu_id)
{
	uint64_t pcpu_sp;
	int64_t ret;

	if (pcpu_id >= MAX_PCPU_NUM) {
		LOG_FTL("invalid arm64 secondary cpu%hu", pcpu_id);
		return;
	}

	pcpu_sp = (uint64_t)(&per_cpu(stack, pcpu_id)[CONFIG_STACK_SIZE - 1]);
	pcpu_sp &= ~(CPU_STACK_ALIGN - 1UL);

	ret = psci_cpu_on(per_cpu(arch.mpidr, pcpu_id), (uint64_t)_start_secondary_psci, pcpu_sp);
	if ((ret != PSCI_RET_SUCCESS) && (ret != PSCI_RET_ALREADY_ON)) {
		LOG_FTL("psci cpu_on failed for cpu%hu mpidr=0x%lx ret=%ld",
			pcpu_id, per_cpu(arch.mpidr, pcpu_id), ret);
	}
}

void arch_cpu_do_idle(void)
{
	arm64_wfi();
}

void arch_cpu_dead(void)
{
	while (true) {
		arm64_wfi();
	}
}
