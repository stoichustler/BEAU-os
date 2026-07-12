/*-
 * Copyright (c) 2015-2016 The FreeBSD Foundation
 * Copyright (c) 2023,2025 Arm Ltd
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
#include <logmsg.h>
#include <rtl.h>
#include <spinlock.h>
#include <asm/irq.h>

#include "gicv5_reg.h"

/* ITS Config Frame */
#define	ITS_IDR0			0x0000U
#define	ITS_IDR1			0x0004U
#define	ITS_IDR2			0x0008U
#define	ITS_IIDR			0x0040U
#define	ITS_AIDR			0x0044U
#define	ITS_CR0				0x0080U
#define	 ITS_CR0_IDLE			(0x1U << 1U)
#define	 ITS_CR0_ITSEN			(0x1U << 0U)

/* ITS Translate Frame */
#define	ITS_TRANSLATER			0x0000U

#define	BEAU_GICV5_FIRST_LPI		8192U
#define	BEAU_GICV5_ITS_MAX_VECTORS	1024U

struct beau_gicv5_its_irqsrc {
	bool		used;
	bool		msix;
	uint32_t	dev_id;
	uint32_t	event_id;
	uint32_t	lpi;
};

static uint64_t beau_gicv5_its_base;
static uint64_t beau_gicv5_its_size;
static bool beau_gicv5_its_ready;
static uint32_t beau_gicv5_its_alloc_msi_ok;
static uint32_t beau_gicv5_its_alloc_msi_fail;
static uint32_t beau_gicv5_its_alloc_msix_ok;
static uint32_t beau_gicv5_its_alloc_msix_fail;
static uint32_t beau_gicv5_its_release_msi;
static uint32_t beau_gicv5_its_release_msix;
static spinlock_t beau_gicv5_its_lock = { .head = 0U, .tail = 0U };
static struct beau_gicv5_its_irqsrc beau_gicv5_its_irqs[BEAU_GICV5_ITS_MAX_VECTORS];

void beau_gicv5_its_init(uint64_t base, uint64_t size)
{
	beau_gicv5_its_base = base;
	beau_gicv5_its_size = size;
	beau_gicv5_its_ready = false;
	beau_gicv5_its_alloc_msi_ok = 0U;
	beau_gicv5_its_alloc_msi_fail = 0U;
	beau_gicv5_its_alloc_msix_ok = 0U;
	beau_gicv5_its_alloc_msix_fail = 0U;
	beau_gicv5_its_release_msi = 0U;
	beau_gicv5_its_release_msix = 0U;

	if ((base == 0UL) || (size == 0UL)) {
		return;
	}

	beau_gicv5_its_ready = true;
	LOG_INF("gicv5 its at 0x%016lx (0x%08lx)", base, size);
}

bool beau_gicv5_its_present(void)
{
	return beau_gicv5_its_ready;
}

static int32_t beau_gicv5_its_find_free_run(uint32_t count, uint32_t *first)
{
	uint32_t run = 0U;
	uint32_t start = 0U;
	uint32_t i;

	if ((count == 0U) || (count > BEAU_GICV5_ITS_MAX_VECTORS) || (first == NULL)) {
		return -EINVAL;
	}

	for (i = 0U; i < BEAU_GICV5_ITS_MAX_VECTORS; i++) {
		if (!beau_gicv5_its_irqs[i].used) {
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

static struct beau_gicv5_its_irqsrc *beau_gicv5_its_find_irq(uint32_t lpi)
{
	uint32_t idx;

	if ((lpi < BEAU_GICV5_FIRST_LPI) ||
		(lpi >= (BEAU_GICV5_FIRST_LPI + BEAU_GICV5_ITS_MAX_VECTORS))) {
		return NULL;
	}

	idx = lpi - BEAU_GICV5_FIRST_LPI;
	return beau_gicv5_its_irqs[idx].used ? &beau_gicv5_its_irqs[idx] : NULL;
}

static bool beau_gicv5_its_event_busy(uint32_t dev_id, uint32_t event_id)
{
	uint32_t i;

	for (i = 0U; i < BEAU_GICV5_ITS_MAX_VECTORS; i++) {
		if (beau_gicv5_its_irqs[i].used &&
			(beau_gicv5_its_irqs[i].dev_id == dev_id) &&
			(beau_gicv5_its_irqs[i].event_id == event_id)) {
			return true;
		}
	}

	return false;
}

static int32_t beau_gicv5_its_fill_msg(const struct beau_gicv5_its_irqsrc *irq,
	struct arm64_gicv3_msi_msg *msg)
{
	if ((irq == NULL) || (msg == NULL)) {
		return -EINVAL;
	}
	if (!beau_gicv5_its_ready) {
		return -ENODEV;
	}

	msg->addr = beau_gicv5_its_base + ITS_TRANSLATER;
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

	if (!beau_gicv5_its_ready) {
		return -ENODEV;
	}
	if ((count == 0U) || (first_lpi == NULL)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&beau_gicv5_its_lock, &flags);
	ret = beau_gicv5_its_find_free_run(count, &first);
	if (ret != 0) {
		beau_gicv5_its_alloc_msi_fail++;
		spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
		return ret;
	}

	for (i = 0U; i < count; i++) {
		struct beau_gicv5_its_irqsrc *irq = &beau_gicv5_its_irqs[first + i];

		irq->used = true;
		irq->msix = false;
		irq->dev_id = dev_id;
		irq->event_id = i;
		irq->lpi = BEAU_GICV5_FIRST_LPI + first + i;
		if (msgs != NULL) {
			ret = beau_gicv5_its_fill_msg(irq, &msgs[i]);
			if (ret != 0) {
				for (j = 0U; j <= i; j++) {
					beau_gicv5_its_irqs[first + j].used = false;
				}
				beau_gicv5_its_alloc_msi_fail++;
				spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
				return ret;
			}
		}
	}

	*first_lpi = BEAU_GICV5_FIRST_LPI + first;
	beau_gicv5_its_alloc_msi_ok++;
	spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);

	return 0;
}

void arm64_gicv3_its_release_msi(uint32_t dev_id, uint32_t first_lpi, uint32_t count)
{
	uint32_t i;
	uint64_t flags;

	if ((count == 0U) || (first_lpi < BEAU_GICV5_FIRST_LPI)) {
		return;
	}

	spinlock_irqsave_obtain(&beau_gicv5_its_lock, &flags);
	for (i = 0U; i < count; i++) {
		struct beau_gicv5_its_irqsrc *irq = beau_gicv5_its_find_irq(first_lpi + i);

		if ((irq != NULL) && !irq->msix && (irq->dev_id == dev_id)) {
			irq->used = false;
			beau_gicv5_its_release_msi++;
		}
	}
	spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
}

int32_t arm64_gicv3_its_alloc_msix(uint32_t dev_id, uint32_t vector, uint32_t *lpi,
	struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv5_its_irqsrc *irq;
	uint32_t first;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv5_its_ready) {
		return -ENODEV;
	}
	if (lpi == NULL) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&beau_gicv5_its_lock, &flags);
	if (beau_gicv5_its_event_busy(dev_id, vector)) {
		beau_gicv5_its_alloc_msix_fail++;
		spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
		return -EBUSY;
	}

	ret = beau_gicv5_its_find_free_run(1U, &first);
	if (ret != 0) {
		beau_gicv5_its_alloc_msix_fail++;
		spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
		return ret;
	}

	irq = &beau_gicv5_its_irqs[first];
	irq->used = true;
	irq->msix = true;
	irq->dev_id = dev_id;
	irq->event_id = vector;
	irq->lpi = BEAU_GICV5_FIRST_LPI + first;

	if (msg != NULL) {
		ret = beau_gicv5_its_fill_msg(irq, msg);
		if (ret != 0) {
			irq->used = false;
			beau_gicv5_its_alloc_msix_fail++;
			spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
			return ret;
		}
	}

	*lpi = irq->lpi;
	beau_gicv5_its_alloc_msix_ok++;
	spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);

	return 0;
}

void arm64_gicv3_its_release_msix(uint32_t dev_id, uint32_t lpi)
{
	struct beau_gicv5_its_irqsrc *irq;
	uint64_t flags;

	spinlock_irqsave_obtain(&beau_gicv5_its_lock, &flags);
	irq = beau_gicv5_its_find_irq(lpi);
	if ((irq != NULL) && irq->msix && (irq->dev_id == dev_id)) {
		irq->used = false;
		beau_gicv5_its_release_msix++;
	}
	spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
}

int32_t arm64_gicv3_its_map_msi(uint32_t lpi, struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv5_its_irqsrc *irq;
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&beau_gicv5_its_lock, &flags);
	irq = beau_gicv5_its_find_irq(lpi);
	ret = beau_gicv5_its_fill_msg(irq, msg);
	spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);

	return ret;
}

void arm64_gicv3_its_get_stats(struct arm64_gicv3_its_stats *stats)
{
	uint64_t flags;
	uint32_t i;

	if (stats == NULL) {
		return;
	}

	(void)memset(stats, 0U, sizeof(*stats));
	spinlock_irqsave_obtain(&beau_gicv5_its_lock, &flags);
	stats->base = beau_gicv5_its_base;
	stats->size = beau_gicv5_its_size;
	stats->vector_capacity = BEAU_GICV5_ITS_MAX_VECTORS;
	stats->alloc_msi_ok = beau_gicv5_its_alloc_msi_ok;
	stats->alloc_msi_fail = beau_gicv5_its_alloc_msi_fail;
	stats->alloc_msix_ok = beau_gicv5_its_alloc_msix_ok;
	stats->alloc_msix_fail = beau_gicv5_its_alloc_msix_fail;
	stats->release_msi = beau_gicv5_its_release_msi;
	stats->release_msix = beau_gicv5_its_release_msix;
	stats->ready = beau_gicv5_its_ready;
	for (i = 0U; i < BEAU_GICV5_ITS_MAX_VECTORS; i++) {
		if (beau_gicv5_its_irqs[i].used) {
			stats->vectors_used++;
		}
	}
	stats->vectors_programmed = stats->vectors_used;
	spinlock_irqrestore_release(&beau_gicv5_its_lock, flags);
}
