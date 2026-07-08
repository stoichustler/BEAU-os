/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <spinlock.h>
#include <bsp/pci.h>
#include <serial.h>
#include <io.h>
#include <mmu.h>
#include <cpu.h>
#include <asm/platform.h>

#define PL011_DR		0x000U
#define PL011_FR		0x018U
#define PL011_IBRD		0x024U
#define PL011_FBRD		0x028U
#define PL011_LCRH		0x02CU
#define PL011_CR		0x030U
#define PL011_IMSC		0x038U
#define PL011_ICR		0x044U

#define PL011_FR_TXFF		(1U << 5U)
#define PL011_FR_RXFE		(1U << 4U)
#define PL011_LCRH_FEN		(1U << 4U)
#define PL011_LCRH_WLEN_8	(3U << 5U)
#define PL011_CR_UARTEN		(1U << 0U)
#define PL011_CR_TXE		(1U << 8U)
#define PL011_CR_RXE		(1U << 9U)
#define PL011_INT_ALL		0x7ffU

struct pl011_uart {
	bool enabled;
	void *mmio_base_vaddr;
	spinlock_t rx_lock;
	spinlock_t tx_lock;
};

#if defined(CONFIG_SERIAL_MMIO_BASE)
static struct pl011_uart uart = {
	.enabled = true,
	.mmio_base_vaddr = (void *)CONFIG_SERIAL_MMIO_BASE,
};
#else
static struct pl011_uart uart = {
	.enabled = true,
	.mmio_base_vaddr = NULL,
};
#endif

static inline uint32_t pl011_read_reg(uint32_t reg_idx)
{
	return mmio_read32(uart.mmio_base_vaddr + reg_idx);
}

static inline void pl011_write_reg(uint32_t val, uint32_t reg_idx)
{
	mmio_write32(val, uart.mmio_base_vaddr + reg_idx);
}

void serial_init(bool early_boot)
{
	void *mmio_base_va = NULL;

	if (!uart.enabled) {
		return;
	}

	if (uart.mmio_base_vaddr == NULL) {
		uart.mmio_base_vaddr = (void *)beau_config.console_mmio_base;
	}

	if (!early_boot) {
		mmio_base_va = hpa2hva(hva2hpa_early(uart.mmio_base_vaddr));
		if (mmio_base_va != NULL) {
			set_paging_supervisor((uint64_t)mmio_base_va, PAGE_SIZE);
		}
		return;
	}

	spinlock_init(&uart.rx_lock);
	spinlock_init(&uart.tx_lock);

	pl011_write_reg(0U, PL011_CR);
	pl011_write_reg(PL011_INT_ALL, PL011_ICR);
	pl011_write_reg(0U, PL011_IMSC);
	pl011_write_reg(1U, PL011_IBRD);
	pl011_write_reg(40U, PL011_FBRD);
	pl011_write_reg(PL011_LCRH_WLEN_8 | PL011_LCRH_FEN, PL011_LCRH);
	pl011_write_reg(PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE, PL011_CR);
}

char serial_getc(void)
{
	char ret = -1;
	uint64_t rflags;

	if (!uart.enabled) {
		return ret;
	}

	spinlock_irqsave_obtain(&uart.rx_lock, &rflags);
	if ((pl011_read_reg(PL011_FR) & PL011_FR_RXFE) == 0U) {
		ret = (char)(pl011_read_reg(PL011_DR) & 0xffU);
	}
	spinlock_irqrestore_release(&uart.rx_lock, rflags);

	return ret;
}

static void pl011_putc(char c)
{
	while ((pl011_read_reg(PL011_FR) & PL011_FR_TXFF) != 0U) {
		cpu_relax();
	}
	pl011_write_reg((uint32_t)c, PL011_DR);
}

size_t serial_puts(const char *buf, uint32_t len)
{
	uint32_t i;
	uint64_t rflags;

	if (!uart.enabled) {
		return len;
	}

	spinlock_irqsave_obtain(&uart.tx_lock, &rflags);
	for (i = 0U; i < len; i++) {
		if (buf[i] == '\n') {
			pl011_putc('\r');
			pl011_putc('\n');
			if (((i + 1U) < len) && (buf[i + 1U] == '\r')) {
				i++;
			}
		} else if (buf[i] == '\r') {
			pl011_putc('\r');
			if (((i + 1U) < len) && (buf[i + 1U] == '\n')) {
				pl011_putc('\n');
				i++;
			}
		} else {
			pl011_putc(buf[i]);
		}
	}
	spinlock_irqrestore_release(&uart.tx_lock, rflags);

	return len;
}

void serial_set_property(bool enabled, enum serial_dev_type uart_type, uint64_t data)
{
	uart.enabled = enabled;

	if (uart_type == PL011) {
		uart.mmio_base_vaddr = (void *)data;
	}
}

bool is_pci_dbg_uart(__unused union pci_bdf bdf_value)
{
	return false;
}

bool get_pio_dbg_uart_cfg(__unused uint16_t *pio_address, __unused uint32_t *nbytes)
{
	return false;
}
