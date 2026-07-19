/*
 * Copyright (C) 2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/cpu.h>
#include <atomic.h>
#include <bits.h>
#include <per_cpu.h>
#include <asm/notify.h>
#include <common/notify.h>
#include <logmsg.h>
#include <asm/cpu.h>
#include <cpu.h>
#include <delay.h>
#include <errno.h>

static volatile uint64_t smp_call_mask = 0UL;
static volatile uint64_t smp_call_owner;
static uint64_t smp_call_next_generation;
static uint64_t smp_call_active_generation;

/**
 * Run in interrupt context.
 *
 * Two use cases are covered:
 *  - SMP call: when the corresponding bit in smp_call_mask is set and
 *              smp_call_info in the per-CPU region is specified.
 *              The registered callback will be invoked.
 *
 *  - Kick pCPU out of non-root mode: when the corresponding bit in smp_call_mask is clear and
 *                                    smp_call_info in the per-CPU region is not specified.
 *                                    No callback is invoked.
 */
void kick_notification(__unused uint32_t irq, __unused void *data)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (bitmap_test(pcpu_id, &smp_call_mask)) {
		struct smp_call_info_data *smp_call = &per_cpu(smp_call_info, pcpu_id);
		uint64_t generation = __atomic_load_n(&smp_call->generation, __ATOMIC_ACQUIRE);
		smp_call_func_t func = __atomic_load_n(&smp_call->func, __ATOMIC_ACQUIRE);
		void *call_data = __atomic_load_n(&smp_call->data, __ATOMIC_ACQUIRE);

		if ((generation != 0UL) &&
			(generation == __atomic_load_n(&smp_call_active_generation,
				__ATOMIC_ACQUIRE)) && (func != NULL)) {
			func(call_data);
		}
		if ((generation != 0UL) &&
			(generation == __atomic_load_n(&smp_call_active_generation,
				__ATOMIC_ACQUIRE)) &&
			(generation == __atomic_load_n(&smp_call->generation,
				__ATOMIC_ACQUIRE))) {
			bitmap_clear(pcpu_id, &smp_call_mask);
		}
	}
}

/**
 * Execute the SMP call notification handler
 */
void handle_smp_call(void)
{
	kick_notification(0U, NULL);
}

static bool wait_smp_call_done(uint32_t timeout_us)
{
	bool completed = true;

	if (timeout_us == 0U) {
		wait_sync_change(&smp_call_mask, 0UL);
	} else {
		while ((smp_call_mask != 0UL) && (timeout_us != 0U)) {
			udelay(10U);
			timeout_us = (timeout_us > 10U) ? (timeout_us - 10U) : 0U;
		}
		completed = (smp_call_mask == 0UL);
	}

	return completed;
}

static void clear_smp_call_info(uint64_t mask)
{
	uint16_t pcpu_id = ffs64(mask);

	while (pcpu_id < MAX_PCPU_NUM) {
		struct smp_call_info_data *smp_call = &per_cpu(smp_call_info, pcpu_id);

		__atomic_store_n(&smp_call->generation, 0UL, __ATOMIC_RELEASE);
		__atomic_store_n(&smp_call->func, NULL, __ATOMIC_RELEASE);
		__atomic_store_n(&smp_call->data, NULL, __ATOMIC_RELEASE);
		bitmap_clear_non_atomic(pcpu_id, &mask);
		pcpu_id = ffs64(mask);
	}
}

static int32_t smp_call_function_common(uint64_t mask, smp_call_func_t func,
	void *data, uint32_t timeout_us, bool try_acquire)
{
	uint16_t pcpu_id;
	struct smp_call_info_data *smp_call;
	uint64_t orig_mask = mask;
	uint64_t generation;
	bool completed;

	/*
	 * Fault capture cannot wait behind an unrelated SMP call indefinitely.
	 * Its try path either owns the global slot immediately or falls back to
	 * durable state; legacy callers retain their blocking acquisition contract.
	 */
	if (try_acquire) {
		if (atomic_cmpxchg64(&smp_call_owner, 0UL, 1UL) != 0UL) {
			return -EBUSY;
		}
	} else {
		/* wait for previous smp call complete, which may run on other cpus */
		while (atomic_cmpxchg64(&smp_call_owner, 0UL, 1UL) != 0UL);
	}
	generation = __atomic_add_fetch(&smp_call_next_generation, 1UL,
		__ATOMIC_SEQ_CST);
	if (generation == 0UL) {
		generation = __atomic_add_fetch(&smp_call_next_generation, 1UL,
			__ATOMIC_SEQ_CST);
	}
	__atomic_store_n(&smp_call_active_generation, generation, __ATOMIC_RELEASE);
	__atomic_store_n(&smp_call_mask, mask, __ATOMIC_RELEASE);

	pcpu_id = ffs64(mask);
	while (pcpu_id < MAX_PCPU_NUM) {
		bitmap_clear_non_atomic(pcpu_id, &mask);
		if (pcpu_id == get_pcpu_id()) {
			func(data);
			bitmap_clear_non_atomic(pcpu_id, &smp_call_mask);
		} else if (is_pcpu_active(pcpu_id)) {
			smp_call = &per_cpu(smp_call_info, pcpu_id);

			__atomic_store_n(&smp_call->func, func, __ATOMIC_RELEASE);
			__atomic_store_n(&smp_call->data, data, __ATOMIC_RELEASE);
			__atomic_store_n(&smp_call->generation, generation,
				__ATOMIC_RELEASE);

			/**
			 * arch_smp_call_kick_pcpu() is abstracted because:
			 *  - On x86, special handling is required when LAPIC is passthrough.
			 *  - On RISC-V, a plain IPI is sufficient to kick the target pCPU.
			 */
			arch_smp_call_kick_pcpu(pcpu_id);
		} else {
			/* pcpu is not in active, print error */
			//LOG_ERR("pcpu_id %d not in active!", pcpu_id);
			bitmap_clear_non_atomic(pcpu_id, &smp_call_mask);
		}
		pcpu_id = ffs64(mask);
	}
	/* wait for current smp call complete */
	completed = wait_smp_call_done(timeout_us);
	__atomic_store_n(&smp_call_active_generation, 0UL, __ATOMIC_RELEASE);
	clear_smp_call_info(orig_mask);
	if (!completed) {
		__atomic_store_n(&smp_call_mask, 0UL, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&smp_call_owner, 0UL, __ATOMIC_RELEASE);

	return completed ? 0 : -ETIMEDOUT;
}

/**
 * Trigger the SMP call request to target pCPUs
 */
void smp_call_function(uint64_t mask, smp_call_func_t func, void *data)
{
	(void)smp_call_function_common(mask, func, data, 0U, false);
}

bool smp_call_function_timeout(uint64_t mask, smp_call_func_t func, void *data, uint32_t timeout_us)
{
	return smp_call_function_common(mask, func, data, timeout_us, false) == 0;
}

int32_t smp_try_call_function_timeout(uint64_t mask, smp_call_func_t func,
	void *data, uint32_t timeout_us)
{
	return smp_call_function_common(mask, func, data, timeout_us, true);
}

/**
 * Initialize the SMP call support during pCPU initialization
 */
void init_smp_call(void)
{
	/**
	 * arch_init_smp_call() is abstracted because:
	 *  - On x86, during CPU initialization, software reserves dedicated vectors and registers callback handlers
	 *    for purposes such as notifications or posted interrupts.
	 *  - On RISC-V, no special handling is required at present; this can be extended in the future if needed.
	 */
	arch_init_smp_call();
}
