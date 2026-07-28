/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <crypto/crypto_api.h>
#include <errno.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>
#include <asm/aes.h>
#include <asm/ddb.h>
#include <asm/trusty.h>

#include "shell_cmds.h"

#define SHELL_AES_MAX_PLAINTEXT_LEN	31U
#define SHELL_AES_MAX_CIPHERTEXT_LEN	32U
#define SHELL_AES_MAX_HEX_LEN	(SHELL_AES_MAX_CIPHERTEXT_LEN * 2U)
#define SHELL_AES_CIPHERTEXT_PREFIX	"ciphertext: "
#define SHELL_AES_PLAINTEXT_PREFIX	"plaintext : "
#define SHELL_DDB_PASSWORD_MAX_LEN	32U
#define SHELL_DDB_AUTH_FAILURE_LIMIT	3U
#define SHELL_DDB_AUTH_LOCKOUT_US	5000000U
#define SHELL_TRUSTY_VERSION_SIZE	129U

static const uint8_t shell_aes_test_key[ARM64_AES_BLOCK_SIZE] = {
	0x2bU, 0x7eU, 0x15U, 0x16U, 0x28U, 0xaeU, 0xd2U, 0xa6U,
	0xabU, 0xf7U, 0x15U, 0x88U, 0x09U, 0xcfU, 0x4fU, 0x3cU,
};

static const uint8_t shell_ddb_password_digest[SHA256_DIGEST_SIZE] = {
	0x1aU, 0xeaU, 0x2dU, 0xf6U, 0xa0U, 0x05U, 0xbbU, 0x83U,
	0xdfU, 0x9bU, 0x1cU, 0x26U, 0x80U, 0xb5U, 0x6cU, 0x47U,
	0xddU, 0x56U, 0x75U, 0x2cU, 0xdeU, 0x8eU, 0x53U, 0x0bU,
	0x2aU, 0xc4U, 0xe6U, 0x30U, 0x49U, 0x7bU, 0xe1U, 0xe8U,
};

static uint64_t shell_ddb_auth_lockout_deadline;
static uint32_t shell_ddb_auth_failures;

/* [20260719] ARM64 AES console validation flow
 *
 * shell argv -> bound and validate -> stack-owned key/data/IV
 *                                      |
 *                                      v
 *                              OpenBSD aes.S routine
 *                                      |
 *                    +-----------------+-----------------+
 *                    |                                   |
 *                    v                                   v
 *             validate padding                    format ciphertext
 *                    |                                   |
 *                    +-----------------+-----------------+
 *                                      v
 *                              publish console output
 *
 * Key rule:
 *   - the shell command owns all mutable AES state on its stack;
 *   - capability, length, character, hex, and padding checks complete before
 *     plaintext or ciphertext is published;
 *   - the AES call does not yield or invoke a shell output checkpoint while
 *     caller-saved SIMD registers hold live cipher state;
 *   - every exit clears the stack-owned key schedule, data, IV, and output.
 */
static bool shell_aes_printable_text(const uint8_t *text, size_t length)
{
	bool valid = (text != NULL) && (length > 0U) &&
		(length <= SHELL_AES_MAX_PLAINTEXT_LEN);

	for (size_t idx = 0U; valid && (idx < length); idx++) {
		valid = (text[idx] >= 0x21U) && (text[idx] <= 0x7eU) &&
			(text[idx] != (uint8_t)',');
	}

	return valid;
}

static bool shell_aes_hex_nibble(char ch, uint8_t *nibble)
{
	bool valid = nibble != NULL;

	if (!valid) {
		return false;
	}
	if ((ch >= '0') && (ch <= '9')) {
		*nibble = (uint8_t)(ch - '0');
	} else if ((ch >= 'a') && (ch <= 'f')) {
		*nibble = (uint8_t)(ch - 'a') + 10U;
	} else if ((ch >= 'A') && (ch <= 'F')) {
		*nibble = (uint8_t)(ch - 'A') + 10U;
	} else {
		valid = false;
	}

	return valid;
}

static bool shell_aes_decode_hex(const char *hex, uint8_t *data, size_t *data_len)
{
	size_t hex_len;
	uint8_t high;
	uint8_t low;
	bool valid;

	if ((hex == NULL) || (data == NULL) || (data_len == NULL)) {
		return false;
	}
	hex_len = strnlen_s(hex, SHELL_AES_MAX_HEX_LEN + 1U);
	valid = (hex_len == (ARM64_AES_BLOCK_SIZE * 2U)) ||
		(hex_len == SHELL_AES_MAX_HEX_LEN);
	for (size_t idx = 0U; valid && (idx < hex_len); idx += 2U) {
		valid = shell_aes_hex_nibble(hex[idx], &high) &&
			shell_aes_hex_nibble(hex[idx + 1U], &low);
		if (valid) {
			data[idx / 2U] = (uint8_t)((high << 4U) | low);
		}
	}
	if (valid) {
		*data_len = hex_len / 2U;
	}

	return valid;
}

static bool shell_aes_remove_padding(uint8_t *data, size_t data_len,
	size_t *plaintext_len)
{
	uint8_t padding;
	bool valid;

	if ((data == NULL) || (plaintext_len == NULL) ||
		(data_len < ARM64_AES_BLOCK_SIZE) ||
		(data_len > SHELL_AES_MAX_CIPHERTEXT_LEN)) {
		return false;
	}
	padding = data[data_len - 1U];
	valid = (padding > 0U) && (padding <= ARM64_AES_BLOCK_SIZE) &&
		((size_t)padding <= data_len);
	for (size_t idx = 0U; valid && (idx < (size_t)padding); idx++) {
		valid = data[data_len - 1U - idx] == padding;
	}
	if (valid) {
		*plaintext_len = data_len - (size_t)padding;
		valid = shell_aes_printable_text(data, *plaintext_len);
	}

	return valid;
}

static int32_t shell_aes_encrypt_text(const char *text)
{
	static const char hex_digits[] = "0123456789abcdef";
	struct arm64_aes_key key = { 0U };
	uint8_t data[SHELL_AES_MAX_CIPHERTEXT_LEN] = { 0U };
	uint8_t iv[ARM64_AES_BLOCK_SIZE] = { 0U };
	char output[sizeof(SHELL_AES_CIPHERTEXT_PREFIX) + SHELL_AES_MAX_HEX_LEN + 2U] = { 0 };
	size_t text_len;
	size_t data_len;
	size_t output_idx;
	uint8_t padding;
	int32_t status = -EINVAL;

	text_len = strnlen_s(text, SHELL_AES_MAX_PLAINTEXT_LEN + 1U);
	if (!shell_aes_printable_text((const uint8_t *)text, text_len)) {
		shell_puts("AES plaintext must contain 1-31 printable non-comma characters\r\n");
		goto out;
	}
	padding = (uint8_t)(ARM64_AES_BLOCK_SIZE - (text_len % ARM64_AES_BLOCK_SIZE));
	data_len = text_len + (size_t)padding;
	memcpy(data, text, text_len);
	(void)memset(&data[text_len], padding, (size_t)padding);
	if (aes_v8_set_encrypt_key(shell_aes_test_key, ARM64_AES_KEY_BITS_128, &key) != 0) {
		shell_puts("AES encryption key setup failed\r\n");
		status = -EIO;
		goto out;
	}
	aes_v8_cbc_encrypt(data, data, data_len, &key, iv, 1);

	memcpy(output, SHELL_AES_CIPHERTEXT_PREFIX,
		sizeof(SHELL_AES_CIPHERTEXT_PREFIX) - 1U);
	output_idx = sizeof(SHELL_AES_CIPHERTEXT_PREFIX) - 1U;
	for (size_t idx = 0U; idx < data_len; idx++) {
		output[output_idx++] = hex_digits[data[idx] >> 4U];
		output[output_idx++] = hex_digits[data[idx] & 0x0fU];
	}
	output[output_idx++] = '\r';
	output[output_idx++] = '\n';
	output[output_idx] = '\0';
	shell_puts(output);
	status = 0;

out:
	(void)memset(&key, 0U, sizeof(key));
	(void)memset(data, 0U, sizeof(data));
	(void)memset(iv, 0U, sizeof(iv));
	(void)memset(output, 0U, sizeof(output));
	return status;
}

static int32_t shell_aes_decrypt_text(const char *hex)
{
	struct arm64_aes_key key = { 0U };
	uint8_t data[SHELL_AES_MAX_CIPHERTEXT_LEN + 1U] = { 0U };
	uint8_t iv[ARM64_AES_BLOCK_SIZE] = { 0U };
	char output[sizeof(SHELL_AES_PLAINTEXT_PREFIX) + SHELL_AES_MAX_PLAINTEXT_LEN + 2U] = { 0 };
	size_t data_len = 0U;
	size_t plaintext_len = 0U;
	int32_t status = -EINVAL;

	if (!shell_aes_decode_hex(hex, data, &data_len)) {
		shell_puts("AES ciphertext must contain 32 or 64 hexadecimal characters\r\n");
		goto out;
	}
	if (aes_v8_set_decrypt_key(shell_aes_test_key, ARM64_AES_KEY_BITS_128, &key) != 0) {
		shell_puts("AES decryption key setup failed\r\n");
		status = -EIO;
		goto out;
	}
	aes_v8_cbc_encrypt(data, data, data_len, &key, iv, 0);
	if (!shell_aes_remove_padding(data, data_len, &plaintext_len)) {
		shell_puts("AES ciphertext has invalid padding or plaintext\r\n");
		goto out;
	}
	data[plaintext_len] = '\0';
	(void)snprintf(output, sizeof(output), "%s%s\r\n",
		SHELL_AES_PLAINTEXT_PREFIX, (const char *)data);
	shell_puts(output);
	status = 0;

out:
	(void)memset(&key, 0U, sizeof(key));
	(void)memset(data, 0U, sizeof(data));
	(void)memset(iv, 0U, sizeof(iv));
	(void)memset(output, 0U, sizeof(output));
	return status;
}

int32_t shell_aes(int32_t argc, char **argv)
{
	/* enc prints ciphertext hex; dec prints validated printable plaintext. Neither
	 * output is retained after the stack-owned cryptographic buffers are cleared.
	 */
	if (argc != 3) {
		shell_puts("usage: aes <enc|dec> <text|hex>\r\n");
		return -EINVAL;
	}
	if (!arm64_aes_supported()) {
		shell_puts("ARMv8 AES instructions are not supported on this pCPU\r\n");
		return -ENOTSUP;
	}
	if (strcmp(argv[1], "enc") == 0) {
		return shell_aes_encrypt_text(argv[2]);
	}
	if (strcmp(argv[1], "dec") == 0) {
		return shell_aes_decrypt_text(argv[2]);
	}

	shell_puts("usage: aes <enc|dec> <text|hex>\r\n");
	return -EINVAL;
}

/* [20260728] Trusty shell diagnostics
 *
 * shell argv -> version -> bounded build-string query -> console line
 *           |
 *           `-> dump -> complete system snapshot -> console lines
 *                              |
 *                              +--> unavailable: no partial result
 *
 * Key rule:
 *   - the shell owns the stack buffer and the ARM64 helper owns secure-world
 *     argument validation;
 *   - only fixed diagnostics can leave BEAU, after argument validation;
 *   - failed queries publish no partial Trusty result and clear local storage.
 */
static int32_t shell_tee_dump(void)
{
	struct arm64_trusty_system_info info = { 0U };
	char line[MAX_STR_SIZE] = { 0 };
	int32_t status;

	status = arm64_trusty_get_system_info(&info);
	if (status != 0) {
		shell_puts("Trusty TEE Information: N/A\r\n");
		return status;
	}

	shell_puts("Trusty TEE Information\r\n");
	(void)snprintf(line, sizeof(line), "smp.max-cpus: %u\r\n", info.smp_max_cpus);
	shell_puts(line);
	if (info.api_version_valid) {
		(void)snprintf(line, sizeof(line), "smc.api-version: %u\r\n",
			info.api_version);
	} else {
		(void)snprintf(line, sizeof(line), "smc.api-version: N/A\r\n");
	}
	shell_puts(line);
	(void)memset(line, 0U, sizeof(line));
	(void)memset(&info, 0U, sizeof(info));

	return 0;
}

int32_t shell_tee(int32_t argc, char **argv)
{
	char version[SHELL_TRUSTY_VERSION_SIZE] = { 0 };
	int32_t status;

	if (argc != 2) {
		shell_puts("usage: tee <version|dump>\r\n");
		return -EINVAL;
	}
	if (strcmp(argv[1], "dump") == 0) {
		return shell_tee_dump();
	}
	if (strcmp(argv[1], "version") != 0) {
		shell_puts("usage: tee <version|dump>\r\n");
		return -EINVAL;
	}

	status = arm64_trusty_get_version(version, sizeof(version));
	if (status == 0) {
		shell_puts("Trusty TEE version: ");
		shell_puts(version);
		shell_puts("\r\n");
	} else {
		shell_puts("Trusty TEE version unavailable\r\n");
	}
	(void)memset(version, 0U, sizeof(version));

	return status;
}

/* [20260720] Privileged DDB shell entry
 *
 *   masked candidate -> SHA-256 -> fixed-length digest comparison
 *                              |
 *                              +--> fail: bounded attempt state
 *                              |
 *                              `--> pass: clear state -> Host BRK
 *
 * Key rule:
 *   - the shell owns authentication state and DDB owns exception-time state;
 *   - plaintext exists only in the shell's private command buffer;
 *   - repeated failures are rejected without blocking the shell thread.
 */
static bool shell_ddb_digest_equal(const uint8_t *left, const uint8_t *right)
{
	uint8_t difference = 0U;
	uint32_t index;

	for (index = 0U; index < SHA256_DIGEST_SIZE; index++) {
		difference |= left[index] ^ right[index];
	}

	return difference == 0U;
}

static bool shell_ddb_auth_locked(uint64_t now)
{
	if ((shell_ddb_auth_lockout_deadline != 0UL) &&
		((int64_t)(now - shell_ddb_auth_lockout_deadline) >= 0L)) {
		shell_ddb_auth_lockout_deadline = 0UL;
		shell_ddb_auth_failures = 0U;
	}

	return shell_ddb_auth_lockout_deadline != 0UL;
}

int32_t shell_ddb(int32_t argc, char **argv)
{
	uint8_t digest[SHA256_DIGEST_SIZE] = { 0U };
	uint64_t now = cpu_ticks();
	size_t password_len;
	bool authenticated = false;
	int32_t status = -EACCES;

	/* Authentication output reports only result/lockout state; password bytes are
	 * never echoed and are cleared by the sensitive-input handling path.
	 */
	if (argc != 2) {
		shell_puts("usage: ddb <passwd>\r\n");
		return -EINVAL;
	}
	if (shell_ddb_auth_locked(now)) {
		shell_puts("DDB authentication temporarily locked\r\n");
		return status;
	}

	password_len = strnlen_s(argv[1], SHELL_DDB_PASSWORD_MAX_LEN + 1U);
	if ((password_len > 0U) && (password_len <= SHELL_DDB_PASSWORD_MAX_LEN) &&
		(sha256_digest(digest, (const uint8_t *)argv[1], password_len) != 0)) {
		authenticated = shell_ddb_digest_equal(digest,
			shell_ddb_password_digest);
	}
	(void)memset(argv[1], 0U, password_len);
	(void)memset(digest, 0U, sizeof(digest));

	if (!authenticated) {
		shell_ddb_auth_failures++;
		if (shell_ddb_auth_failures >= SHELL_DDB_AUTH_FAILURE_LIMIT) {
			shell_ddb_auth_lockout_deadline = now +
				us_to_ticks(SHELL_DDB_AUTH_LOCKOUT_US);
		}
		shell_puts("DDB authentication failed\r\n");
		return status;
	}

	shell_ddb_auth_failures = 0U;
	shell_ddb_auth_lockout_deadline = 0UL;
	arm64_ddb_break();
	return 0;
}
