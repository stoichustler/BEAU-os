/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <io_req.h>
#include <console.h>
#include <vuart.h>
#include <debug/serial.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vpl011.h>

#define PL011_DR		0x000U
#define PL011_FR		0x018U
#define PL011_IBRD		0x024U
#define PL011_FBRD		0x028U
#define PL011_LCRH		0x02CU
#define PL011_CR		0x030U
#define PL011_IFLS		0x034U
#define PL011_IMSC		0x038U
#define PL011_RIS		0x03CU
#define PL011_MIS		0x040U
#define PL011_ICR		0x044U

#define PL011_FR_TXFE		(1U << 7U)
#define PL011_FR_TXFF		(1U << 5U)
#define PL011_FR_RXFE		(1U << 4U)
#define PL011_FR_BUSY		(1U << 3U)
#define PL011_INT_TX		(1U << 5U)
#define PL011_INT_RX		(1U << 4U)
#define PL011_INT_RT		(1U << 6U)
#define PL011_INT_RX_ANY	(PL011_INT_RX | PL011_INT_RT)

#define PL011_PID_BASE		0xFE0U
#define PL011_CID_BASE		0xFF0U

static const uint8_t pl011_pid[] = { 0x11U, 0x10U, 0x14U, 0x00U, 0x0dU, 0xf0U, 0x05U, 0xb1U };
static const uint8_t pl011_cid[] = { 0x0dU, 0xf0U, 0x05U, 0xb1U };

/*
 * 2026-06-30, vPL011 principle:
 *
 * vPL011 is the ARM64 guest console device. Stage-2 deliberately leaves the
 * UART IPA unmapped, data-abort MMIO reaches this file, and the common console
 * ring remains the owner of host-side buffering and vsh replay ordering.
 *
 *   guest PL011 MMIO
 *        -> stage-2 data abort
 *        -> common MMIO request
 *        -> vPL011 register model
 *        -> console vuart ring and vGIC UART IRQ
 */
struct arm64_vpl011 {
	uint64_t tx_count;
	uint64_t tx_irq_count;
	uint64_t irq_assert_count;
	uint64_t irq_deassert_count;
	uint32_t ibrd;
	uint32_t fbrd;
	uint32_t lcrh;
	uint32_t cr;
	uint32_t ifls;
	uint32_t imsc;
	uint32_t ris;
	uint32_t pending;
	uint8_t last_tx;
	bool irq_asserted;
	bool initialized;
};

static struct arm64_vpl011 vpl011_devs[CONFIG_MAX_VM_NUM];

static uint32_t vpl011_rx_int_state(struct acrn_vm *vm)
{
	return vuart_rx_pending(vm_console_vuart(vm)) ? PL011_INT_RX_ANY : 0U;
}

static uint32_t vpl011_pending_state(struct acrn_vm *vm, const struct arm64_vpl011 *vu)
{
	return vu->ris | vpl011_rx_int_state(vm);
}

static bool vpl011_tx_can_accept(const struct acrn_vm *vm)
{
	return console_vm_tx_can_accept(vm->vm_id);
}

static void vpl011_refresh_tx_state(struct acrn_vm *vm, struct arm64_vpl011 *vu)
{
	if (vpl011_tx_can_accept(vm)) {
		if ((vu->ris & PL011_INT_TX) == 0U) {
			vu->ris |= PL011_INT_TX;
			vu->tx_irq_count++;
		}
	} else {
		vu->ris &= ~PL011_INT_TX;
	}
}

static void vpl011_update_irq(struct acrn_vm *vm, struct arm64_vpl011 *vu, bool kick_asserted)
{
	uint32_t pending = vpl011_pending_state(vm, vu);
	bool assert = ((pending & vu->imsc) != 0U);
	uint32_t irq = get_vm_config(vm->vm_id)->arch.guest_uart_irq;

	vu->pending = pending;
	/*
	 * vGIC level inject/deassert syncs and flushes the target vCPU. Keep the
	 * virtual line state level-triggered, but only replay an asserted line
	 * when fresh RX data arrived. Guest MMIO polling can refresh the current
	 * LR in place; new host input must also kick the remote target vCPU if the
	 * line was already high and Linux is stopped in WFI behind another IRQ.
	 */
	if (assert && !vu->irq_asserted) {
		arch_trigger_level_intr(vm, irq, true);
		vu->irq_asserted = true;
		vu->irq_assert_count++;
	} else if (assert && kick_asserted) {
		arch_trigger_level_intr(vm, irq, true);
	} else if (assert) {
		arm64_vgicv3_refresh_current_level_irq(vm, irq);
	} else if (!assert && vu->irq_asserted) {
		arch_trigger_level_intr(vm, irq, false);
		vu->irq_asserted = false;
		vu->irq_deassert_count++;
	}
}

static void vpl011_set_imsc(struct acrn_vuart *console, struct arm64_vpl011 *vu, uint32_t value)
{
	uint32_t old_imsc = vu->imsc;
	struct acrn_vm *vm = console->vm;

	vu->imsc = value;
	if (((old_imsc & PL011_INT_TX) == 0U) && ((value & PL011_INT_TX) != 0U)) {
		vpl011_refresh_tx_state(vm, vu);
	}
	if ((value & PL011_INT_RX_ANY) != 0U) {
		console->ier |= IER_ERBFI;
	} else {
		console->ier &= (uint8_t)~IER_ERBFI;
	}
}

static bool vpl011_push_tx(struct acrn_vm *vm, struct arm64_vpl011 *vu, char ch)
{
	uint32_t old_ris = vu->ris;

	vu->tx_count++;
	vu->last_tx = (uint8_t)ch;
	(void)console_vm_tx_put(vm->vm_id, ch);
	vpl011_refresh_tx_state(vm, vu);

	return ((old_ris ^ vu->ris) & PL011_INT_TX) != 0U;
}

static void vpl011_notify_rx(struct acrn_vuart *console)
{
	struct acrn_vm *vm = console->vm;
	struct arm64_vpl011 *vu = &vpl011_devs[vm->vm_id];

	if (vuart_rx_pending(console)) {
		console->ier |= IER_ERBFI;
	} else {
		console->ier &= (uint8_t)~IER_ERBFI;
	}
	vpl011_update_irq(vm, vu, true);
}

void console_vm_tx_space_changed(uint16_t vmid)
{
	struct acrn_vm *vm;
	struct arm64_vpl011 *vu;
	uint32_t old_ris;

	if (vmid < CONFIG_MAX_VM_NUM) {
		vm = get_vm_from_vmid(vmid);
		if ((vm != NULL) && !is_poweroff_vm(vm)) {
			vu = &vpl011_devs[vmid];
			if (vu->initialized) {
				old_ris = vu->ris;
				vpl011_refresh_tx_state(vm, vu);
				if (((old_ris ^ vu->ris) & PL011_INT_TX) != 0U) {
					vpl011_update_irq(vm, vu, true);
				}
			}
		}
	}
}

static const struct vuart_backend_ops vpl011_backend_ops = {
	.notify_rx = vpl011_notify_rx,
};

void arm64_vpl011_init_vm(struct acrn_vm *vm)
{
	struct arm64_vpl011 *vu = &vpl011_devs[vm->vm_id];
	struct acrn_vuart *console;
	uint32_t irq = get_vm_config(vm->vm_id)->arch.guest_uart_irq;

	(void)memset(vu, 0U, sizeof(*vu));
	vu->ifls = 0x12U;
	vu->cr = (1U << 0U) | (1U << 8U) | (1U << 9U);
	vu->initialized = true;
	init_console_vuart(vm, irq);
	console = vm_console_vuart(vm);
	vuart_set_backend(console, &vpl011_backend_ops);
}

void arm64_vpl011_get_debug(uint16_t vm_id, struct arm64_vpl011_debug *debug)
{
	if ((debug != NULL) && (vm_id < CONFIG_MAX_VM_NUM)) {
		const struct arm64_vpl011 *vu = &vpl011_devs[vm_id];

		debug->tx_count = vu->tx_count;
		debug->tx_irq_count = vu->tx_irq_count;
		debug->irq_assert_count = vu->irq_assert_count;
		debug->irq_deassert_count = vu->irq_deassert_count;
		debug->cr = vu->cr;
		debug->imsc = vu->imsc;
		debug->ris = vu->ris;
		debug->pending = vu->pending;
		debug->last_tx = vu->last_tx;
		debug->irq_asserted = vu->irq_asserted;
	}
}

static uint32_t vpl011_read(struct acrn_vm *vm, struct arm64_vpl011 *vu, uint32_t offset)
{
	struct acrn_vuart *console = vm_console_vuart(vm);
	uint32_t value = 0U;
	bool update_irq = false;
	char ch;

	switch (offset) {
	case PL011_DR:
		ch = vuart_get_rx_char(console);
		if (ch != -1) {
			value = (uint32_t)(uint8_t)ch;
		}
		if (console_vm_rx_refill(console)) {
			console->ier |= IER_ERBFI;
		}
		if (!vuart_rx_pending(console)) {
			console->ier &= (uint8_t)~IER_ERBFI;
		}
		update_irq = true;
		break;
	case PL011_FR:
		if (vpl011_tx_can_accept(vm)) {
			value = PL011_FR_TXFE;
		} else {
			value = PL011_FR_TXFF | PL011_FR_BUSY;
		}
		if (!vuart_rx_pending(console)) {
			value |= PL011_FR_RXFE;
		}
		break;
	case PL011_IBRD:
		value = vu->ibrd;
		break;
	case PL011_FBRD:
		value = vu->fbrd;
		break;
	case PL011_LCRH:
		value = vu->lcrh;
		break;
	case PL011_CR:
		value = vu->cr;
		break;
	case PL011_IFLS:
		value = vu->ifls;
		break;
	case PL011_IMSC:
		value = vu->imsc;
		break;
	case PL011_RIS:
		value = vu->ris | vpl011_rx_int_state(vm);
		update_irq = true;
		break;
	case PL011_MIS:
		value = (vu->ris | vpl011_rx_int_state(vm)) & vu->imsc;
		update_irq = true;
		break;
	default:
		if ((offset >= PL011_PID_BASE) && (offset < (PL011_PID_BASE + sizeof(pl011_pid) * 4U))) {
			value = pl011_pid[(offset - PL011_PID_BASE) >> 2U];
		} else if ((offset >= PL011_CID_BASE) && (offset < (PL011_CID_BASE + sizeof(pl011_cid) * 4U))) {
			value = pl011_cid[(offset - PL011_CID_BASE) >> 2U];
		}
		break;
	}

	if (update_irq) {
		vpl011_update_irq(vm, vu, false);
	} else {
		vu->pending = vpl011_pending_state(vm, vu);
	}
	return value;
}

static void vpl011_write(struct acrn_vm *vm, struct arm64_vpl011 *vu, uint32_t offset, uint32_t value)
{
	struct acrn_vuart *console = vm_console_vuart(vm);
	bool update_irq = true;
	char ch;

	switch (offset) {
	case PL011_DR:
		ch = (char)(value & 0xffU);
		update_irq = vpl011_push_tx(vm, vu, ch) && ((vu->imsc & PL011_INT_TX) != 0U);
		break;
	case PL011_IBRD:
		vu->ibrd = value;
		break;
	case PL011_FBRD:
		vu->fbrd = value;
		break;
	case PL011_LCRH:
		vu->lcrh = value;
		break;
	case PL011_CR:
		vu->cr = value;
		break;
	case PL011_IFLS:
		vu->ifls = value;
		break;
	case PL011_IMSC:
		vpl011_set_imsc(console, vu, value);
		break;
	case PL011_ICR:
		vu->ris &= ~value;
		break;
	default:
		break;
	}

	if (update_irq) {
		vpl011_update_irq(vm, vu, false);
	}
}

int32_t arm64_vpl011_mmio_handler(struct io_request *io_req, void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct arm64_vpl011 *vu;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	uint64_t base;
	uint32_t offset;
	int32_t ret = -EINVAL;

	/*
	 * Performance bottleneck: the guest PL011 is byte-oriented and stage-2
	 * trapped. Heavy console output can turn into one VM-exit per character,
	 * plus possible vGIC IRQ sync when RX/TX interrupt state changes.
	 */
	if ((vm != NULL) && (vm->vm_id < CONFIG_MAX_VM_NUM) &&
		((mmio->size == 1UL) || (mmio->size == 2UL) || (mmio->size == 4UL))) {
		base = get_vm_config(vm->vm_id)->arch.guest_uart_base;
		offset = (uint32_t)(mmio->address - base);
		vu = &vpl011_devs[vm->vm_id];

		if (mmio->direction == ACRN_IOREQ_DIR_READ) {
			mmio->value = vpl011_read(vm, vu, offset);
		} else {
			vpl011_write(vm, vu, offset, (uint32_t)mmio->value);
		}
		ret = 0;
	}

	return ret;
}
