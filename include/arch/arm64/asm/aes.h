/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_AES_H
#define ARM64_AES_H

#include <types.h>
#include <util.h>
#include <asm/sysreg.h>

#ifndef ASSEMBLER

#define ARM64_AES_BLOCK_SIZE		16U
#define ARM64_AES_MAX_ROUNDS		14U
#define ARM64_AES_ROUND_KEY_WORDS	(4U * (ARM64_AES_MAX_ROUNDS + 1U))
#define ARM64_AES_KEY_BITS_128		128

#define ID_AA64ISAR0_AES_SHIFT		4U
#define ID_AA64ISAR0_AES_MASK		0xfUL
#define ID_AA64ISAR0_AES_BASE		1UL
#define ID_AA64ISAR0_AES_PMULL		2UL

struct arm64_aes_key {
	uint32_t rd_key[ARM64_AES_ROUND_KEY_WORDS];
	int32_t rounds;
};

_Static_assert(offsetof(struct arm64_aes_key, rounds) == 240U,
	"ARM64 AES round count offset must match aes.S");
_Static_assert(sizeof(struct arm64_aes_key) == 244U,
	"ARM64 AES key schedule size must match aes.S");

static inline bool arm64_aes_supported(void)
{
	uint64_t field = (arm64_sysreg_read(s3_0_c0_c6_0) >>
		ID_AA64ISAR0_AES_SHIFT) & ID_AA64ISAR0_AES_MASK;

	return (field == ID_AA64ISAR0_AES_BASE) ||
		(field == ID_AA64ISAR0_AES_PMULL);
}

int32_t aes_v8_set_encrypt_key(const uint8_t *user_key, int32_t bits,
	struct arm64_aes_key *key);
int32_t aes_v8_set_decrypt_key(const uint8_t *user_key, int32_t bits,
	struct arm64_aes_key *key);
void aes_v8_encrypt(const uint8_t *in, uint8_t *out,
	const struct arm64_aes_key *key);
void aes_v8_decrypt(const uint8_t *in, uint8_t *out,
	const struct arm64_aes_key *key);
void aes_v8_cbc_encrypt(const uint8_t *in, uint8_t *out, size_t length,
	const struct arm64_aes_key *key, uint8_t *ivec, int32_t enc);
void aes_v8_ctr32_encrypt_blocks(const uint8_t *in, uint8_t *out,
	size_t blocks, const struct arm64_aes_key *key,
	const uint8_t ivec[ARM64_AES_BLOCK_SIZE]);

#endif /* ASSEMBLER */

#endif /* ARM64_AES_H */
