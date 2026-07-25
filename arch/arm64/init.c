/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <per_cpu.h>
#include <logmsg.h>
#include <mmu.h>
#include <timer.h>
#include <vm.h>
#include <vm_wdt.h>
#include <ptdev.h>
#include <console.h>
#include <shell.h>
#include <serial.h>
#include <boot.h>
#include <bsp/pci.h>
#include <bsp/cpufreq.h>
#include <fdt_api.h>
#include <barrier.h>
#include <asm/platform.h>
#include <asm/irq.h>
#include <asm/cache.h>
#include <asm/pmu.h>
#if CONFIG_ARM64_SPE
#include <asm/spe.h>
#endif
#include <asm/security.h>

/* [20260630] ARM64 host boot principle:
 *
 * Assembly entry performs only the EL2 state that must exist before C code can
 * run: exception masking, SP_EL2 selection, vector-base installation, BSS
 * clearing, and the first stack. The C path then builds the host in two phases:
 * global BSP setup first, followed by common per-pCPU setup for both the BSP
 * and APs.
 *
 *   _start
 *      |
 *      v
 *   init_primary_pcpu()
 *     - percpu identity
 *     - early console
 *     - EL2 stage-1 MMU
 *     - early GIC
 *     - switch to per-CPU stack
 *      |
 *      v
 *   init_pcpu_comm_post()
 *     - IRQ / SGI / timer
 *     - scheduler
 *     - shell on BSP
 *     - VM launch after APs are running
 *     - idle thread
 *
 * Secondary CPUs skip global setup: PSCI drops them into
 * _start_secondary_psci(), they enable the already-built EL2 stage-1 map, and
 * then join the same init_pcpu_comm_post() path.
 */
static void init_pcpu_comm_post(void);
static volatile bool boot_vm_launch_released;

static void arm64_release_vm_launch(void)
{
	cpu_write_memory_barrier();
	boot_vm_launch_released = true;
}

static void arm64_wait_vm_launch(uint16_t pcpu_id)
{
	if (pcpu_id == BSP_CPU_ID) {
		return;
	}

	while (!boot_vm_launch_released) {
		asm_pause();
	}
	cpu_read_memory_barrier();
}

/* [20260630] ARM64 BSP stack handoff principle:
 *
 * The boot CPU enters C on the temporary _boot_stack_end stack. After EL2
 * stage-1 mappings and per-pCPU identity are ready, common pCPU setup should
 * run on the BSP's own scheduler stack so IRQ, timer, scheduler, shell, and VM
 * launch code all share the same per-CPU stack model used by secondary CPUs.
 *
 *   _boot_stack_end
 *        |
 *        v
 *   SWITCH_TO(per_cpu(stack), init_pcpu_comm_post)
 *        |
 *        v
 *   IRQ / timer / scheduler / idle use the pCPU stack
 *
 * The magic word seeds the bottom frame for host call-trace unwinding. The
 * target path is expected to end in run_idle_thread() or a fatal path, not by
 * returning to the temporary boot-stack flow.
 */
#define SWITCH_TO(sp, to)					\
{											\
	asm volatile (							\
		"mov	sp, %0\n"					\
		"sub	sp, sp, #16\n"				\
		"str	%1, [sp]\n"					\
		"blr	%2\n"						\
		:									\
		: "r" (sp), "r" (SP_BOTTOM_MAGIC), "r" (to) \
		: "memory");						\
}

static void init_debug_pre(void)
{
	console_init();

	/* [20260723] BEAU OS BANNER */
	beau_tag();
}

static void init_debug_post(uint16_t pcpu_id)
{
	if (pcpu_id == BSP_CPU_ID) {
		shell_init();
	}

	if (pcpu_id == VUART_TIMER_CPU) {
		console_setup_timer();
	}
}

static void arm64_init_pci(void)
{
	pci_switch_to_mmio_cfg_ops();
	init_pci_pdev_list();
}

void init_primary_pcpu(uint64_t mpidr, uint64_t fdt_paddr)
{
	uint16_t pcpu_id = BSP_CPU_ID;
	uint32_t boot_regs[2] = { 0U };
	uint64_t pcpu_sp;

	arm64_platform_init_early();
	init_percpu_mpidr(mpidr);
	set_pcpu_active(pcpu_id);
	arm64_security_early_init();
#ifdef STACK_PROTECTOR
	init_stack_canary();
#endif

	pcpu_set_current_state(pcpu_id, PCPU_STATE_INITIALIZING);

	arm64_platform_init(fdt_paddr);

	init_debug_pre();
	arm64_platform_init_post_console();
	init_acrn_boot_info(boot_regs);
	init_paging();
	if (!arm64_mmu_is_enabled()) {
		panic("arm64 mmu is not enabled on bsp");
	}
	arm64_cache_init();
	serial_init(false);
	arm64_security_log_bsp_info();
	arm64_gicv3_init_early();

	pcpu_sp = (uint64_t)(&get_cpu_var(stack)[CONFIG_STACK_SIZE - 1]);
	pcpu_sp &= ~(CPU_STACK_ALIGN - 1UL);
	SWITCH_TO(pcpu_sp, init_pcpu_comm_post);
}

void init_secondary_pcpu(uint64_t mpidr)
{
	uint16_t pcpu_id = get_pcpu_id_from_mpidr(mpidr);

	if (pcpu_id >= MAX_PCPU_NUM) {
		panic("invalid pcpu id!");
	}

	set_pcpu_active(pcpu_id);
	if (!enable_paging() || !arm64_mmu_is_enabled()) {
		panic("arm64 mmu is not enabled on ap");
	}

	pcpu_set_current_state(pcpu_id, PCPU_STATE_INITIALIZING);
	arm64_security_validate_pcpu(pcpu_id);

	init_pcpu_comm_post();
}

static void init_guest_mode(uint16_t pcpu_id)
{
	/*
	 * VM launch is intentionally after IRQ, timer, scheduler, all-AP bring-up,
	 * and BSP debug services. Static VM creation builds vCPU scheduler
	 * threads across configured pCPU affinity sets; every target scheduler must
	 * exist before any pCPU starts creating or waking guest vCPUs.
	 */
	arm64_wait_vm_launch(pcpu_id);
	launch_vms(pcpu_id);
}

static void init_pcpu_comm_post(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (pcpu_id == BSP_CPU_ID) {
		arm64_gicv3_log_boot_info();
		arm64_gicv3_init_its();
		arm64_platform_init_smmu();
		arm64_init_pci();
	}

	init_interrupt(pcpu_id);
	init_smp_call();
	timer_init();
	arm64_core_pmu_init_pcpu();
#if CONFIG_ARM64_SPE
	arm64_spe_init_pcpu();
#endif
	ptdev_init();

	init_sched(pcpu_id);

	init_debug_post(pcpu_id);

	pcpu_set_current_state(pcpu_id, PCPU_STATE_RUNNING);
	LOG_INF("MP:     CPU%hu up and running", pcpu_id);
	LOG_INF("GICv3:  CPU%hu redistributor 0x%016lx",
		pcpu_id, arm64_gicv3_redist_base(pcpu_id));

	if (pcpu_id == BSP_CPU_ID) {
		if (!start_pcpus(AP_MASK)) {
			panic("failed to start all secondary cores!");
		}
		if (!wait_pcpus_running(AP_MASK)) {
			panic("failed to initialize all secondary cores!");
		}
		arm64_security_finalize();
		arm64_core_pmu_init_workers();
		cpufreq_init();
		arm64_rttest_init();
		shell_start();
		vm_wdt_start();
		vm_publish_static_boot_state();
		arm64_release_vm_launch();
	}

	init_guest_mode(pcpu_id);

	run_idle_thread();
}
