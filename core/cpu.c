/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <per_cpu.h>
#include <cpu.h>
#include <delay.h>
#include <logmsg.h>
#include <bits.h>
#include <schedule.h>
#include <version.h>

static volatile uint64_t pcpu_active_bitmap = 0UL;

uint16_t get_pcpu_nums(void)
{
	return arch_get_pcpu_num();
}

bool is_pcpu_active(uint16_t pcpu_id)
{
	return bitmap_test(pcpu_id, &pcpu_active_bitmap);
}

void set_pcpu_active(uint16_t pcpu_id)
{
	bitmap_set(pcpu_id, &pcpu_active_bitmap);
}

void clear_pcpu_active(uint16_t pcpu_id)
{

	bitmap_clear(pcpu_id, &pcpu_active_bitmap);
}

bool check_pcpus_active(uint64_t mask)
{
	return ((pcpu_active_bitmap & mask) == mask);
}

bool check_pcpus_inactive(uint64_t mask)
{
	return ((pcpu_active_bitmap & mask) != 0UL);
}

static bool check_pcpus_running(uint64_t mask)
{
	uint16_t i = ffs64(mask);

	while (i != INVALID_BIT_INDEX) {
		bitmap_clear_non_atomic(i, &mask);
		if (per_cpu(boot_state, i) != PCPU_STATE_RUNNING) {
			return false;
		}
		i = ffs64(mask);
	}

	return true;
}

uint64_t get_active_pcpu_bitmap(void)
{
	return pcpu_active_bitmap;
}

void pcpu_set_current_state(uint16_t pcpu_id, enum pcpu_boot_state state)
{
	if (state == PCPU_STATE_INITIALIZING) {
		set_current_pcpu_id(pcpu_id);
	}

	per_cpu(boot_state, pcpu_id) = state;
}

bool start_pcpus(uint64_t mask)
{
	uint16_t i;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t expected_start_mask = mask;
	uint32_t timeout;

	i = ffs64(expected_start_mask);
	while (i != INVALID_BIT_INDEX) {
		bitmap_clear_non_atomic(i, &expected_start_mask);

		if (pcpu_id == i) {
			continue; /* Avoid start itself */
		}

		arch_start_pcpu(i);

		/* Wait until the pcpu with pcpu_id is running and set
		 * the active bitmap or configured time-out has expired
		 */
		timeout = CPU_UP_TIMEOUT * 1000U;
		while (!is_pcpu_active(i) && (timeout != 0U)) {
			/* Delay 10us */
			udelay(10U);

			/* Decrement timeout value */
			timeout -= 10U;
		}

		/* Check to see if expected CPU is actually up */
		if (!is_pcpu_active(i)) {
			LOG_FTL("MP:     cpu%hu up failed", i);
			pcpu_set_current_state(i, PCPU_STATE_DEAD);
		}

		i = ffs64(expected_start_mask);
	}

	return check_pcpus_active(mask);
}

bool wait_pcpus_running(uint64_t mask)
{
	uint16_t i = ffs64(mask);
	uint32_t timeout;

	while (i != INVALID_BIT_INDEX) {
		bitmap_clear_non_atomic(i, &mask);

		timeout = CPU_UP_TIMEOUT * 1000U;
		while ((per_cpu(boot_state, i) != PCPU_STATE_RUNNING) && (timeout != 0U)) {
			udelay(10U);
			timeout -= 10U;
		}

		if (per_cpu(boot_state, i) != PCPU_STATE_RUNNING) {
			LOG_FTL("MP:     cpu%hu run failed", i);
			pcpu_set_current_state(i, PCPU_STATE_DEAD);
		}

		i = ffs64(mask);
	}

	return check_pcpus_running(mask);
}

void cpu_dead(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	deinit_sched(pcpu_id);
	arch_cpu_dead();
}

void beau_tag(void)
{
	LOG_INF("Launching BEAU HYPERVISOR v%s [%s] %s (%s)", BEAU_OS_VERSION,
		HV_BUILD_TYPE, HV_COMMIT_TIME, HV_COMMIT_DIRTY);
}
