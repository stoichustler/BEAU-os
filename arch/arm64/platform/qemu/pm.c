/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <irq.h>
#include <io.h>
#include <logmsg.h>
#include <pgtable.h>
#include <bsp/pm.h>
#include <asm/hv_pm.h>
#include <asm/irq.h>

#define QEMU_PM_UART_BASE	0x09000000UL
#define QEMU_PM_UART_IRQ	33U
#define QEMU_PM_PL011_DR	0x000U
#define QEMU_PM_PL011_FR	0x018U
#define QEMU_PM_PL011_IMSC	0x038U
#define QEMU_PM_PL011_ICR	0x044U
#define QEMU_PM_PL011_FR_RXFE	(1U << 4U)
#define QEMU_PM_PL011_INT_RX	(1U << 4U)
#define QEMU_PM_PL011_INT_RT	(1U << 6U)
#define QEMU_PM_PL011_INT_ALL	0x7ffU

enum qemu_pm_mode {
	QEMU_PM_SIMULATED = HV_PM_PLATFORM_SIMULATED,
	QEMU_PM_STRICT = HV_PM_PLATFORM_STRICT,
};

static uint32_t qemu_pm_saved_imsc;
static uint32_t qemu_pm_acrn_irq = IRQ_INVALID;

static inline void asm_wfi(void)
{
	asm volatile ("wfi" : : : "memory");
}

static inline void *qemu_pm_uart_reg(uint32_t offset)
{
	return (void *)(QEMU_PM_UART_BASE + offset);
}

static void qemu_pm_wake_irq(__unused uint32_t irq, __unused void *data)
{
	while ((mmio_read32(qemu_pm_uart_reg(QEMU_PM_PL011_FR)) &
		QEMU_PM_PL011_FR_RXFE) == 0U) {
		(void)mmio_read32(qemu_pm_uart_reg(QEMU_PM_PL011_DR));
	}
	mmio_write32(QEMU_PM_PL011_INT_ALL,
		qemu_pm_uart_reg(QEMU_PM_PL011_ICR));
	(void)bsp_pm_request_wake(QEMU_PM_UART_IRQ);
}

static int32_t qemu_pm_arm_uart_wake(void)
{
	int32_t status;

	qemu_pm_acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC,
		QEMU_PM_UART_IRQ);
	if (qemu_pm_acrn_irq == IRQ_INVALID) {
		return -ENODEV;
	}
	status = request_irq(qemu_pm_acrn_irq, qemu_pm_wake_irq, NULL, IRQF_NONE);
	if (status < 0) {
		qemu_pm_acrn_irq = IRQ_INVALID;
		return status;
	}
	qemu_pm_saved_imsc = mmio_read32(qemu_pm_uart_reg(QEMU_PM_PL011_IMSC));
	mmio_write32(QEMU_PM_PL011_INT_ALL,
		qemu_pm_uart_reg(QEMU_PM_PL011_ICR));
	mmio_write32(qemu_pm_saved_imsc | QEMU_PM_PL011_INT_RX |
		QEMU_PM_PL011_INT_RT, qemu_pm_uart_reg(QEMU_PM_PL011_IMSC));

	return 0;
}

static void qemu_pm_disarm_uart_wake(void)
{
	mmio_write32(qemu_pm_saved_imsc,
		qemu_pm_uart_reg(QEMU_PM_PL011_IMSC));
	mmio_write32(QEMU_PM_PL011_INT_ALL,
		qemu_pm_uart_reg(QEMU_PM_PL011_ICR));
	if (qemu_pm_acrn_irq != IRQ_INVALID) {
		free_irq(qemu_pm_acrn_irq);
		qemu_pm_acrn_irq = IRQ_INVALID;
	}
}

static int32_t qemu_pm_simulated_wait(uint64_t epoch)
{
	struct beau_pm_snapshot snapshot;
	int32_t status = qemu_pm_arm_uart_wake();

	if (status != 0) {
		return status;
	}
	do {
		asm_wfi();
		hv_pm_get_snapshot(&snapshot);
	} while ((snapshot.epoch == epoch) && (snapshot.state == PM_SUSPENDED) &&
		(snapshot.wake_bitmap == 0UL));
	qemu_pm_disarm_uart_wake();

	return ((snapshot.epoch == epoch) && (snapshot.wake_bitmap != 0UL)) ?
		0 : -EIO;
}

static int32_t qemu_pm_strict_enter(struct arm64_host_pm_context *context)
{
	/* arm64_suspend_enter saves callee state before issuing psci_system_suspend. */
	int64_t ret = arm64_suspend_enter(&context->callee,
		hva2hpa(arm64_suspend_resume), hva2hpa(context));

	if (ret == 0L) {
		return 0;
	}
	LOG_ERR("QEMU PM strict PSCI SYSTEM_SUSPEND failed: %ld", ret);
	return (ret == -1L) ? -ENOSYS : -EIO;
}

int32_t qemu_platform_pm_enter(uint64_t epoch,
	struct arm64_host_pm_context *context)
{
	struct beau_pm_snapshot snapshot;

	hv_pm_get_snapshot(&snapshot);
	if ((context == NULL) || !context->valid || (context->epoch != epoch) ||
		(snapshot.epoch != epoch)) {
		return -EINVAL;
	}

	switch ((enum qemu_pm_mode)snapshot.platform_mode) {
	case QEMU_PM_SIMULATED:
		return qemu_pm_simulated_wait(epoch);
	case QEMU_PM_STRICT:
		return qemu_pm_strict_enter(context);
	default:
		return -ENOSYS;
	}
}
