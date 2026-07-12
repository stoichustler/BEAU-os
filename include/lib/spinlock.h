/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#ifndef ASSEMBLER
#include <types.h>
#include <rtl.h>
#include <cpu.h>
#include <asm/lib/spinlock.h>

/* [20260630] common spinlock principle:
 *
 * spinlock_t is the architecture ticket-lock object exposed to shared code.
 * Callers use it for short critical sections that may be reached by multiple
 * pCPUs, IRQ handlers, scheduler code, or VM/device emulation paths. The lock
 * is non-sleeping: a waiter burns CPU until the current owner releases it, so
 * code inside the critical section must stay bounded and must not block.
 *
 *   shared state
 *       ^
 *       |
 *   spinlock_obtain()
 *       |
 *       v
 *   mutate/read protected fields
 *       |
 *       v
 *   spinlock_release()
 *
 * The architecture implementation provides acquire/release ordering. That
 * ordering makes writes inside the critical section visible before the next
 * CPU observes the lock as available.
 */
typedef arch_spinlock_t spinlock_t;

/* The mandatory functions should be implemented by arch spinlock library */
static inline void arch_spinlock_obtain(arch_spinlock_t *lock);
static inline void arch_spinlock_release(arch_spinlock_t *lock);

/* Function prototypes */
static inline void spinlock_init(spinlock_t *lock)
{
	(void)memset(lock, 0U, sizeof(spinlock_t));
}

/*
 * irqsave protects against two owners on the same pCPU: normal thread context
 * can take a lock, then a local IRQ handler can interrupt and try to take the
 * same lock. Saving and disabling local IRQs prevents that self-deadlock while
 * the spinlock still serializes remote pCPUs.
 */
static inline void spinlock_irqsave_obtain(spinlock_t *lock, uint64_t * flags)
{
	local_irq_save(flags);
	arch_spinlock_obtain(lock);
}

static inline void spinlock_irqrestore_release(spinlock_t *lock, uint64_t flags)
{
	arch_spinlock_release(lock);
	local_irq_restore(flags);
}

static inline void spinlock_obtain(spinlock_t *lock)
{
    return arch_spinlock_obtain(lock);
}

static inline void spinlock_release(spinlock_t *lock)
{
    return arch_spinlock_release(lock);
}

#else /* ASSEMBLER */
#include <asm/lib/spinlock.h>
#define spinlock_obtain arch_spinlock_obtain
#define spinlock_release arch_spinlock_release
#endif /* ASSEMBLER */

#endif /* SPINLOCK_H */
