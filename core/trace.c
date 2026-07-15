/*
 * Copyright (C) 2018-2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <rtl.h>
#include <ticks.h>
#include <trace.h>

#ifdef CONFIG_ACRNTRACE_ENABLED

#ifndef CONFIG_TRACE_RECORDS_PER_CPU
#define CONFIG_TRACE_RECORDS_PER_CPU	256U
#endif

#define TRACE_STOP_TIMEOUT_US		1000U

struct trace_cpu_ring {
	struct trace_record records[CONFIG_TRACE_RECORDS_PER_CPU];
	uint32_t head;
	uint32_t count;
	uint64_t overwritten;
	volatile bool writer_active;
};

static struct trace_cpu_ring trace_rings[MAX_PCPU_NUM];
static volatile bool trace_running;
static uint64_t trace_event_mask;

_Static_assert(sizeof(struct trace_record) == 32U, "trace record ABI must stay 32 bytes");

/*
 * Runtime trace data flow:
 *
 *   TRACE_* caller
 *        |
 *        v
 *   stopped/mask fast-path check
 *        |
 *        v
 *   local IRQ-off publish window
 *        |
 *        v
 *   current pCPU fixed ring
 *        |
 *        v
 *   trace stop -> shell chronological merge
 *
 * A ring has one physical producer. Local IRQ masking prevents a timer or HVC
 * trace from nesting another write on the same pCPU. The shell reads only after
 * capture stops, so the hot path needs no global lock or shared Service VM
 * buffer. Full rings retain the newest records and account overwritten history.
 */
static uint64_t trace_event_category(uint32_t evid)
{
	uint64_t category = 0UL;

	switch (evid) {
	case TRACE_TIMER_ACTION_ADDED:
	case TRACE_TIMER_ACTION_PCKUP:
	case TRACE_TIMER_ACTION_UPDAT:
	case TRACE_TIMER_IRQ:
		category = TRACE_MASK_TIMER;
		break;
	case TRACE_SCHED_NEXT:
		category = TRACE_MASK_SCHED;
		break;
	case TRACE_VM_ENTER:
	case TRACE_VM_EXIT:
		category = TRACE_MASK_VM;
		break;
	case TRACE_VMEXIT_VMCALL:
		category = TRACE_MASK_HCALL;
		break;
	default:
		break;
	}

	return category;
}

static bool trace_event_enabled(uint32_t evid)
{
	uint64_t category;

	if (!trace_running) {
		return false;
	}

	category = trace_event_category(evid);
	return (category != 0UL) && ((trace_event_mask & category) != 0UL);
}

static void trace_put(uint32_t evid, uint32_t n_data, struct trace_record *record)
{
	struct trace_cpu_ring *ring;
	uint64_t rflags;
	uint16_t pcpu_id;
	uint32_t next;

	if (!trace_event_enabled(evid)) {
		return;
	}

	pcpu_id = get_pcpu_id();
	if (pcpu_id >= get_pcpu_nums()) {
		return;
	}

	local_irq_save(&rflags);
	ring = &trace_rings[pcpu_id];
	ring->writer_active = true;
	cpu_write_memory_barrier();
	if (!trace_event_enabled(evid)) {
		ring->writer_active = false;
		local_irq_restore(rflags);
		return;
	}

	record->tsc = cpu_ticks();
	record->id = evid;
	record->n_data = (uint8_t)n_data;
	record->cpu = (uint8_t)pcpu_id;
	ring->records[ring->head] = *record;
	next = ring->head + 1U;
	if (next >= CONFIG_TRACE_RECORDS_PER_CPU) {
		next = 0U;
	}

	/* Publish the complete record before exposing the new ring position. */
	cpu_write_memory_barrier();
	ring->head = next;
	if (ring->count < CONFIG_TRACE_RECORDS_PER_CPU) {
		ring->count++;
	} else {
		ring->overwritten++;
	}
	cpu_write_memory_barrier();
	ring->writer_active = false;
	local_irq_restore(rflags);
}

static int32_t trace_wait_writers(void)
{
	uint64_t deadline = cpu_ticks() + us_to_ticks(TRACE_STOP_TIMEOUT_US);
	uint16_t pcpu_id;

	do {
		bool active = false;

		cpu_read_memory_barrier();
		for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
			if (trace_rings[pcpu_id].writer_active) {
				active = true;
				break;
			}
		}
		if (!active) {
			return 0;
		}
		cpu_relax();
	} while (cpu_ticks() < deadline);

	return -ETIMEDOUT;
}

bool trace_is_running(void)
{
	return trace_running;
}

uint64_t trace_get_mask(void)
{
	return trace_event_mask;
}

uint32_t trace_get_capacity(void)
{
	return CONFIG_TRACE_RECORDS_PER_CPU;
}

int32_t trace_clear(void)
{
	int32_t ret;

	if (trace_running) {
		return -EBUSY;
	}

	ret = trace_wait_writers();
	if (ret == 0) {
		(void)memset(trace_rings, 0U, sizeof(trace_rings));
	}

	return ret;
}

int32_t trace_start(uint64_t event_mask)
{
	int32_t ret;

	if ((event_mask == 0UL) || ((event_mask & ~TRACE_MASK_ALL) != 0UL)) {
		return -EINVAL;
	}
	if (trace_running) {
		return -EBUSY;
	}

	ret = trace_clear();
	if (ret == 0) {
		trace_event_mask = event_mask;
		cpu_write_memory_barrier();
		trace_running = true;
	}

	return ret;
}

int32_t trace_stop(void)
{
	trace_running = false;
	cpu_memory_barrier();

	return trace_wait_writers();
}

void trace_get_cpu_status(uint16_t pcpu_id, struct trace_cpu_status *status)
{
	if (status == NULL) {
		return;
	}

	(void)memset(status, 0U, sizeof(*status));
	if (pcpu_id < get_pcpu_nums()) {
		status->count = trace_rings[pcpu_id].count;
		status->overwritten = trace_rings[pcpu_id].overwritten;
		status->writer_active = trace_rings[pcpu_id].writer_active;
	}
}

bool trace_get_record(uint16_t pcpu_id, uint32_t index, struct trace_record *record)
{
	const struct trace_cpu_ring *ring;
	uint32_t oldest;
	uint32_t slot;

	if (trace_running || (record == NULL) || (pcpu_id >= get_pcpu_nums())) {
		return false;
	}

	ring = &trace_rings[pcpu_id];
	if (index >= ring->count) {
		return false;
	}

	oldest = (ring->count == CONFIG_TRACE_RECORDS_PER_CPU) ? ring->head : 0U;
	slot = oldest + index;
	if (slot >= CONFIG_TRACE_RECORDS_PER_CPU) {
		slot -= CONFIG_TRACE_RECORDS_PER_CPU;
	}
	cpu_read_memory_barrier();
	*record = ring->records[slot];

	return true;
}

void TRACE_2L(uint32_t evid, uint64_t e, uint64_t f)
{
	struct trace_record record;

	if (!trace_event_enabled(evid)) {
		return;
	}
	record.payload.fields_64.e = e;
	record.payload.fields_64.f = f;
	trace_put(evid, 2U, &record);
}

void TRACE_4I(uint32_t evid, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	struct trace_record record;

	if (!trace_event_enabled(evid)) {
		return;
	}
	record.payload.fields_32.a = a;
	record.payload.fields_32.b = b;
	record.payload.fields_32.c = c;
	record.payload.fields_32.d = d;
	trace_put(evid, 4U, &record);
}

void TRACE_16STR(uint32_t evid, const char name[])
{
	struct trace_record record;
	size_t len;

	if (!trace_event_enabled(evid) || (name == NULL)) {
		return;
	}

	(void)memset(record.payload.str, 0U, sizeof(record.payload.str));
	len = strnlen_s(name, sizeof(record.payload.str) - 1U);
	if (len != 0U) {
		(void)memcpy_s(record.payload.str, sizeof(record.payload.str), name, len);
	}
	trace_put(evid, 16U, &record);
}

#else

bool trace_is_running(void)
{
	return false;
}

uint64_t trace_get_mask(void)
{
	return 0UL;
}

uint32_t trace_get_capacity(void)
{
	return 0U;
}

int32_t trace_start(__unused uint64_t event_mask)
{
	return -ENODEV;
}

int32_t trace_stop(void)
{
	return -ENODEV;
}

int32_t trace_clear(void)
{
	return -ENODEV;
}

void trace_get_cpu_status(__unused uint16_t pcpu_id, struct trace_cpu_status *status)
{
	if (status != NULL) {
		(void)memset(status, 0U, sizeof(*status));
	}
}

bool trace_get_record(__unused uint16_t pcpu_id, __unused uint32_t index,
	__unused struct trace_record *record)
{
	return false;
}

void TRACE_2L(__unused uint32_t evid, __unused uint64_t e, __unused uint64_t f) {}

void TRACE_4I(__unused uint32_t evid, __unused uint32_t a, __unused uint32_t b,
	__unused uint32_t c, __unused uint32_t d)
{
}

void TRACE_16STR(__unused uint32_t evid, __unused const char name[]) {}

#endif
