/*
 * Copyright (C) 2021-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <bits.h>
#include <irq.h>
#include <common/softirq.h>
#include <per_cpu.h>
#include <rtl.h>
#if CONFIG_IRQSTAT_LATENCY
#include <ticks.h>
#endif

static spinlock_t irq_alloc_spinlock = { .head = 0U, .tail = 0U, };

#if CONFIG_IRQSTAT_LATENCY
struct irq_latency_accum {
	uint64_t count;
	uint64_t min_ticks;
	uint64_t max_ticks;
	uint64_t sum_ticks;
};
#endif

uint64_t irq_alloc_bitmap[IRQ_ALLOC_BITMAP_SIZE];
struct irq_desc irq_desc_array[NR_IRQS];
static uint64_t irq_rsvd_bitmap[IRQ_ALLOC_BITMAP_SIZE];
#if CONFIG_IRQSTAT_LATENCY
static struct irq_latency_accum irq_latency[NR_IRQS];

static void irq_record_latency(uint32_t irq, uint64_t delta_ticks)
{
	struct irq_latency_accum *latency = &irq_latency[irq];

	if (latency->count == 0UL) {
		latency->min_ticks = delta_ticks;
		latency->max_ticks = delta_ticks;
	} else {
		if (delta_ticks < latency->min_ticks) {
			latency->min_ticks = delta_ticks;
		}
		if (delta_ticks > latency->max_ticks) {
			latency->max_ticks = delta_ticks;
		}
	}
	if (latency->sum_ticks <= (UINT64_MAX - delta_ticks)) {
		latency->sum_ticks += delta_ticks;
	} else {
		latency->sum_ticks = UINT64_MAX;
	}
	if (latency->count != UINT64_MAX) {
		latency->count++;
	}
}
#endif

void get_irq_latency_stats(uint32_t irq, struct irq_latency_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	(void)memset(stats, 0U, sizeof(*stats));
#if CONFIG_IRQSTAT_LATENCY
	if (irq < NR_IRQS) {
		struct irq_desc *desc = &irq_desc_array[irq];
		const struct irq_latency_accum *latency = &irq_latency[irq];
		uint64_t flags;

		spinlock_irqsave_obtain(&desc->lock, &flags);
		stats->count = latency->count;
		if (latency->count != 0UL) {
			stats->min_us = ticks_to_us(latency->min_ticks);
			stats->avg_us = ticks_to_us(latency->sum_ticks / latency->count);
			stats->max_us = ticks_to_us(latency->max_ticks);
		}
		spinlock_irqrestore_release(&desc->lock, flags);
	}
#else
	(void)irq;
#endif
}

/*
 * alloc an free irq if req_irq is IRQ_INVALID, or else set assigned
 * return: irq num on success, IRQ_INVALID on failure
 */
static uint32_t alloc_irq_num(uint32_t req_irq, bool reserve)
{
	uint32_t irq = req_irq;
	uint64_t rflags;
	uint32_t ret;

	if ((irq >= NR_IRQS) && (irq != IRQ_INVALID)) {
		LOG_ERR("[%s] invalid req_irq %u", __func__, req_irq);
	        ret = IRQ_INVALID;
	} else {
		spinlock_irqsave_obtain(&irq_alloc_spinlock, &rflags);
		if (irq == IRQ_INVALID) {
			/* if no valid irq num given, find a free one */
			irq = (uint32_t)ffz64_ex(irq_alloc_bitmap, NR_IRQS);
		}

		if (irq >= NR_IRQS) {
			irq = IRQ_INVALID;
		} else {
			bitmap_set_non_atomic((uint16_t)(irq & 0x3FU),
					irq_alloc_bitmap + (irq >> 6U));
			if (reserve) {
				bitmap_set_non_atomic((uint16_t)(irq & 0x3FU),
						irq_rsvd_bitmap + (irq >> 6U));
			}
		}
		spinlock_irqrestore_release(&irq_alloc_spinlock, rflags);
		ret = irq;
	}
	return ret;
}

uint32_t reserve_irq_num(uint32_t irq)
{
	return alloc_irq_num(irq, true);
}

/*
 * @pre: irq is not in irq_static_mappings
 * free irq num allocated via alloc_irq_num()
 */
static void free_irq_num(uint32_t irq)
{
	uint64_t rflags;

	if (irq < NR_IRQS) {
		spinlock_irqsave_obtain(&irq_alloc_spinlock, &rflags);

		if (bitmap_test((uint16_t)(irq & 0x3FU),
			irq_rsvd_bitmap + (irq >> 6U)) == false) {
			bitmap_clear_non_atomic((uint16_t)(irq & 0x3FU),
					irq_alloc_bitmap + (irq >> 6U));
		}
		spinlock_irqrestore_release(&irq_alloc_spinlock, rflags);
	}
}

void free_irq(uint32_t irq)
{
	uint64_t rflags;
	struct irq_desc *desc;

	if (irq < NR_IRQS) {
		desc = &irq_desc_array[irq];

		spinlock_irqsave_obtain(&desc->lock, &rflags);
		desc->action = NULL;
		desc->priv_data = NULL;
		desc->flags = IRQF_NONE;
		spinlock_irqrestore_release(&desc->lock, rflags);

		arch_free_irq(irq);
		free_irq_num(irq);
	}
}

/*
 * There are four cases as to irq/vector allocation:
 * case 1: req_irq = IRQ_INVALID
 *      caller did not know which irq to use, and want system to
 *      allocate available irq for it. These irq are in range:
 *      nr_gsi ~ NR_IRQS
 *      an irq will be allocated and a vector will be assigned to this
 *      irq automatically.
 * case 2: req_irq >= NR_LAGACY_IRQ and irq < nr_gsi
 *      caller want to add device ISR handler into ioapic pins.
 *      a vector will automatically assigned.
 * case 3: req_irq >=0 and req_irq < NR_LEGACY_IRQ
 *      caller want to add device ISR handler into ioapic pins, which
 *      is a legacy irq, vector already reserved.
 *      Nothing to do in this case.
 * case 4: irq with speical type (not from IOAPIC/MSI)
 *      These irq value are pre-defined for Timer, IPI, Spurious etc,
 *      which is listed in irq_static_mappings[].
 *	Nothing to do in this case.
 *
 * return value: valid irq (>=0) on success, otherwise errno (< 0).
 */
int32_t request_irq(uint32_t req_irq, irq_action_t action_fn, void *priv_data,
			uint32_t flags)
{
	struct irq_desc *desc;
	uint32_t irq;
	uint64_t rflags;
	int32_t ret;

	irq = alloc_irq_num(req_irq, false);
	if (irq == IRQ_INVALID) {
		LOG_ERR("[%s] invalid irq num", __func__);
		ret = -EINVAL;
	} else {
		if (!arch_request_irq(irq)) {
			LOG_ERR("[%s] failed to alloc vector for irq %u",
				__func__, irq);
			free_irq_num(irq);
			ret = -EINVAL;
		} else {
			desc = &irq_desc_array[irq];
			if (desc->action == NULL) {
				spinlock_irqsave_obtain(&desc->lock, &rflags);
				desc->flags = flags;
				desc->priv_data = priv_data;
				desc->action = action_fn;
				spinlock_irqrestore_release(&desc->lock, rflags);
				ret = (int32_t)irq;
			} else {
				ret = -EBUSY;
				LOG_ERR("%s: request irq(%u) failed, already requested",
				       __func__, irq);
			}
		}
	}

	return ret;
}

void set_irq_trigger_mode(uint32_t irq, bool is_level_triggered)
{
	uint64_t rflags;
	struct irq_desc *desc;

	if (irq < NR_IRQS) {
		desc = &irq_desc_array[irq];
		spinlock_irqsave_obtain(&desc->lock, &rflags);
		if (is_level_triggered) {
			desc->flags |= IRQF_LEVEL;
		} else {
			desc->flags &= ~IRQF_LEVEL;
		}
		spinlock_irqrestore_release(&desc->lock, rflags);
	}
}

static inline void handle_irq(const struct irq_desc *desc)
{
	irq_action_t action = desc->action;

	arch_pre_irq(desc);

	if (action != NULL) {
		action(desc->irq, desc->priv_data);
	}

	arch_post_irq(desc);
}

static void do_irq_common(const uint32_t irq, bool handle_softirq)
{
	struct irq_desc *desc;

	if (irq < NR_IRQS) {
		desc = &irq_desc_array[irq];
		count_irq(irq);

		/* XXX irq_alloc_bitmap is used lockless here */
		if (bitmap_test((uint16_t)(irq & 0x3FU), irq_alloc_bitmap + (irq >> 6U))) {
#if CONFIG_IRQSTAT_LATENCY
			uint64_t start_ticks = cpu_ticks();
			uint64_t latency_flags;

			handle_irq(desc);
			spinlock_irqsave_obtain(&desc->lock, &latency_flags);
			irq_record_latency(irq, cpu_ticks() - start_ticks);
			spinlock_irqrestore_release(&desc->lock, latency_flags);
#else
			handle_irq(desc);
#endif
		}
	}

	if (handle_softirq) {
		do_softirq();
	}
}

void count_irq(const uint32_t irq)
{
	if (irq < NR_IRQS) {
		uint64_t *count = &per_cpu(irq_count, get_pcpu_id())[irq];

		if (*count != UINT64_MAX) {
			(*count)++;
		}
	}
}

void do_irq(const uint32_t irq)
{
	do_irq_common(irq, true);
}

void do_irq_no_softirq(const uint32_t irq)
{
	do_irq_common(irq, false);
}

static void init_irq_descs(void)
{
	uint32_t i;

	for (i = 0U; i < NR_IRQS; i++) {
		struct irq_desc *desc = &irq_desc_array[i];

		desc->irq = i;
		desc->arch_data = NULL;
		desc->action = NULL;
		desc->priv_data = NULL;
		desc->flags = IRQF_NONE;
		spinlock_init(&desc->lock);
	}

	arch_init_irq_descs(irq_desc_array);
}

void init_interrupt(uint16_t pcpu_id)
{
	arch_init_interrupt(pcpu_id);

	if (pcpu_id == BSP_CPU_ID) {
		init_irq_descs();
		arch_setup_irqs();
		init_softirq();
	}

	local_irq_enable();
}
