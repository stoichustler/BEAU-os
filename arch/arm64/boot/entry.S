/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ─────────────────────────────────────────────────────────────────
 * _________ _________  _____   ___ ___     ________   ________
 * \_____   \\_   ___/ /  _  \ /   |   \    \_____  \ /  _____/
 *  |   |  _/ |   ___)/  /_\  \\   |   /     /   |   \\____  \
 *  |______ \ |_____ \\___|___ \\_____/      \_____  //____  /
 *         \/       \/        \/                   \/      \/ (2026)
 * ─────────────────────────────────────────────────────────────────
 *
 * [20260714] BEAU OS EL2 bootstrap and runtime framework:
 *
 *   reset / PSCI
 *       |
 *       +--> _start                  (boot pCPU)
 *       |       - require EL2
 *       |       - install EL2 vectors
 *       |       - clear BSS and use the boot stack
 *       |       - pass MPIDR + DTB PA to C init
 *       |
 *       +--> _start_secondary_psci   (secondary pCPU)
 *               - keep exceptions masked
 *               - install EL2 vectors
 *               - use PSCI-provided EL2 stack
 *               - pass MPIDR to C init
 *       |
 *       v
 *   arch/arm64/init.c
 *       |
 *       +--> platform.dts / boot modules
 *       |       host HW policy, VM layout, image metadata
 *       |
 *       +--> host EL2 services
 *       |       stage-1 MMU, GIC/ITS, SMMU, PCI, timer, scheduler, shell
 *       |
 *       +--> VM construction
 *       |       stage-2 MMU, vGIC, vtimer, vPL011/vPCI/virtio frontends
 *       |
 *       v
 *   scheduler thread -> arch_vcpu_thread() -> guest EL1
 *       |
 *       v
 *   EL2 vectors -> trap/IRQ emulation -> resume guest or service host work
 *
 * Key rule:
 *   - this file owns only the minimal EL2-safe bootstrap state;
 *   - C initialization owns platform policy and subsystem ordering;
 *   - guest CPU/device ownership is not exposed before stage-2, interrupt,
 *     and scheduler state are initialized.
 */

#include <asm/cpu.h>
#include <asm/boot/macro.h>

	.section .text.entry, "ax", %progbits

	/*
	 * Main entry point.
	 * Raw firmware convention: x1 = DTB physical address.
	 * U-Boot booti convention: x0 = DTB physical address, x1 = 0.
	 */
LABEL(_start)
	/* DAIF masks Debug, SError, IRQ, and FIQ while EL2 state is incomplete. */
	msr    daifset, #0xf

	/* CurrentEL must report EL2; BEAU runs as a hypervisor, not as EL1 code. */
	mrs    x2, CurrentEL
	cmp    x2, #CURRENTEL_EL2
	b.ne   _start_hang
	/* SPSel=1 selects SP_EL2 as the active stack pointer at EL2. */
	msr    spsel, #1
	isb

	/* VBAR_EL2 points all EL2 exceptions to arm64_exception_vectors. */
	loadl  x2, arm64_exception_vectors
	msr    vbar_el2, x2
	isb

	loadl  x2, _bss_start
	loadl  x3, _bss_end
1:
	cmp    x2, x3
	b.hs   2f
	str    xzr, [x2], #8
	b      1b
2:
	loadl  x4, _boot_stack_end
	mov    sp, x4

	/* Prefer the existing raw-loader ABI; use booti's x0 only when x1 is zero. */
#if defined(CONFIG_AARCH64_IMAGE_HEADER)
	cbnz   x1, 3f
	mov    x1, x0
3:
#endif
	/* MPIDR_EL1 carries the CPU affinity ID used to identify the boot pCPU. */
	mrs    x0, mpidr_el1
	tst    x1, x1
	b.ne   4f
	mov    x1, xzr
4:
	bl     init_primary_pcpu
	b      _start_hang


LABEL_ALIGN(_start_secondary_psci, 2)
	/* Secondary CPUs enter with interrupts masked until per-CPU init is done. */
	msr    daifset, #0xf
	/* Use SP_EL2 for the scheduler/EL2 stack on secondary CPUs as well. */
	msr    spsel, #1
	isb

	/* Install the EL2 vector base before calling C code. */
	loadl  x2, arm64_exception_vectors
	msr    vbar_el2, x2
	isb

	mov    sp, x0
	/* Pass this CPU's MPIDR affinity to init_secondary_pcpu(). */
	mrs    x0, mpidr_el1
	bl     init_secondary_pcpu
	b      _start_hang


LABEL_ALIGN(_start_hang, 2)
	/* WFI keeps a failed or returned boot path parked without spinning hot. */
	wfi
	b      _start_hang
