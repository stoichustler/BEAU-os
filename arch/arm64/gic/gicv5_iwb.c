/*-
 * Copyright (c) 2025 Arm Ltd
 * Copyright (c) 2026 Hustler Lo
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <io.h>
#include <logmsg.h>
#include <spinlock.h>

#define	IWB_IDR0		0x0000U
#define	 IDR0_IW_RANGE_SHIFT	0U
#define	 IDR0_IW_RANGE_MASK	(0x7ffU << IDR0_IW_RANGE_SHIFT)
#define	 IDR0_IW_RANGE_IRQs(x)	((((x) & IDR0_IW_RANGE_MASK) + 1U) * 32U)
#define	IWB_IIDR		0x0040U
#define	IWB_AIDR		0x0044U
#define	IWB_CR0			0x0080U
#define	 CR0_IWBEN		(0x1U << 0U)
#define	IWB_WENABLE_STATUSR	0x00C0U
#define	 WENABLE_STATUSR_IDLE	(0x1U << 0U)
#define	IWB_WRESAMPLER		0x00C8U
#define	IWB_WENABLER(irq)	(0x2000U + (4U * ((irq) / 32U)))
#define	 WENABLER_MASK(irq)	(0x1U << ((irq) % 32U))
#define	 WENABLER_ENABLED(irq)	(0x1U << ((irq) % 32U))
#define	IWB_WTMR(irq)		(0x4000U + (4U * ((irq) / 32U)))
#define	 WTMR_MASK(irq)		(0x1U << ((irq) % 32U))
#define	 WTMR_LEVEL(irq)	(0x1U << ((irq) % 32U))

#define	BEAU_GICV5_IWB_WAIT_RETRIES	10000U

static uint64_t beau_gicv5_iwb_base;
static uint64_t beau_gicv5_iwb_size;
static uint32_t beau_gicv5_iwb_nirq;
static bool beau_gicv5_iwb_ready;
static spinlock_t beau_gicv5_iwb_lock = { .head = 0U, .tail = 0U };

static inline void *beau_gicv5_iwb_addr(uint32_t off)
{
	return (void *)(beau_gicv5_iwb_base + off);
}

static inline uint32_t beau_gicv5_iwb_read_4(uint32_t off)
{
	return mmio_read32(beau_gicv5_iwb_addr(off));
}

static inline void beau_gicv5_iwb_write_4(uint32_t off, uint32_t val)
{
	mmio_write32(val, beau_gicv5_iwb_addr(off));
}

static bool beau_gicv5_iwb_valid_irq(uint32_t irq)
{
	return beau_gicv5_iwb_ready && (irq < beau_gicv5_iwb_nirq);
}

static int32_t beau_gicv5_iwb_wait_for_wenabler(void)
{
	uint32_t i;

	for (i = 0U; i < BEAU_GICV5_IWB_WAIT_RETRIES; i++) {
		uint32_t reg = beau_gicv5_iwb_read_4(IWB_WENABLE_STATUSR);

		if ((reg & WENABLE_STATUSR_IDLE) != 0U) {
			return 0;
		}
		cpu_relax();
	}

	return -ETIMEDOUT;
}

void beau_gicv5_iwb_init(uint64_t base, uint64_t size)
{
	uint32_t cr0;
	uint32_t i;
	uint64_t flags;

	beau_gicv5_iwb_base = base;
	beau_gicv5_iwb_size = size;
	beau_gicv5_iwb_nirq = 0U;
	beau_gicv5_iwb_ready = false;

	if ((base == 0UL) || (size == 0UL)) {
		return;
	}

	cr0 = beau_gicv5_iwb_read_4(IWB_CR0);
	if ((cr0 & CR0_IWBEN) == 0U) {
		LOG_WRN("gicv5 iwb disabled by firmware: cr0=0x%x", cr0);
		return;
	}

	beau_gicv5_iwb_nirq = IDR0_IW_RANGE_IRQs(beau_gicv5_iwb_read_4(IWB_IDR0));

	spinlock_irqsave_obtain(&beau_gicv5_iwb_lock, &flags);
	for (i = 0U; i < beau_gicv5_iwb_nirq; i += 32U) {
		beau_gicv5_iwb_write_4(IWB_WENABLER(i), 0U);
	}
	if (beau_gicv5_iwb_wait_for_wenabler() == 0) {
		beau_gicv5_iwb_ready = true;
	}
	spinlock_irqrestore_release(&beau_gicv5_iwb_lock, flags);

	if (beau_gicv5_iwb_ready) {
		LOG_INF("gicv5 iwb at 0x%016lx (0x%08lx), wires=%u",
			beau_gicv5_iwb_base, beau_gicv5_iwb_size, beau_gicv5_iwb_nirq);
	} else {
		LOG_WRN("gicv5 iwb enable status timeout");
	}
}

bool beau_gicv5_iwb_present(void)
{
	return beau_gicv5_iwb_ready;
}

uint32_t beau_gicv5_iwb_irq_count(void)
{
	return beau_gicv5_iwb_ready ? beau_gicv5_iwb_nirq : 0U;
}

int32_t beau_gicv5_iwb_enable(uint32_t irq)
{
	uint32_t reg;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv5_iwb_valid_irq(irq)) {
		return -ENODEV;
	}

	spinlock_irqsave_obtain(&beau_gicv5_iwb_lock, &flags);
	reg = beau_gicv5_iwb_read_4(IWB_WENABLER(irq));
	reg |= WENABLER_ENABLED(irq);
	beau_gicv5_iwb_write_4(IWB_WENABLER(irq), reg);
	ret = beau_gicv5_iwb_wait_for_wenabler();
	spinlock_irqrestore_release(&beau_gicv5_iwb_lock, flags);

	return ret;
}

int32_t beau_gicv5_iwb_disable(uint32_t irq)
{
	uint32_t reg;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv5_iwb_valid_irq(irq)) {
		return -ENODEV;
	}

	spinlock_irqsave_obtain(&beau_gicv5_iwb_lock, &flags);
	reg = beau_gicv5_iwb_read_4(IWB_WENABLER(irq));
	reg &= ~WENABLER_MASK(irq);
	beau_gicv5_iwb_write_4(IWB_WENABLER(irq), reg);
	ret = beau_gicv5_iwb_wait_for_wenabler();
	spinlock_irqrestore_release(&beau_gicv5_iwb_lock, flags);

	return ret;
}

int32_t beau_gicv5_iwb_set_level(uint32_t irq, bool level)
{
	uint32_t reg;
	uint64_t flags;

	if (!beau_gicv5_iwb_valid_irq(irq)) {
		return -ENODEV;
	}

	spinlock_irqsave_obtain(&beau_gicv5_iwb_lock, &flags);
	reg = beau_gicv5_iwb_read_4(IWB_WTMR(irq));
	reg &= ~WTMR_MASK(irq);
	if (level) {
		reg |= WTMR_LEVEL(irq);
	}
	beau_gicv5_iwb_write_4(IWB_WTMR(irq), reg);
	spinlock_irqrestore_release(&beau_gicv5_iwb_lock, flags);

	return 0;
}
