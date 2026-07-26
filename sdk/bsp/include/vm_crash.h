/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VM_CRASH_H
#define VM_CRASH_H

#include <types.h>

struct acrn_vcpu;
struct acrn_vm;

#define BEAU_VM_CRASH_MAGIC		0x42435253U
#define BEAU_VM_CRASH_ABI_VERSION_V1	1U
#define BEAU_VM_CRASH_ABI_VERSION	2U
#define BEAU_VM_CRASH_TEXT_MAX		96U
#define BEAU_VM_CRASH_COMM_MAX		16U
#define BEAU_VM_CRASH_STACK_MAX		16U
#define BEAU_VM_CRASH_REPORT_V1_SIZE	144U
#define BEAU_VM_CRASH_REPORT_SIZE	320U
#define BEAU_VM_CRASH_HISTORY_SLOTS	8U

#define BEAU_VM_CRASH_F_REGS_VALID	(1U << 0U)
#define BEAU_VM_CRASH_F_STACK_VALID	(1U << 1U)

enum beau_vm_crash_kind {
	BEAU_VM_CRASH_PANIC = 1U,
	BEAU_VM_CRASH_OOPS,
};

struct beau_vm_crash_report {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t kind;
	uint32_t cpu_id;
	uint64_t sequence;
	uint64_t pc;
	uint64_t fault_address;
	uint64_t error_code;
	char message[BEAU_VM_CRASH_TEXT_MAX];
	uint64_t sp;
	uint64_t pstate;
	uint32_t pid;
	uint32_t tgid;
	uint16_t stack_count;
	uint16_t flags;
	char comm[BEAU_VM_CRASH_COMM_MAX];
	uint64_t stack[BEAU_VM_CRASH_STACK_MAX];
};

struct vm_crash_record {
	uint32_t checksum;
	bool valid;
	uint16_t vm_id;
	uint64_t lifecycle_generation;
	uint64_t captured_tsc;
	uint64_t capture_sequence;
	struct beau_vm_crash_report report;
};

_Static_assert(offsetof(struct beau_vm_crash_report, message) == 48U,
	"BEAU VM crash ABI v1 prefix changed");
_Static_assert(sizeof(struct beau_vm_crash_report) == BEAU_VM_CRASH_REPORT_SIZE,
	"BEAU VM crash ABI layout changed");

int32_t hcall_vm_crash_report(struct acrn_vcpu *vcpu, struct acrn_vm *target_vm,
	uint64_t param1, uint64_t param2);
uint32_t vm_crash_copy_history(uint16_t vm_id, struct vm_crash_record *records,
	uint32_t records_capacity, bool *corrupt);
void vm_crash_erase(uint16_t vm_id);
void vm_crash_print(const struct vm_crash_record *record, uint32_t history_index,
	uint32_t history_count);

#endif /* VM_CRASH_H */
