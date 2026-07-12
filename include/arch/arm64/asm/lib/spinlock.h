/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_SPINLOCK_H
#define ARM64_LIB_SPINLOCK_H

#ifndef ASSEMBLER

typedef struct _arch_spinlock {
	uint64_t head;
	uint64_t tail;
} arch_spinlock_t;

/* [20260630] ARM64 ticket-lock principle:
 *
 * head is the next ticket to hand out; tail is the ticket currently allowed to
 * enter. Each caller atomically increments head and keeps the previous value as
 * its ticket. The caller spins until tail catches up, which gives FIFO fairness
 * instead of letting a later CPU steal the lock.
 *
 *   CPU gets ticket N from head
 *              |
 *              v
 *   wait while tail != N
 *              |
 *              v
 *   critical section
 *              |
 *              v
 *   release stores tail = N + 1
 *
 * ldaxr/ldar provide acquire ordering for the owner entering the critical
 * section. stlr on release publishes protected writes before the next ticket
 * holder can observe its turn.
 */
static inline void arch_spinlock_obtain(arch_spinlock_t *lock)
{
	uint64_t ticket;
	uint64_t owner;

	asm volatile (
		"1:	ldaxr	%0, %2\n"
		"	add	%0, %0, #1\n"
		"	stxr	%w1, %0, %2\n"
		"	cbnz	%w1, 1b\n"
		"	sub	%0, %0, #1\n"
		: "=&r" (ticket), "=&r" (owner), "+Q" (lock->head)
		:
		: "memory");

	do {
		/*
		 * A waiting CPU only reads tail. arch_asm_pause() is the
		 * architecture hint that this is a spin-wait loop rather than
		 * useful work.
		 */
		asm volatile ("ldar %x0, %1" : "=r" (owner) : "Q" (lock->tail) : "memory");
		if (owner != ticket) {
			arch_asm_pause();
		}
	} while (owner != ticket);
}

static inline void arch_spinlock_release(arch_spinlock_t *lock)
{
	uint64_t owner = lock->tail + 1UL;

	/* Release the next ticket with store-release ordering. */
	asm volatile ("stlr %x1, %0" : "=Q" (lock->tail) : "r" (owner) : "memory");
}

#else /* ASSEMBLER */

#define SPINLOCK_HEAD_OFFSET	0
#define SPINLOCK_TAIL_OFFSET	8

#endif /* ASSEMBLER */

#endif /* ARM64_LIB_SPINLOCK_H */
