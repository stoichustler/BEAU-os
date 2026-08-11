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
#include <common/ticks.h>
#include <logmsg.h>
#include <asm/cpu.h>
#include <cpu.h>
#include <delay.h>
#include <errno.h>

#define SMP_CALL_RETRY_DELAY_US		10U
#define SMP_CALL_DEFAULT_TIMEOUT_US	100000U

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

static bool smp_call_deadline_expired(uint64_t deadline)
{
	return (int64_t)(cpu_ticks() - deadline) >= 0L;
}

static int32_t smp_call_acquire(uint64_t deadline, bool try_acquire)
{
	if (try_acquire) {
		return atomic_cmpxchg64(&smp_call_owner, 0UL, 1UL) == 0UL ?
			0 : -EBUSY;
	}

	while (atomic_cmpxchg64(&smp_call_owner, 0UL, 1UL) != 0UL) {
		if (smp_call_deadline_expired(deadline)) {
			return -ETIMEDOUT;
		}
		udelay(SMP_CALL_RETRY_DELAY_US);
	}

	return 0;
}

static bool wait_smp_call_done(uint64_t deadline)
{
	while (smp_call_mask != 0UL) {
		if (smp_call_deadline_expired(deadline)) {
			return false;
		}
		udelay(SMP_CALL_RETRY_DELAY_US);
	}

	return true;
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
	uint64_t deadline;
	uint64_t generation;
	int32_t ret;
	bool completed;

	/*
	 * [20260811] SMP callback deadline and mailbox ownership
	 *
	 * contender -> acquire owner -> publish mailbox -> kick pCPU -> collect ACK
	 *     |              |                                      |
	 *     +--> deadline -+------------------------------> revoke generation
	 *
	 * Key rule:
	 *   - smp_call_owner serializes the shared mailbox and is acquired before
	 *     any generation, callback, data, or mask state is published;
	 *   - one deadline covers owner contention and remote completion, so a
	 *     caller cannot wait indefinitely behind a stalled request;
	 *   - timeout first revokes the active generation, then clears mailbox state
	 *     and releases the owner, preventing a stale IPI from authorizing a new
	 *     callback.
	 */
	if ((mask == 0UL) || (func == NULL) || (timeout_us == 0U)) {
		return -EINVAL;
	}
	deadline = cpu_ticks() + us_to_ticks(timeout_us);
	ret = smp_call_acquire(deadline, try_acquire);
	if ((ret == 0) && smp_call_deadline_expired(deadline)) {
		__atomic_store_n(&smp_call_owner, 0UL, __ATOMIC_RELEASE);
		ret = -ETIMEDOUT;
	}
	if (ret != 0) {
		if (ret == -ETIMEDOUT) {
			LOG_ERR("SMP: owner timeout mask:0x%lx budget:%uus", mask, timeout_us);
		}
		return ret;
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
	completed = wait_smp_call_done(deadline);
	__atomic_store_n(&smp_call_active_generation, 0UL, __ATOMIC_RELEASE);
	clear_smp_call_info(orig_mask);
	if (!completed) {
		__atomic_store_n(&smp_call_mask, 0UL, __ATOMIC_RELEASE);
		LOG_ERR("SMP: callback timeout mask:0x%lx budget:%uus", orig_mask,
			timeout_us);
	}
	__atomic_store_n(&smp_call_owner, 0UL, __ATOMIC_RELEASE);

	return completed ? 0 : -ETIMEDOUT;
}

/**
 * Trigger the SMP call request to target pCPUs
 */
void smp_call_function(uint64_t mask, smp_call_func_t func, void *data)
{
	int32_t ret = smp_call_function_common(mask, func, data,
		SMP_CALL_DEFAULT_TIMEOUT_US, false);

	if (ret != 0) {
		LOG_ERR("SMP: legacy call failed mask:0x%lx ret:%d", mask, ret);
	}
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
