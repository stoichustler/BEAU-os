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
#include <bsp/pm.h>
#include <asm/hv_pm.h>
#include <asm/instruction.h>
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

/* [20260716] QEMU simulated STR wake path
 *
 * host terminal byte -> PL011 RX/RT IRQ -> qemu_pm_wake_irq()
 *                                              |
 *                                              +-- drain RX FIFO
 *                                              +-- clear PL011 IRQ
 *                                              +-- validate wake allow-list
 *                                              +-- publish epoch wake bitmap
 *                                                                |
 * BSP: arm UART -> WFI loop -------------------------------------+-> restore
 *
 * Key rule:
 *   - QEMU simulated mode exercises the same PM transaction and retention
 *     ordering without claiming that virtual hardware lost power;
 *   - prepare saves the original PL011 interrupt mask, and wake/abort both
 *     restore it and release the temporary IRQ handler deterministically;
 *   - the ISR drains input before recording the wake so a level-triggered UART
 *     cannot immediately retrigger while host interrupt state is restored.
 */
static uint32_t qemu_pm_saved_imsc;
static uint32_t qemu_pm_acrn_irq = IRQ_INVALID;
static bool qemu_pm_uart_wake_armed;

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

	if (qemu_pm_uart_wake_armed) {
		return -EBUSY;
	}
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
	qemu_pm_uart_wake_armed = true;

	return 0;
}

static void qemu_pm_disarm_uart_wake(void)
{
	if (!qemu_pm_uart_wake_armed) {
		return;
	}
	mmio_write32(qemu_pm_saved_imsc,
		qemu_pm_uart_reg(QEMU_PM_PL011_IMSC));
	mmio_write32(QEMU_PM_PL011_INT_ALL,
		qemu_pm_uart_reg(QEMU_PM_PL011_ICR));
	if (qemu_pm_acrn_irq != IRQ_INVALID) {
		free_irq(qemu_pm_acrn_irq);
		qemu_pm_acrn_irq = IRQ_INVALID;
	}
	qemu_pm_uart_wake_armed = false;
}

static int32_t qemu_pm_prepare(__unused uint64_t epoch,
	struct arm64_host_pm_context *context)
{
	return ((context != NULL) && context->valid) ?
		qemu_pm_arm_uart_wake() : -EINVAL;
}

static int32_t qemu_pm_simulated_wait(uint64_t epoch,
	struct arm64_host_pm_context *context)
{
	struct beau_pm_snapshot snapshot;

	if ((context == NULL) || !context->valid || (context->epoch != epoch)) {
		return -EINVAL;
	}
	LOG_INF("STR: PM_SUSPENDED epoch:%lu", epoch);
	do {
		arm64_wfi();
		hv_pm_get_snapshot(&snapshot);
	} while ((snapshot.epoch == epoch) && (snapshot.state == PM_SUSPENDED) &&
		(snapshot.wake_bitmap == 0UL));

	return ((snapshot.epoch == epoch) && (snapshot.wake_bitmap != 0UL)) ?
		0 : -EIO;
}

static int32_t qemu_pm_release(__unused uint64_t epoch,
	__unused struct arm64_host_pm_context *context)
{
	qemu_pm_disarm_uart_wake();
	return 0;
}

static const struct arm64_platform_pm_ops qemu_pm_ops = {
	.name = "qemu-simulated",
	.capabilities = ARM64_PLATFORM_PM_CAP_SIMULATED,
	.prepare = qemu_pm_prepare,
	.enter = qemu_pm_simulated_wait,
	.wake = qemu_pm_release,
	.abort = qemu_pm_release,
};

const struct arm64_platform_pm_ops *arm64_platform_pm_get_ops(void)
{
	return &qemu_pm_ops;
}
