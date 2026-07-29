/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/cpu.h>
#include <asm/boot/macro.h>
#include <generated/offsets.h>

	.text

	/*
	 * [20260714] ARM64 scheduler context switch principle
	 *
	 *   core/schedule.c
	 *       |
	 *       +--> prev->switch_out(prev)
	 *       |       save architecture-specific payload outside this frame
	 *       |       (for a vCPU: guest EL1 system registers, vtimer, stage-2)
	 *       |
	 *       +--> next->switch_in(next)
	 *       |       load architecture-specific payload before the host stack
	 *       |       starts executing the next thread body
	 *       |
	 *       v
	 *   arch_switch_to(&prev->host_sp, &next->host_sp, next_key, resume, pac)
	 *       |
	 *       +--> build a small host stack frame on prev's current SP
	 *       +--> publish that SP through prev->host_sp
	 *       +--> load next->host_sp as the architectural SP
	 *       +--> pop the next frame and ret into the next thread
	 *
	 * The switch is stack-centric: a thread is resumed by making SP point at
	 * its saved stack_frame.  The same layout is also used for a never-run
	 * thread prepared by arch_setup_thread_stack(): its synthetic LR points to
	 * thread_entry and its synthetic x0 carries the thread_object argument.
	 *
	 * Key rule:
	 *   - save only the AAPCS64 callee-saved host context here; caller-saved
	 *     registers either belong to the interrupted C call path or are rebuilt;
	 *   - keep DAIF with the scheduled host context so IRQ masking state does
	 *     not leak between idle, shell, watchdog, and vCPU threads;
	 *   - guest CPU state is not part of this frame and must be saved/restored
	 *     by switch_out/switch_in before the host SP swap;
	 *   - prev_sp and next_sp must point to durable thread_object.host_sp fields
	 *     because the stores/loads below are the ownership transfer.
	 */
FUNC_ALIGN(arch_switch_to, 4)
	/* Keep the incoming PAC key pointer while x2 is reused for stack transfer. */
	mov	x5, x2
	/*
	 * BL arch_switch_to() creates a fresh, unsigned LR. Sign it while SP and
	 * the outgoing thread's APIA key still match; AUTIASP below runs only
	 * after the incoming frame has restored that same SP and key.
	 */
	cbz	x4, 0f
	hint	#25
0:
	/*
	 * Reserve the exact frame described by struct stack_frame.  C and assembly
	 * share offsets through asm/cpu.h; changing the layout in only one side
	 * would make first-run threads and resumed threads return to different
	 * addresses.
	 */
	sub	sp, sp, #STACK_FRAME_SIZE

	/*
	 * AAPCS64 requires x19-x29 and LR to survive a C call.  Because the switch
	 * returns later on another stack, those registers must be attached to the
	 * outgoing thread rather than to the physical CPU.
	 */
	stp	x19, x20, [sp, #STACK_FRAME_OFFSET_X19]
	stp	x21, x22, [sp, #STACK_FRAME_OFFSET_X21]
	stp	x23, x24, [sp, #STACK_FRAME_OFFSET_X23]
	stp	x25, x26, [sp, #STACK_FRAME_OFFSET_X25]
	stp	x27, x28, [sp, #STACK_FRAME_OFFSET_X27]
	stp	x29, x30, [sp, #STACK_FRAME_OFFSET_X29]
	/*
	 * x0 is caller-saved for ordinary C calls, but the scheduler frame also
	 * represents first entry.  Keeping x0 in the common layout lets a synthetic
	 * frame enter thread_entry(thread_object *) through the same restore path.
	 */
	str	x0, [sp, #STACK_FRAME_OFFSET_X0]
	/*
	 * DAIF is host execution policy, not general-purpose data.  Saving it here
	 * prevents a thread that blocked with IRQs masked from silently changing
	 * the IRQ state of the next scheduled thread.
	 */
	mrs	x2, daif
	str	x2, [sp, #STACK_FRAME_OFFSET_SPSR]

	/*
	 * This is the actual ownership transfer:
	 *
	 *   *prev_sp = outgoing frame base
	 *   SP       = *next_sp
	 *
	 * After the MOV to SP, all stack-relative memory accesses belong to the
	 * next thread.  No outgoing-thread stack data may be touched after this
	 * point.
	 */
	mov	x2, sp
	str	x2, [x0]
	ldr	x2, [x1]
	mov	sp, x2
	cbz	x4, 1f
	cbz	x5, 1f
	ldp	x6, x7, [x5]
	.arch_extension pauth
	msr	APIAKeyLo_EL1, x6
	msr	APIAKeyHi_EL1, x7
	.arch_extension nopauth
	isb
1:

	/*
	 * Restore the preserved host context from the incoming stack.  For an
	 * already-run thread, LR points back to the call site after its previous
	 * arch_switch_to().  For a first-run thread, LR is thread_entry.
	 */
	ldp	x19, x20, [sp, #STACK_FRAME_OFFSET_X19]
	ldp	x21, x22, [sp, #STACK_FRAME_OFFSET_X21]
	ldp	x23, x24, [sp, #STACK_FRAME_OFFSET_X23]
	ldp	x25, x26, [sp, #STACK_FRAME_OFFSET_X25]
	ldp	x27, x28, [sp, #STACK_FRAME_OFFSET_X27]
	ldp	x29, x30, [sp, #STACK_FRAME_OFFSET_X29]
	ldr	x0, [sp, #STACK_FRAME_OFFSET_X0]
	ldr	x2, [sp, #STACK_FRAME_OFFSET_SPSR]
	/*
	 * Restore DAIF before removing the frame so the returned thread observes
	 * its own interrupt mask at the first instruction after RET.
	 */
	msr	daif, x2
	add	sp, sp, #STACK_FRAME_SIZE

	/*
	 * RET completes the illusion that arch_switch_to() returned on the incoming
	 * thread.  The physical pCPU continued executing; only the host stack and
	 * preserved register set changed.
	 */
	cbz	x4, 2f
	cbz	x3, 2f
	hint	#29
2:
	ret
END(arch_switch_to)
