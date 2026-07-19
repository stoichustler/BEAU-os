/*	$OpenBSD: aesv8-armx.S,v 1.3 2023/03/27 19:02:47 kettenis Exp $	*/
/*
 * Copyright (c) 2006, CRYPTOGAMS by <appro@openssl.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *       *	Redistributions of source code must retain copyright notices,
 * 	this list of conditions and the following disclaimer.
 *
 *       *	Redistributions in binary form must reproduce the above
 * 	copyright notice, this list of conditions and the following
 * 	disclaimer in the documentation and/or other materials
 * 	provided with the distribution.
 *
 *       *	Neither the name of the CRYPTOGAMS nor the names of its
 * 	copyright holder and contributors may be used to endorse or
 * 	promote products derived from this software without specific
 * 	prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL), in which case the provisions of the GPL apply INSTEAD OF
 * those given above.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* [20260719] ARMv8 AES implementation
 *
 * caller checks FEAT_AES
 *          |
 *          v
 * expand 128/192/256-bit key -> round keys + round count
 *          |
 *          +--> single-block AES encrypt/decrypt
 *          +--> CBC encrypt/decrypt with caller-owned IV
 *          `--> CTR32 keystream XOR with caller-owned counter block
 *
 * Key rule:
 *   - this file is ported from OpenBSD and uses the ARMv8 Crypto Extension;
 *   - all mutable input, output, key, and IV storage is caller-owned; the only
 *     file-local data is the read-only key-expansion constant table;
 *   - the key schedule stores up to fifteen 16-byte round keys at offset 0 and
 *     the 10, 12, or 14 round count at byte offset 240;
 *   - key setup validates its pointers and key size, while block operations
 *     require valid buffers, a valid schedule, and prior FEAT_AES validation;
 *   - CBC consumes complete 16-byte blocks and updates its IV. CTR32 requires
 *     at least one block and treats its IV as read-only.
 */

.arch	armv8-a+crypto

.section .rodata

.align	5
.Lrcon:
/* Key expansion uses x in GF(2^8). The first vector seeds Rcon, the byte mask
 * rotates the last key word into SubWord order, and 0x1b reduces x^8 modulo
 * the AES polynomial when the high bit of Rcon is carried out.
 */
.long	0x01,0x01,0x01,0x01
.long	0x0c0f0e0d,0x0c0f0e0d,0x0c0f0e0d,0x0c0f0e0d	// rotate-n-splat
.long	0x1b,0x1b,0x1b,0x1b

.text

.globl	aes_v8_set_encrypt_key
.type	aes_v8_set_encrypt_key,%function
.align	5
/* Expand a user key into struct arm64_aes_key.
 *
 *   x0 = user key, w1 = key bits, x2 = output schedule
 *   x0 = 0 on success, -1 for a NULL pointer, -2 for an invalid key size
 *
 * AES accepts Nk = 4, 6, or 8 input words. Each path applies RotWord,
 * SubWord, and Rcon at the Nk boundary, then stores Nr + 1 round keys where
 * Nr is 10, 12, or 14. The AES-256 path also applies SubWord halfway through
 * each eight-word expansion group.
 */
aes_v8_set_encrypt_key:
	bti	c
.Lenc_key:
	stp	x29,x30,[sp,#-16]!
	add	x29,sp,#0
	mov	x3,#-1
	cmp	x0,#0
	b.eq	.Lenc_key_abort
	cmp	x2,#0
	b.eq	.Lenc_key_abort
	mov	x3,#-2
	cmp	w1,#128
	b.lt	.Lenc_key_abort
	cmp	w1,#256
	b.gt	.Lenc_key_abort
	tst	w1,#0x3f
	b.ne	.Lenc_key_abort

	adrp	x3,.Lrcon
	add	x3,x3,:lo12:.Lrcon
	cmp	w1,#192

	eor	v0.16b,v0.16b,v0.16b
	ld1	{v3.16b},[x0],#16
	mov	w1,#8		// reuse w1
	ld1	{v1.4s,v2.4s},[x3],#32

	b.lt	.Loop128
	b.eq	.L192
	b	.L256

.align	4
.Loop128:
	/* AES-128 derives four new words per iteration. The EXT/EOR sequence
	 * accumulates w[i-Nk] through the preceding new words, while AESE with a
	 * zero key supplies SubWord(RotWord(last)) and Rcon is XORed afterward.
	 */
	tbl	v6.16b,{v3.16b},v2.16b
	ext	v5.16b,v0.16b,v3.16b,#12
	st1	{v3.4s},[x2],#16
	aese	v6.16b,v0.16b
	subs	w1,w1,#1

	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v6.16b,v6.16b,v1.16b
	eor	v3.16b,v3.16b,v5.16b
	shl	v1.16b,v1.16b,#1
	eor	v3.16b,v3.16b,v6.16b
	b.ne	.Loop128

	ld1	{v1.4s},[x3]

	tbl	v6.16b,{v3.16b},v2.16b
	ext	v5.16b,v0.16b,v3.16b,#12
	st1	{v3.4s},[x2],#16
	aese	v6.16b,v0.16b

	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v6.16b,v6.16b,v1.16b
	eor	v3.16b,v3.16b,v5.16b
	shl	v1.16b,v1.16b,#1
	eor	v3.16b,v3.16b,v6.16b

	tbl	v6.16b,{v3.16b},v2.16b
	ext	v5.16b,v0.16b,v3.16b,#12
	st1	{v3.4s},[x2],#16
	aese	v6.16b,v0.16b

	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v6.16b,v6.16b,v1.16b
	eor	v3.16b,v3.16b,v5.16b
	eor	v3.16b,v3.16b,v6.16b
	st1	{v3.4s},[x2]
	add	x2,x2,#0x50

	mov	w12,#10
	b	.Ldone

.align	4
.L192:
	/* AES-192 carries its six-word key as v3 plus the low half of v4. The
	 * stores alternate 16-byte and 8-byte pieces so the output remains a
	 * contiguous sequence of 16-byte round keys.
	 */
	ld1	{v4.8b},[x0],#8
	movi	v6.16b,#8			// borrow v6.16b
	st1	{v3.4s},[x2],#16
	sub	v2.16b,v2.16b,v6.16b	// adjust the mask

.Loop192:
	tbl	v6.16b,{v4.16b},v2.16b
	ext	v5.16b,v0.16b,v3.16b,#12
	st1	{v4.8b},[x2],#8
	aese	v6.16b,v0.16b
	subs	w1,w1,#1

	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v3.16b,v3.16b,v5.16b

	dup	v5.4s,v3.s[3]
	eor	v5.16b,v5.16b,v4.16b
	eor	v6.16b,v6.16b,v1.16b
	ext	v4.16b,v0.16b,v4.16b,#12
	shl	v1.16b,v1.16b,#1
	eor	v4.16b,v4.16b,v5.16b
	eor	v3.16b,v3.16b,v6.16b
	eor	v4.16b,v4.16b,v6.16b
	st1	{v3.4s},[x2],#16
	b.ne	.Loop192

	mov	w12,#12
	add	x2,x2,#0x20
	b	.Ldone

.align	4
.L256:
	/* AES-256 keeps the two halves in v3 and v4. One half uses
	 * SubWord(RotWord(last)) + Rcon; the other uses SubWord(last) without
	 * rotation or Rcon, matching the Nk = 8 key schedule rule.
	 */
	ld1	{v4.16b},[x0]
	mov	w1,#7
	mov	w12,#14
	st1	{v3.4s},[x2],#16

.Loop256:
	tbl	v6.16b,{v4.16b},v2.16b
	ext	v5.16b,v0.16b,v3.16b,#12
	st1	{v4.4s},[x2],#16
	aese	v6.16b,v0.16b
	subs	w1,w1,#1

	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v3.16b,v3.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v6.16b,v6.16b,v1.16b
	eor	v3.16b,v3.16b,v5.16b
	shl	v1.16b,v1.16b,#1
	eor	v3.16b,v3.16b,v6.16b
	st1	{v3.4s},[x2],#16
	b.eq	.Ldone

	dup	v6.4s,v3.s[3]		// just splat
	ext	v5.16b,v0.16b,v4.16b,#12
	aese	v6.16b,v0.16b

	eor	v4.16b,v4.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v4.16b,v4.16b,v5.16b
	ext	v5.16b,v0.16b,v5.16b,#12
	eor	v4.16b,v4.16b,v5.16b

	eor	v4.16b,v4.16b,v6.16b
	b	.Loop256

.Ldone:
	/* All key-size paths converge with x2 at schedule byte offset 240. Keep
	 * this store synchronized with struct arm64_aes_key::rounds.
	 */
	str	w12,[x2]
	mov	x3,#0

.Lenc_key_abort:
	mov	x0,x3			// return value
	ldr	x29,[sp],#16
	ret
.size	aes_v8_set_encrypt_key,.-aes_v8_set_encrypt_key

.globl	aes_v8_set_decrypt_key
.type	aes_v8_set_decrypt_key,%function
.align	5
/* Build a decryption schedule from the encryption expansion.
 *
 * Decryption consumes round keys in reverse order. The first and last keys
 * are only swapped; each intermediate key is additionally transformed by
 * AESIMC because AESD is paired with AESIMC in the block-decrypt loop.
 * Key-expansion errors are returned unchanged.
 */
aes_v8_set_decrypt_key:
	bti	c
.inst	0xd503233f		// paciasp
	stp	x29,x30,[sp,#-16]!
	add	x29,sp,#0
	bl	.Lenc_key

	cmp	x0,#0
	b.ne	.Ldec_key_abort

	sub	x2,x2,#240		// restore original x2
	mov	x4,#-16
	add	x0,x2,x12,lsl#4	// end of key schedule

	ld1	{v0.4s},[x2]
	ld1	{v1.4s},[x0]
	st1	{v0.4s},[x0],x4
	st1	{v1.4s},[x2],#16

.Loop_imc:
	ld1	{v0.4s},[x2]
	ld1	{v1.4s},[x0]
	aesimc	v0.16b,v0.16b
	aesimc	v1.16b,v1.16b
	st1	{v0.4s},[x0],x4
	st1	{v1.4s},[x2],#16
	cmp	x0,x2
	b.hi	.Loop_imc

	ld1	{v0.4s},[x2]
	aesimc	v0.16b,v0.16b
	st1	{v0.4s},[x0]

	eor	x0,x0,x0		// return value
.Ldec_key_abort:
	ldp	x29,x30,[sp],#16
.inst	0xd50323bf		// autiasp
	ret
.size	aes_v8_set_decrypt_key,.-aes_v8_set_decrypt_key
.globl	aes_v8_encrypt
.type	aes_v8_encrypt,%function
.align	5
/* Encrypt one 16-byte block.
 *
 *   x0 = input, x1 = output, x2 = encryption schedule
 *
 * AESE performs AddRoundKey, SubBytes, and ShiftRows; AESMC supplies
 * MixColumns for every non-final round. The last AESE omits AESMC and the
 * final EOR applies the last round key. Loading the input before storing the
 * output permits in-place operation.
 */
aes_v8_encrypt:
	bti	c
	ldr	w3,[x2,#240]
	ld1	{v0.4s},[x2],#16
	ld1	{v2.16b},[x0]
	sub	w3,w3,#2
	ld1	{v1.4s},[x2],#16

.Loop_enc:
	aese	v2.16b,v0.16b
	aesmc	v2.16b,v2.16b
	ld1	{v0.4s},[x2],#16
	subs	w3,w3,#2
	aese	v2.16b,v1.16b
	aesmc	v2.16b,v2.16b
	ld1	{v1.4s},[x2],#16
	b.gt	.Loop_enc

	aese	v2.16b,v0.16b
	aesmc	v2.16b,v2.16b
	ld1	{v0.4s},[x2]
	aese	v2.16b,v1.16b
	eor	v2.16b,v2.16b,v0.16b

	st1	{v2.16b},[x1]
	ret
.size	aes_v8_encrypt,.-aes_v8_encrypt
.globl	aes_v8_decrypt
.type	aes_v8_decrypt,%function
.align	5
/* Decrypt one 16-byte block with the transformed reverse schedule.
 *
 * AESD performs AddRoundKey, inverse ShiftRows, and inverse SubBytes;
 * AESIMC supplies inverse MixColumns for every non-final round. The final
 * round omits AESIMC and applies the last key with EOR.
 */
aes_v8_decrypt:
	bti	c
	ldr	w3,[x2,#240]
	ld1	{v0.4s},[x2],#16
	ld1	{v2.16b},[x0]
	sub	w3,w3,#2
	ld1	{v1.4s},[x2],#16

.Loop_dec:
	aesd	v2.16b,v0.16b
	aesimc	v2.16b,v2.16b
	ld1	{v0.4s},[x2],#16
	subs	w3,w3,#2
	aesd	v2.16b,v1.16b
	aesimc	v2.16b,v2.16b
	ld1	{v1.4s},[x2],#16
	b.gt	.Loop_dec

	aesd	v2.16b,v0.16b
	aesimc	v2.16b,v2.16b
	ld1	{v0.4s},[x2]
	aesd	v2.16b,v1.16b
	eor	v2.16b,v2.16b,v0.16b

	st1	{v2.16b},[x1]
	ret
.size	aes_v8_decrypt,.-aes_v8_decrypt
.globl	aes_v8_cbc_encrypt
.type	aes_v8_cbc_encrypt,%function
.align	5
/* Encrypt or decrypt complete blocks in Cipher Block Chaining mode.
 *
 *   x0 = input, x1 = output, x2 = byte length
 *   x3 = matching key schedule, x4 = mutable IV, w5 = encrypt flag
 *
 * encryption: P[i] XOR C[i-1] -> AES encrypt -> C[i]
 * decryption: C[i] -> AES decrypt -> XOR C[i-1] -> P[i]
 *
 * Key rule:
 *   - a length below one block is a no-op; any trailing partial block is not
 *     consumed, so callers must provide a block-aligned length when all input
 *     bytes must be processed;
 *   - encryption is serial because each block owns the next chaining value;
 *     decryption pipelines independent AES operations and applies the saved
 *     preceding ciphertext only after the final round;
 *   - input and output may alias because ciphertext needed for later chaining
 *     is loaded before the corresponding plaintext store;
 *   - the IV is replaced with the last ciphertext block on either path.
 */
aes_v8_cbc_encrypt:
	bti	c
	stp	x29,x30,[sp,#-16]!
	add	x29,sp,#0
	subs	x2,x2,#16
	mov	x8,#16
	b.lo	.Lcbc_abort
	csel	x8,xzr,x8,eq

	/* Preserve the encrypt/decrypt decision in NZCV while w5 is repurposed
	 * for the round count. None of the intervening setup instructions updates
	 * flags before the branch to .Lcbc_dec.
	 */
	cmp	w5,#0			// en- or decrypting?
	ldr	w5,[x3,#240]
	and	x2,x2,#-16
	ld1	{v6.16b},[x4]
	ld1	{v0.16b},[x0],x8

	ld1	{v16.4s,v17.4s},[x3]		// load key schedule...
	sub	w5,w5,#6
	add	x7,x3,x5,lsl#4	// pointer to last 7 round keys
	sub	w5,w5,#2
	ld1	{v18.4s,v19.4s},[x7],#32
	ld1	{v20.4s,v21.4s},[x7],#32
	ld1	{v22.4s,v23.4s},[x7],#32
	ld1	{v7.4s},[x7]

	add	x7,x3,#32
	mov	w6,w5
	b.eq	.Lcbc_dec

	cmp	w5,#2
	eor	v0.16b,v0.16b,v6.16b
	eor	v5.16b,v16.16b,v7.16b
	b.eq	.Lcbc_enc128

	ld1	{v2.4s,v3.4s},[x7]
	add	x7,x3,#16
	add	x6,x3,#16*4
	add	x12,x3,#16*5
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	add	x14,x3,#16*6
	add	x3,x3,#16*7
	b	.Lenter_cbc_enc

.align	4
.Loop_cbc_enc:
	/* v6 owns C[i-1] (the IV for the first block). The loop publishes C[i]
	 * only after all rounds complete, then uses it to chain the next block.
	 * Separate 128/192/256 setup paths account for their different round keys.
	 */
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	st1	{v6.16b},[x1],#16
.Lenter_cbc_enc:
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v2.16b
	aesmc	v0.16b,v0.16b
	ld1	{v16.4s},[x6]
	cmp	w5,#4
	aese	v0.16b,v3.16b
	aesmc	v0.16b,v0.16b
	ld1	{v17.4s},[x12]
	b.eq	.Lcbc_enc192

	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	ld1	{v16.4s},[x14]
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	ld1	{v17.4s},[x3]
	nop

.Lcbc_enc192:
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	subs	x2,x2,#16
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	csel	x8,xzr,x8,eq
	aese	v0.16b,v18.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v19.16b
	aesmc	v0.16b,v0.16b
	ld1	{v16.16b},[x0],x8
	aese	v0.16b,v20.16b
	aesmc	v0.16b,v0.16b
	eor	v16.16b,v16.16b,v5.16b
	aese	v0.16b,v21.16b
	aesmc	v0.16b,v0.16b
	ld1	{v17.4s},[x7]		// re-pre-load rndkey[1]
	aese	v0.16b,v22.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v23.16b
	eor	v6.16b,v0.16b,v7.16b
	b.hs	.Loop_cbc_enc

	st1	{v6.16b},[x1],#16
	b	.Lcbc_done

.align	5
.Lcbc_enc128:
	ld1	{v2.4s,v3.4s},[x7]
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	b	.Lenter_cbc_enc128
.Loop_cbc_enc128:
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	st1	{v6.16b},[x1],#16
.Lenter_cbc_enc128:
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	subs	x2,x2,#16
	aese	v0.16b,v2.16b
	aesmc	v0.16b,v0.16b
	csel	x8,xzr,x8,eq
	aese	v0.16b,v3.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v18.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v19.16b
	aesmc	v0.16b,v0.16b
	ld1	{v16.16b},[x0],x8
	aese	v0.16b,v20.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v21.16b
	aesmc	v0.16b,v0.16b
	aese	v0.16b,v22.16b
	aesmc	v0.16b,v0.16b
	eor	v16.16b,v16.16b,v5.16b
	aese	v0.16b,v23.16b
	eor	v6.16b,v0.16b,v7.16b
	b.hs	.Loop_cbc_enc128

	st1	{v6.16b},[x1],#16
	b	.Lcbc_done
.align	5
.Lcbc_dec:
	/* CBC decryption has no AES dependency between ciphertext blocks. Keep
	 * original ciphertext in v2/v3/v27/v28/v29 for the later chaining XOR,
	 * while v0/v1/v24/v25/v26 advance through inverse AES rounds in parallel.
	 */
	ld1	{v24.16b},[x0],#16
	subs	x2,x2,#32		// bias
	add	w6,w5,#2
	orr	v3.16b,v0.16b,v0.16b
	orr	v1.16b,v0.16b,v0.16b
	orr	v27.16b,v24.16b,v24.16b
	b.lo	.Lcbc_dec_tail

	orr	v1.16b,v24.16b,v24.16b
	ld1	{v24.16b},[x0],#16
	orr	v2.16b,v0.16b,v0.16b
	orr	v3.16b,v1.16b,v1.16b
	orr	v27.16b,v24.16b,v24.16b
	cmp	x2,#32
	b.lo	.Loop3x_cbc_dec

	ld1	{v25.16b},[x0],#16
	ld1	{v26.16b},[x0],#16
	sub	x2,x2,#32		// bias
	mov	w6,w5
	orr	v28.16b,v25.16b,v25.16b
	orr	v29.16b,v26.16b,v26.16b

.Loop5x_cbc_dec:
	/* Five interleaved blocks hide AES instruction latency. Stores occur only
	 * after the final inverse round and XOR with each saved predecessor, which
	 * also keeps in-place decryption safe.
	 */
	aesd	v0.16b,v16.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v16.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v16.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v16.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v16.16b
	aesimc	v26.16b,v26.16b
	ld1	{v16.4s},[x7],#16
	subs	w6,w6,#2
	aesd	v0.16b,v17.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v17.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v17.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v17.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v17.16b
	aesimc	v26.16b,v26.16b
	ld1	{v17.4s},[x7],#16
	b.gt	.Loop5x_cbc_dec

	aesd	v0.16b,v16.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v16.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v16.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v16.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v16.16b
	aesimc	v26.16b,v26.16b
	cmp	x2,#0x40		// because .Lcbc_tail4x
	sub	x2,x2,#0x50

	aesd	v0.16b,v17.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v17.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v17.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v17.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v17.16b
	aesimc	v26.16b,v26.16b
	csel	x6,xzr,x2,gt		// borrow x6, w6, "gt" is not typo
	mov	x7,x3

	aesd	v0.16b,v18.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v18.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v18.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v18.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v18.16b
	aesimc	v26.16b,v26.16b
	add	x0,x0,x6		// x0 is adjusted in such way that
					// at exit from the loop v1.16b-v26.16b
					// are loaded with last "words"
	add	x6,x2,#0x60		// because .Lcbc_tail4x

	aesd	v0.16b,v19.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v19.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v19.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v19.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v19.16b
	aesimc	v26.16b,v26.16b

	aesd	v0.16b,v20.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v20.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v20.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v20.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v20.16b
	aesimc	v26.16b,v26.16b

	aesd	v0.16b,v21.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v21.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v21.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v21.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v21.16b
	aesimc	v26.16b,v26.16b

	aesd	v0.16b,v22.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v22.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v22.16b
	aesimc	v24.16b,v24.16b
	aesd	v25.16b,v22.16b
	aesimc	v25.16b,v25.16b
	aesd	v26.16b,v22.16b
	aesimc	v26.16b,v26.16b

	eor	v4.16b,v6.16b,v7.16b
	aesd	v0.16b,v23.16b
	eor	v5.16b,v2.16b,v7.16b
	ld1	{v2.16b},[x0],#16
	aesd	v1.16b,v23.16b
	eor	v17.16b,v3.16b,v7.16b
	ld1	{v3.16b},[x0],#16
	aesd	v24.16b,v23.16b
	eor	v30.16b,v27.16b,v7.16b
	ld1	{v27.16b},[x0],#16
	aesd	v25.16b,v23.16b
	eor	v31.16b,v28.16b,v7.16b
	ld1	{v28.16b},[x0],#16
	aesd	v26.16b,v23.16b
	orr	v6.16b,v29.16b,v29.16b
	ld1	{v29.16b},[x0],#16
	cbz	x6,.Lcbc_tail4x
	ld1	{v16.4s},[x7],#16	// re-pre-load rndkey[0]
	eor	v4.16b,v4.16b,v0.16b
	orr	v0.16b,v2.16b,v2.16b
	eor	v5.16b,v5.16b,v1.16b
	orr	v1.16b,v3.16b,v3.16b
	eor	v17.16b,v17.16b,v24.16b
	orr	v24.16b,v27.16b,v27.16b
	eor	v30.16b,v30.16b,v25.16b
	orr	v25.16b,v28.16b,v28.16b
	eor	v31.16b,v31.16b,v26.16b
	st1	{v4.16b},[x1],#16
	orr	v26.16b,v29.16b,v29.16b
	st1	{v5.16b},[x1],#16
	mov	w6,w5
	st1	{v17.16b},[x1],#16
	ld1	{v17.4s},[x7],#16	// re-pre-load rndkey[1]
	st1	{v30.16b},[x1],#16
	st1	{v31.16b},[x1],#16
	b.hs	.Loop5x_cbc_dec

	add	x2,x2,#0x50
	cbz	x2,.Lcbc_done

	add	w6,w5,#2
	subs	x2,x2,#0x30
	orr	v0.16b,v27.16b,v27.16b
	orr	v2.16b,v27.16b,v27.16b
	orr	v1.16b,v28.16b,v28.16b
	orr	v3.16b,v28.16b,v28.16b
	orr	v24.16b,v29.16b,v29.16b
	orr	v27.16b,v29.16b,v29.16b
	b.lo	.Lcbc_dec_tail

	b	.Loop3x_cbc_dec

.align	4
.Lcbc_tail4x:
	/* The four-block residue already completed its AES rounds in the 5-way
	 * pipeline; finish only the CBC predecessor XOR and ordered stores.
	 */
	eor	v5.16b,v4.16b,v1.16b
	eor	v17.16b,v17.16b,v24.16b
	eor	v30.16b,v30.16b,v25.16b
	eor	v31.16b,v31.16b,v26.16b
	st1	{v5.16b},[x1],#16
	st1	{v17.16b},[x1],#16
	st1	{v30.16b},[x1],#16
	st1	{v31.16b},[x1],#16

	b	.Lcbc_done
.align	4
.Loop3x_cbc_dec:
	/* Use the same parallel-decrypt rule for three-block groups. Pointer bias
	 * selects the final loaded ciphertexts without reading beyond the input.
	 */
	aesd	v0.16b,v16.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v16.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v16.16b
	aesimc	v24.16b,v24.16b
	ld1	{v16.4s},[x7],#16
	subs	w6,w6,#2
	aesd	v0.16b,v17.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v17.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v17.16b
	aesimc	v24.16b,v24.16b
	ld1	{v17.4s},[x7],#16
	b.gt	.Loop3x_cbc_dec

	aesd	v0.16b,v16.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v16.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v16.16b
	aesimc	v24.16b,v24.16b
	eor	v4.16b,v6.16b,v7.16b
	subs	x2,x2,#0x30
	eor	v5.16b,v2.16b,v7.16b
	csel	x6,x2,x6,lo			// x6, w6, is zero at this point
	aesd	v0.16b,v17.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v17.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v17.16b
	aesimc	v24.16b,v24.16b
	eor	v17.16b,v3.16b,v7.16b
	add	x0,x0,x6		// x0 is adjusted in such way that
					// at exit from the loop v1.16b-v24.16b
					// are loaded with last "words"
	orr	v6.16b,v27.16b,v27.16b
	mov	x7,x3
	aesd	v0.16b,v20.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v20.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v20.16b
	aesimc	v24.16b,v24.16b
	ld1	{v2.16b},[x0],#16
	aesd	v0.16b,v21.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v21.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v21.16b
	aesimc	v24.16b,v24.16b
	ld1	{v3.16b},[x0],#16
	aesd	v0.16b,v22.16b
	aesimc	v0.16b,v0.16b
	aesd	v1.16b,v22.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v22.16b
	aesimc	v24.16b,v24.16b
	ld1	{v27.16b},[x0],#16
	aesd	v0.16b,v23.16b
	aesd	v1.16b,v23.16b
	aesd	v24.16b,v23.16b
	ld1	{v16.4s},[x7],#16	// re-pre-load rndkey[0]
	add	w6,w5,#2
	eor	v4.16b,v4.16b,v0.16b
	eor	v5.16b,v5.16b,v1.16b
	eor	v24.16b,v24.16b,v17.16b
	ld1	{v17.4s},[x7],#16	// re-pre-load rndkey[1]
	st1	{v4.16b},[x1],#16
	orr	v0.16b,v2.16b,v2.16b
	st1	{v5.16b},[x1],#16
	orr	v1.16b,v3.16b,v3.16b
	st1	{v24.16b},[x1],#16
	orr	v24.16b,v27.16b,v27.16b
	b.hs	.Loop3x_cbc_dec

	cmn	x2,#0x30
	b.eq	.Lcbc_done
	nop

.Lcbc_dec_tail:
	/* One- and two-block residues share the inverse-round work. The length
	 * predicate selects how many chaining XOR results are committed.
	 */
	aesd	v1.16b,v16.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v16.16b
	aesimc	v24.16b,v24.16b
	ld1	{v16.4s},[x7],#16
	subs	w6,w6,#2
	aesd	v1.16b,v17.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v17.16b
	aesimc	v24.16b,v24.16b
	ld1	{v17.4s},[x7],#16
	b.gt	.Lcbc_dec_tail

	aesd	v1.16b,v16.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v16.16b
	aesimc	v24.16b,v24.16b
	aesd	v1.16b,v17.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v17.16b
	aesimc	v24.16b,v24.16b
	aesd	v1.16b,v20.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v20.16b
	aesimc	v24.16b,v24.16b
	cmn	x2,#0x20
	aesd	v1.16b,v21.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v21.16b
	aesimc	v24.16b,v24.16b
	eor	v5.16b,v6.16b,v7.16b
	aesd	v1.16b,v22.16b
	aesimc	v1.16b,v1.16b
	aesd	v24.16b,v22.16b
	aesimc	v24.16b,v24.16b
	eor	v17.16b,v3.16b,v7.16b
	aesd	v1.16b,v23.16b
	aesd	v24.16b,v23.16b
	b.eq	.Lcbc_dec_one
	eor	v5.16b,v5.16b,v1.16b
	eor	v17.16b,v17.16b,v24.16b
	orr	v6.16b,v27.16b,v27.16b
	st1	{v5.16b},[x1],#16
	st1	{v17.16b},[x1],#16
	b	.Lcbc_done

.Lcbc_dec_one:
	eor	v5.16b,v5.16b,v24.16b
	orr	v6.16b,v27.16b,v27.16b
	st1	{v5.16b},[x1],#16

.Lcbc_done:
	/* v6 is C[n-1]: the encryption result or the last original ciphertext. */
	st1	{v6.16b},[x4]
.Lcbc_abort:
	ldr	x29,[sp],#16
	ret
.size	aes_v8_cbc_encrypt,.-aes_v8_cbc_encrypt
.globl	aes_v8_ctr32_encrypt_blocks
.type	aes_v8_ctr32_encrypt_blocks,%function
.align	5
/* Encrypt or decrypt blocks with a 32-bit counter-mode keystream.
 *
 *   x0 = input, x1 = output, x2 = block count
 *   x3 = encryption schedule, x4 = read-only 16-byte counter block
 *
 * counter[0..11] remains fixed
 * counter[12..15] (big-endian) increments modulo 2^32 for each block
 *          |
 *          v
 * AES_encrypt(counter) XOR input -> output
 *
 * CTR uses the encryption schedule in both directions. The routine does not
 * write the caller's counter block or report the advanced counter, and the
 * caller must pass at least one block. Input and output may alias because each
 * input batch is loaded before its output is stored.
 */
aes_v8_ctr32_encrypt_blocks:
	bti	c
	stp	x29,x30,[sp,#-16]!
	add	x29,sp,#0
	ldr	w5,[x3,#240]

	ldr	w8, [x4, #12]
	ld1	{v0.4s},[x4]

	ld1	{v16.4s,v17.4s},[x3]		// load key schedule...
	sub	w5,w5,#4
	mov	x12,#16
	cmp	x2,#2
	add	x7,x3,x5,lsl#4	// pointer to last 5 round keys
	sub	w5,w5,#2
	ld1	{v20.4s,v21.4s},[x7],#32
	ld1	{v22.4s,v23.4s},[x7],#32
	ld1	{v7.4s},[x7]
	add	x7,x3,#32
	mov	w6,w5
	csel	x12,xzr,x12,lo
#ifndef __ARMEB__
	/* Scalar arithmetic uses the host byte order. Convert the counter word
	 * from its big-endian byte representation before incrementing it.
	 */
	rev	w8, w8
#endif
	orr	v1.16b,v0.16b,v0.16b
	add	w10, w8, #1
	orr	v18.16b,v0.16b,v0.16b
	add	w8, w8, #2
	orr	v6.16b,v0.16b,v0.16b
	rev	w10, w10
	mov	v1.s[3],w10
	b.ls	.Lctr32_tail
	rev	w12, w8
	sub	x2,x2,#3		// bias
	mov	v18.s[3],w12
	cmp	x2,#2
	b.lo	.Loop3x_ctr32

	add	w13,w8,#1
	add	w14,w8,#2
	orr	v24.16b,v0.16b,v0.16b
	rev	w13,w13
	orr	v25.16b,v0.16b,v0.16b
	rev	w14,w14
	mov	v24.s[3],w13
	sub	x2,x2,#2		// bias
	mov	v25.s[3],w14
	add	w8,w8,#2
	b	.Loop5x_ctr32

.align	4
.Loop5x_ctr32:
	/* Encrypt five adjacent counter values in parallel to cover AES latency.
	 * v0/v1/v18/v24/v25 carry counter blocks; v2/v3/v19/v26/v27 carry input.
	 * The final EOR combines each plaintext/ciphertext block with its keystream.
	 */
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v16.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v16.16b
	aesmc	v18.16b,v18.16b
	aese	v24.16b,v16.16b
	aesmc	v24.16b,v24.16b
	aese	v25.16b,v16.16b
	aesmc	v25.16b,v25.16b
	ld1	{v16.4s},[x7],#16
	subs	w6,w6,#2
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v17.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v17.16b
	aesmc	v18.16b,v18.16b
	aese	v24.16b,v17.16b
	aesmc	v24.16b,v24.16b
	aese	v25.16b,v17.16b
	aesmc	v25.16b,v25.16b
	ld1	{v17.4s},[x7],#16
	b.gt	.Loop5x_ctr32

	mov	x7,x3
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v16.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v16.16b
	aesmc	v18.16b,v18.16b
	aese	v24.16b,v16.16b
	aesmc	v24.16b,v24.16b
	aese	v25.16b,v16.16b
	aesmc	v25.16b,v25.16b
	ld1	{v16.4s},[x7],#16	// re-pre-load rndkey[0]

	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v17.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v17.16b
	aesmc	v18.16b,v18.16b
	aese	v24.16b,v17.16b
	aesmc	v24.16b,v24.16b
	aese	v25.16b,v17.16b
	aesmc	v25.16b,v25.16b
	ld1	{v17.4s},[x7],#16	// re-pre-load rndkey[1]

	aese	v0.16b,v20.16b
	aesmc	v0.16b,v0.16b
	add	w9,w8,#1
	add	w10,w8,#2
	aese	v1.16b,v20.16b
	aesmc	v1.16b,v1.16b
	add	w12,w8,#3
	add	w13,w8,#4
	aese	v18.16b,v20.16b
	aesmc	v18.16b,v18.16b
	add	w14,w8,#5
	rev	w9,w9
	aese	v24.16b,v20.16b
	aesmc	v24.16b,v24.16b
	rev	w10,w10
	rev	w12,w12
	aese	v25.16b,v20.16b
	aesmc	v25.16b,v25.16b
	rev	w13,w13
	rev	w14,w14

	aese	v0.16b,v21.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v21.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v21.16b
	aesmc	v18.16b,v18.16b
	aese	v24.16b,v21.16b
	aesmc	v24.16b,v24.16b
	aese	v25.16b,v21.16b
	aesmc	v25.16b,v25.16b

	aese	v0.16b,v22.16b
	aesmc	v0.16b,v0.16b
	ld1	{v2.16b},[x0],#16
	aese	v1.16b,v22.16b
	aesmc	v1.16b,v1.16b
	ld1	{v3.16b},[x0],#16
	aese	v18.16b,v22.16b
	aesmc	v18.16b,v18.16b
	ld1	{v19.16b},[x0],#16
	aese	v24.16b,v22.16b
	aesmc	v24.16b,v24.16b
	ld1	{v26.16b},[x0],#16
	aese	v25.16b,v22.16b
	aesmc	v25.16b,v25.16b
	ld1	{v27.16b},[x0],#16

	aese	v0.16b,v23.16b
	eor	v2.16b,v2.16b,v7.16b
	aese	v1.16b,v23.16b
	eor	v3.16b,v3.16b,v7.16b
	aese	v18.16b,v23.16b
	eor	v19.16b,v19.16b,v7.16b
	aese	v24.16b,v23.16b
	eor	v26.16b,v26.16b,v7.16b
	aese	v25.16b,v23.16b
	eor	v27.16b,v27.16b,v7.16b

	eor	v2.16b,v2.16b,v0.16b
	orr	v0.16b,v6.16b,v6.16b
	eor	v3.16b,v3.16b,v1.16b
	orr	v1.16b,v6.16b,v6.16b
	eor	v19.16b,v19.16b,v18.16b
	orr	v18.16b,v6.16b,v6.16b
	eor	v26.16b,v26.16b,v24.16b
	orr	v24.16b,v6.16b,v6.16b
	eor	v27.16b,v27.16b,v25.16b
	orr	v25.16b,v6.16b,v6.16b

	st1	{v2.16b},[x1],#16
	mov	v0.s[3],w9
	st1	{v3.16b},[x1],#16
	mov	v1.s[3],w10
	st1	{v19.16b},[x1],#16
	mov	v18.s[3],w12
	st1	{v26.16b},[x1],#16
	mov	v24.s[3],w13
	st1	{v27.16b},[x1],#16
	mov	v25.s[3],w14

	mov	w6,w5
	cbz	x2,.Lctr32_done

	add	w8,w8,#5
	subs	x2,x2,#5
	b.hs	.Loop5x_ctr32

	add	x2,x2,#5
	sub	w8,w8,#5

	cmp	x2,#2
	mov	x12,#16
	csel	x12,xzr,x12,lo
	b.ls	.Lctr32_tail

	sub	x2,x2,#3		// bias
	add	w8,w8,#3
	b	.Loop3x_ctr32

.align	4
.Loop3x_ctr32:
	/* Process three counters when fewer than five blocks remain. The counter
	 * template is restored from v6 and only lane 3 is replaced for each block.
	 */
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v16.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v16.16b
	aesmc	v18.16b,v18.16b
	ld1	{v16.4s},[x7],#16
	subs	w6,w6,#2
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v17.16b
	aesmc	v1.16b,v1.16b
	aese	v18.16b,v17.16b
	aesmc	v18.16b,v18.16b
	ld1	{v17.4s},[x7],#16
	b.gt	.Loop3x_ctr32

	aese	v0.16b,v16.16b
	aesmc	v4.16b,v0.16b
	aese	v1.16b,v16.16b
	aesmc	v5.16b,v1.16b
	ld1	{v2.16b},[x0],#16
	orr	v0.16b,v6.16b,v6.16b
	aese	v18.16b,v16.16b
	aesmc	v18.16b,v18.16b
	ld1	{v3.16b},[x0],#16
	orr	v1.16b,v6.16b,v6.16b
	aese	v4.16b,v17.16b
	aesmc	v4.16b,v4.16b
	aese	v5.16b,v17.16b
	aesmc	v5.16b,v5.16b
	ld1	{v19.16b},[x0],#16
	mov	x7,x3
	aese	v18.16b,v17.16b
	aesmc	v17.16b,v18.16b
	orr	v18.16b,v6.16b,v6.16b
	add	w9,w8,#1
	aese	v4.16b,v20.16b
	aesmc	v4.16b,v4.16b
	aese	v5.16b,v20.16b
	aesmc	v5.16b,v5.16b
	eor	v2.16b,v2.16b,v7.16b
	add	w10,w8,#2
	aese	v17.16b,v20.16b
	aesmc	v17.16b,v17.16b
	eor	v3.16b,v3.16b,v7.16b
	add	w8,w8,#3
	aese	v4.16b,v21.16b
	aesmc	v4.16b,v4.16b
	aese	v5.16b,v21.16b
	aesmc	v5.16b,v5.16b
	eor	v19.16b,v19.16b,v7.16b
	rev	w9,w9
	aese	v17.16b,v21.16b
	aesmc	v17.16b,v17.16b
	mov	v0.s[3], w9
	rev	w10,w10
	aese	v4.16b,v22.16b
	aesmc	v4.16b,v4.16b
	aese	v5.16b,v22.16b
	aesmc	v5.16b,v5.16b
	mov	v1.s[3], w10
	rev	w12,w8
	aese	v17.16b,v22.16b
	aesmc	v17.16b,v17.16b
	mov	v18.s[3], w12
	subs	x2,x2,#3
	aese	v4.16b,v23.16b
	aese	v5.16b,v23.16b
	aese	v17.16b,v23.16b

	eor	v2.16b,v2.16b,v4.16b
	ld1	{v16.4s},[x7],#16	// re-pre-load rndkey[0]
	st1	{v2.16b},[x1],#16
	eor	v3.16b,v3.16b,v5.16b
	mov	w6,w5
	st1	{v3.16b},[x1],#16
	eor	v19.16b,v19.16b,v17.16b
	ld1	{v17.4s},[x7],#16	// re-pre-load rndkey[1]
	st1	{v19.16b},[x1],#16
	b.hs	.Loop3x_ctr32

	adds	x2,x2,#3
	b.eq	.Lctr32_done
	cmp	x2,#1
	mov	x12,#16
	csel	x12,xzr,x12,eq

.Lctr32_tail:
	/* The one- and two-block tail shares two AES lanes. x12 is zero for a
	 * single block, preventing the second input load from advancing beyond the
	 * first block; the final count check suppresses the second store.
	 */
	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v16.16b
	aesmc	v1.16b,v1.16b
	ld1	{v16.4s},[x7],#16
	subs	w6,w6,#2
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v17.16b
	aesmc	v1.16b,v1.16b
	ld1	{v17.4s},[x7],#16
	b.gt	.Lctr32_tail

	aese	v0.16b,v16.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v16.16b
	aesmc	v1.16b,v1.16b
	aese	v0.16b,v17.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v17.16b
	aesmc	v1.16b,v1.16b
	ld1	{v2.16b},[x0],x12
	aese	v0.16b,v20.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v20.16b
	aesmc	v1.16b,v1.16b
	ld1	{v3.16b},[x0]
	aese	v0.16b,v21.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v21.16b
	aesmc	v1.16b,v1.16b
	eor	v2.16b,v2.16b,v7.16b
	aese	v0.16b,v22.16b
	aesmc	v0.16b,v0.16b
	aese	v1.16b,v22.16b
	aesmc	v1.16b,v1.16b
	eor	v3.16b,v3.16b,v7.16b
	aese	v0.16b,v23.16b
	aese	v1.16b,v23.16b

	cmp	x2,#1
	eor	v2.16b,v2.16b,v0.16b
	eor	v3.16b,v3.16b,v1.16b
	st1	{v2.16b},[x1],#16
	b.eq	.Lctr32_done
	st1	{v3.16b},[x1]

.Lctr32_done:
	ldr	x29,[sp],#16
	ret
.size	aes_v8_ctr32_encrypt_blocks,.-aes_v8_ctr32_encrypt_blocks
