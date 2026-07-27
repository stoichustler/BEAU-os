/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <per_cpu.h>
#include <sprintf.h>
#include <util.h>
#include <asm/irq.h>
#include <asm/guest/vgicv3.h>

#include "shell_cmds.h"

#ifdef CONFIG_ARM64
static struct arm64_vgic_irq_stats shell_irqstat_vgic_stats[ARM64_VGIC_IRQSTAT_MAX];
#endif
struct irqstat_total {
	uint64_t count;
	bool overflow;
};

struct irqstat_snapshot {
	uint64_t count[MAX_PCPU_NUM];
	struct irqstat_total total;
	struct irq_latency_stats latency;
	bool show;
};

static void irqstat_add_count(struct irqstat_total *total, uint64_t count)
{
	if ((count == UINT64_MAX) || (total->count > (UINT64_MAX - count))) {
		total->count = UINT64_MAX;
		total->overflow = true;
	} else {
		total->count += count;
	}
}

static void irqstat_take_snapshot(uint32_t irq, uint16_t pcpu_num,
	struct irqstat_snapshot *snapshot)
{
	uint16_t pcpu_id;
	bool has_handler;

	has_handler = irq_desc_array[irq].action != NULL;
	(void)memset(snapshot, 0U, sizeof(*snapshot));

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		snapshot->count[pcpu_id] = per_cpu(irq_count, pcpu_id)[irq];
		irqstat_add_count(&snapshot->total, snapshot->count[pcpu_id]);
	}

	snapshot->show = (snapshot->total.count != 0UL) || has_handler;
#if CONFIG_IRQSTAT_LATENCY
	get_irq_latency_stats(irq, &snapshot->latency);
#endif
}

#if CONFIG_IRQSTAT_LATENCY
static void shell_irqstat_format_latency(char *buf, size_t size,
	const struct irq_latency_stats *latency)
{
	if ((latency == NULL) || (latency->count == 0UL)) {
		snprintf(buf, size, "-");
	} else {
		uint64_t min_ms = latency->min_us / 1000UL;
		uint64_t avg_ms = latency->avg_us / 1000UL;
		uint64_t max_ms = latency->max_us / 1000UL;
		uint64_t min_frac = latency->min_us % 1000UL;
		uint64_t avg_frac = latency->avg_us % 1000UL;
		uint64_t max_frac = latency->max_us % 1000UL;

		snprintf(buf, size, "%lu.%03lums/%lu.%03lums/%lu.%03lums",
			min_ms, min_frac, avg_ms, avg_frac, max_ms, max_frac);
	}
}
#endif

static void shell_print_irq_cpu_headers(uint16_t pcpu_num)
{
	char temp_str[16U];
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		snprintf(temp_str, sizeof(temp_str), "cpu%-6hu", pcpu_id);
		shell_puts(temp_str);
	}
}

static void shell_print_irq_cpu_counts(const struct irqstat_snapshot *snapshot,
	uint16_t pcpu_num)
{
	char token[32U];
	uint16_t pcpu_id;
	uint64_t count;

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		count = snapshot->count[pcpu_id];

		if (count == UINT64_MAX) {
			snprintf(token, sizeof(token), " %-8s", "sat");
		} else {
			snprintf(token, sizeof(token), " %-8lu", count);
		}
		shell_puts(token);
	}
}

#if defined(CONFIG_ARM64) && CONFIG_IRQSTAT_LATENCY
static void shell_irqstat_format_vgic_latency(char *buf, size_t size,
	const struct arm64_vgic_irq_latency_stats *latency)
{
	if ((latency == NULL) || (latency->count == 0UL)) {
		snprintf(buf, size, "-");
	} else {
		uint64_t min_ms = latency->min_us / 1000UL;
		uint64_t avg_ms = latency->avg_us / 1000UL;
		uint64_t max_ms = latency->max_us / 1000UL;
		uint64_t min_frac = latency->min_us % 1000UL;
		uint64_t avg_frac = latency->avg_us % 1000UL;
		uint64_t max_frac = latency->max_us % 1000UL;

		snprintf(buf, size, "%lu.%03lums/%lu.%03lums/%lu.%03lums",
			min_ms, min_frac, avg_ms, avg_frac, max_ms, max_frac);
	}
}
#endif

#ifdef CONFIG_ARM64
static void shell_print_guest_irqstat(void)
{
	struct arm64_vgic_irqstat_summary summary;
	uint16_t count;
	uint16_t idx;

	/*
	 * Guest vIRQ latency columns:
	 *
	 *   source raise/assert
	 *          |
	 *          | raise-lr
	 *          v
	 *   vGIC writes a hardware LR
	 *          |
	 *          | lr-eoi
	 *          v
	 *   guest EOI/deactivation observed by EL2
	 *
	 * Keeping both segments visible is more useful than a single end-to-end
	 * number. It separates host-side delivery delay from guest-side interrupt
	 * handling/completion delay.
	 *
	 * Fields: assert/deassert count level transitions; lr/eoi count virtual
	 * delivery/completion; raise-lr and lr-eoi are min/avg/max latency in us.
	 */
	count = arm64_vgicv3_get_irq_stats(shell_irqstat_vgic_stats,
		ARRAY_SIZE(shell_irqstat_vgic_stats));
	arm64_vgicv3_get_irqstat_summary(&summary);
	shell_item_section("guest virq: entries:%hu/%hu evicted:%lu dropped:%lu",
		summary.used, summary.capacity, summary.evicted, summary.dropped);
	if (count == 0U) {
		shell_item_line("(no guest-visible virtual IRQ activity)");
		return;
	}

#if CONFIG_IRQSTAT_LATENCY
	shell_item_line("vm   vcpu virq  type  live assert   deassert lr       eoi      raise-lr min/avg/max      lr-eoi min/avg/max");
	shell_item_line("──── ──── ───── ───── ──── ──────── ──────── ──────── ──────── ───────────────────────── ─────────────────────────");
#else
	shell_item_line("vm   vcpu virq  type  live assert   deassert lr       eoi");
	shell_item_line("──── ──── ───── ───── ──── ──────── ──────── ──────── ────────");
#endif
	for (idx = 0U; idx < count; idx++) {
		const struct arm64_vgic_irq_stats *entry = &shell_irqstat_vgic_stats[idx];

#if CONFIG_IRQSTAT_LATENCY
		char raise_to_lr[40U];
		char lr_to_eoi[40U];

		shell_irqstat_format_vgic_latency(raise_to_lr, sizeof(raise_to_lr),
			&entry->raise_to_lr);
		shell_irqstat_format_vgic_latency(lr_to_eoi, sizeof(lr_to_eoi),
			&entry->lr_to_eoi);
		shell_item_line("%-4hu %-4hu %-5u %-5s %-4s %-8lu %-8lu %-8lu %-8lu %-25s %-25s",
			entry->vm_id,
			entry->vcpu_id,
			entry->virq,
			entry->level ? "level" : "edge",
			entry->in_flight ? "Y" : "N",
			entry->assert_count,
			entry->deassert_count,
			entry->lr_count,
			entry->eoi_count,
			raise_to_lr,
			lr_to_eoi);
#else
		shell_item_line("%-4hu %-4hu %-5u %-5s %-4s %-8lu %-8lu %-8lu %-8lu",
			entry->vm_id,
			entry->vcpu_id,
			entry->virq,
			entry->level ? "level" : "edge",
			entry->in_flight ? "Y" : "N",
			entry->assert_count,
			entry->deassert_count,
			entry->lr_count,
			entry->eoi_count);
#endif
		shell_output_checkpoint();
	}
}
#endif

/* [20260630] irqstat monitor:
 *
 * irqstat prints two layers of interrupt accounting:
 *
 * - host IRQ handler entries from irq_desc/action + per_cpu(irq_count)
 * - ARM64 guest-visible vIRQ lifecycle and raise-to-LR / LR-to-EOI latency
 *
 * Keeping both views in one command makes it possible to distinguish a missing
 * EL2 physical IRQ from a vGIC delivery/completion problem.
 */
int32_t shell_irqstat(int32_t argc, __unused char **argv)
{
	char temp_str[MAX_STR_SIZE];
	uint16_t pcpu_num = get_pcpu_nums();
	uint32_t irq;
	uint32_t shown = 0U;

	if (argc != 1) {
		shell_puts("usage: irqstat\r\n");
		return -EINVAL;
	}

	shell_item_begin("IRQSTAT");
	/* Host active is descriptor allocation; CPU columns are entry counts; optional
	 * handler-lat is min/avg/max handler duration in us.
	 */
	shell_item_section("host pirq: nr_irqs=%u, pcpus=%hu", NR_IRQS, pcpu_num);
	shell_puts("│   irq   name             active ");
	shell_print_irq_cpu_headers(pcpu_num);
#if CONFIG_IRQSTAT_LATENCY
	shell_puts("handler-lat min/avg/max");
#endif
	shell_puts("\r\n");
	shell_puts("│   ───── ──────────────── ──────");
	for (uint16_t pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		shell_puts(" ────────");
	}
#if CONFIG_IRQSTAT_LATENCY
	shell_puts(" ─────────────────────────");
#endif
	shell_puts("\r\n");

	for (irq = 0U; irq < NR_IRQS; irq++) {
		struct irqstat_snapshot snapshot;
		bool allocated;
#if CONFIG_IRQSTAT_LATENCY
		char latency[40U];
#endif

		irqstat_take_snapshot(irq, pcpu_num, &snapshot);
		if (!snapshot.show) {
			continue;
		}

		allocated = bitmap_test((uint16_t)(irq & 0x3FU), irq_alloc_bitmap + (irq >> 6U));
#if CONFIG_IRQSTAT_LATENCY
		shell_irqstat_format_latency(latency, sizeof(latency), &snapshot.latency);
#endif
		snprintf(temp_str, MAX_STR_SIZE, "%-5u %-16s %-6s",
			irq,
			arch_irq_name(irq),
			allocated ? "Y" : "N");
		shell_puts("│   ");
		shell_puts(temp_str);
		shell_print_irq_cpu_counts(&snapshot, pcpu_num);
#if CONFIG_IRQSTAT_LATENCY
		snprintf(temp_str, MAX_STR_SIZE, " %-25s\r\n", latency);
#else
		snprintf(temp_str, MAX_STR_SIZE, "\r\n");
#endif
		shell_puts(temp_str);
		shown++;
		shell_output_checkpoint();
	}

	if (shown == 0U) {
		shell_item_line("(no active irq handlers and no interrupt counts)");
	}

#ifdef CONFIG_ARM64
	shell_print_guest_irqstat();
#endif
	shell_item_end();

	return 0;
}

uint16_t sanitize_vmid(uint16_t vmid)
{
	uint16_t sanitized_vmid = vmid;
	char temp_str[TEMP_STR_SIZE];

	if (vmid >= CONFIG_MAX_VM_NUM) {
		snprintf(temp_str, TEMP_STR_SIZE,
			"vm id given exceeds the max_vm_num(%u), using 0 instead\r\n",
			CONFIG_MAX_VM_NUM);
		shell_puts(temp_str);
		sanitized_vmid = 0U;
	}

	return sanitized_vmid;
}
