/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bits.h>
#include <cpu.h>
#include <delay.h>
#include <timer.h>
#include <hv_pm.h>
#include <console.h>
#include <rtl.h>
#include <schedule.h>
#include <asm/hv_pm.h>
#include <asm/irq.h>
#include <asm/pmu.h>
#include <asm/psci.h>
#include <asm/sysreg.h>
#include <asm/vtd.h>

#define ARM64_PM_SMP_TIMEOUT_US	100000U

struct arm64_pm_secondary_request {
	volatile uint64_t epoch;
	volatile uint64_t target_mask;
	volatile uint64_t parked_mask;
	volatile uint64_t resumed_mask;
	volatile uint32_t release;
	int32_t suspend_status[MAX_PCPU_NUM];
	int32_t resume_status[MAX_PCPU_NUM];
};

static struct arm64_host_pm_context arm64_host_pm_context;
static struct arm64_pm_secondary_request arm64_pm_secondary;

/* [20260716] Secondary pCPU suspend ownership
 *
 * BSP idle owner               AP scheduler/idle owner
 *      | request idle(AP mask)          |
 *      +------------------------------->| save local timer/GIC
 *      |<-------------------------------+ publish parked bit
 *      | save global host state         | WFE on release flag
 *      | platform suspend/wake          |
 *      | restore global host state      |
 *      +------------------------------->| restore local GIC/timer
 *      |<-------------------------------+ publish resumed bit
 *
 * Key rules:
 *   - each AP alone accesses its banked timer and GIC CPU-interface state;
 *   - BSP waits for bounded parked/resumed masks and never runs AP work remotely;
 *   - release uses shared memory plus SEV, so it remains usable while IRQ state
 *     is being restored;
 *   - every independent restore is attempted and the first error is retained.
 */

static void arm64_pm_record_first_error(int32_t status, int32_t *first_error);

void arm64_save_el2_context(struct arm64_el2_pm_context *context)
{
	if (context == NULL) {
		return;
	}
	context->vbar_el2 = arm64_sysreg_read(vbar_el2);
	context->sctlr_el2 = arm64_sysreg_read(sctlr_el2);
	context->tcr_el2 = arm64_sysreg_read(tcr_el2);
	context->ttbr0_el2 = arm64_sysreg_read(ttbr0_el2);
	context->mair_el2 = arm64_sysreg_read(mair_el2);
	context->hcr_el2 = arm64_sysreg_read(hcr_el2);
	context->vtcr_el2 = arm64_sysreg_read(vtcr_el2);
	context->vttbr_el2 = arm64_sysreg_read(vttbr_el2);
	context->cptr_el2 = arm64_sysreg_read(cptr_el2);
	context->cnthctl_el2 = arm64_sysreg_read(cnthctl_el2);
	context->cntvoff_el2 = arm64_sysreg_read(cntvoff_el2);
	context->mdcr_el2 = arm64_sysreg_read(mdcr_el2);
	context->tpidr_el2 = arm64_sysreg_read(tpidr_el2);
}

void arm64_restore_el2_context(const struct arm64_el2_pm_context *context)
{
	if (context == NULL) {
		return;
	}
	/*
	 * Rebuild the EL2 translation regime before restoring registers that can
	 * resume guest execution. DSB makes the table-base writes visible and ISB
	 * makes the new execution context architectural before SCTLR_EL2 is used.
	 */
	arm64_sysreg_write(vbar_el2, context->vbar_el2);
	arm64_sysreg_write(mair_el2, context->mair_el2);
	arm64_sysreg_write(tcr_el2, context->tcr_el2);
	arm64_sysreg_write(ttbr0_el2, context->ttbr0_el2);
	arm64_dsb_ish();
	arm64_isb();
	arm64_sysreg_write(sctlr_el2, context->sctlr_el2);
	arm64_isb();
	arm64_sysreg_write(hcr_el2, context->hcr_el2);
	arm64_sysreg_write(vtcr_el2, context->vtcr_el2);
	arm64_sysreg_write(vttbr_el2, context->vttbr_el2);
	arm64_sysreg_write(cptr_el2, context->cptr_el2);
	arm64_sysreg_write(cnthctl_el2, context->cnthctl_el2);
	arm64_sysreg_write(cntvoff_el2, context->cntvoff_el2);
	arm64_sysreg_write(mdcr_el2, context->mdcr_el2);
	arm64_sysreg_write(tpidr_el2, context->tpidr_el2);
	arm64_isb();
}

static bool arm64_pm_wait_secondary_mask(const volatile uint64_t *ack_mask,
	uint64_t expected_mask)
{
	uint32_t timeout_us = ARM64_PM_SMP_TIMEOUT_US;

	while (((__atomic_load_n(ack_mask, __ATOMIC_ACQUIRE) & expected_mask) !=
		expected_mask) && (timeout_us != 0U)) {
		udelay(10U);
		timeout_us = (timeout_us > 10U) ? (timeout_us - 10U) : 0U;
	}

	return ((__atomic_load_n(ack_mask, __ATOMIC_ACQUIRE) & expected_mask) ==
		expected_mask);
}

static void arm64_pm_request_secondary_idle(uint64_t mask)
{
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if ((mask & (1UL << pcpu_id)) != 0UL) {
			make_system_suspend_request(pcpu_id);
		}
	}
}

static uint16_t arm64_pm_first_unacked_pcpu(uint64_t target_mask,
	uint64_t ack_mask)
{
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if (((target_mask & (1UL << pcpu_id)) != 0UL) &&
			((ack_mask & (1UL << pcpu_id)) == 0UL)) {
			return pcpu_id;
		}
	}

	return MAX_PCPU_NUM;
}

static int32_t arm64_pm_secondary_status(const int32_t *statuses, uint64_t mask,
	uint16_t *failed_pcpu)
{
	uint16_t pcpu_id;

	if (failed_pcpu != NULL) {
		*failed_pcpu = MAX_PCPU_NUM;
	}

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if (((mask & (1UL << pcpu_id)) != 0UL) &&
			(statuses[pcpu_id] != 0)) {
			if (failed_pcpu != NULL) {
				*failed_pcpu = pcpu_id;
			}
			return statuses[pcpu_id];
		}
	}
	return 0;
}

void arch_pm_process_secondary_from_idle(uint16_t pcpu_id)
{
	uint64_t epoch = __atomic_load_n(&arm64_pm_secondary.epoch,
		__ATOMIC_ACQUIRE);
	uint64_t target_mask = __atomic_load_n(&arm64_pm_secondary.target_mask,
		__ATOMIC_ACQUIRE);
	int32_t first_error = 0;
	int32_t status;
	bool timer_suspended = false;
	bool gic_suspended = false;

	if ((pcpu_id == BSP_CPU_ID) || (pcpu_id >= get_pcpu_nums()) ||
		(epoch == 0UL) || ((target_mask & (1UL << pcpu_id)) == 0UL)) {
		return;
	}

	arm64_core_pmu_suspend_cpu(epoch);
	status = arch_pm_suspend_timer(epoch);
	if (status == 0) {
		timer_suspended = true;
		status = arm64_gicv3_pm_suspend_cpu(epoch);
	}
	if (status == 0) {
		gic_suspended = true;
	} else {
		first_error = status;
	}
	arm64_pm_secondary.suspend_status[pcpu_id] = first_error;
	bitmap_set(pcpu_id, &arm64_pm_secondary.parked_mask);

	if (first_error == 0) {
		/* SEVL closes the store-release/WFE race if BSP released us early. */
		arm64_sevl();
		while (__atomic_load_n(&arm64_pm_secondary.release,
			__ATOMIC_ACQUIRE) == 0U) {
			arm64_wfe();
		}
	}

	first_error = 0;
	if (gic_suspended) {
		status = arm64_gicv3_pm_resume_cpu(epoch);
		arm64_pm_record_first_error(status, &first_error);
	}
	if (timer_suspended) {
		status = arch_pm_resume_timer(epoch);
		arm64_pm_record_first_error(status, &first_error);
	}
	arm64_core_pmu_resume_cpu(epoch);
	arm64_pm_secondary.resume_status[pcpu_id] = first_error;
	bitmap_set(pcpu_id, &arm64_pm_secondary.resumed_mask);
}

static int32_t arm64_pm_release_secondary_cpus(uint64_t epoch,
	uint64_t target_mask)
{
	uint64_t parked_mask;
	uint64_t resumed_mask;
	uint16_t pcpu_id;
	uint16_t failed_pcpu;
	int32_t status;

	if ((epoch == 0UL) ||
		(__atomic_load_n(&arm64_pm_secondary.epoch, __ATOMIC_ACQUIRE) != epoch) ||
		(__atomic_load_n(&arm64_pm_secondary.target_mask,
		 __ATOMIC_ACQUIRE) != target_mask)) {
		return -EINVAL;
	}

	__atomic_store_n(&arm64_pm_secondary.release, 1U, __ATOMIC_RELEASE);
	arm64_sev();
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if ((target_mask & (1UL << pcpu_id)) != 0UL) {
			make_reschedule_request(pcpu_id);
		}
	}

	if (!arm64_pm_wait_secondary_mask(&arm64_pm_secondary.resumed_mask,
		target_mask)) {
		parked_mask = __atomic_load_n(&arm64_pm_secondary.parked_mask,
			__ATOMIC_ACQUIRE);
		resumed_mask = __atomic_load_n(&arm64_pm_secondary.resumed_mask,
			__ATOMIC_ACQUIRE);
		failed_pcpu = arm64_pm_first_unacked_pcpu(target_mask, resumed_mask);
		LOG_ERR("STR: PM_AP_RELEASE_FAILED epoch:%lu target:0x%lx parked:0x%lx "
			"resumed:0x%lx pcpu:%hu status:%d", epoch, target_mask,
			parked_mask, resumed_mask, failed_pcpu, -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	status = arm64_pm_secondary_status(arm64_pm_secondary.resume_status,
		target_mask, &failed_pcpu);
	if (status != 0) {
		parked_mask = __atomic_load_n(&arm64_pm_secondary.parked_mask,
			__ATOMIC_ACQUIRE);
		resumed_mask = __atomic_load_n(&arm64_pm_secondary.resumed_mask,
			__ATOMIC_ACQUIRE);
		LOG_ERR("STR: PM_AP_RESUME_FAILED epoch:%lu target:0x%lx parked:0x%lx "
			"resumed:0x%lx pcpu:%hu status:%d", epoch, target_mask,
			parked_mask, resumed_mask, failed_pcpu, status);
	}
	if (status == 0) {
		(void)memset(&arm64_pm_secondary, 0U,
			sizeof(arm64_pm_secondary));
	}

	return status;
}

int32_t arch_pm_suspend_secondary_cpus(uint64_t epoch, bool *restored)
{
	uint64_t mask = AP_MASK & get_active_pcpu_bitmap();
	uint64_t parked_mask;
	uint64_t resumed_mask;
	uint16_t failed_pcpu;
	int32_t release_status;
	int32_t status;

	if ((epoch == 0UL) || (restored == NULL)) {
		return -EINVAL;
	}
	*restored = false;
	(void)memset(&arm64_pm_secondary, 0U, sizeof(arm64_pm_secondary));
	__atomic_store_n(&arm64_pm_secondary.epoch, epoch, __ATOMIC_RELEASE);
	__atomic_store_n(&arm64_pm_secondary.target_mask, mask, __ATOMIC_RELEASE);
	arm64_pm_request_secondary_idle(mask);
	if (!arm64_pm_wait_secondary_mask(&arm64_pm_secondary.parked_mask, mask)) {
		parked_mask = __atomic_load_n(&arm64_pm_secondary.parked_mask,
			__ATOMIC_ACQUIRE);
		resumed_mask = __atomic_load_n(&arm64_pm_secondary.resumed_mask,
			__ATOMIC_ACQUIRE);
		failed_pcpu = arm64_pm_first_unacked_pcpu(mask, parked_mask);
		status = -ETIMEDOUT;
		LOG_ERR("STR: PM_AP_PARK_FAILED epoch:%lu target:0x%lx parked:0x%lx "
			"resumed:0x%lx pcpu:%hu status:%d", epoch, mask,
			parked_mask, resumed_mask, failed_pcpu, status);
	} else {
		status = arm64_pm_secondary_status(
			arm64_pm_secondary.suspend_status, mask, &failed_pcpu);
		if (status != 0) {
			parked_mask = __atomic_load_n(&arm64_pm_secondary.parked_mask,
				__ATOMIC_ACQUIRE);
			resumed_mask = __atomic_load_n(&arm64_pm_secondary.resumed_mask,
				__ATOMIC_ACQUIRE);
			LOG_ERR("STR: PM_AP_SUSPEND_FAILED epoch:%lu target:0x%lx "
				"parked:0x%lx resumed:0x%lx pcpu:%hu status:%d",
				epoch, mask, parked_mask, resumed_mask, failed_pcpu,
				status);
		}
	}
	if (status != 0) {
		release_status = arm64_pm_release_secondary_cpus(epoch, mask);
		*restored = (release_status == 0);
	}
	return status;
}

int32_t arch_pm_resume_secondary_cpus(uint64_t epoch)
{
	uint64_t mask = __atomic_load_n(&arm64_pm_secondary.target_mask,
		__ATOMIC_ACQUIRE);

	if ((epoch == 0UL) ||
		(__atomic_load_n(&arm64_pm_secondary.epoch, __ATOMIC_ACQUIRE) != epoch)) {
		return -EINVAL;
	}

	return arm64_pm_release_secondary_cpus(epoch, mask);
}

#if defined(CONFIG_PLATFORM_QEMU)
/* Keep this undefined in the common object so the linker extracts QEMU PM. */
extern const struct arm64_platform_pm_ops *arm64_platform_pm_get_ops(void);
#else
static const struct arm64_platform_pm_ops arm64_unsupported_pm_ops = {
	.name = "unsupported",
};

__attribute__((weak)) const struct arm64_platform_pm_ops *
arm64_platform_pm_get_ops(void)
{
	return &arm64_unsupported_pm_ops;
}
#endif

uint32_t platform_pm_capabilities(void)
{
	return arm64_platform_pm_get_ops()->capabilities;
}

int32_t platform_pm_preflight(uint8_t platform_mode)
{
	uint32_t capabilities = platform_pm_capabilities();

	if ((platform_mode == HV_PM_PLATFORM_SIMULATED) &&
		((capabilities & ARM64_PLATFORM_PM_CAP_SIMULATED) != 0U)) {
		return 0;
	}
	if ((platform_mode == HV_PM_PLATFORM_STRICT) &&
		((capabilities & ARM64_PLATFORM_PM_CAP_HARDWARE) != 0U)) {
		return 0;
	}

	return -ENOTSUP;
}

static void arm64_pm_record_first_error(int32_t status, int32_t *first_error)
{
	if ((status != 0) && (*first_error == 0)) {
		*first_error = status;
	}
}

/* [20260716] ARM64 system STR flow
 *
 * BSP idle / core PM       ARM64 host retention         platform backend
 *        |                          |                          |
 *        +-- PM_SUSPENDED --------->| save EL2 context         |
 *        |                          +-- arm wake source ------>|
 *        |                          +-- park APs               |
 *        |                          +-- stop BSP timer/GIC     |
 *        |                          +-- enter ---------------->|
 *        |                          |<--------------- wake/err |
 *        |                          +-- restore EL2/GIC/SMMU   |
 *        |                          +-- timer/APs/console      |
 *        |<-- host_restored --------+-- release wake source -->|
 *
 * Key rule:
 *   - platform prepare owns wake-source arming until wake or abort cleanup;
 *   - APs save banked state on their own CPUs before the BSP retains global
 *     distributor and EL2 state;
 *   - restore order follows availability dependencies, so the SMMU and GIC
 *     are usable before APs and guest-facing device hooks resume;
 *   - all independent restore steps run and preserve the first error; callers
 *     may roll back only when host_restored proves EL2 is operational.
 */
int32_t platform_pm_enter(uint64_t epoch, bool *host_restored)
{
	const struct arm64_platform_pm_ops *ops = arm64_platform_pm_get_ops();
	struct beau_pm_snapshot snapshot;
	int32_t platform_status = 0;
	int32_t restore_status = 0;
	int32_t cleanup_status = 0;
	int32_t status;
	bool secondary_suspended = false;
	bool secondary_restored = false;
	bool timer_suspended = false;
	bool pmu_suspended = false;
	bool gic_suspended = false;

	if (host_restored == NULL) {
		return -EINVAL;
	}
	*host_restored = false;
	hv_pm_get_snapshot(&snapshot);
	if ((epoch == 0UL) || (snapshot.epoch != epoch) ||
		(snapshot.state != PM_SUSPENDED)) {
		return -EINVAL;
	}
	if ((platform_pm_preflight(snapshot.platform_mode) != 0) ||
		(ops->prepare == NULL) || (ops->enter == NULL) ||
		(ops->wake == NULL) || (ops->abort == NULL)) {
		return -ENOTSUP;
	}

	(void)memset(&arm64_host_pm_context, 0U,
		sizeof(arm64_host_pm_context));
	arm64_host_pm_context.epoch = epoch;
	arm64_save_el2_context(&arm64_host_pm_context.el2);
	arm64_host_pm_context.valid = true;
	status = ops->prepare(epoch, &arm64_host_pm_context);
	if (status != 0) {
		(void)ops->abort(epoch, &arm64_host_pm_context);
		arm64_host_pm_context.valid = false;
		*host_restored = true;
		return status;
	}
	suspend_console();

	status = arch_pm_suspend_secondary_cpus(epoch, &secondary_restored);
	if (status == 0) {
		secondary_suspended = true;
		arm64_core_pmu_suspend_cpu(epoch);
		pmu_suspended = true;
		status = arch_pm_suspend_timer(epoch);
	} else if (!secondary_restored) {
		/* An AP that did not acknowledge local restore keeps EL2 failed closed. */
		arm64_pm_record_first_error(status, &restore_status);
	}
	if (status == 0) {
		timer_suspended = true;
		status = arm64_gicv3_pm_suspend(epoch);
	}
	if (status == 0) {
		gic_suspended = true;
		platform_status = ops->enter(epoch, &arm64_host_pm_context);
	} else {
		platform_status = status;
	}

	arm64_restore_el2_context(&arm64_host_pm_context.el2);
	if (gic_suspended) {
		status = arm64_gicv3_pm_resume(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	status = arm_smmu_pm_resume(epoch);
	arm64_pm_record_first_error(status, &restore_status);
	if (timer_suspended) {
		status = arch_pm_resume_timer(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	if (pmu_suspended) {
		arm64_core_pmu_resume_cpu(epoch);
	}
	if (secondary_suspended) {
		status = arch_pm_resume_secondary_cpus(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	resume_console();
	if (restore_status != 0) {
		(void)ops->abort(epoch, &arm64_host_pm_context);
	} else {
		*host_restored = true;
		cleanup_status = (platform_status == 0) ?
			ops->wake(epoch, &arm64_host_pm_context) :
			ops->abort(epoch, &arm64_host_pm_context);
	}
	arm64_host_pm_context.valid = false;

	/* core/pm.c restores device hooks before releasing scheduler freeze gates. */
	if (restore_status != 0) {
		return restore_status;
	}
	return (platform_status != 0) ? platform_status : cleanup_status;
}

void arch_shutdown_host(void)
{
	int64_t ret = psci_system_off();

	panic("arm64 psci system off failed, ret=%ld", ret);
}

void arch_reset_host(__unused bool warm)
{
	int64_t ret = psci_system_reset();

	panic("arm64 psci system reset failed, ret=%ld", ret);
}
