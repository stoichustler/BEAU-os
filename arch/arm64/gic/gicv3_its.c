/*-
 * Copyright (c) 2015-2016 The FreeBSD Foundation
 * Copyright (c) 2023 Arm Ltd
 * Copyright (c) 2026 Hustler Lo
 *
 * This software was developed by Andrew Turner under
 * the sponsorship of the FreeBSD Foundation.
 *
 * This software was developed by Semihalf under
 * the sponsorship of the FreeBSD Foundation.
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
#include <asm/irq.h>

#ifndef PAGE_SIZE_64K
#define	PAGE_SIZE_64K		0x10000UL
#endif

#include "gicv3_reg.h"

#define	GITS_CTLR_QUIESCENT	(1U << 31U)
#define	GITS_WAIT_RETRIES	1000000U
#define	BEAU_GICV3_ITS_MAX_VECTORS	1024U

static uint64_t beau_gicv3_its_base;
static uint64_t beau_gicv3_its_size;
static bool beau_gicv3_its_ready;

struct beau_gicv3_its_irqsrc {
	bool		used;
	bool		msix;
	uint32_t	dev_id;
	uint32_t	event_id;
	uint32_t	lpi;
};

static spinlock_t beau_gicv3_its_lock = { .head = 0U, .tail = 0U };
static struct beau_gicv3_its_irqsrc beau_gicv3_its_irqs[BEAU_GICV3_ITS_MAX_VECTORS];

static inline void *beau_gits_addr(uint32_t off)
{
	return (void *)(beau_gicv3_its_base + off);
}

static inline uint32_t beau_gits_read_4(uint32_t off)
{
	return mmio_read32(beau_gits_addr(off));
}

static inline uint64_t beau_gits_read_8(uint32_t off)
{
	return mmio_read64(beau_gits_addr(off));
}

static inline void beau_gits_write_4(uint32_t off, uint32_t val)
{
	mmio_write32(val, beau_gits_addr(off));
}

void beau_gicv3_its_init(uint64_t base, uint64_t size)
{
	uint32_t retries;

	beau_gicv3_its_base = base;
	beau_gicv3_its_size = size;
	beau_gicv3_its_ready = false;

	if ((base == 0UL) || (size == 0UL)) {
		return;
	}

	beau_gits_write_4(GITS_CTLR, 0U);
	for (retries = GITS_WAIT_RETRIES; retries > 0U; retries--) {
		uint32_t ctlr = beau_gits_read_4(GITS_CTLR);

		if ((ctlr & GITS_CTLR_QUIESCENT) != 0U) {
			beau_gicv3_its_ready = true;
			LOG_INF("GICv3 ITS at 0x%08lx (0x%08lx)", base, size);
			return;
		}
		cpu_relax();
	}

	panic("gicv3 its quiesce timeout base=0x%lx size=0x%lx ctlr=0x%x",
		base, size, beau_gits_read_4(GITS_CTLR));
}

bool beau_gicv3_its_present(void)
{
	return beau_gicv3_its_ready;
}

static int32_t beau_gicv3_its_find_free_run(uint32_t count, uint32_t *first)
{
	uint32_t run = 0U;
	uint32_t start = 0U;
	uint32_t i;

	if ((count == 0U) || (count > BEAU_GICV3_ITS_MAX_VECTORS) || (first == NULL)) {
		return -EINVAL;
	}

	for (i = 0U; i < BEAU_GICV3_ITS_MAX_VECTORS; i++) {
		if (!beau_gicv3_its_irqs[i].used) {
			if (run == 0U) {
				start = i;
			}
			run++;
			if (run == count) {
				*first = start;
				return 0;
			}
		} else {
			run = 0U;
		}
	}

	return -ENOMEM;
}

static struct beau_gicv3_its_irqsrc *beau_gicv3_its_find_irq(uint32_t lpi)
{
	uint32_t idx;

	if ((lpi < GIC_FIRST_LPI) ||
		(lpi >= (GIC_FIRST_LPI + BEAU_GICV3_ITS_MAX_VECTORS))) {
		return NULL;
	}

	idx = lpi - GIC_FIRST_LPI;
	return beau_gicv3_its_irqs[idx].used ? &beau_gicv3_its_irqs[idx] : NULL;
}

static bool beau_gicv3_its_event_busy(uint32_t dev_id, uint32_t event_id)
{
	uint32_t i;

	for (i = 0U; i < BEAU_GICV3_ITS_MAX_VECTORS; i++) {
		if (beau_gicv3_its_irqs[i].used &&
			(beau_gicv3_its_irqs[i].dev_id == dev_id) &&
			(beau_gicv3_its_irqs[i].event_id == event_id)) {
			return true;
		}
	}

	return false;
}

static int32_t beau_gicv3_its_fill_msg(const struct beau_gicv3_its_irqsrc *irq,
	struct arm64_gicv3_msi_msg *msg)
{
	if ((irq == NULL) || (msg == NULL)) {
		return -EINVAL;
	}
	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}

	msg->addr = beau_gicv3_its_base + GITS_TRANSLATER;
	msg->data = irq->event_id;

	return 0;
}

int32_t arm64_gicv3_its_alloc_msi(uint32_t dev_id, uint32_t count, uint32_t *first_lpi,
	struct arm64_gicv3_msi_msg *msgs)
{
	uint32_t first;
	uint32_t i;
	uint32_t j;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}
	if ((count == 0U) || (first_lpi == NULL)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	ret = beau_gicv3_its_find_free_run(count, &first);
	if (ret != 0) {
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return ret;
	}

	for (i = 0U; i < count; i++) {
		struct beau_gicv3_its_irqsrc *irq = &beau_gicv3_its_irqs[first + i];

		irq->used = true;
		irq->msix = false;
		irq->dev_id = dev_id;
		irq->event_id = i;
		irq->lpi = GIC_FIRST_LPI + first + i;
		if (msgs != NULL) {
			ret = beau_gicv3_its_fill_msg(irq, &msgs[i]);
			if (ret != 0) {
				for (j = 0U; j <= i; j++) {
					beau_gicv3_its_irqs[first + j].used = false;
				}
				spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
				return ret;
			}
		}
	}

	*first_lpi = GIC_FIRST_LPI + first;
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return 0;
}

void arm64_gicv3_its_release_msi(uint32_t dev_id, uint32_t first_lpi, uint32_t count)
{
	uint32_t i;
	uint64_t flags;

	if ((count == 0U) || (first_lpi < GIC_FIRST_LPI)) {
		return;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	for (i = 0U; i < count; i++) {
		struct beau_gicv3_its_irqsrc *irq = beau_gicv3_its_find_irq(first_lpi + i);

		if ((irq != NULL) && !irq->msix && (irq->dev_id == dev_id)) {
			irq->used = false;
		}
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
}

int32_t arm64_gicv3_its_alloc_msix(uint32_t dev_id, uint32_t vector, uint32_t *lpi,
	struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint32_t first;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}
	if (lpi == NULL) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	if (beau_gicv3_its_event_busy(dev_id, vector)) {
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return -EBUSY;
	}

	ret = beau_gicv3_its_find_free_run(1U, &first);
	if (ret != 0) {
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return ret;
	}

	irq = &beau_gicv3_its_irqs[first];
	irq->used = true;
	irq->msix = true;
	irq->dev_id = dev_id;
	irq->event_id = vector;
	irq->lpi = GIC_FIRST_LPI + first;

	if (msg != NULL) {
		ret = beau_gicv3_its_fill_msg(irq, msg);
		if (ret != 0) {
			irq->used = false;
			spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
			return ret;
		}
	}

	*lpi = irq->lpi;
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return 0;
}

void arm64_gicv3_its_release_msix(uint32_t dev_id, uint32_t lpi)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint64_t flags;

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	irq = beau_gicv3_its_find_irq(lpi);
	if ((irq != NULL) && irq->msix && (irq->dev_id == dev_id)) {
		irq->used = false;
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
}

int32_t arm64_gicv3_its_map_msi(uint32_t lpi, struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	irq = beau_gicv3_its_find_irq(lpi);
	ret = beau_gicv3_its_fill_msg(irq, msg);
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return ret;
}
