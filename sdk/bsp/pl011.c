/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <atomic.h>
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
#define PL011_DEBUG_TX_RETRY_MAX	1000000U

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
static volatile uint64_t uart_debug_owner;

static uint64_t serial_debug_owner_token(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	return (pcpu_id < MAX_PCPU_NUM) ? (uint64_t)pcpu_id + 1UL : 0UL;
}

static bool serial_debug_is_claimed(void)
{
	return __atomic_load_n(&uart_debug_owner, __ATOMIC_ACQUIRE) != 0UL;
}

static bool serial_debug_is_owner(void)
{
	uint64_t token = serial_debug_owner_token();

	return (token != 0UL) &&
		(__atomic_load_n(&uart_debug_owner, __ATOMIC_ACQUIRE) == token);
}

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

	if (!uart.enabled || serial_debug_is_claimed()) {
		return ret;
	}

	spinlock_irqsave_obtain(&uart.rx_lock, &rflags);
	if ((pl011_read_reg(PL011_FR) & PL011_FR_RXFE) == 0U) {
		ret = (char)(pl011_read_reg(PL011_DR) & 0xffU);
	}
	spinlock_irqrestore_release(&uart.rx_lock, rflags);

	return ret;
}

bool serial_rx_ready(void)
{
	uint64_t rflags;
	bool ready;

	if (!uart.enabled || serial_debug_is_claimed()) {
		return false;
	}

	spinlock_irqsave_obtain(&uart.rx_lock, &rflags);
	ready = (pl011_read_reg(PL011_FR) & PL011_FR_RXFE) == 0U;
	spinlock_irqrestore_release(&uart.rx_lock, rflags);

	return ready;
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
	if (serial_debug_is_claimed()) {
		return serial_debug_is_owner() ? serial_debug_puts(buf, len) : len;
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

/* [20260720] Exception-time UART ownership
 *
 *   normal console -- spinlocks -- PL011
 *                         ^
 *                         |
 *   DDB owner -- atomic claim -- raw bounded polling
 *
 * Key rule:
 *   - a DDB exception never waits for a lock held by interrupted code;
 *   - normal console access is suppressed while the raw owner is published;
 *   - only the claiming pCPU may use or release the raw channel.
 */
bool serial_debug_claim(void)
{
	uint64_t token = serial_debug_owner_token();

	if (!uart.enabled || (uart.mmio_base_vaddr == NULL) || (token == 0UL)) {
		return false;
	}

	return atomic_cmpxchg64(&uart_debug_owner, 0UL, token) == 0UL;
}

void serial_debug_release(void)
{
	uint64_t token = serial_debug_owner_token();

	if (token != 0UL) {
		(void)atomic_cmpxchg64(&uart_debug_owner, token, 0UL);
	}
}

char serial_debug_getc(void)
{
	char ret = -1;

	if (serial_debug_is_owner() &&
		((pl011_read_reg(PL011_FR) & PL011_FR_RXFE) == 0U)) {
		ret = (char)(pl011_read_reg(PL011_DR) & 0xffU);
	}

	return ret;
}

static bool pl011_debug_putc(char c)
{
	uint32_t retry = PL011_DEBUG_TX_RETRY_MAX;

	while (((pl011_read_reg(PL011_FR) & PL011_FR_TXFF) != 0U) &&
		(retry > 0U)) {
		retry--;
		cpu_relax();
	}
	if (retry == 0U) {
		return false;
	}
	pl011_write_reg((uint32_t)c, PL011_DR);
	return true;
}

size_t serial_debug_puts(const char *buf, uint32_t len)
{
	uint32_t index = 0U;

	if ((buf == NULL) || !serial_debug_is_owner()) {
		return 0U;
	}
	while (index < len) {
		if ((buf[index] == '\n') && !pl011_debug_putc('\r')) {
			break;
		}
		if (!pl011_debug_putc(buf[index])) {
			break;
		}
		index++;
	}

	return index;
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
