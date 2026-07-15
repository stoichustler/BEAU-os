/*
 * ACRN TRACE
 *
 * Copyright (C) 2017-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Li Fei <fei1.li@intel.com>
 *
 */

#ifndef TRACE_H
#define TRACE_H
#include <types.h>

/*
 * Common trace event ID layout:
 * - 0x0001..0x0004: common timer queue and hardware IRQ events.
 * - 0x0010..0x0011: VM enter/exit boundaries.
 * - 0x0020: scheduler choice, currently emitted with the next thread name.
 * - 0x10000 + reason: VM-exit namespace. On x86 the low bits mirror VMX
 *   basic exit reasons, so trace decoders can subtract TRACE_VMEXIT_ENTRY
 *   and compare the result with the architecture manual.
 * - 0x20000: fallback event for a VM exit that reached the unhandled path.
 *
 * Payload helper contract:
 * - TRACE_2L stores two 64-bit values.
 * - TRACE_4I stores four 32-bit values.
 * - TRACE_16STR stores a short, NUL-terminated diagnostic string.
 */

/* TIMER EVENT */
#define TRACE_TIMER_ACTION_ADDED	0x1U
#define TRACE_TIMER_ACTION_PCKUP	0x2U
#define TRACE_TIMER_ACTION_UPDAT	0x3U
#define TRACE_TIMER_IRQ			0x4U

#define TRACE_VM_EXIT			0x10U
#define TRACE_VM_ENTER			0X11U

/* event to calculate cpu usage with shared pcpu */
#define TRACE_SCHED_NEXT		0x20U

#define TRACE_VMEXIT_ENTRY		0x10000U

#define TRACE_VMEXIT_EXCEPTION_OR_NMI	    (TRACE_VMEXIT_ENTRY + 0x00000000U)
#define TRACE_VMEXIT_EXTERNAL_INTERRUPT     (TRACE_VMEXIT_ENTRY + 0x00000001U)
#define TRACE_VMEXIT_INTERRUPT_WINDOW	    (TRACE_VMEXIT_ENTRY + 0x00000002U)
#define TRACE_VMEXIT_CPUID		    (TRACE_VMEXIT_ENTRY + 0x00000004U)
#define TRACE_VMEXIT_RDTSC		    (TRACE_VMEXIT_ENTRY + 0x00000010U)
#define TRACE_VMEXIT_VMCALL		    (TRACE_VMEXIT_ENTRY + 0x00000012U)
#define TRACE_VMEXIT_CR_ACCESS		    (TRACE_VMEXIT_ENTRY + 0x0000001CU)
#define TRACE_VMEXIT_IO_INSTRUCTION	    (TRACE_VMEXIT_ENTRY + 0x0000001EU)
#define TRACE_VMEXIT_RDMSR		    (TRACE_VMEXIT_ENTRY + 0x0000001FU)
#define TRACE_VMEXIT_WRMSR		    (TRACE_VMEXIT_ENTRY + 0x00000020U)
#define TRACE_VMEXIT_EPT_VIOLATION	    (TRACE_VMEXIT_ENTRY + 0x00000030U)
#define TRACE_VMEXIT_EPT_MISCONFIGURATION   (TRACE_VMEXIT_ENTRY + 0x00000031U)
#define TRACE_VMEXIT_RDTSCP		    (TRACE_VMEXIT_ENTRY + 0x00000033U)
#define TRACE_VMEXIT_APICV_WRITE	    (TRACE_VMEXIT_ENTRY + 0x00000038U)
#define TRACE_VMEXIT_APICV_ACCESS	    (TRACE_VMEXIT_ENTRY + 0x00000039U)
#define TRACE_VMEXIT_APICV_VIRT_EOI	    (TRACE_VMEXIT_ENTRY + 0x0000003AU)

#define TRACE_VMEXIT_UNHANDLED		0x20000U

#define TRACE_MASK_TIMER		(1UL << 0U)
#define TRACE_MASK_SCHED		(1UL << 1U)
#define TRACE_MASK_HCALL		(1UL << 2U)
#define TRACE_MASK_VM			(1UL << 3U)
#define TRACE_MASK_ALL			(TRACE_MASK_TIMER | TRACE_MASK_SCHED | \
					 TRACE_MASK_HCALL | TRACE_MASK_VM)

/* Fixed 32-byte record shared by all trace producers and shell decoders. */
struct trace_record {
	uint64_t tsc;
	uint64_t id:48;
	uint8_t n_data;
	uint8_t cpu;
	union {
		struct {
			uint32_t a, b, c, d;
		} fields_32;
		struct {
			uint64_t e, f;
		} fields_64;
		char str[16];
	} payload;
} __aligned(8);

struct trace_cpu_status {
	uint32_t count;
	uint64_t overwritten;
	bool writer_active;
};

void TRACE_2L(uint32_t evid, uint64_t e, uint64_t f);
void TRACE_4I(uint32_t evid, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void TRACE_16STR(uint32_t evid, const char name[]);

bool trace_is_running(void);
uint64_t trace_get_mask(void);
uint32_t trace_get_capacity(void);
int32_t trace_start(uint64_t event_mask);
int32_t trace_stop(void);
int32_t trace_clear(void);
void trace_get_cpu_status(uint16_t pcpu_id, struct trace_cpu_status *status);
bool trace_get_record(uint16_t pcpu_id, uint32_t index, struct trace_record *record);

#endif /* TRACE_H */
