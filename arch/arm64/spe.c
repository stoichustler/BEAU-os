/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <cpu.h>
#include <irq.h>
#include <notify.h>
#include <per_cpu.h>
#include <spinlock.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/platform.h>
#include <asm/spe.h>
#include <asm/sysreg.h>

/* [20260721] EL2-owned SPE buffer lifecycle
 *
 * shell command -> bounded SMP callback -> local SPE registers -> static half-buffer
 *                                      |                                  |
 *                                      +--> unavailable/error: disable ---+
 *                                                                         |
 * PPI buffer full -> synchronize records -> seal half -> switch once -> stop
 *
 * Key rules:
 *   - each pCPU owns its SPE registers and the half currently writable by hardware;
 *   - EL2 owns every buffer and guests can neither configure SPE nor read records;
 *   - a second completed half stops sampling before any completed record is overwritten.
 */
#define ARM64_SPE_COMMAND_TIMEOUT_US 100000U
#define ARM64_SPE_SNAPSHOT_TIMEOUT_US 10000U
#define ARM64_SPE_MIN_ALIGNMENT PAGE_SIZE
#define ARM64_SPE_DEFAULT_INTERVAL 4096U

enum arm64_spe_command {
	ARM64_SPE_COMMAND_NONE = 0U,
	ARM64_SPE_COMMAND_START,
	ARM64_SPE_COMMAND_STOP,
	ARM64_SPE_COMMAND_RESET,
};

struct arm64_spe_pcpu {
	uint64_t pmbidr;
	uint64_t pmsidr;
	uint64_t buffer_full_count;
	uint64_t error_count;
	uint64_t data_loss_count;
	uint64_t suspend_epoch;
	uint32_t half_bytes[ARM64_SPE_HALF_NUM];
	uint16_t pcpu_id;
	uint8_t pmsver;
	uint8_t active_half;
	uint8_t ready_mask;
	enum arm64_spe_reason reason;
	bool initialized;
	bool available;
	bool running;
	bool resume_running;
};

static struct arm64_spe_pcpu arm64_spe_pcpus[MAX_PCPU_NUM];
static uint8_t arm64_spe_buffers[MAX_PCPU_NUM][CONFIG_ARM64_SPE_BUFFER_SIZE]
	__aligned(ARM64_SPE_MIN_ALIGNMENT);
static spinlock_t arm64_spe_command_lock = { .head = 0U, .tail = 0U, };
static bool arm64_spe_irq_registered;

static uint64_t arm64_spe_half_size(void)
{
	return CONFIG_ARM64_SPE_BUFFER_SIZE / ARM64_SPE_HALF_NUM;
}

static uint64_t arm64_spe_half_base(uint16_t pcpu_id, uint8_t half)
{
	return (uint64_t)&arm64_spe_buffers[pcpu_id][half * arm64_spe_half_size()];
}

static void arm64_spe_hw_disable(struct arm64_spe_pcpu *spe)
{
	write_pmscr_el1(0UL);
	arm64_isb();
	arm64_dsb_ish();
	write_pmblimitr_el1(0UL);
	write_pmbsr_el1(0UL);
	arm64_isb();
	spe->running = false;
}

static bool arm64_spe_buffer_valid(const struct arm64_spe_pcpu *spe)
{
	uint64_t alignment = 1UL << (spe->pmbidr & ARM64_PMBIDR_ALIGN_MASK);
	uint64_t half_size = arm64_spe_half_size();

	return (alignment != 0UL) && (alignment <= ARM64_SPE_MIN_ALIGNMENT) &&
		(half_size >= PAGE_SIZE) && ((half_size & (PAGE_SIZE - 1UL)) == 0UL) &&
		((half_size & (alignment - 1UL)) == 0UL);
}

static void arm64_spe_program_half(struct arm64_spe_pcpu *spe, uint8_t half)
{
	uint64_t base = arm64_spe_half_base(spe->pcpu_id, half);
	uint64_t limit = (base + arm64_spe_half_size()) & ARM64_PMBLIMITR_LIMIT_MASK;

	write_pmbptr_el1(base);
	write_pmblimitr_el1(limit | ARM64_PMBLIMITR_ENABLE);
	arm64_isb();
	spe->active_half = half;
}

static int32_t arm64_spe_start_local(struct arm64_spe_pcpu *spe)
{
	if (!spe->available) {
		return -ENOTSUP;
	}
	if (spe->running) {
		return 0;
	}
	if (spe->ready_mask != 0U) {
		return -EBUSY;
	}
	arm64_spe_hw_disable(spe);
	write_pmsfcr_el1(0UL);
	write_pmsevfr_el1(UINT64_MAX);
	write_pmslatfr_el1(0UL);
	write_pmsirr_el1(ARM64_SPE_DEFAULT_INTERVAL << ARM64_PMSIRR_INTERVAL_SHIFT);
	write_pmsicr_el1(0UL);
	arm64_isb();
	arm64_spe_program_half(spe, 0U);
	write_pmscr_el1(ARM64_PMSCR_E0SPE | ARM64_PMSCR_E1SPE |
		ARM64_PMSCR_PA | ARM64_PMSCR_TS);
	arm64_isb();
	spe->reason = ARM64_SPE_REASON_NONE;
	spe->running = true;
	return 0;
}

static void arm64_spe_command_local(void *data)
{
	enum arm64_spe_command command = *(enum arm64_spe_command *)data;
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_spe_pcpu *spe;
	uint64_t flags;

	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}
	spe = &arm64_spe_pcpus[pcpu_id];
	local_irq_save(&flags);
	if (command == ARM64_SPE_COMMAND_START) {
		(void)arm64_spe_start_local(spe);
	} else if (command == ARM64_SPE_COMMAND_STOP) {
		arm64_spe_hw_disable(spe);
	} else if (command == ARM64_SPE_COMMAND_RESET) {
		arm64_spe_hw_disable(spe);
		spe->ready_mask = 0U;
		spe->active_half = 0U;
		(void)memset(spe->half_bytes, 0U, sizeof(spe->half_bytes));
		spe->reason = ARM64_SPE_REASON_NONE;
	}
	local_irq_restore(flags);
}

static int32_t arm64_spe_command(enum arm64_spe_command command)
{
	uint64_t flags;
	uint64_t mask = (1UL << get_pcpu_nums()) - 1UL;
	int32_t ret;

	spinlock_irqsave_obtain(&arm64_spe_command_lock, &flags);
	ret = smp_call_function_timeout(mask, arm64_spe_command_local, &command,
		ARM64_SPE_COMMAND_TIMEOUT_US) ? 0 : -ETIMEDOUT;
	spinlock_irqrestore_release(&arm64_spe_command_lock, flags);
	return ret;
}

static void arm64_spe_irq_handler(__unused uint32_t irq, __unused void *data)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_spe_pcpu *spe;
	uint64_t status;
	uint64_t ptr;
	uint8_t next;

	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}
	spe = &arm64_spe_pcpus[pcpu_id];
	if (!spe->running) {
		return;
	}
	arm64_psb_csync();
	arm64_dsb_ish();
	arm64_isb();
	status = read_pmbsr_el1();
	if ((status & ARM64_PMBSR_STATUS) == 0UL) {
		return;
	}
	if (((status & ARM64_PMBSR_EC_MASK) != 0UL) ||
		((status & ARM64_PMBSR_MSS_BUFFER_FULL) == 0UL)) {
		spe->error_count++;
		spe->reason = ARM64_SPE_REASON_HW_EVENT;
		arm64_spe_hw_disable(spe);
		return;
	}
	if ((status & ARM64_PMBSR_DATA_LOSS) != 0UL) {
		spe->data_loss_count++;
	}
	ptr = read_pmbptr_el1();
	if ((ptr < arm64_spe_half_base(pcpu_id, spe->active_half)) ||
		(ptr > (arm64_spe_half_base(pcpu_id, spe->active_half) + arm64_spe_half_size()))) {
		spe->error_count++;
		spe->reason = ARM64_SPE_REASON_HW_EVENT;
		arm64_spe_hw_disable(spe);
		return;
	}
	spe->half_bytes[spe->active_half] = (uint32_t)(ptr -
		arm64_spe_half_base(pcpu_id, spe->active_half));
	spe->ready_mask |= 1U << spe->active_half;
	spe->buffer_full_count++;
	next = spe->active_half ^ 1U;
	if ((spe->ready_mask & (1U << next)) != 0U) {
		spe->reason = ARM64_SPE_REASON_BUFFER_EXHAUSTED;
		arm64_spe_hw_disable(spe);
	} else {
		arm64_spe_program_half(spe, next);
		write_pmbsr_el1(0UL);
		arm64_isb();
	}
}

void arm64_spe_init_pcpu(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_spe_pcpu *spe;
	uint64_t dfr0;
	uint32_t irq;

	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}
	spe = &arm64_spe_pcpus[pcpu_id];
	(void)memset(spe, 0U, sizeof(*spe));
	spe->pcpu_id = pcpu_id;
	spe->reason = ARM64_SPE_REASON_NO_PMSVER;
	dfr0 = read_id_aa64dfr0_el1();
	spe->pmsver = (uint8_t)((dfr0 & ID_AA64DFR0_PMSVER_MASK) >> ID_AA64DFR0_PMSVER_SHIFT);
	if (spe->pmsver == 0U) {
		spe->initialized = true;
		return;
	}
	spe->pmbidr = read_pmbidr_el1();
	spe->pmsidr = read_pmsidr_el1();
	if ((spe->pmbidr & ARM64_PMBIDR_P) != 0UL) {
		spe->reason = ARM64_SPE_REASON_HIGHER_EL_OWNER;
	} else if (beau_config.spe_ppi == UINT32_MAX) {
		spe->reason = ARM64_SPE_REASON_NO_PPI;
	} else if (!arm64_spe_buffer_valid(spe)) {
		spe->reason = ARM64_SPE_REASON_BUFFER;
	} else {
		spe->available = true;
		spe->reason = ARM64_SPE_REASON_NONE;
		irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, beau_config.spe_ppi);
		if ((pcpu_id == BSP_CPU_ID) && !arm64_spe_irq_registered &&
			((irq == IRQ_INVALID) || (request_irq(irq, arm64_spe_irq_handler,
			NULL, IRQF_NONE) < 0))) {
			spe->available = false;
			spe->reason = ARM64_SPE_REASON_NO_PPI;
		} else if (irq != IRQ_INVALID) {
			arm64_spe_irq_registered = true;
			arm64_gicv3_enable_irq(beau_config.spe_ppi);
		}
	}
	spe->initialized = true;
}

void arm64_spe_suspend_cpu(uint64_t epoch)
{
	struct arm64_spe_pcpu *spe = &arm64_spe_pcpus[get_pcpu_id()];

	if (spe->available && (epoch != 0UL)) {
		spe->resume_running = spe->running && (spe->ready_mask == 0U);
		spe->suspend_epoch = epoch;
		arm64_spe_hw_disable(spe);
	}
}

void arm64_spe_resume_cpu(uint64_t epoch)
{
	struct arm64_spe_pcpu *spe = &arm64_spe_pcpus[get_pcpu_id()];

	if (spe->available && spe->resume_running && (spe->suspend_epoch == epoch)) {
		spe->resume_running = false;
		(void)arm64_spe_start_local(spe);
	}
}

int32_t arm64_spe_start(void)
{
	uint16_t pcpu_id;
	int32_t ret = arm64_spe_command(ARM64_SPE_COMMAND_START);

	if (ret != 0) {
		return ret;
	}
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if (arm64_spe_pcpus[pcpu_id].running) {
			return 0;
		}
	}
	return -ENOTSUP;
}

int32_t arm64_spe_stop(void)
{
	return arm64_spe_command(ARM64_SPE_COMMAND_STOP);
}

int32_t arm64_spe_reset(void)
{
	return arm64_spe_command(ARM64_SPE_COMMAND_RESET);
}

static void arm64_spe_capture_snapshot(void *data)
{
	struct arm64_spe_snapshot *snapshot = data;
	uint16_t pcpu_id = get_pcpu_id();

	if (pcpu_id < MAX_PCPU_NUM) {
		snapshot->pcpu[pcpu_id].pmbidr = arm64_spe_pcpus[pcpu_id].pmbidr;
		snapshot->pcpu[pcpu_id].pmsidr = arm64_spe_pcpus[pcpu_id].pmsidr;
		snapshot->pcpu[pcpu_id].buffer_full_count = arm64_spe_pcpus[pcpu_id].buffer_full_count;
		snapshot->pcpu[pcpu_id].error_count = arm64_spe_pcpus[pcpu_id].error_count;
		snapshot->pcpu[pcpu_id].data_loss_count = arm64_spe_pcpus[pcpu_id].data_loss_count;
		(void)memcpy(snapshot->pcpu[pcpu_id].half_bytes, arm64_spe_pcpus[pcpu_id].half_bytes,
			sizeof(snapshot->pcpu[pcpu_id].half_bytes));
		snapshot->pcpu[pcpu_id].pcpu_id = pcpu_id;
		snapshot->pcpu[pcpu_id].pmsver = arm64_spe_pcpus[pcpu_id].pmsver;
		snapshot->pcpu[pcpu_id].ready_mask = arm64_spe_pcpus[pcpu_id].ready_mask;
		snapshot->pcpu[pcpu_id].reason = arm64_spe_pcpus[pcpu_id].reason;
		snapshot->pcpu[pcpu_id].available = arm64_spe_pcpus[pcpu_id].available;
		snapshot->pcpu[pcpu_id].running = arm64_spe_pcpus[pcpu_id].running;
	}
}

int32_t arm64_spe_take_snapshot(struct arm64_spe_snapshot *snapshot)
{
	uint16_t pcpu_num = get_pcpu_nums();

	if (snapshot == NULL) {
		return -EINVAL;
	}
	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->pcpu_num = pcpu_num;
	snapshot->complete = smp_call_function_timeout((1UL << pcpu_num) - 1UL,
		arm64_spe_capture_snapshot, snapshot, ARM64_SPE_SNAPSHOT_TIMEOUT_US);
	return snapshot->complete ? 0 : -ETIMEDOUT;
}

int32_t arm64_spe_dump(uint16_t pcpu_id, uint8_t *buffer, uint32_t *length)
{
	struct arm64_spe_pcpu *spe;
	uint8_t half;
	uint32_t bytes;

	if ((buffer == NULL) || (length == NULL) || (pcpu_id >= get_pcpu_nums())) {
		return -EINVAL;
	}
	spe = &arm64_spe_pcpus[pcpu_id];
	if (spe->ready_mask == 0U) {
		return -ENODATA;
	}
	half = (spe->ready_mask & (1U << 1U)) != 0U ? 1U : 0U;
	bytes = spe->half_bytes[half] > ARM64_SPE_SHELL_DUMP_MAX ?
		ARM64_SPE_SHELL_DUMP_MAX : spe->half_bytes[half];
	(void)memcpy(buffer, (const void *)arm64_spe_half_base(pcpu_id, half), bytes);
	*length = bytes;
	return 0;
}

bool arm64_spe_guest_sysreg(uint64_t sysreg)
{
	uint64_t op0 = (sysreg >> 20U) & 0x3UL;
	uint64_t op1 = (sysreg >> 14U) & 0x7UL;
	uint64_t crn = (sysreg >> 10U) & 0xfUL;
	uint64_t crm = (sysreg >> 1U) & 0xfUL;

	return (op0 == 3UL) && (op1 == 0UL) && (crn == 9UL) &&
		((crm == 9UL) || (crm == 10UL));
}

void arm64_spe_record_guest_access(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (pcpu_id < MAX_PCPU_NUM) {
		arm64_spe_pcpus[pcpu_id].error_count++;
	}
}
