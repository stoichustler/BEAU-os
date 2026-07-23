/* $OpenBSD: copy.S,v 1.10 2023/08/12 06:28:13 miod Exp $ */
/*
 * Copyright (c) 2015 Dale Rahn <drahn@dalerahn.com>
 * Copyright (c) 2014 Patrick Wildt <patrick@blueri.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Adapted from the OpenBSD arm64 kcopy() transfer loop for BEAU OS.
 * OpenBSD copyin/copyout and their PCB onfault recovery are deliberately not
 * carried over: BEAU has no kernel/user address split or PCB fault ABI.
 */
#include <asm/boot/macro.h>

	.text
	.align	2

/* [20260723] EL2 memory copy ownership
 *
 * BEAU memcpy/memmove
 *       |
 *       +--> C contract: alignment and overlap semantics
 *       |
 *       `--> ASM backend: arch_memcpy / arch_memcpy_backwards / arch_memset
 *              - AAPCS64 argument registers and return values
 *              - byte alignment prologue, then ARM64-wide transfer
 *              - no user-copy, MMIO, or fault-recovery ABI
 *
 * Key rule:
 *   - the public C memory interface accepts arbitrary-aligned normal-memory
 *     ranges; wide accesses begin only after both endpoints are 8-byte aligned;
 *   - callers own normal-memory range validation; this code never accesses
 *     guest VA, device MMIO, PCB state, or fault-recovery state;
 *   - zero-length requests do not dereference either pointer;
 *   - memmove selects direction before entry, preventing a forward copy from
 *     overwriting unread source bytes.
 */

FUNC(arch_memcpy)
	mov	x5, x0
	cbz	x2, .Lcopy_done
	eor	x6, x0, x1
	tst	x6, #7
	b.ne	.Lcopy_unaligned
.Lcopy_align:
	tst	x0, #7
	b.eq	.Lcopy_wide
	ldrb	w3, [x1], #1
	strb	w3, [x0], #1
	sub	x2, x2, #1
	cbnz	x2, .Lcopy_align
	b	.Lcopy_done
.Lcopy_wide:
	cmp	x2, #16
	b.lo	.Lcopy_tail
.Lcopy_16:
	ldp	x3, x4, [x1], #16
	stp	x3, x4, [x0], #16
	sub	x2, x2, #16
	cmp	x2, #16
	b.hs	.Lcopy_16

.Lcopy_tail:
	tbz	x2, #3, .Lcopy_4
	ldr	x3, [x1], #8
	str	x3, [x0], #8
	sub	x2, x2, #8
.Lcopy_4:
	tbz	x2, #2, .Lcopy_1
	ldr	w3, [x1], #4
	str	w3, [x0], #4
	sub	x2, x2, #4
.Lcopy_1:
	cbz	x2, .Lcopy_done
.Lcopy_byte:
	ldrb	w3, [x1], #1
	strb	w3, [x0], #1
	subs	x2, x2, #1
	b.ne	.Lcopy_byte
	b	.Lcopy_done
.Lcopy_unaligned:
	ldrb	w3, [x1], #1
	strb	w3, [x0], #1
	subs	x2, x2, #1
	b.ne	.Lcopy_unaligned
.Lcopy_done:
	mov	x0, x5
	ret
END(arch_memcpy)
	.globl	memcpy
	.type	memcpy, %function
	.set	memcpy, arch_memcpy

FUNC(arch_memcpy_backwards)
	cbz	x2, .Lcopy_backwards_done
	add	x0, x0, x2
	add	x1, x1, x2
.Lcopy_backwards_byte:
	ldrb	w3, [x1, #-1]!
	strb	w3, [x0, #-1]!
	subs	x2, x2, #1
	b.ne	.Lcopy_backwards_byte
.Lcopy_backwards_done:
	ret
END(arch_memcpy_backwards)
	.globl	memcpy_backwards
	.type	memcpy_backwards, %function
	.set	memcpy_backwards, arch_memcpy_backwards

FUNC(arch_memset)
	mov	x4, x0
	cbz	x2, .Lset_done
	and	x3, x1, #0xff
	orr	x3, x3, x3, lsl #8
	orr	x3, x3, x3, lsl #16
	orr	x3, x3, x3, lsl #32

.Lset_align:
	tst	x0, #7
	b.eq	.Lset_wide
	strb	w3, [x0], #1
	sub	x2, x2, #1
	cbnz	x2, .Lset_align
	b	.Lset_done
.Lset_wide:
	cmp	x2, #16
	b.lo	.Lset_tail
.Lset_16:
	stp	x3, x3, [x0], #16
	sub	x2, x2, #16
	cmp	x2, #16
	b.hs	.Lset_16

.Lset_tail:
	tbz	x2, #3, .Lset_4
	str	x3, [x0], #8
	sub	x2, x2, #8
.Lset_4:
	tbz	x2, #2, .Lset_1
	str	w3, [x0], #4
	sub	x2, x2, #4
.Lset_1:
	cbz	x2, .Lset_done
.Lset_byte:
	strb	w3, [x0], #1
	subs	x2, x2, #1
	b.ne	.Lset_byte
.Lset_done:
	mov	x0, x4
	ret
END(arch_memset)
	.globl	memset
	.type	memset, %function
	.set	memset, arch_memset
