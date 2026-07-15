/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <cpu.h>
#include <notify.h>
#include <timer.h>
#include <hv_pm.h>
#include <console.h>
#include <rtl.h>
#include <asm/hv_pm.h>
#include <asm/irq.h>
#include <asm/psci.h>
#include <asm/vtd.h>

#define ARM64_PM_SMP_TIMEOUT_US	100000U

#define ARM64_PM_READ_SYSREG(reg) ({ \
	uint64_t value; \
	asm volatile ("mrs %0, " #reg : "=r" (value)); \
	value; \
})

#define ARM64_PM_WRITE_SYSREG(reg, value) \
	asm volatile ("msr " #reg ", %0" : : "r" ((uint64_t)(value)) : "memory")

struct arm64_pm_secondary_request {
	uint64_t epoch;
	int32_t status[MAX_PCPU_NUM];
};

static struct arm64_host_pm_context arm64_host_pm_context;
static struct arm64_pm_secondary_request arm64_pm_secondary;

_Static_assert(sizeof(struct arm64_suspend_callee_context) == 104U,
	"arm64 suspend assembly context layout mismatch");

void arm64_save_el2_context(struct arm64_el2_pm_context *context)
{
	if (context == NULL) {
		return;
	}
	context->vbar_el2 = ARM64_PM_READ_SYSREG(vbar_el2);
	context->sctlr_el2 = ARM64_PM_READ_SYSREG(sctlr_el2);
	context->tcr_el2 = ARM64_PM_READ_SYSREG(tcr_el2);
	context->ttbr0_el2 = ARM64_PM_READ_SYSREG(ttbr0_el2);
	context->mair_el2 = ARM64_PM_READ_SYSREG(mair_el2);
	context->hcr_el2 = ARM64_PM_READ_SYSREG(hcr_el2);
	context->vtcr_el2 = ARM64_PM_READ_SYSREG(vtcr_el2);
	context->vttbr_el2 = ARM64_PM_READ_SYSREG(vttbr_el2);
	context->cptr_el2 = ARM64_PM_READ_SYSREG(cptr_el2);
	context->cnthctl_el2 = ARM64_PM_READ_SYSREG(cnthctl_el2);
	context->cntvoff_el2 = ARM64_PM_READ_SYSREG(cntvoff_el2);
	context->mdcr_el2 = ARM64_PM_READ_SYSREG(mdcr_el2);
	context->tpidr_el2 = ARM64_PM_READ_SYSREG(tpidr_el2);
}

void arm64_restore_el2_context(const struct arm64_el2_pm_context *context)
{
	if (context == NULL) {
		return;
	}
	ARM64_PM_WRITE_SYSREG(vbar_el2, context->vbar_el2);
	ARM64_PM_WRITE_SYSREG(mair_el2, context->mair_el2);
	ARM64_PM_WRITE_SYSREG(tcr_el2, context->tcr_el2);
	ARM64_PM_WRITE_SYSREG(ttbr0_el2, context->ttbr0_el2);
	asm volatile ("dsb ish; isb" : : : "memory");
	ARM64_PM_WRITE_SYSREG(sctlr_el2, context->sctlr_el2);
	asm volatile ("isb" : : : "memory");
	ARM64_PM_WRITE_SYSREG(hcr_el2, context->hcr_el2);
	ARM64_PM_WRITE_SYSREG(vtcr_el2, context->vtcr_el2);
	ARM64_PM_WRITE_SYSREG(vttbr_el2, context->vttbr_el2);
	ARM64_PM_WRITE_SYSREG(cptr_el2, context->cptr_el2);
	ARM64_PM_WRITE_SYSREG(cnthctl_el2, context->cnthctl_el2);
	ARM64_PM_WRITE_SYSREG(cntvoff_el2, context->cntvoff_el2);
	ARM64_PM_WRITE_SYSREG(mdcr_el2, context->mdcr_el2);
	ARM64_PM_WRITE_SYSREG(tpidr_el2, context->tpidr_el2);
	asm volatile ("isb" : : : "memory");
}

static void arm64_pm_suspend_secondary(void *data)
{
	struct arm64_pm_secondary_request *request = data;
	uint16_t pcpu_id = get_pcpu_id();
	int32_t status = arch_pm_suspend_timer(request->epoch);

	if (status == 0) {
		status = arm64_gicv3_pm_suspend_cpu(request->epoch);
	}
	request->status[pcpu_id] = status;
}

static void arm64_pm_resume_secondary(void *data)
{
	struct arm64_pm_secondary_request *request = data;
	uint16_t pcpu_id = get_pcpu_id();
	int32_t status = arm64_gicv3_pm_resume_cpu(request->epoch);

	if (status == 0) {
		status = arch_pm_resume_timer(request->epoch);
	}
	request->status[pcpu_id] = status;
}

static int32_t arm64_pm_secondary_status(uint64_t mask)
{
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if (((mask & (1UL << pcpu_id)) != 0UL) &&
			(arm64_pm_secondary.status[pcpu_id] != 0)) {
			return arm64_pm_secondary.status[pcpu_id];
		}
	}
	return 0;
}

int32_t arch_pm_suspend_secondary_cpus(uint64_t epoch)
{
	uint64_t mask = AP_MASK & get_active_pcpu_bitmap();
	int32_t status;

	if (epoch == 0UL) {
		return -EINVAL;
	}
	if (mask == 0UL) {
		arm64_pm_secondary.epoch = epoch;
		return 0;
	}
	(void)memset(&arm64_pm_secondary, 0U, sizeof(arm64_pm_secondary));
	arm64_pm_secondary.epoch = epoch;
	if (!smp_call_function_timeout(mask, arm64_pm_suspend_secondary,
		&arm64_pm_secondary, ARM64_PM_SMP_TIMEOUT_US)) {
		status = -ETIMEDOUT;
	} else {
		status = arm64_pm_secondary_status(mask);
	}
	if (status != 0) {
		(void)smp_call_function_timeout(mask, arm64_pm_resume_secondary,
			&arm64_pm_secondary, ARM64_PM_SMP_TIMEOUT_US);
		arm64_pm_secondary.epoch = 0UL;
	}
	return status;
}

int32_t arch_pm_resume_secondary_cpus(uint64_t epoch)
{
	uint64_t mask = AP_MASK & get_active_pcpu_bitmap();

	if ((epoch == 0UL) || (arm64_pm_secondary.epoch != epoch)) {
		return -EINVAL;
	}
	if (mask == 0UL) {
		arm64_pm_secondary.epoch = 0UL;
		return 0;
	}
	(void)memset(arm64_pm_secondary.status, 0U,
		sizeof(arm64_pm_secondary.status));
	if (!smp_call_function_timeout(mask, arm64_pm_resume_secondary,
		&arm64_pm_secondary, ARM64_PM_SMP_TIMEOUT_US)) {
		return -ETIMEDOUT;
	}
	{
		int32_t status = arm64_pm_secondary_status(mask);

		if (status == 0) {
			arm64_pm_secondary.epoch = 0UL;
		}
		return status;
	}
}

#if defined(CONFIG_PLATFORM_QEMU)
int32_t qemu_platform_pm_enter(uint64_t epoch,
	struct arm64_host_pm_context *context);

int32_t arm64_platform_pm_enter(uint64_t epoch,
	struct arm64_host_pm_context *context)
{
	return qemu_platform_pm_enter(epoch, context);
}
#else
int32_t arm64_platform_pm_enter(__unused uint64_t epoch,
	__unused struct arm64_host_pm_context *context)
{
	return -ENOSYS;
}
#endif

static void arm64_pm_record_first_error(int32_t status, int32_t *first_error)
{
	if ((status != 0) && (*first_error == 0)) {
		*first_error = status;
	}
}

int32_t platform_pm_enter(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	int32_t platform_status = 0;
	int32_t restore_status = 0;
	int32_t status;
	bool secondary_suspended = false;
	bool timer_suspended = false;
	bool gic_suspended = false;

	hv_pm_get_snapshot(&snapshot);
	if ((epoch == 0UL) || (snapshot.epoch != epoch) ||
		(snapshot.state != PM_SUSPENDED)) {
		return -EINVAL;
	}

	(void)memset(&arm64_host_pm_context, 0U,
		sizeof(arm64_host_pm_context));
	arm64_host_pm_context.epoch = epoch;
	arm64_save_el2_context(&arm64_host_pm_context.el2);
	arm64_host_pm_context.valid = true;
	LOG_INF("PM: epoch %lu host suspended", epoch);
	suspend_console();

	status = arch_pm_suspend_secondary_cpus(epoch);
	if (status == 0) {
		secondary_suspended = true;
		status = arch_pm_suspend_timer(epoch);
	}
	if (status == 0) {
		timer_suspended = true;
		status = arm64_gicv3_pm_suspend(epoch);
	}
	if (status == 0) {
		gic_suspended = true;
		platform_status = arm64_platform_pm_enter(epoch,
			&arm64_host_pm_context);
	} else {
		platform_status = status;
	}

	arm64_restore_el2_context(&arm64_host_pm_context.el2);
	if (gic_suspended) {
		status = arm64_gicv3_pm_resume(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	if (restore_status == 0) {
		status = arm_smmu_pm_resume(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	if (timer_suspended && (restore_status == 0)) {
		status = arch_pm_resume_timer(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	if (secondary_suspended && (restore_status == 0)) {
		status = arch_pm_resume_secondary_cpus(epoch);
		arm64_pm_record_first_error(status, &restore_status);
	}
	resume_console();
	arm64_host_pm_context.valid = false;

	/* core/pm.c calls hv_pm_resume_guests only after device hook rollback. */
	return (restore_status != 0) ? restore_status : platform_status;
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
