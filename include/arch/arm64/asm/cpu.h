/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_CPU_H
#define ARM64_CPU_H

#ifndef ASSEMBLER
#include <types.h>
#include <lib/util.h>
#include <logmsg.h>
#include <asm/sysreg.h>
#ifndef CONFIG_STATIC_ARM64_PLATFORM
#include <board_info.h>
#endif
#include <barrier.h>
#endif

#define DAIF_IRQ		(1UL << 7U)
#define DAIF_FIQ		(1UL << 6U)
#define DAIF_ASYNC_ABORT	(1UL << 8U)
#define DAIF_DEBUG		(1UL << 9U)
#define DAIF_ALL		(DAIF_DEBUG | DAIF_ASYNC_ABORT | DAIF_IRQ | DAIF_FIQ)

#define CURRENTEL_EL2		0x8

#ifndef ASSEMBLER

struct cpu_regs {
	uint64_t x0;
	uint64_t x1;
	uint64_t x2;
	uint64_t x3;
	uint64_t x4;
	uint64_t x5;
	uint64_t x6;
	uint64_t x7;
	uint64_t x8;
	uint64_t x9;
	uint64_t x10;
	uint64_t x11;
	uint64_t x12;
	uint64_t x13;
	uint64_t x14;
	uint64_t x15;
	uint64_t x16;
	uint64_t x17;
	uint64_t x18;
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29;
	uint64_t lr;
	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
	uint64_t hpfar;
	uint64_t host_tpidr;
	uint64_t exc_sp;
	uint64_t reserved;
};

struct stack_frame {
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29;
	uint64_t lr;
	uint64_t x0;
	uint64_t spsr;
	uint64_t magic;
	uint64_t reserved;
};

#define cpu_relax()	arch_asm_pause()
#define NR_CPUS		MAX_PCPU_NUM

#define LONG_BYTEORDER	3
#define BYTES_PER_LONG	(1 << LONG_BYTEORDER)
#define BITS_PER_LONG	(BYTES_PER_LONG << 3)

#define CPU_STACK_ALIGN	16UL

static inline uint64_t read_daif(void)
{
	return arm64_sysreg_read(daif);
}

static inline uint16_t arch_get_pcpu_id(void)
{
	return (uint16_t)arm64_sysreg_read(tpidr_el2);
}

static inline void arch_set_current_pcpu_id(uint16_t pcpu_id)
{
	arm64_sysreg_write(tpidr_el2, pcpu_id);
}

static inline void arch_asm_pause(void)
{
	arm64_yield();
}

static inline void arch_local_irq_enable(void)
{
	arm64_sysreg_write_imm(daifclr, 2);
}

static inline void arch_local_irq_disable(void)
{
	arm64_sysreg_write_imm(daifset, 2);
}

static inline void arch_local_irq_save(uint64_t *flags_ptr)
{
	uint64_t flags = read_daif();

	arch_local_irq_disable();
	*flags_ptr = flags;
}

static inline void arch_local_irq_restore(uint64_t flags)
{
	if ((flags & DAIF_IRQ) == 0UL) {
		arch_local_irq_enable();
	} else {
		arch_local_irq_disable();
	}
}

static inline void arch_pre_user_access(void)
{
}

static inline void arch_post_user_access(void)
{
}

static inline bool need_offline(uint16_t pcpu_id)
{
	(void)pcpu_id;
	return false;
}

static inline void stac(void) {}
static inline void clac(void) {}

void wait_sync_change(volatile const uint64_t *sync, uint64_t wake_sync);
void init_percpu_mpidr(uint64_t bsp_mpidr);
uint16_t get_pcpu_id_from_mpidr(uint64_t mpidr);

#endif /* ASSEMBLER */

#endif /* ARM64_CPU_H */
