/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <logmsg.h>
#include <memory.h>
#include <spinlock.h>
#include <cpu.h>
#include <per_cpu.h>
#include <schedule.h>
#include <ticks.h>
#include <timer.h>
#include <acrn_common.h>
#include <vconfig.h>
#include <vcpu.h>
#include <vm.h>
#include <asm/trusty.h>

#define TRUSTY_SMC_FC_GET_VERSION_STR	0xbc00000aUL
#define TRUSTY_SMC_FC_API_VERSION	0xbc00000bUL
#define TRUSTY_SMC_FC_GET_SMP_MAX_CPUS	0xbc00000dUL
#define TRUSTY_SMC_FC_BEAU_HEARTBEAT	0xbc00000eUL
#define TRUSTY_SMC_SC_RESTART_LAST	0x3c000000UL
#define TRUSTY_SMC_SC_RESTART_FIQ	0x3c000002UL
#define TRUSTY_SMC_SC_NOP		0x3c000003UL
#define TRUSTY_SMC_BEAU_HEARTBEAT_ACK	0x42454155UL
#define TRUSTY_SMC_API_VERSION_SMP	2UL
#define TRUSTY_API_VERSION_MIN		1UL
#define TRUSTY_API_VERSION_MAX		5UL
#define TRUSTY_VERSION_LENGTH_INDEX	0xffffffffUL
#define TRUSTY_VERSION_MAX_LEN		128U
#define TRUSTY_HEARTBEAT_PERIOD_MS	10000U

static uint64_t trusty_api_version_cache;
static spinlock_t trusty_smc_lock = { .head = 0U, .tail = 0U };

struct trusty_guest_transaction {
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint16_t pcpu_id;
	uint64_t function_id;
	uint16_t api_owner_vm_id;
	bool api_owner_valid;
	bool valid;
};

static struct trusty_guest_transaction trusty_guest_transaction;

#if defined(CONFIG_PLATFORM_QEMU)
static struct thread_object trusty_heartbeat_thread;
static uint8_t trusty_heartbeat_stack[CONFIG_STACK_SIZE] __aligned(16);
static struct hv_timer trusty_heartbeat_timer;
static uint64_t trusty_heartbeat_sequence;
static bool trusty_heartbeat_started;
#endif

/* [20260727] Restricted Trusty API-version forwarding
 *
 * VM1 guest x0/x1             BEAU EL2                  TF-A / Trusty
 *       |                        |                            |
 *       +-- exact API request -->+-- client/version gate ------>+
 *                                |                            |
 *                                +-- reject ------------------> SMC_UNK
 *
 * Key rule:
 *   - BEAU owns guest policy; TF-A owns secure-world state;
 *   - only a Trusty-client VM may request API versions 1 through 5;
 *   - x2 through x7 are zeroed, so no guest pointer or unapproved argument
 *     crosses into secure world; every validation or TF-A failure is SMC_UNK.
 */
static uint64_t trusty_raw_smc(uint64_t function_id, uint64_t x1,
	uint64_t x2, uint64_t x3, uint64_t vm_id)
{
	register uint64_t x0 asm("x0") = function_id;
	register uint64_t reg_x1 asm("x1") = x1;
	register uint64_t reg_x2 asm("x2") = x2;
	register uint64_t reg_x3 asm("x3") = x3;
	register uint64_t x4 asm("x4") = 0UL;
	register uint64_t x5 asm("x5") = 0UL;
	register uint64_t x6 asm("x6") = 0UL;
	register uint64_t x7 asm("x7") = vm_id;

	asm volatile ("smc #0"
		: "+r" (x0), "+r" (reg_x1), "+r" (reg_x2), "+r" (reg_x3), "+r" (x4),
		  "+r" (x5), "+r" (x6), "+r" (x7)
		:
		: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		  "x16", "x17", "memory");

	return x0;
}

/* [20260813] Serialized Trusty world entry
 *
 * BEAU caller -> Trusty SMC lock -> TF-A SPD -> Trusty LK
 *                         |                         |
 *                         +--> one VMID at a time    +--> return x0
 *
 * Key rule:
 *   - BEAU owns the lock and supplies the VMID; the guest never supplies x7;
 *   - only x0 through x3 and x7 cross the EL2/EL3 boundary, with x4 through
 *     x6 cleared on every entry;
 *   - serialization matches the SPD's single current-VM transaction and
 *     prevents one guest from interleaving another guest's secure call.
 */
static uint64_t trusty_call(uint64_t function_id, uint64_t x1, uint64_t x2,
	uint64_t x3, uint64_t vm_id)
{
	uint64_t result;

	spinlock_obtain(&trusty_smc_lock);
	result = trusty_guest_transaction.valid ? ARM64_TRUSTY_SMC_UNK :
		trusty_raw_smc(function_id, x1, x2, x3, vm_id);
	spinlock_release(&trusty_smc_lock);

	return result;
}

#if defined(CONFIG_PLATFORM_QEMU)
/* [20260810] BSP-owned Trusty heartbeat
 *
 * BSP periodic timer -> wake Trusty worker -> private fast SMC -> Trusty LK
 *          |                                                       |
 *          +-- no secure call in softirq                           +-- ACK
 *
 * Key rule:
 *   - the BSP owns the timer, worker, and monotonically increasing sequence;
 *   - the timer only wakes the worker, while the worker performs the secure
 *     transition with x2 through x7 cleared;
 *   - an unknown or malformed reply is reported and dropped, so a secure
 *     service failure cannot block timer dispatch, scheduling, or VM launch.
 */
static void trusty_heartbeat_timer_callback(__unused void *data)
{
	wake_thread(&trusty_heartbeat_thread);
}

static uint32_t trusty_next_heartbeat_sequence(void)
{
	if (trusty_heartbeat_sequence == UINT64_MAX) {
		trusty_heartbeat_sequence = 1U;
	} else {
		trusty_heartbeat_sequence++;
	}

	return trusty_heartbeat_sequence;
}

static void trusty_heartbeat_thread_main(__unused struct thread_object *obj)
{
	uint64_t reply;
	uint64_t sequence;

	while (true) {
		sleep_thread(&trusty_heartbeat_thread);
		schedule();

		sequence = trusty_next_heartbeat_sequence();
		reply = trusty_call(TRUSTY_SMC_FC_BEAU_HEARTBEAT,
			(uint64_t)sequence, 0UL, 0UL, 0UL);
        (void)daemon_log(LOG_INFO, "TEE: ack:  %s count:%016lu",
                reply == TRUSTY_SMC_BEAU_HEARTBEAT_ACK ? "LIVE" : "DEAD",
                sequence);
	}
}
#endif /* CONFIG_PLATFORM_QEMU */

void arm64_trusty_heartbeat_start(void)
{
#if defined(CONFIG_PLATFORM_QEMU)
	struct sched_params params = {0U};
	uint64_t period_ticks = us_to_ticks(TRUSTY_HEARTBEAT_PERIOD_MS * 1000U);
	uint64_t now;

	if (trusty_heartbeat_started) {
		return;
	}
	if (period_ticks == 0UL) {
		LOG_ERR("TEE:    Trusty heartbeat has no timer period");
		return;
	}

	now = cpu_ticks();
	if (now > (UINT64_MAX - period_ticks)) {
		LOG_ERR("TEE:    Trusty heartbeat timer overflow");
		return;
	}

	(void)strncpy_s(trusty_heartbeat_thread.name,
		sizeof(trusty_heartbeat_thread.name), "TEE-core",
		sizeof(trusty_heartbeat_thread.name));
	trusty_heartbeat_thread.pcpu_id = BSP_CPU_ID;
	trusty_heartbeat_thread.sched_ctl = &per_cpu(sched_ctl, BSP_CPU_ID);
	trusty_heartbeat_thread.thread_entry = trusty_heartbeat_thread_main;
	trusty_heartbeat_thread.switch_out = NULL;
	trusty_heartbeat_thread.switch_in = NULL;
	trusty_heartbeat_thread.host_sp = arch_setup_thread_stack(
		&trusty_heartbeat_thread, trusty_heartbeat_stack, CONFIG_STACK_SIZE);
	if (trusty_heartbeat_thread.host_sp == 0UL) {
		LOG_ERR("TEE:    cannot prepare Trusty heartbeat worker");
		return;
	}

	params.prio = PRIO_LOW;
	init_thread_data(&trusty_heartbeat_thread, &params);
	initialize_timer(&trusty_heartbeat_timer, trusty_heartbeat_timer_callback,
		NULL, now + period_ticks, period_ticks);
	if (add_timer(&trusty_heartbeat_timer) != 0) {
		LOG_ERR("TEE:    cannot start Trusty heartbeat timer");
		return;
	}

	wake_thread(&trusty_heartbeat_thread);
	trusty_heartbeat_started = true;
#endif /* CONFIG_PLATFORM_QEMU */
}

/* [20260728] Read-only Trusty version query
 *
 * BEAU shell -> bounded SMC index -> TF-A / Trusty LK character
 *                                      |
 *                                      +--> invalid reply: clear buffer
 *                                      |
 *                                      v
 *                              printable version output
 *
 * Key rule:
 *   - BEAU owns the caller buffer and Trusty owns the immutable build string;
 *   - the length is validated before each fixed-index fastcall and before the
 *     NUL terminator is published;
 *   - no pointer, shared buffer, or API-version negotiation crosses the
 *     secure boundary, preventing a diagnostic query from changing Trusty
 *     state or exposing arbitrary secure-world data.
 */
int32_t arm64_trusty_get_version(char *version, size_t version_size)
{
	if ((version == NULL) || (version_size < 2U) ||
		(version_size > (TRUSTY_VERSION_MAX_LEN + 1U))) {
		return -EINVAL;
	}
	(void)memset(version, 0U, version_size);

#if defined(CONFIG_PLATFORM_QEMU)
	{
		uint64_t version_length;
		uint64_t value;
		size_t index;
		int32_t status = -EIO;

		version_length = trusty_call(TRUSTY_SMC_FC_GET_VERSION_STR,
			TRUSTY_VERSION_LENGTH_INDEX, 0UL, 0UL, 0UL);
		if ((version_length == ARM64_TRUSTY_SMC_UNK) || (version_length == 0UL) ||
			(version_length > TRUSTY_VERSION_MAX_LEN) ||
			(version_length >= version_size)) {
			return status;
		}

		for (index = 0U; index < (size_t)version_length; index++) {
			value = trusty_call(TRUSTY_SMC_FC_GET_VERSION_STR, index,
				0UL, 0UL, 0UL);
			if ((value == ARM64_TRUSTY_SMC_UNK) || (value < 0x20UL) ||
				(value > 0x7eUL)) {
				(void)memset(version, 0U, version_size);
				return status;
			}
			version[index] = (char)value;
		}

		return 0;
	}
#else /* CONFIG_PLATFORM_QEMU */
	return -ENOTSUP;
#endif
}

/* [20260728] Trusty system-info snapshot
 *
 * VM1 API-version success -> release-store ABI cache
 *                                        |
 * shell dump -> fixed CPU SMC -> acquire-load cache -> complete snapshot
 *                    |
 *                    +--> invalid reply: no snapshot output
 *
 * Key rule:
 *   - Trusty owns the reported CPU limit while BEAU owns the ABI cache;
 *   - the VM1 policy validation completes before it publishes the cached ABI
 *     version, and the shell acquires that value only after its CPU query;
 *   - dump never invokes the stateful API-version SMC, preventing a diagnostic
 *     command from selecting or downgrading Trusty's global ABI state.
 */
int32_t arm64_trusty_get_system_info(struct arm64_trusty_system_info *info)
{
	if (info == NULL) {
		return -EINVAL;
	}
	(void)memset(info, 0U, sizeof(*info));

#if defined(CONFIG_PLATFORM_QEMU)
	{
		uint64_t max_cpus = trusty_call(TRUSTY_SMC_FC_GET_SMP_MAX_CPUS,
			0UL, 0UL, 0UL, 0UL);
		uint64_t api_version;

		if ((max_cpus == ARM64_TRUSTY_SMC_UNK) || (max_cpus == 0UL) ||
			(max_cpus > UINT32_MAX)) {
			return -EIO;
		}

		api_version = __atomic_load_n(&trusty_api_version_cache,
			__ATOMIC_ACQUIRE);
		info->smp_max_cpus = (uint32_t)max_cpus;
		if ((api_version >= TRUSTY_API_VERSION_MIN) &&
			(api_version <= TRUSTY_API_VERSION_MAX)) {
			info->api_version = (uint32_t)api_version;
			info->api_version_valid = true;
		}

		return 0;
	}
#else
	return -ENOTSUP;
#endif
}

static bool trusty_guest_is_client(const struct acrn_vcpu *vcpu)
{
	const struct acrn_vm_config *vm_config;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return false;
	}

	vm_config = get_vm_config(vcpu->vm->vm_id);
	return (vm_config != NULL) &&
		((vm_config->guest_flags & GUEST_FLAG_TRUSTY_CLIENT) != 0UL);
}

static bool trusty_guest_args_are_zero(const struct acrn_vcpu *vcpu)
{
	const struct cpu_regs *regs = &vcpu->arch.regs;

	return (regs->x1 == 0UL) && (regs->x2 == 0UL) &&
		(regs->x3 == 0UL) && (regs->x4 == 0UL) && (regs->x5 == 0UL) &&
		(regs->x6 == 0UL) && (regs->x7 == 0UL);
}

static bool trusty_guest_has_continuation(const struct acrn_vcpu *vcpu,
	uint16_t pcpu_id, uint64_t function_id)
{
	return trusty_guest_transaction.valid &&
		(trusty_guest_transaction.vm_id == vcpu->vm->vm_id) &&
		(trusty_guest_transaction.vcpu_id == vcpu->vcpu_id) &&
		(trusty_guest_transaction.pcpu_id == pcpu_id) &&
		(trusty_guest_transaction.function_id == function_id);
}

static bool trusty_guest_result_needs_continuation(uint64_t result)
{
	return (result == (uint64_t)(int64_t)-3L) ||
		(result == (uint64_t)(int64_t)-12L) ||
		(result == (uint64_t)(int64_t)-14L);
}

/* [20260813] Trusty-client SMC gate
 *
 * Trusty-client VM -> validate ABI/continuation -> serialized Trusty call
 *          |                    |                         |
 *          |                    +--> reject               +--> x0 result
 *          v
 * global transaction <- interrupted result
 *
 * Key rule:
 *   - the VM policy owns client admission; the global transaction records its
 *     VM/vCPU/pCPU owner until Trusty reports completion;
 *   - API selection precedes NOP/restart, and only the vCPU that received an
 *     interrupted result may resume on the same pCPU;
 *   - no guest pointer, x4-x6 data, or guest-selected x7 reaches Trusty,
 *     preventing cross-VM secure state access and unintended shared memory.
 */
uint64_t arm64_trusty_handle_guest_smc(struct acrn_vcpu *vcpu)
{
	uint64_t function_id;
	uint64_t result;
	uint16_t pcpu_id;

	if (!trusty_guest_is_client(vcpu)) {
		return ARM64_TRUSTY_SMC_UNK;
	}

	pcpu_id = get_pcpu_id();
	if (pcpu_id >= MAX_PCPU_NUM) {
		return ARM64_TRUSTY_SMC_UNK;
	}

	function_id = vcpu->arch.regs.x0;
	spinlock_obtain(&trusty_smc_lock);
	switch (function_id) {
	case TRUSTY_SMC_FC_API_VERSION:
		if ((vcpu->arch.regs.x1 < TRUSTY_API_VERSION_MIN) ||
			(vcpu->arch.regs.x1 > TRUSTY_API_VERSION_MAX) ||
			(vcpu->arch.regs.x2 != 0UL) || (vcpu->arch.regs.x3 != 0UL) ||
			(vcpu->arch.regs.x4 != 0UL) || (vcpu->arch.regs.x5 != 0UL) ||
			(vcpu->arch.regs.x6 != 0UL) || (vcpu->arch.regs.x7 != 0UL) ||
			trusty_guest_transaction.valid ||
			(trusty_guest_transaction.api_owner_valid &&
			(trusty_guest_transaction.api_owner_vm_id != vcpu->vm->vm_id))) {
			result = ARM64_TRUSTY_SMC_UNK;
			break;
		}
		result = trusty_raw_smc(function_id, vcpu->arch.regs.x1, 0UL, 0UL,
			(uint64_t)vcpu->vm->vm_id);
		if ((result < TRUSTY_API_VERSION_MIN) ||
			(result > vcpu->arch.regs.x1)) {
			result = ARM64_TRUSTY_SMC_UNK;
			break;
		}
		__atomic_store_n(&trusty_api_version_cache, result, __ATOMIC_RELEASE);
		trusty_guest_transaction.api_owner_vm_id = vcpu->vm->vm_id;
		trusty_guest_transaction.api_owner_valid = true;
		break;
	case TRUSTY_SMC_SC_NOP:
		if (!trusty_guest_args_are_zero(vcpu) ||
			(trusty_guest_transaction.valid &&
			!trusty_guest_has_continuation(vcpu, pcpu_id,
			TRUSTY_SMC_SC_NOP)) ||
			!trusty_guest_transaction.api_owner_valid ||
			(trusty_guest_transaction.api_owner_vm_id != vcpu->vm->vm_id) ||
			(__atomic_load_n(&trusty_api_version_cache, __ATOMIC_ACQUIRE) <
			TRUSTY_SMC_API_VERSION_SMP)) {
			result = ARM64_TRUSTY_SMC_UNK;
			break;
		}
		result = trusty_raw_smc(function_id, 0UL, 0UL, 0UL,
			(uint64_t)vcpu->vm->vm_id);
		break;
	case TRUSTY_SMC_SC_RESTART_LAST:
	case TRUSTY_SMC_SC_RESTART_FIQ:
		if (!trusty_guest_args_are_zero(vcpu) ||
			!trusty_guest_has_continuation(vcpu, pcpu_id, function_id)) {
			result = ARM64_TRUSTY_SMC_UNK;
			break;
		}
		result = trusty_raw_smc(function_id, 0UL, 0UL, 0UL,
			(uint64_t)vcpu->vm->vm_id);
		break;
	default:
		result = ARM64_TRUSTY_SMC_UNK;
		break;
	}

	if (trusty_guest_result_needs_continuation(result)) {
		trusty_guest_transaction.vm_id = vcpu->vm->vm_id;
		trusty_guest_transaction.vcpu_id = vcpu->vcpu_id;
		trusty_guest_transaction.pcpu_id = pcpu_id;
		if (result == (uint64_t)(int64_t)-12L) {
			trusty_guest_transaction.function_id = TRUSTY_SMC_SC_RESTART_FIQ;
		} else if (result == (uint64_t)(int64_t)-14L) {
			trusty_guest_transaction.function_id = TRUSTY_SMC_SC_NOP;
		} else {
			trusty_guest_transaction.function_id = TRUSTY_SMC_SC_RESTART_LAST;
		}
		trusty_guest_transaction.valid = true;
	} else if ((function_id == TRUSTY_SMC_SC_NOP) ||
		(function_id == TRUSTY_SMC_SC_RESTART_LAST) ||
		(function_id == TRUSTY_SMC_SC_RESTART_FIQ)) {
		trusty_guest_transaction.valid = false;
	}
	spinlock_release(&trusty_smc_lock);

	return result;
}
