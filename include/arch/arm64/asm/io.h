/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_IO_H
#define ARM64_IO_H

#include <types.h>
#include <barrier.h>

#define HAS_ARCH_MMIO
#define IO_SPACE_BITMASK	0xffffU

static inline void writeb(uint8_t val, volatile void *addr)
{
	asm volatile ("strb %w0, %1" : : "r" (val), "Q" (*(volatile uint8_t *)addr) : "memory");
}

static inline void writew(uint16_t val, volatile void *addr)
{
	asm volatile ("strh %w0, %1" : : "r" (val), "Q" (*(volatile uint16_t *)addr) : "memory");
}

static inline void writel(uint32_t val, volatile void *addr)
{
	asm volatile ("str %w0, %1" : : "r" (val), "Q" (*(volatile uint32_t *)addr) : "memory");
}

static inline void writeq(uint64_t val, volatile void *addr)
{
	asm volatile ("str %0, %1" : : "r" (val), "Q" (*(volatile uint64_t *)addr) : "memory");
}

static inline uint8_t readb(const volatile void *addr)
{
	uint8_t val;

	asm volatile ("ldrb %w0, %1" : "=r" (val) : "Q" (*(const volatile uint8_t *)addr) : "memory");
	return val;
}

static inline uint16_t readw(const volatile void *addr)
{
	uint16_t val;

	asm volatile ("ldrh %w0, %1" : "=r" (val) : "Q" (*(const volatile uint16_t *)addr) : "memory");
	return val;
}

static inline uint32_t readl(const volatile void *addr)
{
	uint32_t val;

	asm volatile ("ldr %w0, %1" : "=r" (val) : "Q" (*(const volatile uint32_t *)addr) : "memory");
	return val;
}

static inline uint64_t readq(const volatile void *addr)
{
	uint64_t val;

	asm volatile ("ldr %0, %1" : "=r" (val) : "Q" (*(const volatile uint64_t *)addr) : "memory");
	return val;
}

static inline uint8_t arch_mmio_read8(const volatile void *addr)
{
	uint8_t val = readb(addr);

	cpu_read_memory_barrier();
	return val;
}

static inline uint16_t arch_mmio_read16(const volatile void *addr)
{
	uint16_t val = readw(addr);

	cpu_read_memory_barrier();
	return val;
}

static inline uint32_t arch_mmio_read32(const volatile void *addr)
{
	uint32_t val = readl(addr);

	cpu_read_memory_barrier();
	return val;
}

static inline uint64_t arch_mmio_read64(const volatile void *addr)
{
	uint64_t val = readq(addr);

	cpu_read_memory_barrier();
	return val;
}

static inline void arch_mmio_write8(uint8_t val, volatile void *addr)
{
	cpu_write_memory_barrier();
	writeb(val, addr);
}

static inline void arch_mmio_write16(uint16_t val, volatile void *addr)
{
	cpu_write_memory_barrier();
	writew(val, addr);
}

static inline void arch_mmio_write32(uint32_t val, volatile void *addr)
{
	cpu_write_memory_barrier();
	writel(val, addr);
}

static inline void arch_mmio_write64(uint64_t val, volatile void *addr)
{
	cpu_write_memory_barrier();
	writeq(val, addr);
}

#endif /* ARM64_IO_H */
