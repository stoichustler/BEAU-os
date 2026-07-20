/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_DDB_INTERNAL_H
#define ARM64_DDB_INTERNAL_H

#include <types.h>
#include <asm/irq.h>

#define DDB_LINE_SIZE		128U
#define DDB_MAX_ARGS		8U
#define DDB_EXAMINE_DEFAULT	64U
#define DDB_EXAMINE_MAX		256U
#define DDB_STACK_DEPTH		32U
#define DDB_SMP_TIMEOUT_US	1000U

struct ddb_session {
	struct intr_excp_ctx *ctx;
	uint16_t pcpu_id;
	bool done;
};

void ddb_puts(const char *text);
void ddb_printf(const char *fmt, ...);
int32_t ddb_read_line(char *line, uint32_t size);
uint32_t ddb_split_line(char *line, char **argv, uint32_t max_args);
bool ddb_parse_u64(const char *text, uint32_t base, uint64_t *value);

bool ddb_mem_handle_fault(struct intr_excp_ctx *ctx);
void ddb_mem_cancel(uint16_t pcpu_id);
int32_t ddb_read_memory(uint64_t address, void *buffer, uint32_t size,
	uint64_t *fault_address, uint64_t *fault_esr);
int32_t ddb_cmd_examine(struct ddb_session *session, uint32_t argc,
	char **argv);

int32_t ddb_cmd_backtrace(struct ddb_session *session, uint32_t argc,
	char **argv);
int32_t ddb_cmd_cpu(struct ddb_session *session, uint32_t argc,
	char **argv);

#endif /* ARM64_DDB_INTERNAL_H */
