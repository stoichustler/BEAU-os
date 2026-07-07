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
#include <vm_wdt.h>
#include <console.h>
#include <shell.h>
#include <serial.h>
#include <fdt_api.h>
#include <asm/platform.h>
#ifdef STACK_PROTECTOR
#include <asm/security.h>
#endif

/*
 * 2026-06-30, ARM64 host boot principle:
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

/*
 * 2026-06-30, ARM64 BSP stack handoff principle:
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

void init_primary_pcpu(uint64_t mpidr, uint64_t fdt_paddr)
{
	uint16_t pcpu_id = BSP_CPU_ID;
	uint32_t boot_regs[2] = { 0U };
	uint64_t pcpu_sp;

	(void)fdt_paddr;
	init_percpu_mpidr(mpidr);
	set_pcpu_active(pcpu_id);
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
	serial_init(false);
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
	enable_paging();
	if (!arm64_mmu_is_enabled()) {
		panic("arm64 mmu is not enabled on ap");
	}

	pcpu_set_current_state(pcpu_id, PCPU_STATE_INITIALIZING);

	init_pcpu_comm_post();
}

static void init_guest_mode(uint16_t pcpu_id)
{
	/*
	 * VM launch is intentionally after IRQ, timer, scheduler, and AP bring-up.
	 * Static VM creation builds vCPU scheduler threads; those threads must be
	 * bound to initialized per-pCPU scheduler state before any guest can run.
	 */
	launch_vms(pcpu_id);
}

static void init_pcpu_comm_post(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (pcpu_id == BSP_CPU_ID) {
		print_hv_banner();
	}

	init_interrupt(pcpu_id);
	init_smp_call();
	timer_init();

	init_sched(pcpu_id);

	init_debug_post(pcpu_id);

	pcpu_set_current_state(pcpu_id, PCPU_STATE_RUNNING);

	if (pcpu_id == BSP_CPU_ID) {
		if (!start_pcpus(AP_MASK)) {
			panic("failed to start all secondary cores!");
		}
		if (!wait_pcpus_running(AP_MASK)) {
			panic("failed to initialize all secondary cores!");
		}
		shell_start();
		vm_wdt_start();
	}

	init_guest_mode(pcpu_id);

	run_idle_thread();
}
