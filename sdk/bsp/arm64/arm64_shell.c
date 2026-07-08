/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
		 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <guest_memory.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <sprintf.h>
#include <util.h>
#include <reloc.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <vm_wdt.h>
#include <host_pm.h>
#include <schedule.h>
#include <ticks.h>
#include <console.h>
#include <vuart.h>
#include <debug/symbol.h>
#include <asm/mmu.h>
#include <asm/platform.h>
#include <asm/guest/vcpu.h>
#include <asm/guest/vgicv3.h>
#include <asm/sysreg.h>
#include "../shell_priv.h"

#define SHELL_CMD_MEM_MAP		"mmap"
#define SHELL_CMD_MEM_MAP_PARAM		NULL
#define SHELL_CMD_MEM_MAP_HELP		"list arm64 host stage-1 and vm stage-2 memory mappings"
#define SHELL_CMD_DUMPSTAT		"dumpstat"
#define SHELL_CMD_DUMPSTAT_PARAM	"[vm id]"
#define SHELL_CMD_DUMPSTAT_HELP		"dump arm64 vcpu stats and vgic/vtimer diagnostics"
#define SHELL_CMD_VMSTAT		"vmstat"
#define SHELL_CMD_VMSTAT_PARAM		NULL
#define SHELL_CMD_VMSTAT_HELP		"list arm64 vm state"
#define SHELL_CMD_RESET			"reset"
#define SHELL_CMD_RESET_PARAM		"<vm id>"
#define SHELL_CMD_RESET_HELP		"reset a non-service VM by id"
#define SHELL_CMD_REBOOT		"reboot"
#define SHELL_CMD_REBOOT_PARAM		NULL
#define SHELL_CMD_REBOOT_HELP		"trigger a system reboot (immediately)"
#define DUMPSTAT_SMP_CALL_TIMEOUT_US	1000U
#define DUMPSTAT_STACK_DEPTH		16U
#define DUMPSTAT_REG_KEY_FMT		"%5s:0x%016lx"
#define DUMPSTAT_REGS_PER_LINE_MAX	4U
#define VMSTAT_CPU_WAIT_WARN_US		20000UL

static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv);
static int32_t shell_dumpstat(int32_t argc, char **argv);
static int32_t shell_vmstat(int32_t argc, __unused char **argv);
static int32_t shell_reset_vm(int32_t argc, char **argv);
static int32_t shell_reboot(__unused int32_t argc, __unused char **argv);

struct shell_cmd arch_shell_cmds[] = {
	{
		.str		= SHELL_CMD_MEM_MAP,
		.cmd_param	= SHELL_CMD_MEM_MAP_PARAM,
		.help_str	= SHELL_CMD_MEM_MAP_HELP,
		.fcn		= shell_list_mem,
	},
	{
		.str		= SHELL_CMD_DUMPSTAT,
		.cmd_param	= SHELL_CMD_DUMPSTAT_PARAM,
		.help_str	= SHELL_CMD_DUMPSTAT_HELP,
		.fcn		= shell_dumpstat,
	},
	{
		.str		= SHELL_CMD_VMSTAT,
		.cmd_param	= SHELL_CMD_VMSTAT_PARAM,
		.help_str	= SHELL_CMD_VMSTAT_HELP,
		.fcn		= shell_vmstat,
	},
	{
		.str		= SHELL_CMD_RESET,
		.cmd_param	= SHELL_CMD_RESET_PARAM,
		.help_str	= SHELL_CMD_RESET_HELP,
		.fcn		= shell_reset_vm,
	},
	{
		.str		= SHELL_CMD_REBOOT,
		.cmd_param	= SHELL_CMD_REBOOT_PARAM,
		.help_str	= SHELL_CMD_REBOOT_HELP,
		.fcn		= shell_reboot,
	},
};
uint32_t arch_shell_cmds_sz = ARRAY_SIZE(arch_shell_cmds);

static void shell_print_mem_header(void)
{
	shell_puts("domain      type       attr       va/ipa start        pa start           size\r\n");
	shell_puts("──────────  ─────────  ─────────  ──────────────────  ──────────────────  ──────────────────\r\n");
}

static void shell_print_mem_map(const char *domain, const char *type, const char *attr,
	uint64_t addr, uint64_t paddr, uint64_t size)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE,
		"%-10s  %-9s  %-9s  0x%016lx  0x%016lx  0x%016lx\r\n",
		domain, type, attr, addr, paddr, size);
	shell_puts(temp_str);
}

static void shell_print_mem_special(const char *domain, const char *type, const char *attr,
	uint64_t addr, uint64_t size)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE,
		"%-10s  %-9s  %-9s  0x%016lx  %-18s  0x%016lx\r\n",
		domain, type, attr, addr, "-", size);
	shell_puts(temp_str);
}

static void shell_print_host_maps(void)
{
	const struct arm64_mem_region *mmio_regions;
	const struct mem_region *rsvd_regions;
	uint32_t mmio_count;
	uint32_t rsvd_count;
	uint32_t idx;
	uint64_t hv_base = get_hv_image_base();

	mmio_regions = arm64_get_platform_mmio_regions(&mmio_count);
	for (idx = 0U; idx < mmio_count; idx++) {
		shell_print_mem_map("host s1", "mmio", "device",
			mmio_regions[idx].base, mmio_regions[idx].base, mmio_regions[idx].size);
	}

	shell_print_mem_map("host s1", "ram", "normal",
		arm64_get_phys_mem_start(), arm64_get_phys_mem_start(), arm64_get_phys_mem_size());
	shell_print_mem_map("host s1", "hv_image", "normal-x",
		hv_base, hv_base, get_hv_image_size());

	rsvd_regions = arm64_get_reserved_mem_regions(&rsvd_count);
	for (idx = 0U; idx < rsvd_count; idx++) {
		shell_print_mem_special("host s1", "rsvd", "unmapped",
			rsvd_regions[idx].addr, rsvd_regions[idx].size);
	}
}

static void shell_print_vm_stage2_maps(const struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char domain[16];

	snprintf(domain, sizeof(domain), "vm-%u s2", vm->vm_id);
	shell_print_mem_map(domain, "ram", "normal",
		arch_config->guest_ram_start,
		arch_config->guest_ram_hpa,
		arch_config->guest_ram_size);
	shell_print_mem_special(domain, "vgicd", "vio",
		arch_config->guest_gicd_base,
		arch_config->guest_gicd_size);
	shell_print_mem_special(domain, "vgicr", "vio",
		arch_config->guest_gicr_base,
		arch_config->guest_gicr_size);
	shell_print_mem_special(domain, "vpl011", "vio",
		arch_config->guest_uart_base,
		arch_config->guest_uart_size);
}

/*
 * 2026-06-30, mmap monitor:
 *
 * mmap is the quick ownership map for ARM64 address translation. It prints the
 * EL2 stage-1 host view first, then each VM's stage-2 view. RAM rows are leaf
 * mappings; vio rows are intentionally unmapped stage-2 device IPAs that exit
 * to EL2 through MMIO emulation.
 *
 *   host VA -> EL2 stage-1 -> HPA
 *   guest IPA -> VM stage-2 -> HPA, or vio data abort
 */
static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv)
{
	uint16_t vm_id;

	shell_puts("\r\narm64 memory mappings:\r\n");
	shell_print_mem_header();
	shell_print_host_maps();

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (vm->root_stg2ptp != NULL) {
			shell_print_vm_stage2_maps(vm);
		}
	}

	return 0;
}

static const char *thread_state_to_str(enum thread_object_state state)
{
	const char *str;

	switch (state) {
	case THREAD_STS_RUNNING:
		str = "running";
		break;
	case THREAD_STS_RUNNABLE:
		str = "runnable";
		break;
	case THREAD_STS_BLOCKED:
		str = "blocked";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *vcpu_sched_state_to_str(const struct acrn_vcpu *vcpu)
{
	return (vcpu->state == VCPU_OFFLINE) ?
		"offline" : thread_state_to_str(vcpu->thread_obj.status);
}

static void shell_dumpstat_format_reg(char *buf, size_t size, const char *name,
	uint64_t value)
{
	snprintf(buf, size, DUMPSTAT_REG_KEY_FMT, name, value);
}

static void shell_dumpstat_reg_line(uint32_t count, ...)
{
	char reg[DUMPSTAT_REGS_PER_LINE_MAX][32U];
	va_list args;
	uint32_t idx;

	if ((count == 0U) || (count > DUMPSTAT_REGS_PER_LINE_MAX)) {
		return;
	}

	va_start(args, count);
	for (idx = 0U; idx < count; idx++) {
		const char *name = __builtin_va_arg(args, const char *);
		uint64_t value = __builtin_va_arg(args, uint64_t);

		shell_dumpstat_format_reg(reg[idx], sizeof(reg[idx]), name, value);
	}
	va_end(args);

	switch (count) {
	case 1U:
		shell_item_line("%s", reg[0U]);
		break;
	case 2U:
		shell_item_line("%s %s", reg[0U], reg[1U]);
		break;
	case 3U:
		shell_item_line("%s %s %s", reg[0U], reg[1U], reg[2U]);
		break;
	default:
		shell_item_line("%s %s %s %s", reg[0U], reg[1U], reg[2U], reg[3U]);
		break;
	}
}

static void shell_dumpstat_regs(const struct cpu_regs *regs)
{
	shell_dumpstat_reg_line(3U, "elr", regs->elr, "spsr", regs->spsr, "esr", regs->esr);
	shell_dumpstat_reg_line(2U, "far", regs->far, "hpfar", regs->hpfar);
	shell_dumpstat_reg_line(4U, "x00", regs->x0, "x01", regs->x1, "x02", regs->x2, "x03", regs->x3);
	shell_dumpstat_reg_line(4U, "x04", regs->x4, "x05", regs->x5, "x06", regs->x6, "x07", regs->x7);
	shell_dumpstat_reg_line(4U, "x08", regs->x8, "x09", regs->x9, "x10", regs->x10, "x11", regs->x11);
	shell_dumpstat_reg_line(4U, "x12", regs->x12, "x13", regs->x13, "x14", regs->x14, "x15", regs->x15);
	shell_dumpstat_reg_line(4U, "x16", regs->x16, "x17", regs->x17, "x18", regs->x18, "x19", regs->x19);
	shell_dumpstat_reg_line(4U, "x20", regs->x20, "x21", regs->x21, "x22", regs->x22, "x23", regs->x23);
	shell_dumpstat_reg_line(4U, "x24", regs->x24, "x25", regs->x25, "x26", regs->x26, "x27", regs->x27);
	shell_dumpstat_reg_line(4U, "x28", regs->x28, "x29", regs->x29, "lr", regs->lr, "sp", regs->sp);
}

static const char *shell_yes_no(bool value)
{
	return value ? "Y" : "N";
}

static uint32_t shell_lr_state(uint64_t lr)
{
	return (uint32_t)((lr >> ICH_LR_STATE_SHIFT) & 0x3UL);
}

static const char *shell_vtimer_trace_event_to_str(uint32_t event)
{
	const char *str;

	/*
	 * dumpstat prints short event names in the vtimer ring:
	 * load/unload : vCPU switch boundary saved or restored timer state.
	 * sysreg      : guest timer sysreg access updated the EL2 shadow.
	 * ppi         : host generic-timer PPI reached EL2.
	 * poll        : EL2 sampled an expired timer at a bounded sync point.
	 * update      : timer state was synchronized into the vGIC line.
	 * inject      : an expired timer became a guest-visible PPI.
	 * eoi         : guest completed the virtual timer interrupt.
	 * requeue     : timer line was queued again after vGIC/timer sync.
	 * backup      : offline backup timer fired for an unloaded vCPU.
	 * lr-pending  : pending-only timer PPI already resides in a guest LR.
	 * lr-noeoi    : old pending timer LR is absent and no EOI was reported.
	 * mask        : EL2 live CNTV IMASK ownership changed.
	 * stall       : stale pending LR/host handoff stall was detected.
	 */
	switch (event) {
	case ARM64_VTIMER_TRACE_LOAD:
		str = "load";
		break;
	case ARM64_VTIMER_TRACE_UNLOAD:
		str = "unload";
		break;
	case ARM64_VTIMER_TRACE_SYSREG:
		str = "sysreg";
		break;
	case ARM64_VTIMER_TRACE_PPI:
		str = "ppi";
		break;
	case ARM64_VTIMER_TRACE_POLL:
		str = "poll";
		break;
	case ARM64_VTIMER_TRACE_UPDATE:
		str = "update";
		break;
	case ARM64_VTIMER_TRACE_INJECT:
		str = "inject";
		break;
	case ARM64_VTIMER_TRACE_EOI:
		str = "eoi";
		break;
	case ARM64_VTIMER_TRACE_REQUEUE:
		str = "requeue";
		break;
	case ARM64_VTIMER_TRACE_BACKUP:
		str = "backup";
		break;
	case ARM64_VTIMER_TRACE_PENDING_LR:
		str = "lr-pending";
		break;
	case ARM64_VTIMER_TRACE_LOST_LR:
		str = "lr-noeoi";
		break;
	case ARM64_VTIMER_TRACE_MASK:
		str = "mask";
		break;
	case ARM64_VTIMER_TRACE_STALL:
		str = "stall";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *shell_guest_trace_event_to_str(uint8_t event)
{
	const char *str;

	/*
	 * Guest trace event names describe EL1/EL2 control boundaries:
	 * enter  : vCPU thread is about to enter guest EL1.
	 * exit   : guest returned to EL2 because of a trap or IRQ.
	 * resume : EL2 handled the exit and is about to return to EL1.
	 */
	switch (event) {
	case ARM64_VCPU_GUEST_TRACE_ENTER:
		str = "enter";
		break;
	case ARM64_VCPU_GUEST_TRACE_EXIT:
		str = "exit";
		break;
	case ARM64_VCPU_GUEST_TRACE_RESUME:
		str = "resume";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static void shell_dumpstat_guest_trace(const struct arm64_vcpu_guest_trace *trace)
{
	uint32_t count = trace->count;
	uint32_t start;
	uint32_t idx;
	uint64_t prev_tsc = 0UL;

	if (count > ARM64_VCPU_GUEST_TRACE_NUM) {
		count = ARM64_VCPU_GUEST_TRACE_NUM;
	}
	if (count == 0U) {
		shell_item_line("guest-trace:none");
		return;
	}

	start = (trace->head + ARM64_VCPU_GUEST_TRACE_NUM - count) %
		ARM64_VCPU_GUEST_TRACE_NUM;
	/*
	 * src identifies sync versus IRQ exits, ec is the ESR exception class
	 * when available, and delta.us is time since the previous boundary row.
	 */
	for (idx = 0U; idx < count; idx++) {
		uint32_t ring_idx = (start + idx) % ARM64_VCPU_GUEST_TRACE_NUM;
		const struct arm64_vcpu_guest_trace_entry *entry = &trace->entry[ring_idx];
		uint64_t delta_us = ((prev_tsc == 0UL) || (entry->tsc < prev_tsc)) ?
			0UL : ticks_to_us(entry->tsc - prev_tsc);

		if (entry->ec == ARM64_VCPU_DEBUG_EXIT_EC_INVALID) {
			shell_item_line("gt[%02u] %-6s pcpu:%hu src:0x%02x ec:N/A  status:%3d delta.us:%8lu",
				idx, shell_guest_trace_event_to_str(entry->event),
				entry->pcpu_id, entry->source, entry->status,
				delta_us);
		} else {
			shell_item_line("gt[%02u] %-6s pcpu:%hu src:0x%02x ec:0x%02x status:%3d delta.us:%8lu",
				idx, shell_guest_trace_event_to_str(entry->event),
				entry->pcpu_id, entry->source, entry->ec, entry->status,
				delta_us);
		}
		shell_item_line("       elr:0x%016lx esr:0x%016lx far:0x%016lx hpfar:0x%016lx",
			entry->elr, entry->esr, entry->far, entry->hpfar);
		prev_tsc = entry->tsc;
	}
}

static void shell_dumpstat_vtimer_trace(const struct arm64_vcpu_vtimer_trace *trace)
{
	uint32_t count = trace->count;
	uint32_t start;
	uint32_t idx;

	if (count > ARM64_VCPU_VTIMER_TRACE_NUM) {
		count = ARM64_VCPU_VTIMER_TRACE_NUM;
	}
	if (count == 0U) {
		shell_item_line("trace:none");
		return;
	}

	start = (trace->head + ARM64_VCPU_VTIMER_TRACE_NUM - count) %
		ARM64_VCPU_VTIMER_TRACE_NUM;
	for (idx = 0U; idx < count; idx++) {
		uint32_t ring_idx = (start + idx) % ARM64_VCPU_VTIMER_TRACE_NUM;
		const struct arm64_vcpu_vtimer_trace_entry *entry = &trace->entry[ring_idx];
		int64_t delta = (int64_t)(entry->cval - entry->cntvct);

		shell_item_line("vt[%02u] %-10s pcpu:%hu virq:%u ctl:0x%08x exp:%s mask:%s p/a/l:%s/%s/%s wr:%s inj:%s delta:%ld",
			idx, shell_vtimer_trace_event_to_str(entry->event),
			entry->pcpu_id, entry->virq, entry->ctl,
			shell_yes_no(entry->expired), shell_yes_no(entry->masked),
			shell_yes_no(entry->pending), shell_yes_no(entry->active),
			shell_yes_no(entry->level), shell_yes_no(entry->write),
			shell_yes_no(entry->injected), delta);
		shell_item_line("       cval:0x%016lx cntvct:0x%016lx lr0:0x%016lx hcr:0x%016lx",
			entry->cval, entry->cntvct, entry->lr0, entry->hcr);
	}
}

static bool shell_stack_contains(uint64_t start, uint64_t end, uint64_t addr, uint64_t bytes)
{
	uint64_t left;

	if ((addr < start) || (addr >= end)) {
		return false;
	}

	left = end - addr;
	return bytes <= left;
}

static void shell_dumpstat_print_frame(uint32_t idx, uint64_t fp, uint64_t lr, bool symbolize)
{
	char sym[96U];

	if (symbolize) {
		dbg_format_symbol(lr, sym, sizeof(sym));
		shell_item_line("[%02u] fp:0x%016lx  lr:0x%016lx  %s",
			idx, fp, lr, sym);
	} else {
		shell_item_line("[%02u] fp:0x%016lx  lr:0x%016lx",
			idx, fp, lr);
	}
}

static void shell_dumpstat_unwind_host_stack(uint64_t sp, uint64_t fp, uint64_t lr,
	uint64_t stack_start, uint64_t stack_end)
{
	uint32_t idx;

	if (!shell_stack_contains(stack_start, stack_end, sp, sizeof(uint64_t))) {
		shell_item_line("trace unavailable: sp is outside the stack");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_STACK_DEPTH; idx++) {
		uint64_t next_fp = 0UL;

		shell_dumpstat_print_frame(idx, fp, lr, true);

		if (!shell_stack_contains(stack_start, stack_end, fp, sizeof(uint64_t) * 2UL)) {
			break;
		}

		next_fp = *((const uint64_t *)fp);
		lr = *(((const uint64_t *)fp) + 1UL);
		if ((next_fp == SP_BOTTOM_MAGIC) || (next_fp <= fp)) {
			break;
		}

		fp = next_fp;
	}
}

struct dumpstat_guest_frame {
	uint64_t fp;
	uint64_t lr;
};

static void shell_dumpstat_vm_stack(struct acrn_vcpu *vcpu, const struct cpu_regs *regs)
{
	struct dumpstat_guest_frame frame;
	uint64_t fp = regs->x29;
	uint64_t lr = regs->lr;
	uint32_t idx;

	if ((fp == 0UL) && (lr == 0UL)) {
		shell_item_line("trace unavailable: empty frame registers");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_STACK_DEPTH; idx++) {
		shell_dumpstat_print_frame(idx, fp, lr, false);

		if (fp == 0UL) {
			break;
		}

		if (copy_from_gpa(vcpu->vm, &frame, fp, sizeof(frame)) != 0) {
			break;
		}
		if ((frame.fp == 0UL) || (frame.fp <= fp)) {
			break;
		}

		fp = frame.fp;
		lr = frame.lr;
	}
}

static void shell_dumpstat_host_stack(const struct acrn_vcpu *vcpu)
{
	const struct stack_frame *saved_frame = (const struct stack_frame *)vcpu->thread_obj.host_sp;
	uint64_t stack_start = (uint64_t)&vcpu->stack[0];
	uint64_t stack_end = (uint64_t)&vcpu->stack[CONFIG_STACK_SIZE];

	if (!shell_stack_contains(stack_start, stack_end, vcpu->thread_obj.host_sp, sizeof(*saved_frame))) {
		shell_item_line("trace unavailable: saved sp is outside the vcpu thread stack");
		return;
	}

	shell_dumpstat_unwind_host_stack(vcpu->thread_obj.host_sp,
		saved_frame->x29, saved_frame->lr, stack_start, stack_end);
}

struct dumpstat_snapshot {
	struct acrn_vcpu *vcpu;
	struct cpu_regs regs;
	struct arm64_vcpu_debug_info debug;
	struct arm64_vcpu_guest_ctx gctx;
	struct arm64_vgicv3_vcpu_ctx vgic_ctx;
	struct arm64_vgic_irq timer_irq;
	uint64_t live_cntvct_el0;
	uint64_t live_cntv_cval_el0;
	uint64_t live_ich_hcr_el2;
	uint64_t live_ich_vmcr_el2;
	uint64_t live_ich_lr[4U];
	uint64_t pending_req;
	uint64_t irqs_pending;
	uint64_t irqs_pending_mask;
	uint32_t timer_pending_word;
	uint32_t live_cntv_ctl_el0;
	bool has_timer_irq;
	bool has_live_timer;
	bool captured;
};

static void shell_dumpstat_snapshot_timer_irq(struct dumpstat_snapshot *snapshot)
{
	const struct acrn_vcpu *vcpu = snapshot->vcpu;
	uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized &&
		(vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS) && (virq < ARM64_VGIC_IRQ_NUM)) {
		const struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint32_t word = virq / 32U;

		snapshot->timer_irq = vgic->irq[vcpu->vcpu_id][virq];
		snapshot->timer_pending_word = vgic->pending_bitmap[vcpu->vcpu_id][word];
		snapshot->has_timer_irq = true;
	}
}

static void shell_dumpstat_capture(void *data)
{
	struct dumpstat_snapshot *snapshot = (struct dumpstat_snapshot *)data;

	if (get_running_vcpu(get_pcpu_id()) == snapshot->vcpu) {
		(void)memcpy_s(&snapshot->regs, sizeof(snapshot->regs),
			&snapshot->vcpu->arch.regs, sizeof(snapshot->regs));
		(void)memcpy_s(&snapshot->debug, sizeof(snapshot->debug),
			&snapshot->vcpu->arch.debug, sizeof(snapshot->debug));
		(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
			&snapshot->vcpu->arch.gctx, sizeof(snapshot->gctx));
		(void)memcpy_s(&snapshot->vgic_ctx, sizeof(snapshot->vgic_ctx),
			&snapshot->vcpu->arch.vgic, sizeof(snapshot->vgic_ctx));
		snapshot->pending_req = snapshot->vcpu->pending_req;
		snapshot->irqs_pending = snapshot->vcpu->arch.irqs_pending;
		snapshot->irqs_pending_mask = snapshot->vcpu->arch.irqs_pending_mask;
		shell_dumpstat_snapshot_timer_irq(snapshot);
		snapshot->live_cntvct_el0 = read_cntvct_el0();
		snapshot->live_cntv_cval_el0 = read_cntv_cval_el0();
		snapshot->live_cntv_ctl_el0 = read_cntv_ctl_el0();
		snapshot->live_ich_hcr_el2 = read_ich_hcr_el2();
		snapshot->live_ich_vmcr_el2 = read_ich_vmcr_el2();
		/*
		 * Live LRs can differ from the saved software copy while the vCPU
		 * is running. Capturing both views identifies save/restore drift
		 * versus hardware state that is genuinely stuck at the CPU interface.
		 */
		snapshot->live_ich_lr[0U] = read_ich_lr_el2(0U);
		snapshot->live_ich_lr[1U] = read_ich_lr_el2(1U);
		snapshot->live_ich_lr[2U] = read_ich_lr_el2(2U);
		snapshot->live_ich_lr[3U] = read_ich_lr_el2(3U);
		snapshot->has_live_timer = true;
		snapshot->captured = true;
	}
}

static const struct cpu_regs *shell_dumpstat_get_regs(struct acrn_vcpu *vcpu,
	struct dumpstat_snapshot *snapshot)
{
	uint16_t pcpu_id = vcpu->thread_obj.pcpu_id;

	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->vcpu = vcpu;
	(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
		&vcpu->arch.gctx, sizeof(snapshot->gctx));
	(void)memcpy_s(&snapshot->vgic_ctx, sizeof(snapshot->vgic_ctx),
		&vcpu->arch.vgic, sizeof(snapshot->vgic_ctx));
	snapshot->pending_req = vcpu->pending_req;
	snapshot->irqs_pending = vcpu->arch.irqs_pending;
	snapshot->irqs_pending_mask = vcpu->arch.irqs_pending_mask;
	shell_dumpstat_snapshot_timer_irq(snapshot);
	snapshot->has_live_timer = false;
	snapshot->captured = false;

	if ((vcpu->state == VCPU_RUNNING) &&
		(sched_get_current(pcpu_id) == &vcpu->thread_obj) &&
		(pcpu_id != get_pcpu_id())) {
		(void)smp_call_function_timeout(1UL << pcpu_id, shell_dumpstat_capture,
			snapshot, DUMPSTAT_SMP_CALL_TIMEOUT_US);
	}

	return snapshot->captured ? &snapshot->regs : &vcpu->arch.regs;
}

/*
 * 2026-06-30, dumpstat monitor:
 *
 * dumpstat is a per-vCPU deep snapshot. If the target vCPU is current on a
 * remote pCPU, an IPI samples live EL2 timer and vGIC state; otherwise the
 * command falls back to the durable vCPU state saved in memory. Printing both
 * saved and live fields separates context-save bugs from guest-visible stalls.
 *
 *   target vCPU current on remote pCPU
 *        -> IPI live capture -> regs/vGIC/vtimer + stacks
 *      otherwise
 *        -> saved vCPU image -> regs/vGIC/vtimer + stacks
 */
static int32_t shell_find_valid_lr_for_virq(const uint64_t *lrs, uint32_t count, uint32_t virq)
{
	uint32_t idx;

	for (idx = 0U; idx < count; idx++) {
		uint64_t lr = lrs[idx];

		if ((shell_lr_state(lr) != ICH_LR_STATE_INVALID) &&
			((uint32_t)(lr & ICH_LR_VINTID_MASK) == virq)) {
			return (int32_t)idx;
		}
	}

	return -1;
}

static void shell_dumpstat_timer_irq_state(const struct dumpstat_snapshot *snapshot)
{
	uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	uint32_t bit = 1U << (virq % 32U);
	uint64_t vmcr = snapshot->has_live_timer ?
		snapshot->live_ich_vmcr_el2 : snapshot->vgic_ctx.vmcr;
	int32_t saved_lr = shell_find_valid_lr_for_virq(snapshot->vgic_ctx.lr,
		snapshot->vgic_ctx.used_lrs, virq);
	int32_t live_lr = snapshot->has_live_timer ?
		shell_find_valid_lr_for_virq(snapshot->live_ich_lr,
			ARRAY_SIZE(snapshot->live_ich_lr), virq) : -1;
	bool gicd_g1 = ((snapshot->vcpu->vm->arch_vm.vgic.gicd_ctlr & (1U << 1U)) != 0U);
	bool vmcr_g1 = ((vmcr & ICH_VMCR_VENG1) != 0UL);
	bool bitmap = ((snapshot->timer_pending_word & bit) != 0U);

	if (!snapshot->has_timer_irq) {
		shell_item_line("      vgic:desc:none");
		return;
	}

	shell_item_line("      vgic:en:%s pend:%s act:%s level:%s bitmap:%s deliverable:%s",
		shell_yes_no(snapshot->timer_irq.enabled),
		shell_yes_no(snapshot->timer_irq.pending),
		shell_yes_no(snapshot->timer_irq.active),
		shell_yes_no(snapshot->timer_irq.level),
		shell_yes_no(bitmap),
		shell_yes_no(gicd_g1 && vmcr_g1 && snapshot->timer_irq.enabled));
	shell_item_line("      route:saved-lr:%d live-lr:%d hcr:0x%016lx live-hcr:0x%016lx",
		saved_lr, live_lr, snapshot->vgic_ctx.hcr, snapshot->live_ich_hcr_el2);
}

static void shell_dumpstat_timer_state(const struct dumpstat_snapshot *snapshot)
{
	const struct arm64_vcpu_guest_ctx *gctx = &snapshot->gctx;

	if (snapshot->has_live_timer) {
		uint64_t guest_now = snapshot->live_cntvct_el0;

		shell_item_line("PPI%u cntv_ctl:0x%08x guest_ctl:0x%08x cntv_cval:0x%08lx cntvct:0x%08lx delta:%ld el2_mask:%s",
			gctx->timer_virq, snapshot->live_cntv_ctl_el0,
			gctx->cntv_ctl_el0, snapshot->live_cntv_cval_el0,
			snapshot->live_cntvct_el0,
			(int64_t)(snapshot->live_cntv_cval_el0 - guest_now),
			shell_yes_no(gctx->cntv_el2_masked));
	} else {
		shell_item_line("PPI%u live:none el2_mask:%s",
			gctx->timer_virq, shell_yes_no(gctx->cntv_el2_masked));
	}
	shell_dumpstat_timer_irq_state(snapshot);
}

static void shell_dumpstat_vtimer_diag(const struct arm64_vcpu_vtimer_diag *diag)
{
	uint64_t mask_ticks = diag->max_el2_mask_ticks;

	if (diag->el2_mask_since_ticks != 0UL) {
		uint64_t now = cpu_ticks();
		uint64_t active_ticks = (now > diag->el2_mask_since_ticks) ?
			(now - diag->el2_mask_since_ticks) : 0UL;

		if (active_ticks > mask_ticks) {
			mask_ticks = active_ticks;
		}
	}

	/*
	 * This section deliberately avoids repeating raw CNTV/LR fields from
	 * "timer/vgic state" and "vtimer trace". It gives cumulative counters for
	 * the four transitions that matter when Linux reports timer-softirq stalls:
	 * WFI wakeup, pending-only LR preservation, EL2 host-timer masking, and the
	 * last-chance flush before ERET.
	 */
	shell_item_line("wfi:trap:%lu irq-masked:%lu pending-irq:%lu",
		diag->wfi_trap, diag->wfi_irq_masked, diag->wfi_pending_irq);
	shell_item_line("lr-pending-only:seen:%lu preserve:%lu drop:%lu missing-no-eoi:%lu",
		diag->pending_only_lr_seen, diag->pending_only_lr_preserve,
		diag->pending_only_lr_drop, diag->lost_pending_lr);
	shell_item_line("el2-mask:set:%lu clear:%lu max-age.us:%lu active:%s",
		diag->el2_mask_set, diag->el2_mask_clear,
		ticks_to_us(mask_ticks),
		shell_yes_no(diag->el2_mask_since_ticks != 0UL));
	shell_item_line("pre-eret-flush:run:%lu expired:%lu",
		diag->pre_eret_flush,
		diag->pre_eret_flush_expired);
}

static int32_t shell_dumpstat_vcpu(struct acrn_vcpu *vcpu)
{
	struct thread_object *current;
	struct dumpstat_snapshot snapshot;
	const struct cpu_regs *regs;
	const struct arm64_vcpu_debug_info *debug;

	current = (vcpu->state == VCPU_OFFLINE) ?
		NULL : sched_get_current(vcpu->thread_obj.pcpu_id);
	regs = shell_dumpstat_get_regs(vcpu, &snapshot);
	debug = snapshot.captured ? &snapshot.debug : &vcpu->arch.debug;

	shell_item_begin("vm%hu/vcpu%hu", vcpu->vm->vm_id, vcpu->vcpu_id);
	/*
	 * Header fields identify CPU binding, scheduler state, whether this vCPU
	 * is the current thread, and whether live EL2 state was sampled by IPI.
	 */
	shell_item_line("pcpu:%hu sched:%s current:%s live:%s", vcpu->thread_obj.pcpu_id,
		vcpu_sched_state_to_str(vcpu),
		shell_yes_no(current == &vcpu->thread_obj),
		shell_yes_no(snapshot.captured));
	shell_item_line("requests:pending:0x%016lx arch-irqs:0x%016lx mask:0x%016lx",
		snapshot.pending_req, snapshot.irqs_pending,
		snapshot.irqs_pending_mask);
	shell_item_section("vcpu stats");
	/*
	 * vcpu stats keeps the execution context compact. Timer/vGIC-specific
	 * evidence is printed below without the older shadow-register block.
	 */
	shell_item_line("guest regs:");
	shell_dumpstat_regs(regs);
	shell_dumpstat_guest_trace(&debug->guest_trace);
	if (vcpu->state != VCPU_OFFLINE) {
		shell_item_line("vcpu stack:");
		shell_dumpstat_vm_stack(vcpu, regs);
		shell_item_line("pcpu stack:");
		shell_dumpstat_host_stack(vcpu);
	}
	shell_item_section("vgic/vtimer");
	shell_dumpstat_timer_state(&snapshot);
	shell_dumpstat_vtimer_diag(&vcpu->arch.debug.vtimer_diag);
	shell_dumpstat_vtimer_trace(&debug->vtimer_trace);
	shell_item_end();

	return 0;
}

static int32_t shell_dumpstat(int32_t argc, char **argv)
{
	struct acrn_vm *vm;
	int64_t param;
	uint16_t vm_id = 0U;
	uint16_t vcpu_id;

	if (argc > 2) {
		return -EINVAL;
	}

	if (argc == 2) {
		param = strtol_deci(argv[1]);
		if ((param < 0) || (param >= CONFIG_MAX_VM_NUM)) {
			return -EINVAL;
		}
		vm_id = (uint16_t)param;
	}

	vm = get_vm_from_vmid(vm_id);
	if (is_poweroff_vm(vm)) {
		return -EINVAL;
	}

	shell_item_begin("dumpstat vm%hu:%s", vm_id, vm->name);
	shell_item_line("vcpus:%hu", vm->hw.created_vcpus);
	shell_item_end();

	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		(void)shell_dumpstat_vcpu(vcpu_from_vid(vm, vcpu_id));
	}

	return 0;
}

static const char *shell_vm_state_to_str(enum vm_state state)
{
	const char *str;

	switch (state) {
	case VM_POWERED_OFF:
		str = "poweroff";
		break;
	case VM_CREATED:
		str = "created";
		break;
	case VM_RUNNING:
		str = "running";
		break;
	case VM_READY_TO_POWEROFF:
		str = "ready-off";
		break;
	case VM_PAUSED:
		str = "paused";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *shell_vcpu_state_to_str(enum vcpu_state state)
{
	const char *str;

	switch (state) {
	case VCPU_OFFLINE:
		str = "offline";
		break;
	case VCPU_INIT:
		str = "init";
		break;
	case VCPU_RUNNING:
		str = "running";
		break;
	case VCPU_ZOMBIE:
		str = "zombie";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

/*
 * 2026-06-30, vmstat monitor:
 *
 * vmstat is the broad health summary before using dumpstat. It keeps one VM
 * visible at a time: configured resources, runtime state, watchdog and console
 * status, scheduler diagnostics, and the guest timer/vGIC delivery summary.
 *
 *   VM config + runtime VM object
 *        -> VM resource/state rows
 *        -> per-vCPU scheduler rows
 *        -> per-vCPU timer/vGIC rows
 */
static bool shell_vm_config_present(const struct acrn_vm_config *vm_config)
{
	return (vm_config->name[0] != '\0') || (vm_config->cpu_affinity != 0UL) ||
		((vm_config->guest_flags & GUEST_FLAG_STATIC_VM) != 0UL);
}

static void shell_print_cpu_bitmap(uint64_t bitmap)
{
	char temp_str[MAX_STR_SIZE];
	bool first = true;
	uint16_t cpu_id;

	for (cpu_id = 0U; cpu_id < MAX_PCPU_NUM; cpu_id++) {
		if ((bitmap & (1UL << cpu_id)) != 0UL) {
			(void)snprintf(temp_str, MAX_STR_SIZE, "%spcpu%hu", first ? "" : ",", cpu_id);
			shell_puts(temp_str);
			first = false;
		}
	}

	if (first) {
		shell_puts("-");
	}
}

static uint32_t shell_cpu_bitmap_weight(uint64_t bitmap)
{
	uint32_t weight = 0U;
	uint16_t cpu_id;

	for (cpu_id = 0U; cpu_id < MAX_PCPU_NUM; cpu_id++) {
		if ((bitmap & (1UL << cpu_id)) != 0UL) {
			weight++;
		}
	}

	return weight;
}

struct shell_vmstat_timer_summary {
	uint64_t cntv_ppi;
	uint64_t cntv_backup;
	uint64_t cntv_poll;
	uint64_t pre_eret_flush;
	uint64_t pre_eret_flush_expired;
	uint64_t lost_pending_lr;
};

static const char *shell_vmstat_wdt_status_to_str(enum vm_wdt_status status)
{
	const char *str;

	switch (status) {
	case VM_WDT_STATUS_OFFLINE:
		str = "offline";
		break;
	case VM_WDT_STATUS_UNKNOWN:
		str = "none";
		break;
	case VM_WDT_STATUS_ALIVE:
		str = "alive";
		break;
	case VM_WDT_STATUS_STUCK:
		str = "stuck";
		break;
	default:
		str = "unused";
		break;
	}

	return str;
}

static int64_t shell_vmstat_ticks_delta_us(int64_t delta)
{
	if (delta < 0L) {
		uint64_t magnitude = (uint64_t)(-(delta + 1L)) + 1UL;

		return -(int64_t)ticks_to_us(magnitude);
	}

	return (int64_t)ticks_to_us((uint64_t)delta);
}

static void shell_vmstat_collect_timer_summary(const struct acrn_vm *vm,
	struct shell_vmstat_timer_summary *summary)
{
	uint16_t vcpu_id;

	if ((vm == NULL) || (summary == NULL)) {
		return;
	}

	summary->cntv_ppi = 0UL;
	summary->cntv_backup = 0UL;
	summary->cntv_poll = 0UL;
	summary->pre_eret_flush = 0UL;
	summary->pre_eret_flush_expired = 0UL;
	summary->lost_pending_lr = 0UL;
	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		const struct acrn_vcpu *vcpu = vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);

		if (vcpu != NULL) {
			const struct arm64_vcpu_vtimer_diag *diag = &vcpu->arch.debug.vtimer_diag;

			summary->cntv_ppi += diag->cntv_ppi;
			summary->cntv_backup += diag->cntv_backup;
			summary->cntv_poll += diag->cntv_poll;
			summary->pre_eret_flush += diag->pre_eret_flush;
			summary->pre_eret_flush_expired += diag->pre_eret_flush_expired;
			summary->lost_pending_lr += diag->lost_pending_lr;
		}
	}
}

static void shell_vmstat_vm_config(uint16_t vm_id, const struct acrn_vm_config *vm_config,
	const struct acrn_vm *vm)
{
	const struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	struct shell_vmstat_timer_summary timer = { 0U };
	struct vm_wdt_snapshot wdt = { 0U };
	struct console_vm_ring_stats ring = { 0U };
	struct acrn_vuart *vu = NULL;
	char temp_str[MAX_STR_SIZE];

	(void)console_vm_ring_get_stats(vm_id, &ring);
	if (!is_poweroff_vm(vm)) {
		vu = vm_console_vuart((struct acrn_vm *)vm);
	}

	/*
	 * VM-level fields describe configured resources versus runtime state:
	 * vCPU count, affinity, scheduler policy, guest memory, interrupt
	 * topology, console backlog, and boot image placement.
	 */
	shell_item_line("vcpus:configured:%u created:%hu state:%s flags:0x%08lx load:%u",
		shell_cpu_bitmap_weight(vm_config->cpu_affinity), vm->hw.created_vcpus,
		shell_vm_state_to_str(vm->state), vm_config->guest_flags,
		(uint32_t)vm_config->load_order);
	shell_puts("│   affinity:");
	shell_print_cpu_bitmap(vm_config->cpu_affinity);
	shell_puts("\r\n");

	shell_item_line("gic:initialized:%s vcpus:%hu rdist:%hu lr-count:%u vmcr:0x%08x ctlr:0x%08x",
		shell_yes_no(vgic->initialized), vgic->vcpu_count,
		vgic->rdist_count, vgic->lr_count, vgic->vmcr, vgic->gicd_ctlr);
	shell_item_line("its:enabled:%s typer:0x%08lx ctlr:0x%08x",
		shell_yes_no(vgic->its_enabled), vgic->its.typer, vgic->its.ctlr);
	shell_vmstat_collect_timer_summary(vm, &timer);
	shell_item_line("timer:cntv:Y ppi:%lu backup:%lu poll:%lu pre-eret:%lu/%lu lr-miss:%lu cnthp:Y cntp-emul:Y maintenance:Y time-delta:%ld",
		timer.cntv_ppi, timer.cntv_backup, timer.cntv_poll,
		timer.pre_eret_flush_expired, timer.pre_eret_flush,
		timer.lost_pending_lr, vm->arch_vm.time_delta);
	if (vm_wdt_get_snapshot(vm_id, &wdt) == 0) {
		shell_item_line("wdt:status:%s kick:%lu timeout:%lu age.ms:%lu token:0x%016lx",
			shell_vmstat_wdt_status_to_str(wdt.status),
			wdt.kick_count, wdt.timeout_count, wdt.age_ms, wdt.last_token);
	}
	shell_item_line("console:selected:%s bound:%s ring:%u/%u drain:%u pending:%s",
		shell_yes_no(console_vmid == vm_id), shell_yes_no(ring.vuart_bound),
		ring.queued, ring.capacity, ring.drain_budget,
		shell_yes_no(ring.pending));

	if (vu != NULL) {
		shell_item_line("        vuart:active:%s irq:%u rx:%u tx:%u ier:0x%02x lsr:0x%02x",
			shell_yes_no(vu->active), vu->irq, vuart_rx_numchars(vu),
			vu->txfifo.num, vu->ier, vu->lsr);
	}

	(void)snprintf(temp_str, MAX_STR_SIZE, "boot:kernel:%s entry:0x%016lx load:0x%016lx",
		vm_config->os_config.name, vm_config->os_config.kernel_entry_addr,
		vm_config->os_config.kernel_load_addr);
	shell_item_line("%s", temp_str);
}

static void shell_vmstat_append_flag(char *flags, size_t flags_len, const char *flag)
{
	size_t len = strnlen_s(flags, flags_len);

	if (len != 0U) {
		(void)strncat_s(flags, flags_len, ",", 1U);
	}
	(void)strncat_s(flags, flags_len, flag, strnlen_s(flag, flags_len));
}

static void shell_vmstat_vcpu_diag(const struct acrn_vcpu *vcpu,
	const struct thread_object *current, const struct sched_latency_stats *latency,
	const struct sched_rtds_stats *rtds, bool has_rtds,
	char *flags, size_t flags_len)
{
	uint64_t now = cpu_ticks();
	bool cpu_wait = false;

	flags[0] = '\0';

	/*
	 * These flags are quick "why might this VM look stuck?" hints from the
	 * current vmstat snapshot. They do not prove a permanent hang by themselves;
	 * they point to the next subsystem to inspect.
	 *
	 * cpu-wait:
	 *   The vCPU is runnable but is not the current thread on its pCPU for at
	 *   least two RTDS periods. A runnable thread wants CPU time; a long wait
	 *   usually means another vCPU/thread is occupying that pCPU or the shared
	 *   core is overloaded.
	 *
	 * rtds-depleted:
	 *   The vCPU runs under RTDS, is not currently executing, and its
	 *   current-period budget is already zero. RTDS gives each vCPU a fixed
	 *   budget every period; once that budget is spent, a non-current vCPU must
	 *   wait for replenishment unless spare CPU time is available through
	 *   work-conserving slack. A current vCPU with zero budget is probably using
	 *   that slack, so it is not flagged here.
	 *
	 * rtds-overrun:
	 *   The vCPU is already in cpu-wait and the RTDS replenishment/deadline
	 *   boundary is also due. This combines "wanted CPU for a while" with "the
	 *   current RTDS period should have rolled", which is a stronger signal that
	 *   the shared core is behind schedule.
	 */
	if ((vcpu->thread_obj.status == THREAD_STS_RUNNABLE) &&
		(current != &vcpu->thread_obj) &&
		(latency->runnable_since != 0UL) &&
		(ticks_to_us(now - latency->runnable_since) >= VMSTAT_CPU_WAIT_WARN_US)) {
		cpu_wait = true;
		shell_vmstat_append_flag(flags, flags_len, "cpu-wait");
	}
	if (has_rtds && (current != &vcpu->thread_obj) &&
		(vcpu->thread_obj.status == THREAD_STS_RUNNABLE) &&
		(rtds->remaining_ticks == 0UL)) {
		shell_vmstat_append_flag(flags, flags_len, "rtds-depleted");
	}
	if (has_rtds && cpu_wait && (rtds->deadline_ticks <= now)) {
		shell_vmstat_append_flag(flags, flags_len, "rtds-overrun");
	}
	if (flags[0] == '\0') {
		(void)snprintf(flags, flags_len, "ok");
	}
}

static void shell_vmstat_vcpu_timer(const struct acrn_vcpu *vcpu)
{
	const struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	const struct arm64_vcpu_vtimer_diag *diag = &vcpu->arch.debug.vtimer_diag;
	const struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	const struct arm64_vgic_irq *timer_irq = NULL;
	uint64_t now = cpu_ticks() - gctx->cntvoff_el2;
	int64_t delta = (int64_t)(gctx->cntv_cval_el0 - now);
	int64_t delta_us = shell_vmstat_ticks_delta_us(delta);
	bool expired = ((gctx->cntv_ctl_el0 & CNTV_CTL_ENABLE) != 0U) &&
		((gctx->cntv_ctl_el0 & CNTV_CTL_IMASK) == 0U) && (delta <= 0L);
	bool bitmap = false;
	bool deliverable = false;

	if (vgic->initialized && (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
		uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		uint32_t word = virq / 32U;
		uint32_t bit = 1U << (virq % 32U);

		timer_irq = &vgic->irq[vcpu->vcpu_id][virq];
		bitmap = ((vgic->pending_bitmap[vcpu->vcpu_id][word] & bit) != 0U);
		deliverable = ((vgic->gicd_ctlr & (1U << 1U)) != 0U) &&
			((vcpu->arch.vgic.vmcr & ICH_VMCR_VENG1) != 0UL) &&
			timer_irq->enabled;
	}

	if (timer_irq != NULL) {
		shell_item_line("      timer:PPI%u ctl:0x%08x cval:0x%016lx delta.us:%ld exp:%s mask:%s ppi:%lu backup:%lu poll:%lu pre-eret:%lu/%lu",
			ARM64_GIC_PPI_VIRTUAL_TIMER, gctx->cntv_ctl_el0,
			gctx->cntv_cval_el0, delta_us, shell_yes_no(expired),
			shell_yes_no(gctx->cntv_el2_masked), diag->cntv_ppi,
			diag->cntv_backup, diag->cntv_poll,
			diag->pre_eret_flush_expired, diag->pre_eret_flush);
		shell_item_line("      vgic:PPI%u en:%s pend:%s act:%s level:%s bitmap:%s deliverable:%s lr:%u hcr:0x%016lx vmcr:0x%016lx",
			ARM64_GIC_PPI_VIRTUAL_TIMER,
			shell_yes_no(timer_irq->enabled),
			shell_yes_no(timer_irq->pending),
			shell_yes_no(timer_irq->active),
			shell_yes_no(timer_irq->level),
			shell_yes_no(bitmap), shell_yes_no(deliverable),
			vcpu->arch.vgic.used_lrs, vcpu->arch.vgic.hcr,
			vcpu->arch.vgic.vmcr);
	} else {
		shell_item_line("      timer:PPI%u ctl:0x%08x cval:0x%016lx delta.us:%ld exp:%s mask:%s ppi:%lu backup:%lu poll:%lu pre-eret:%lu/%lu",
			ARM64_GIC_PPI_VIRTUAL_TIMER, gctx->cntv_ctl_el0,
			gctx->cntv_cval_el0, delta_us, shell_yes_no(expired),
			shell_yes_no(gctx->cntv_el2_masked), diag->cntv_ppi,
			diag->cntv_backup, diag->cntv_poll,
			diag->pre_eret_flush_expired, diag->pre_eret_flush);
		shell_item_line("      vgic:PPI%u desc:none lr:%u hcr:0x%016lx vmcr:0x%016lx",
			ARM64_GIC_PPI_VIRTUAL_TIMER, vcpu->arch.vgic.used_lrs,
			vcpu->arch.vgic.hcr, vcpu->arch.vgic.vmcr);
	}
}

static void shell_vmstat_vcpus(const struct acrn_vm *vm)
{
	uint16_t vcpu_id;

	if (vm->hw.created_vcpus == 0U) {
		shell_item_line("vcpu:none");
		return;
	}

	shell_item_section("vcpu state");
	/*
	 * The compact vCPU table is the first pass for "is this vCPU runnable,
	 * currently selected, or waiting with pending work?" questions.
	 */
	shell_item_line("vcpu       pcpu  sched  vcpu     thread    cur  req-mask            diag");
	shell_item_line("─────────  ────  ─────  ───────  ────────  ───  ──────────────────  ────────────");
	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		struct acrn_vcpu *vcpu = vcpu_from_vid((struct acrn_vm *)vm, vcpu_id);
		struct sched_latency_stats latency = { 0U };
		struct sched_bvt_stats bvt = { 0U };
		struct sched_rtds_stats rtds = { 0U };
		struct thread_object *current;
		char diag[96U];
		bool has_bvt;
		bool has_rtds;

		current = sched_get_current(vcpu->thread_obj.pcpu_id);
		sched_get_latency(&vcpu->thread_obj, &latency);
		has_bvt = sched_get_bvt_stats(&vcpu->thread_obj, &bvt);
		has_rtds = sched_get_rtds_stats(&vcpu->thread_obj, &rtds);
		shell_vmstat_vcpu_diag(vcpu, current, &latency, &rtds, has_rtds,
			diag, sizeof(diag));
		shell_item_line("%-9s  %-4hu  %-5s  %-7s  %-8s  %-3s  0x%016lx  %s",
			vcpu->thread_obj.name, vcpu->thread_obj.pcpu_id,
			has_rtds ? "rtds" : (has_bvt ? "bvt" : "-"),
			shell_vcpu_state_to_str(vcpu->state),
			thread_state_to_str(vcpu->thread_obj.status),
			shell_yes_no(current == &vcpu->thread_obj),
			vcpu->pending_req, diag);
		if (has_bvt) {
			/* BVT fields expose weight and virtual-time scheduling order. */
			shell_item_line("      bvt:weight:%u avt:%ld evt:%ld", bvt.weight,
				bvt.avt, bvt.evt);
		}
		if (has_rtds) {
			uint64_t now = cpu_ticks();

			/* RTDS fields expose period budget and time to deadline. */
			shell_item_line("      rtds:period-us:%lu budget-us:%lu remain-us:%lu deadline-in-us:%lu",
				ticks_to_us(rtds.period_ticks), ticks_to_us(rtds.budget_ticks),
				ticks_to_us(rtds.remaining_ticks),
				(rtds.deadline_ticks > now) ?
					ticks_to_us(rtds.deadline_ticks - now) : 0UL);
		}
		shell_vmstat_vcpu_timer(vcpu);
	}
}

static int32_t shell_vmstat(int32_t argc, __unused char **argv)
{
	uint16_t vm_id;

	if (argc != 1) {
		return -EINVAL;
	}

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (!shell_vm_config_present(vm_config) &&
			(vm->hw.created_vcpus == 0U) && is_poweroff_vm(vm)) {
			continue;
		}

		shell_item_begin("vmstat vm%hu:%s", vm_id,
			(vm->name[0] != '\0') ? vm->name : vm_config->name);
		shell_vmstat_vm_config(vm_id, vm_config, vm);
		shell_vmstat_vcpus(vm);
		shell_item_end();
	}

	return 0;
}

static int32_t shell_reset_vm(int32_t argc, char **argv)
{
	struct acrn_vm *vm;
	int64_t param;
	uint16_t vm_id;
	int32_t ret;

	if (argc != 2) {
		shell_puts("usage: reset <vm id>\r\n");
		return -EINVAL;
	}

	param = strtol_deci(argv[1]);
	if ((param < 0) || (param >= CONFIG_MAX_VM_NUM)) {
		shell_puts("invalid vm id\r\n");
		return -EINVAL;
	}

	vm_id = (uint16_t)param;
	vm = get_vm_from_vmid(vm_id);
	if (is_service_vm(vm)) {
		shell_puts("refuse to reset service VM from shell\r\n");
		return -EPERM;
	}
	if (is_poweroff_vm(vm)) {
		shell_puts("vm is powered off\r\n");
		return -EINVAL;
	}

	/*
	 * Shell reset runs from an EL2 management thread, so it can synchronously
	 * drive the VM state machine. Guest PSCI reset uses an async request because
	 * it originates from the vCPU being torn down.
	 */
	ret = restart_vm(vm);

	return ret;
}

static int32_t shell_reboot(__unused int32_t argc, __unused char **argv)
{
	reset_host(false);
	return 0;
}
