/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * Bare boot mode is useful when we run on platforms that do not support
 * Multiboot. Bare boot allows ACRN boot components to be pre-configured at
 * compile time.
 *
 * ARM64 static platforms use this path for the SDK bring-up flow: vconfig.c
 * provides a bare_boot_options[] table whose tags match VM kernel, ramdisk, and
 * FDT module tags. init_bare_boot_info() turns that table into the same
 * acrn_boot_info module list that Multiboot would have produced, so the later
 * per-VM loader code does not care which boot source was used.
 */

#include <types.h>
#include <boot.h>
#include <bare.h>
#include <rtl.h>
#include <pgtable.h>
#include <logmsg.h>

static struct bare_boot_option *options;
static uint16_t nmods;

uint16_t get_mod_count()
{
	return nmods;
}

void *get_mod_addr(uint16_t mod_idx)
{
	void *ret = NULL;
	if (mod_idx < nmods) {
		ret = hpa2hva_early(options[mod_idx].addr);
	}
	return ret;
}

uint64_t get_mod_size(uint16_t mod_idx)
{
	uint64_t ret = 0;
	if (mod_idx < nmods) {
		ret = options[mod_idx].size;
	}
	return ret;
}

const char *get_mod_tag(uint16_t mod_idx)
{
	const char *ret = NULL;
	if (mod_idx < nmods) {
		ret = options[mod_idx].tag;
	}
	return ret;
}

int32_t init_bare_boot_info()
{
	extern struct bare_boot_option bare_boot_options[];
	extern uint16_t n_bare_boot_options;
	struct acrn_boot_info *abi = get_acrn_boot_info();
	struct abi_module *m;
	const char *tag;
	uint64_t mod_size;
	int i;

	(void)strncpy_s((void *)abi->protocol_name, MAX_PROTOCOL_NAME_SIZE,
			"bare boot", (MAX_PROTOCOL_NAME_SIZE - 1U));

	(void)strncpy_s((void *)(abi->loader_name), MAX_LOADER_NAME_SIZE,
			"bare boot loader", (MAX_LOADER_NAME_SIZE - 1U));

	options = bare_boot_options;
	nmods = n_bare_boot_options;

	/*
	 * The bare module table is trusted platform configuration, but the common
	 * boot ABI has a fixed module capacity. Clamp here so later module tag
	 * searches can iterate abi->mods_count without knowing the source table.
	 */
	if (nmods > MAX_MODULE_NUM) {
		LOG_ERR("bareboot: too many boot modules (%d found)", nmods);
		LOG_ERR("bareboot: accepting only %d, ignoring rest", MAX_MODULE_NUM);
		nmods = MAX_MODULE_NUM;
	}

	/* [20260719] Bare module ABI narrowing
	 *
	 *   platform module (uint64_t size)
	 *                 |
	 *                 +--> size > UINT32_MAX --> fail closed
	 *                 |
	 *                 v
	 *   populate private ABI slots -> publish mods_count
	 *
	 * Key rule:
	 *   - static platform configuration owns the source address and size;
	 *   - all sizes are validated before the module list becomes visible;
	 *   - the common 32-bit boot ABI must never observe a truncated payload.
	 */
	for (i = 0; i < nmods; i++) {
		m = &(abi->mods[i]);
		m->start = get_mod_addr(i);
		mod_size = get_mod_size(i);
		if (mod_size > UINT32_MAX) {
			panic("bareboot: module %d size 0x%lx exceeds 32-bit boot ABI",
				i, mod_size);
		}
		m->size = (uint32_t)mod_size;
		tag = get_mod_tag(i);
		(void)strncpy_s((void *)(m->string), MAX_MOD_STRING_SIZE,
				tag, strnlen_s(tag, MAX_MOD_STRING_SIZE));
	}
	abi->mods_count = nmods;

	return 0;
}
