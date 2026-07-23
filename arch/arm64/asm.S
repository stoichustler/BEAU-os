/*-
 * Copyright (c) 2014 Robin Randhawa
 * Copyright (c) 2015 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by Andrew Turner
 * under sponsorship from the FreeBSD Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * Adapted from FreeBSD sys/arm64/arm64/cpufunc_asm.S for BEAU OS.
 * FreeBSD's PCB onfault recovery and its cache-size globals are intentionally
 * not carried over: BEAU has no matching EL2 fault-recovery ABI, and CTR_EL0
 * provides the executing CPU's cache line size directly.
 */
#include <asm/boot/macro.h>

	.text
	.align	2

/*
 * Handle a cache range passed in x0/x1. The operation clobbers x0-x4.
 * CTR_EL0.DminLine and IminLine encode the line size as 4 << field.
 */
.macro cache_handle_range dcop = 0, ic = 0, icop = 0
	cbz	x1, 3f
	mrs	x3, ctr_el0
.if \ic == 0
	ubfx	x3, x3, #16, #4
.else
	ubfx	x3, x3, #0, #4
.endif
	mov	x4, #4
	lsl	x3, x4, x3
	sub	x4, x3, #1
	and	x2, x0, x4
	add	x1, x1, x2
	bic	x0, x0, x4
.if \ic != 0
	mov	x2, x0
	mov	x4, x1
.endif
1:
	dc	\dcop, x0
	add	x0, x0, x3
	subs	x1, x1, x3
	b.hi	1b
	dsb	ish
.if \ic != 0
2:
	ic	\icop, x2
	add	x2, x2, x3
	subs	x4, x4, x3
	b.hi	2b
	dsb	ish
	isb
.endif
3:
.endm

FUNC(arm64_nullop)
	ret
END(arm64_nullop)

/* void arm64_dcache_wb_range(const volatile void *, uint64_t) */
FUNC(arm64_dcache_wb_range)
	cache_handle_range	dcop = cvac
	ret
END(arm64_dcache_wb_range)

/* void arm64_dcache_wbinv_range(const volatile void *, uint64_t) */
FUNC(arm64_dcache_wbinv_range)
	cache_handle_range	dcop = civac
	ret
END(arm64_dcache_wbinv_range)

/* void arm64_dcache_inv_range(const volatile void *, uint64_t) */
FUNC(arm64_dcache_inv_range)
	cache_handle_range	dcop = ivac
	ret
END(arm64_dcache_inv_range)

/* void arm64_dic_idc_icache_sync_range(const volatile void *, uint64_t) */
FUNC(arm64_dic_idc_icache_sync_range)
	dsb	ishst
	isb
	ret
END(arm64_dic_idc_icache_sync_range)

/* void arm64_idc_aliasing_icache_sync_range(const volatile void *, uint64_t) */
FUNC(arm64_idc_aliasing_icache_sync_range)
	dsb	ishst
	ic	ialluis
	dsb	ish
	isb
	ret
END(arm64_idc_aliasing_icache_sync_range)

/* void arm64_aliasing_icache_sync_range(const volatile void *, uint64_t) */
FUNC(arm64_aliasing_icache_sync_range)
	cache_handle_range	dcop = cvau
	ic	ialluis
	dsb	ish
	isb
	ret
END(arm64_aliasing_icache_sync_range)
