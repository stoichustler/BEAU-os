/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <mmu.h>
#include <rtl.h>
#include <asm/ddb.h>
#include <asm/trap.h>
#include "ddb_internal.h"

struct ddb_probe_state {
	uint64_t fault_address;
	uint64_t fault_esr;
	bool active;
};

static struct ddb_probe_state ddb_probe_states[MAX_PCPU_NUM];

extern char arm64_ddb_probe_load[];
extern char arm64_ddb_probe_recover[];
int32_t arm64_ddb_probe_read8(uint64_t address, uint8_t *value);

/* [20260720] FreeBSD-derived recoverable EL2 memory probe
 *
 *   validate Normal/readable mapping -> publish pCPU probe -> one LDRB
 *                                                  |
 *                                                  +--> success -> clear
 *                                                  |
 *                                                  `--> DABT -> fixed recovery
 *
 * Design origin:
 *   - FreeBSD ARM64 db_read_bytes() uses debugger-scoped fault recovery for
 *     invalid memory reads (sys/arm64/arm64/db_interface.c);
 *   - BEAU reimplements it as exact-PC EL2 recovery and rejects MMIO.
 *
 * Key rule:
 *   - probe state is pCPU-owned and active for exactly one assembly load;
 *   - trap recovery requires the expected EC and exact load instruction PC;
 *   - MMIO is rejected before access so a read cannot trigger device effects.
 */
bool ddb_mem_handle_fault(struct intr_excp_ctx *ctx)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct ddb_probe_state *state;

	if ((ctx == NULL) || (pcpu_id >= MAX_PCPU_NUM) ||
		(ESR_EL2_EC(ctx->regs.esr) != ESR_EL2_EC_DABT_CUR) ||
		(ctx->regs.elr != (uint64_t)arm64_ddb_probe_load)) {
		return false;
	}
	state = &ddb_probe_states[pcpu_id];
	if (!__atomic_load_n(&state->active, __ATOMIC_ACQUIRE)) {
		return false;
	}

	state->fault_address = ctx->regs.far;
	state->fault_esr = ctx->regs.esr;
	__atomic_store_n(&state->active, false, __ATOMIC_RELEASE);
	ctx->regs.elr = (uint64_t)arm64_ddb_probe_recover;
	return true;
}

void ddb_mem_cancel(uint16_t pcpu_id)
{
	if (pcpu_id < MAX_PCPU_NUM) {
		__atomic_store_n(&ddb_probe_states[pcpu_id].active, false,
			__ATOMIC_RELEASE);
	}
}

static int32_t ddb_validate_read_address(uint64_t address)
{
	struct arm64_memory_attr attr;
	uint8_t access = 0U;

	if (!arm64_get_hv_s1_memory_attr(address, &attr) ||
		!arm64_get_hv_s1_memory_access(address, &access)) {
		return -EFAULT;
	}
	if ((attr.type == ARM64_MEMORY_UNMAPPED) ||
		((access & ARM64_S1_ACCESS_READ) == 0U)) {
		return -EFAULT;
	}
	if (attr.type != ARM64_MEMORY_NORMAL) {
		return -EACCES;
	}

	return 0;
}

int32_t ddb_read_memory(uint64_t address, void *buffer, uint32_t size,
	uint64_t *fault_address, uint64_t *fault_esr)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct ddb_probe_state *state;
	uint8_t *bytes = buffer;
	uint32_t index;
	int32_t status = 0;

	if ((buffer == NULL) || (size == 0U) || (pcpu_id >= MAX_PCPU_NUM) ||
		(address > (UINT64_MAX - ((uint64_t)size - 1UL)))) {
		return -EINVAL;
	}
	if (fault_address != NULL) {
		*fault_address = 0UL;
	}
	if (fault_esr != NULL) {
		*fault_esr = 0UL;
	}

	for (index = 0U; index < size; index++) {
		status = ddb_validate_read_address(address + index);
		if (status != 0) {
			if (fault_address != NULL) {
				*fault_address = address + index;
			}
			return status;
		}
	}

	state = &ddb_probe_states[pcpu_id];
	for (index = 0U; index < size; index++) {
		state->fault_address = address + index;
		state->fault_esr = 0UL;
		__atomic_store_n(&state->active, true, __ATOMIC_RELEASE);
		status = arm64_ddb_probe_read8(address + index, &bytes[index]);
		__atomic_store_n(&state->active, false, __ATOMIC_RELEASE);
		if (status != 0) {
			if (fault_address != NULL) {
				*fault_address = state->fault_address;
			}
			if (fault_esr != NULL) {
				*fault_esr = state->fault_esr;
			}
			return status;
		}
	}

	return 0;
}

int32_t ddb_cmd_examine(__unused struct ddb_session *session, uint32_t argc,
	char **argv)
{
	uint8_t bytes[DDB_EXAMINE_MAX];
	uint64_t address;
	uint64_t count = DDB_EXAMINE_DEFAULT;
	uint64_t fault_address = 0UL;
	uint64_t fault_esr = 0UL;
	uint32_t row;
	int32_t status;

	if ((argc < 2U) || (argc > 3U) ||
		!ddb_parse_u64(argv[1], 16U, &address) ||
		((argc == 3U) && !ddb_parse_u64(argv[2], 10U, &count)) ||
		(count == 0UL) || (count > DDB_EXAMINE_MAX) ||
		(address > (UINT64_MAX - (count - 1UL)))) {
		ddb_puts("usage: x <hex-address> [1-256 byte-count]\n");
		return -EINVAL;
	}

	status = ddb_read_memory(address, bytes, (uint32_t)count,
		&fault_address, &fault_esr);
	if (status != 0) {
		ddb_printf("ddb: read rejected at 0x%016lx status:%d esr:0x%016lx\n",
			fault_address, status, fault_esr);
		return status;
	}

	for (row = 0U; row < (uint32_t)count; row += 16U) {
		uint32_t column;

		ddb_printf("0x%016lx:", address + row);
		for (column = 0U; (column < 16U) &&
			((row + column) < (uint32_t)count); column++) {
			ddb_printf(" %02x", bytes[row + column]);
		}
		ddb_puts("\n");
	}
	(void)memset(bytes, 0U, sizeof(bytes));
	return 0;
}
