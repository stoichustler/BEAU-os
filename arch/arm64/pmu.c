/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <cpu.h>
#include <notify.h>
#include <per_cpu.h>
#include <schedule.h>
#include <spinlock.h>
#include <timer.h>
#include <ticks.h>
#include <vcpu.h>
#include <vm.h>
#include <rtl.h>
#include <logmsg.h>
#include <sprintf.h>
#include <asm/pmu.h>
#include <asm/sysreg.h>

/* [20260717] EL2-owned core PMU accounting
 *
 * pmustat command -> per-pCPU worker -> local physical PMU
 *                                           |
 * vCPU switch ------------------------------+--> host/vCPU totals
 *                                           |
 * MMIO/vGIC/virtio token -------------------+--> path totals
 *                                           |
 * snapshot request -> callback on each pCPU +--> diagnostic copy
 *
 * Key rules:
 *   - each pCPU owns its PMU registers, timer, baseline, and mutable totals;
 *   - guest execution changes the accounting owner, not the hardware setup;
 *   - guests cannot program or observe the EL2-owned physical counters;
 *   - bounded cross-CPU operations may report partial data but never hang.
 */
#define ARM64_CORE_PMU_INVALID_COUNTER	0xffU
#define ARM64_CORE_PMU_CYCLE_COUNTER	31U
#define ARM64_CORE_PMU_COMMAND_TIMEOUT_US 100000U
#define ARM64_CORE_PMU_SNAPSHOT_TIMEOUT_US 10000U
#define ARM64_CORE_PMU_WORKER_BVT_WEIGHT 1U
#define ARM64_CORE_PMU_WORKER_BVT_WARP_VALUE 8
#define ARM64_CORE_PMU_WORKER_BVT_WARP_LIMIT 1U
#define ARM64_CORE_PMU_WORKER_BVT_UNWARP_PERIOD 4U
#define ARM64_CORE_PMU_COMMAND_MASK	0x3UL
#define ARM64_CORE_PMU_COMMAND_SHIFT	2U
#define ARM64_CORE_PMUVER_NOT_IMPLEMENTED 0U
#define ARM64_CORE_PMUVER_IMPLEMENTATION_DEFINED 0xfU
#define ARM64_CORE_PMUVER_V3P5		6U

#define ARM64_PMU_EVENT_INST_RETIRED	0x0008U
#define ARM64_PMU_EVENT_BRANCH_MISPRED	0x0010U
#define ARM64_PMU_EVENT_STALL_FRONTEND	0x0023U
#define ARM64_PMU_EVENT_STALL_BACKEND	0x0024U
#define ARM64_PMU_EVENT_L1D_REFILL	0x0003U
#define ARM64_PMU_EVENT_DTLB_WALK	0x0034U

enum arm64_core_pmu_owner {
	ARM64_CORE_PMU_OWNER_HOST = 0U,
	ARM64_CORE_PMU_OWNER_VCPU,
};

enum arm64_core_pmu_command {
	ARM64_CORE_PMU_COMMAND_NONE = 0U,
	ARM64_CORE_PMU_COMMAND_START,
	ARM64_CORE_PMU_COMMAND_STOP,
	ARM64_CORE_PMU_COMMAND_RESET,
};

struct arm64_core_pmu_raw {
	uint64_t value[ARM64_CORE_PMU_EVENT_NUM];
};

struct arm64_core_pmu_vcpu_local {
	struct arm64_core_pmu_values total;
	struct arm64_core_pmu_path_values path[ARM64_CORE_PMU_PATH_NUM];
	uint64_t enabled_origin_ticks;
	uint64_t denied_accesses;
	uint16_t vcpu_id;
	bool seen;
};

/* This object combines four ownership domains. Capability and hardware state
 * are pCPU-local; host/vCPU totals are updated only on that pCPU; command
 * tokens are published by the global controller and acknowledged by the local
 * worker; STR fields retain software intent while physical counters are lost.
 */
struct arm64_core_pmu_pcpu {
	struct arm64_core_pmu_capability capability;
	struct arm64_core_pmu_raw baseline;
	struct arm64_core_pmu_values host;
	struct arm64_core_pmu_vcpu_local vcpu[CONFIG_MAX_VM_NUM];
	struct hv_timer poll_timer;
	struct thread_object worker;
	uint8_t worker_stack[CONFIG_STACK_SIZE] __aligned(16);
	uint64_t baseline_ticks;
	uint64_t enabled_ticks;
	uint64_t epoch;
	uint64_t owner_generation;
	uint64_t overflow_count;
	uint64_t suspend_epoch;
	volatile uint64_t request_token;
	volatile uint64_t acknowledge_token;
	volatile int32_t command_status;
	uint16_t pcpu_id;
	uint16_t owner_vm_id;
	uint16_t owner_vcpu_id;
	enum arm64_core_pmu_owner owner;
	bool initialized;
	bool running;
	bool poll_timer_started;
	bool suspended;
	bool resume_running;
};

#if !defined(CONFIG_PLATFORM_QEMU)
struct arm64_core_pmu_event_desc {
	enum arm64_core_pmu_event event;
	uint16_t event_id;
};

static const struct arm64_core_pmu_event_desc arm64_core_pmu_optional_events[] = {
	{ ARM64_CORE_PMU_STALL_FRONTEND, ARM64_PMU_EVENT_STALL_FRONTEND },
	{ ARM64_CORE_PMU_STALL_BACKEND, ARM64_PMU_EVENT_STALL_BACKEND },
	{ ARM64_CORE_PMU_L1D_REFILL, ARM64_PMU_EVENT_L1D_REFILL },
	{ ARM64_CORE_PMU_DTLB_WALK, ARM64_PMU_EVENT_DTLB_WALK },
	{ ARM64_CORE_PMU_BRANCH_MISPRED, ARM64_PMU_EVENT_BRANCH_MISPRED },
};
#endif

static struct arm64_core_pmu_pcpu arm64_core_pmu_pcpus[MAX_PCPU_NUM];
static spinlock_t arm64_core_pmu_command_lock = { .head = 0U, .tail = 0U };
static uint64_t arm64_core_pmu_epoch = 1UL;
static uint64_t arm64_core_pmu_command_generation;
static bool arm64_core_pmu_requested_running;
static bool arm64_core_pmu_workers_initialized;
static bool arm64_core_pmu_command_active;

static uint64_t arm64_core_pmu_saturating_add(uint64_t left, uint64_t right)
{
	return (right > (UINT64_MAX - left)) ? UINT64_MAX : left + right;
}

static void arm64_core_pmu_add_values(struct arm64_core_pmu_values *target,
	const struct arm64_core_pmu_values *source)
{
	uint32_t event;

	for (event = 0U; event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		target->value[event] = arm64_core_pmu_saturating_add(
			target->value[event], source->value[event]);
	}
	target->running_ticks = arm64_core_pmu_saturating_add(
		target->running_ticks, source->running_ticks);
}

static uint64_t arm64_core_pmu_read_event_counter(uint8_t counter)
{
	uint64_t value = 0UL;

	switch (counter) {
	case 0U:
		value = read_pmevcntr0_el0();
		break;
	case 1U:
		value = read_pmevcntr1_el0();
		break;
	case 2U:
		value = read_pmevcntr2_el0();
		break;
	case 3U:
		value = read_pmevcntr3_el0();
		break;
	case 4U:
		value = read_pmevcntr4_el0();
		break;
	case 5U:
		value = read_pmevcntr5_el0();
		break;
	default:
		break;
	}

	return value;
}

static void arm64_core_pmu_write_event_counter(uint8_t counter, uint64_t value)
{
	switch (counter) {
	case 0U:
		write_pmevcntr0_el0(value);
		break;
	case 1U:
		write_pmevcntr1_el0(value);
		break;
	case 2U:
		write_pmevcntr2_el0(value);
		break;
	case 3U:
		write_pmevcntr3_el0(value);
		break;
	case 4U:
		write_pmevcntr4_el0(value);
		break;
	case 5U:
		write_pmevcntr5_el0(value);
		break;
	default:
		break;
	}
}

static void arm64_core_pmu_write_event_type(uint8_t counter, uint64_t value)
{
	switch (counter) {
	case 0U:
		write_pmevtyper0_el0(value);
		break;
	case 1U:
		write_pmevtyper1_el0(value);
		break;
	case 2U:
		write_pmevtyper2_el0(value);
		break;
	case 3U:
		write_pmevtyper3_el0(value);
		break;
	case 4U:
		write_pmevtyper4_el0(value);
		break;
	case 5U:
		write_pmevtyper5_el0(value);
		break;
	default:
		break;
	}
}

static bool arm64_core_pmu_event_supported(
	const struct arm64_core_pmu_capability *capability, uint16_t event_id)
{
	bool supported = false;

	if (event_id < 32U) {
		supported = (capability->pmceid0 & (1UL << event_id)) != 0UL;
	} else if (event_id < 64U) {
		supported = (capability->pmceid1 & (1UL << (event_id - 32U))) != 0UL;
	}

	return supported;
}

static uint64_t arm64_core_pmu_all_counter_mask(
	const struct arm64_core_pmu_capability *capability)
{
	uint64_t event_mask = (capability->counter_num == 0U) ? 0UL :
		((1UL << capability->counter_num) - 1UL);

	return event_mask | (1UL << ARM64_CORE_PMU_CYCLE_COUNTER);
}

static uint64_t arm64_core_pmu_used_counter_mask(
	const struct arm64_core_pmu_capability *capability)
{
	uint64_t mask = 1UL << ARM64_CORE_PMU_CYCLE_COUNTER;
	uint32_t event;

	for (event = ARM64_CORE_PMU_INSTRUCTIONS;
		event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		uint8_t counter = capability->event_counter[event];

		if (counter != ARM64_CORE_PMU_INVALID_COUNTER) {
			mask |= 1UL << counter;
		}
	}

	return mask;
}

static uint16_t arm64_core_pmu_arch_event_id(enum arm64_core_pmu_event event)
{
	uint16_t event_id = 0U;

	switch (event) {
	case ARM64_CORE_PMU_INSTRUCTIONS:
		event_id = ARM64_PMU_EVENT_INST_RETIRED;
		break;
	case ARM64_CORE_PMU_STALL_FRONTEND:
		event_id = ARM64_PMU_EVENT_STALL_FRONTEND;
		break;
	case ARM64_CORE_PMU_STALL_BACKEND:
		event_id = ARM64_PMU_EVENT_STALL_BACKEND;
		break;
	case ARM64_CORE_PMU_L1D_REFILL:
		event_id = ARM64_PMU_EVENT_L1D_REFILL;
		break;
	case ARM64_CORE_PMU_DTLB_WALK:
		event_id = ARM64_PMU_EVENT_DTLB_WALK;
		break;
	case ARM64_CORE_PMU_BRANCH_MISPRED:
		event_id = ARM64_PMU_EVENT_BRANCH_MISPRED;
		break;
	default:
		break;
	}

	return event_id;
}

/* [20260717] Capability-driven event allocation
 *
 * ID_AA64DFR0_EL1 + PMCR_EL0 + PMCEID<n>_EL0
 *                         |
 *                         v
 * cycles + required instructions + supported optional events
 *                         |
 *                         +--> unavailable: keep PMU disabled
 *
 * Key rules:
 *   - software allocates only implemented counters and advertised events;
 *   - INST_RETIRED is required on hardware for useful IPC attribution;
 *   - QEMU may run cycles-only for control and isolation validation.
 */
static void arm64_core_pmu_probe(struct arm64_core_pmu_pcpu *pcpu)
{
	struct arm64_core_pmu_capability *capability = &pcpu->capability;
	uint64_t dfr0 = read_id_aa64dfr0_el1();
	uint64_t pmcr;
	uint8_t next_counter = 0U;
	uint32_t event;
#if !defined(CONFIG_PLATFORM_QEMU)
	uint32_t idx;
#endif

	(void)memset(capability, 0U, sizeof(*capability));
	for (event = 0U; event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		capability->event_counter[event] = ARM64_CORE_PMU_INVALID_COUNTER;
	}
	capability->pmuver = (uint8_t)((dfr0 & ID_AA64DFR0_PMUVER_MASK) >>
		ID_AA64DFR0_PMUVER_SHIFT);
	if ((capability->pmuver == ARM64_CORE_PMUVER_NOT_IMPLEMENTED) ||
		(capability->pmuver == ARM64_CORE_PMUVER_IMPLEMENTATION_DEFINED)) {
		return;
	}

	pmcr = read_pmcr_el0();
	capability->counter_num = (uint8_t)((pmcr >> PMCR_EL0_N_SHIFT) &
		PMCR_EL0_N_MASK);
	capability->cycle_width = 64U;
	capability->event_width = (capability->pmuver >= ARM64_CORE_PMUVER_V3P5) ?
		64U : 32U;
	capability->pmceid0 = read_pmceid0_el0();
	capability->pmceid1 = read_pmceid1_el0();
	capability->event_mask = 1U << ARM64_CORE_PMU_CYCLES;

	if (capability->counter_num == 0U) {
		return;
	}

	if (arm64_core_pmu_event_supported(capability,
		ARM64_PMU_EVENT_INST_RETIRED)) {
		capability->event_counter[ARM64_CORE_PMU_INSTRUCTIONS] = next_counter++;
		capability->event_mask |= 1U << ARM64_CORE_PMU_INSTRUCTIONS;
#if !defined(CONFIG_PLATFORM_QEMU)
	} else {
		return;
#endif
	}

#if !defined(CONFIG_PLATFORM_QEMU)
	for (idx = 0U; (idx < (sizeof(arm64_core_pmu_optional_events) /
		sizeof(arm64_core_pmu_optional_events[0]))) &&
		(next_counter < capability->counter_num) &&
		(next_counter < ARM64_CORE_PMU_MAX_COUNTERS); idx++) {
		const struct arm64_core_pmu_event_desc *desc =
			&arm64_core_pmu_optional_events[idx];

		if (arm64_core_pmu_event_supported(capability, desc->event_id)) {
			capability->event_counter[desc->event] = next_counter++;
			capability->event_mask |= 1U << desc->event;
		}
	}
#endif
	capability->available = true;
}

static bool arm64_core_pmu_present(const struct arm64_core_pmu_pcpu *pcpu)
{
	return (pcpu->capability.pmuver != ARM64_CORE_PMUVER_NOT_IMPLEMENTED) &&
		(pcpu->capability.pmuver != ARM64_CORE_PMUVER_IMPLEMENTATION_DEFINED);
}

/* [20260717] Physical PMU programming and guest isolation
 *
 * disable counters/interrupts
 *        |
 *        v
 * trap guest PMU control in MDCR_EL2
 *        |
 *        v
 * reset and program EL2-only events -> enable selected counters
 *
 * Key rules:
 *   - programming starts from a disabled counter set and clears stale state;
 *   - filters count EL2 work while TPM/TPMCR deny guest register ownership;
 *   - the enable step is last so no partially programmed interval is sampled.
 */
static void arm64_core_pmu_configure_mdcr(
	const struct arm64_core_pmu_pcpu *pcpu)
{
	uint64_t mdcr = arm64_sysreg_read(mdcr_el2);

	/* Keep all counters in the PMCR-controlled range; HPMN=0 is unsafe
	 * before FEAT_HPMN0. TPM/TPMCR still deny Guest register control.
	 */
	mdcr &= ~(MDCR_EL2_HPMN_MASK | MDCR_EL2_HPME | MDCR_EL2_HPMD |
		MDCR_EL2_HCCD);
	mdcr |= ((uint64_t)pcpu->capability.counter_num & MDCR_EL2_HPMN_MASK) |
		MDCR_EL2_TPM | MDCR_EL2_TPMCR;
	arm64_sysreg_write_sync(mdcr_el2, mdcr);
}

static void arm64_core_pmu_hw_disable(struct arm64_core_pmu_pcpu *pcpu)
{
	uint64_t mask = arm64_core_pmu_all_counter_mask(&pcpu->capability);
	uint64_t pmcr = read_pmcr_el0() & PMCR_EL0_WRITABLE_MASK;

	write_pmcntenclr_el0(mask);
	write_pmintenclr_el1(mask);
	write_pmcr_el0(pmcr & ~PMCR_EL0_E);
	write_pmuserenr_el0(0UL);
	arm64_core_pmu_configure_mdcr(pcpu);
}

static void arm64_core_pmu_hw_program(struct arm64_core_pmu_pcpu *pcpu)
{
	const struct arm64_core_pmu_capability *capability = &pcpu->capability;
	uint64_t pmcr = read_pmcr_el0() & PMCR_EL0_WRITABLE_MASK;
	uint64_t all_mask = arm64_core_pmu_all_counter_mask(capability);
	uint32_t event;

	arm64_core_pmu_hw_disable(pcpu);
	pmcr &= ~(PMCR_EL0_E | PMCR_EL0_D | PMCR_EL0_X | PMCR_EL0_DP |
		PMCR_EL0_LP);
	pmcr |= PMCR_EL0_P | PMCR_EL0_C | PMCR_EL0_LC;
	if (capability->event_width == 64U) {
		pmcr |= PMCR_EL0_LP;
	}
	write_pmcr_el0(pmcr);
	write_pmccntr_el0(0UL);
	write_pmccfiltr_el0(PMU_EVENT_INCLUDE_EL2);
	write_pmuserenr_el0(0UL);
	write_pmintenclr_el1(all_mask);
	write_pmovsclr_el0(all_mask);

	for (event = ARM64_CORE_PMU_INSTRUCTIONS;
		event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		uint8_t counter = capability->event_counter[event];

		if (counter != ARM64_CORE_PMU_INVALID_COUNTER) {
			arm64_core_pmu_write_event_counter(counter, 0UL);
			arm64_core_pmu_write_event_type(counter,
				(uint64_t)arm64_core_pmu_arch_event_id(event) |
				PMU_EVENT_INCLUDE_EL2);
		}
	}
	arm64_isb();
}

static void arm64_core_pmu_hw_enable(struct arm64_core_pmu_pcpu *pcpu)
{
	uint64_t pmcr = read_pmcr_el0() & PMCR_EL0_WRITABLE_MASK;

	arm64_core_pmu_configure_mdcr(pcpu);
	write_pmcntenset_el0(arm64_core_pmu_used_counter_mask(&pcpu->capability));
	write_pmcr_el0(pmcr | PMCR_EL0_E);
}

static uint64_t arm64_core_pmu_counter_delta(uint64_t current, uint64_t previous,
	uint8_t width)
{
	return (width == 64U) ? (current - previous) :
		((current - previous) & UINT32_MAX);
}

static void arm64_core_pmu_read_raw(const struct arm64_core_pmu_pcpu *pcpu,
	struct arm64_core_pmu_raw *raw)
{
	const struct arm64_core_pmu_capability *capability = &pcpu->capability;
	uint32_t event;

	(void)memset(raw, 0U, sizeof(*raw));
	raw->value[ARM64_CORE_PMU_CYCLES] = read_pmccntr_el0();
	for (event = ARM64_CORE_PMU_INSTRUCTIONS;
		event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		uint8_t counter = capability->event_counter[event];

		if (counter != ARM64_CORE_PMU_INVALID_COUNTER) {
			raw->value[event] = arm64_core_pmu_read_event_counter(counter);
		}
	}
}

static struct arm64_core_pmu_values *arm64_core_pmu_owner_values(
	struct arm64_core_pmu_pcpu *pcpu)
{
	struct arm64_core_pmu_values *values = &pcpu->host;

	if ((pcpu->owner == ARM64_CORE_PMU_OWNER_VCPU) &&
		(pcpu->owner_vm_id < CONFIG_MAX_VM_NUM) &&
		(pcpu->owner_vcpu_id < MAX_VCPUS_PER_VM)) {
		values = &pcpu->vcpu[pcpu->owner_vm_id].total;
	}
	return values;
}

/* [20260717] Baseline-to-delta accounting
 *
 * previous baseline -> read counters -> width-aware delta -> current owner
 *                              |                              |
 *                              +--> refresh baseline/ticks <--+
 *
 * Key rules:
 *   - the owner is selected only after IRQ-local serialization begins;
 *   - modular subtraction handles architectural counter wraparound;
 *   - saturating software totals preserve monotonic diagnostics;
 *   - polling bounds information loss because overflow IRQs stay disabled.
 */
static void arm64_core_pmu_account_now(struct arm64_core_pmu_pcpu *pcpu)
{
	struct arm64_core_pmu_values *values;
	struct arm64_core_pmu_raw current;
	uint64_t now;
	uint64_t elapsed;
	uint64_t overflow;
	uint64_t flags;
	uint32_t event;

	local_irq_save(&flags);
	if (!pcpu->running || !pcpu->capability.available) {
		local_irq_restore(flags);
		return;
	}

	arm64_core_pmu_read_raw(pcpu, &current);
	now = cpu_ticks();
	elapsed = now - pcpu->baseline_ticks;
	values = arm64_core_pmu_owner_values(pcpu);
	for (event = 0U; event < ARM64_CORE_PMU_EVENT_NUM; event++) {
		uint8_t width = (event == ARM64_CORE_PMU_CYCLES) ?
			pcpu->capability.cycle_width : pcpu->capability.event_width;
		uint64_t delta;

		if ((pcpu->capability.event_mask & (1U << event)) == 0U) {
			continue;
		}
		delta = arm64_core_pmu_counter_delta(current.value[event],
			pcpu->baseline.value[event], width);
		values->value[event] = arm64_core_pmu_saturating_add(
			values->value[event], delta);
	}
	values->running_ticks = arm64_core_pmu_saturating_add(
		values->running_ticks, elapsed);
	pcpu->enabled_ticks = arm64_core_pmu_saturating_add(
		pcpu->enabled_ticks, elapsed);
	pcpu->baseline = current;
	pcpu->baseline_ticks = now;

	overflow = read_pmovsclr_el0() &
		arm64_core_pmu_used_counter_mask(&pcpu->capability);
	if (overflow != 0UL) {
		pcpu->overflow_count = arm64_core_pmu_saturating_add(
			pcpu->overflow_count, 1UL);
		write_pmovsclr_el0(overflow);
	}
	local_irq_restore(flags);
}

static void arm64_core_pmu_set_baseline(struct arm64_core_pmu_pcpu *pcpu)
{
	arm64_core_pmu_read_raw(pcpu, &pcpu->baseline);
	pcpu->baseline_ticks = cpu_ticks();
}

static void arm64_core_pmu_poll(void *data)
{
	struct arm64_core_pmu_pcpu *pcpu = data;

	if ((pcpu != NULL) && (pcpu->pcpu_id == get_pcpu_id())) {
		arm64_core_pmu_account_now(pcpu);
	}
}

static void arm64_core_pmu_clear_stats(struct arm64_core_pmu_pcpu *pcpu)
{
	uint16_t vm_id;

	(void)memset(&pcpu->host, 0U, sizeof(pcpu->host));
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		uint16_t vcpu_id = pcpu->vcpu[vm_id].vcpu_id;
		bool seen = pcpu->vcpu[vm_id].seen;

		(void)memset(&pcpu->vcpu[vm_id], 0U, sizeof(pcpu->vcpu[vm_id]));
		pcpu->vcpu[vm_id].vcpu_id = vcpu_id;
		pcpu->vcpu[vm_id].seen = seen;
	}
	(void)memset(&pcpu->baseline, 0U, sizeof(pcpu->baseline));
	pcpu->baseline_ticks = 0UL;
	pcpu->enabled_ticks = 0UL;
	pcpu->overflow_count = 0UL;
}

static int32_t arm64_core_pmu_add_poll_timer(struct arm64_core_pmu_pcpu *pcpu)
{
	uint64_t period = us_to_ticks(ARM64_CORE_PMU_POLL_US);
	int32_t status = 0;

	if (!pcpu->poll_timer_started) {
		update_timer(&pcpu->poll_timer, cpu_ticks() + period, period);
		status = add_timer(&pcpu->poll_timer);
		pcpu->poll_timer_started = status == 0;
	}
	return status;
}

static void arm64_core_pmu_delete_poll_timer(struct arm64_core_pmu_pcpu *pcpu)
{
	if (pcpu->poll_timer_started) {
		del_timer(&pcpu->poll_timer);
		pcpu->poll_timer_started = false;
		update_timer(&pcpu->poll_timer, 0UL, 0UL);
	}
}

static int32_t arm64_core_pmu_start_local(struct arm64_core_pmu_pcpu *pcpu)
{
	int32_t status;

	pcpu->epoch = arm64_core_pmu_epoch;
	if (!pcpu->capability.available || pcpu->running || pcpu->suspended) {
		return pcpu->capability.available ? 0 : -ENODEV;
	}

	arm64_core_pmu_hw_program(pcpu);
	status = arm64_core_pmu_add_poll_timer(pcpu);
	if (status != 0) {
		arm64_core_pmu_hw_disable(pcpu);
		LOG_ERR("PMU:    CPU%hu poll timer start failed status:%d", pcpu->pcpu_id,
			status);
		return status;
	}
	arm64_core_pmu_hw_enable(pcpu);
	arm64_core_pmu_set_baseline(pcpu);
	pcpu->running = true;
	return 0;
}

static void arm64_core_pmu_stop_local(struct arm64_core_pmu_pcpu *pcpu)
{
	if (pcpu->running) {
		arm64_core_pmu_account_now(pcpu);
		pcpu->running = false;
		arm64_core_pmu_hw_disable(pcpu);
	}
	arm64_core_pmu_delete_poll_timer(pcpu);
}

static int32_t arm64_core_pmu_reset_local(struct arm64_core_pmu_pcpu *pcpu)
{
	bool was_running = pcpu->running;
	int32_t status = 0;

	if (was_running) {
		arm64_core_pmu_account_now(pcpu);
		pcpu->running = false;
		arm64_core_pmu_hw_disable(pcpu);
	}
	arm64_core_pmu_clear_stats(pcpu);
	pcpu->epoch = arm64_core_pmu_epoch;
	pcpu->owner_generation++;

	if (pcpu->capability.available) {
		arm64_core_pmu_hw_program(pcpu);
		if (was_running) {
			arm64_core_pmu_hw_enable(pcpu);
			arm64_core_pmu_set_baseline(pcpu);
			pcpu->running = true;
		}
	} else {
		if (arm64_core_pmu_present(pcpu)) {
			arm64_core_pmu_hw_disable(pcpu);
		} else {
			arm64_core_pmu_configure_mdcr(pcpu);
		}
		status = -ENODEV;
	}
	return status;
}

/* [20260717] Core PMU command ownership
 *
 * shell command                per-pCPU PMU worker
 *      | publish op/generation       |
 *      +---------------------------->| schedule in thread context
 *      |                             +-- add/del local poll timer
 *      |<----------------------------+-- publish acknowledgement
 *
 * Key rules:
 *   - PMU registers and timer lists are touched by their owning pCPU;
 *   - workers run only for explicit commands, so stopped PMU has no timer;
 *   - a bounded generation wait reports partial control instead of hanging.
 */
static void arm64_core_pmu_worker(struct thread_object *thread)
{
	struct arm64_core_pmu_pcpu *pcpu =
		&arm64_core_pmu_pcpus[get_pcpu_id()];

	ASSERT(thread == &pcpu->worker, "PMU worker on wrong pCPU\n");
	while (true) {
		uint64_t request_token = __atomic_load_n(
			&pcpu->request_token, __ATOMIC_ACQUIRE);
		int32_t status = 0;

		if (request_token != __atomic_load_n(&pcpu->acknowledge_token,
			__ATOMIC_RELAXED)) {
			switch ((enum arm64_core_pmu_command)(request_token &
				ARM64_CORE_PMU_COMMAND_MASK)) {
			case ARM64_CORE_PMU_COMMAND_START:
				status = arm64_core_pmu_start_local(pcpu);
				break;
			case ARM64_CORE_PMU_COMMAND_STOP:
				arm64_core_pmu_stop_local(pcpu);
				break;
			case ARM64_CORE_PMU_COMMAND_RESET:
				status = arm64_core_pmu_reset_local(pcpu);
				break;
			default:
				break;
			}
			__atomic_store_n(&pcpu->command_status, status, __ATOMIC_RELAXED);
			__atomic_store_n(&pcpu->acknowledge_token,
				request_token, __ATOMIC_RELEASE);
		}

		sleep_thread(thread);
		if (__atomic_load_n(&pcpu->request_token, __ATOMIC_ACQUIRE) !=
			__atomic_load_n(&pcpu->acknowledge_token, __ATOMIC_RELAXED)) {
			wake_thread(thread);
		}
		schedule();
	}
}

static uint64_t arm64_core_pmu_unacknowledged_mask(uint64_t request_token)
{
	uint64_t mask = 0UL;
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if (__atomic_load_n(
			&arm64_core_pmu_pcpus[pcpu_id].acknowledge_token,
			__ATOMIC_ACQUIRE) != request_token) {
			mask |= 1UL << pcpu_id;
		}
	}
	return mask;
}

static uint64_t arm64_core_pmu_next_command_token(
	enum arm64_core_pmu_command command)
{
	if (arm64_core_pmu_command_generation >=
		(UINT64_MAX >> ARM64_CORE_PMU_COMMAND_SHIFT)) {
		arm64_core_pmu_command_generation = 1UL;
	} else {
		arm64_core_pmu_command_generation++;
	}
	return (arm64_core_pmu_command_generation <<
		ARM64_CORE_PMU_COMMAND_SHIFT) | (uint64_t)command;
}

static void arm64_core_pmu_publish_command(uint64_t request_token)
{
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		struct arm64_core_pmu_pcpu *pcpu = &arm64_core_pmu_pcpus[pcpu_id];

		__atomic_store_n(&pcpu->command_status, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&pcpu->request_token, request_token,
			__ATOMIC_RELEASE);
		request_thread_priority_no_resched(&pcpu->worker);
		wake_thread(&pcpu->worker);
	}
}

static int32_t arm64_core_pmu_wait_command(uint64_t request_token,
	uint64_t *unacknowledged)
{
	uint64_t deadline = cpu_ticks() +
		us_to_ticks(ARM64_CORE_PMU_COMMAND_TIMEOUT_US);
	uint64_t mask;

	do {
		mask = arm64_core_pmu_unacknowledged_mask(request_token);
		if (mask == 0UL) {
			*unacknowledged = 0UL;
			return 0;
		}
		if ((int64_t)(cpu_ticks() - deadline) >= 0L) {
			*unacknowledged = mask;
			return -ETIMEDOUT;
		}
		yield_current();
		schedule();
	} while (true);
}

static bool arm64_core_pmu_any_available(void)
{
	uint16_t pcpu_id;

	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		if (arm64_core_pmu_pcpus[pcpu_id].capability.available) {
			return true;
		}
	}
	return false;
}

static int32_t arm64_core_pmu_issue_command(enum arm64_core_pmu_command command)
{
	uint64_t flags;
	uint64_t request_token;
	uint64_t unacknowledged = 0UL;
	uint16_t pcpu_id;
	int32_t status = 0;

	spinlock_irqsave_obtain(&arm64_core_pmu_command_lock, &flags);
	if (!arm64_core_pmu_workers_initialized || arm64_core_pmu_command_active) {
		spinlock_irqrestore_release(&arm64_core_pmu_command_lock, flags);
		return -EBUSY;
	}
	arm64_core_pmu_command_active = true;
	if (command == ARM64_CORE_PMU_COMMAND_START) {
		arm64_core_pmu_requested_running = true;
	} else if (command == ARM64_CORE_PMU_COMMAND_STOP) {
		arm64_core_pmu_requested_running = false;
	} else if (command == ARM64_CORE_PMU_COMMAND_RESET) {
		arm64_core_pmu_epoch = (arm64_core_pmu_epoch == UINT64_MAX) ?
			1UL : arm64_core_pmu_epoch + 1UL;
	}
	request_token = arm64_core_pmu_next_command_token(command);
	spinlock_irqrestore_release(&arm64_core_pmu_command_lock, flags);

	arm64_core_pmu_publish_command(request_token);
	status = arm64_core_pmu_wait_command(request_token, &unacknowledged);
	if (status == -ETIMEDOUT) {
		LOG_ERR("PMU:    command:%u generation:%lu timeout unacked:0x%lx",
			(uint32_t)command,
			request_token >> ARM64_CORE_PMU_COMMAND_SHIFT,
			unacknowledged);
	}

	if ((status == 0) && !arm64_core_pmu_any_available()) {
		status = -ENODEV;
	} else if (status == 0) {
		for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
			const struct arm64_core_pmu_pcpu *pcpu =
				&arm64_core_pmu_pcpus[pcpu_id];
			int32_t local_status = __atomic_load_n(&pcpu->command_status,
				__ATOMIC_ACQUIRE);

			if ((local_status != 0) && (local_status != -ENODEV)) {
				status = local_status;
				break;
			}
		}
	}

	if ((command == ARM64_CORE_PMU_COMMAND_START) && (status != 0)) {
		int32_t rollback_status;

		spinlock_irqsave_obtain(&arm64_core_pmu_command_lock, &flags);
		arm64_core_pmu_requested_running = false;
		request_token = arm64_core_pmu_next_command_token(
			ARM64_CORE_PMU_COMMAND_STOP);
		spinlock_irqrestore_release(&arm64_core_pmu_command_lock, flags);

		arm64_core_pmu_publish_command(request_token);
		rollback_status = arm64_core_pmu_wait_command(request_token,
			&unacknowledged);
		if (rollback_status != 0) {
			LOG_ERR("PMU:    START rollback generation:%lu timeout unacked:0x%lx",
				request_token >> ARM64_CORE_PMU_COMMAND_SHIFT,
				unacknowledged);
		}
	}
	spinlock_irqsave_obtain(&arm64_core_pmu_command_lock, &flags);
	arm64_core_pmu_command_active = false;
	spinlock_irqrestore_release(&arm64_core_pmu_command_lock, flags);
	return status;
}

int32_t arm64_core_pmu_start(void)
{
	return arm64_core_pmu_issue_command(ARM64_CORE_PMU_COMMAND_START);
}

int32_t arm64_core_pmu_stop(void)
{
	return arm64_core_pmu_issue_command(ARM64_CORE_PMU_COMMAND_STOP);
}

int32_t arm64_core_pmu_reset(void)
{
	return arm64_core_pmu_issue_command(ARM64_CORE_PMU_COMMAND_RESET);
}

/* [20260717] Scheduler-bound accounting ownership
 *
 * host interval -> account old delta -> publish VM/vCPU owner
 * guest interval -> account old delta -> publish host owner
 *
 * Key rules:
 *   - closing the old interval precedes every owner transition;
 *   - owner_generation invalidates path tokens that cross a vCPU switch;
 *   - totals remain attached to the pCPU on which the vCPU actually ran.
 */
void arm64_core_pmu_vcpu_load(const struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	if (!pcpu->initialized) {
		return;
	}
	arm64_core_pmu_account_now(pcpu);
	pcpu->owner_vm_id = vcpu->vm->vm_id;
	pcpu->owner_vcpu_id = vcpu->vcpu_id;
	pcpu->owner_generation++;
	if ((pcpu->owner_vm_id < CONFIG_MAX_VM_NUM) &&
		(pcpu->owner_vcpu_id < MAX_VCPUS_PER_VM)) {
		struct arm64_core_pmu_vcpu_local *local =
			&pcpu->vcpu[pcpu->owner_vm_id];

		if (!local->seen) {
			local->enabled_origin_ticks = pcpu->enabled_ticks;
		}
		local->vcpu_id = vcpu->vcpu_id;
		local->seen = true;
	}
	pcpu->owner = ARM64_CORE_PMU_OWNER_VCPU;
}

void arm64_core_pmu_vcpu_unload(const struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;

	if ((vcpu == NULL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	if (!pcpu->initialized) {
		return;
	}
	arm64_core_pmu_account_now(pcpu);
	pcpu->owner = ARM64_CORE_PMU_OWNER_HOST;
	pcpu->owner_vm_id = CONFIG_MAX_VM_NUM;
	pcpu->owner_vcpu_id = MAX_VCPUS_PER_VM;
	pcpu->owner_generation++;
}

/* Each callback runs on the pCPU whose live counters it closes. The callback
 * then copies only software state into the caller-owned snapshot, so the shell
 * never reads another CPU's mutable PMU state directly.
 */
static void arm64_core_pmu_capture_snapshot(void *data)
{
	struct arm64_core_pmu_snapshot *snapshot = data;
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu_snapshot *target;
	struct arm64_core_pmu_pcpu *source;
	uint16_t vm_id;

	if ((snapshot == NULL) || (pcpu_id >= snapshot->pcpu_num)) {
		return;
	}
	source = &arm64_core_pmu_pcpus[pcpu_id];
	target = &snapshot->pcpu[pcpu_id];
	arm64_core_pmu_account_now(source);
	target->capability = source->capability;
	target->host = source->host;
	target->enabled_ticks = source->enabled_ticks;
	target->overflow_count = source->overflow_count;
	target->pcpu_id = pcpu_id;
	target->running = source->running;
	target->suspended = source->suspended;
	target->valid = true;
	target->total = source->host;
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		const struct arm64_core_pmu_vcpu_local *local = &source->vcpu[vm_id];
		struct arm64_core_pmu_vcpu_snapshot *vcpu = &target->vcpu[vm_id];

		vcpu->total = local->total;
		(void)memcpy(vcpu->path, local->path, sizeof(vcpu->path));
		vcpu->enabled_ticks =
			(source->enabled_ticks > local->enabled_origin_ticks) ?
			(source->enabled_ticks - local->enabled_origin_ticks) : 0UL;
		vcpu->denied_accesses = local->denied_accesses;
		vcpu->vm_id = vm_id;
		vcpu->vcpu_id = local->vcpu_id;
		vcpu->valid = local->seen;
		if (local->seen) {
			arm64_core_pmu_add_values(&target->total, &local->total);
		}
	}
}

int32_t arm64_core_pmu_take_snapshot(struct arm64_core_pmu_snapshot *snapshot)
{
	uint16_t pcpu_num;
	bool complete;

	if (snapshot == NULL) {
		return -EINVAL;
	}
	pcpu_num = get_pcpu_nums();
	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->epoch = arm64_core_pmu_epoch;
	snapshot->requested_running = arm64_core_pmu_requested_running;
	snapshot->pcpu_num = pcpu_num;
	complete = smp_call_function_timeout((1UL << pcpu_num) - 1UL,
		arm64_core_pmu_capture_snapshot, snapshot,
		ARM64_CORE_PMU_SNAPSHOT_TIMEOUT_US);
	snapshot->complete = complete;
	return complete ? 0 : -ETIMEDOUT;
}

/* Guest PMU accesses are diagnostic evidence of a denied ownership request.
 * The trap handler returns an inert value for reads and never forwards writes
 * to the EL2-owned physical PMU.
 */
void arm64_core_pmu_record_guest_access(const struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;
	uint16_t vm_id;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	vm_id = vcpu->vm->vm_id;
	if ((vm_id >= CONFIG_MAX_VM_NUM) || (vcpu->vcpu_id >= MAX_VCPUS_PER_VM)) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	if (!pcpu->vcpu[vm_id].seen) {
		pcpu->vcpu[vm_id].enabled_origin_ticks = pcpu->enabled_ticks;
	}
	pcpu->vcpu[vm_id].vcpu_id = vcpu->vcpu_id;
	pcpu->vcpu[vm_id].seen = true;
	pcpu->vcpu[vm_id].denied_accesses = arm64_core_pmu_saturating_add(
		pcpu->vcpu[vm_id].denied_accesses, 1UL);
}

bool arm64_core_pmu_guest_sysreg(uint64_t sysreg)
{
	uint64_t op0 = (sysreg >> 20U) & 0x3UL;
	uint64_t op2 = (sysreg >> 17U) & 0x7UL;
	uint64_t op1 = (sysreg >> 14U) & 0x7UL;
	uint64_t crn = (sysreg >> 10U) & 0xfUL;
	uint64_t crm = (sysreg >> 1U) & 0xfUL;
	bool pmu = false;

	if ((op0 == 3UL) && (crn == 9UL)) {
		if ((op1 == 3UL) && (((crm >= 12UL) && (crm <= 14UL)) ||
			(crm == 4UL) || (crm == 6UL))) {
			pmu = true;
		} else if ((op1 == 0UL) && (crm == 13UL) && (op2 == 3UL)) {
			pmu = true;
		} else if ((op1 == 0UL) && (crm == 14UL) &&
			((op2 == 1UL) || (op2 == 2UL) ||
			 (op2 >= 4UL))) {
			pmu = true;
		}
	} else if ((crn == 14UL) && (crm >= 8UL) && (crm <= 15UL) &&
		(((op0 == 3UL) && (op1 == 3UL)) ||
		 ((op0 == 2UL) && (op1 == 0UL)))) {
		pmu = true;
	}

	return pmu;
}

/* [20260717] Hot-path token validation
 *
 * begin: counter image + epoch + pCPU + VM/vCPU + owner generation
 *                               |
 * measured operation -----------+
 *                               v
 * end: validate same interval -> accumulate delta
 *                               +--> changed interval: drop sample
 *
 * Key rules:
 *   - begin/end must execute on the same pCPU for the same vCPU owner;
 *   - reset, suspend, or a scheduler transition invalidates the sample;
 *   - nested paths are inclusive measurements and may overlap by design.
 */
void arm64_core_pmu_path_begin(struct arm64_core_pmu_path_token *token)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;
	uint8_t instruction_counter;

	if (token == NULL) {
		return;
	}
	(void)memset(token, 0U, sizeof(*token));
	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	instruction_counter =
		pcpu->capability.event_counter[ARM64_CORE_PMU_INSTRUCTIONS];
	if (!pcpu->running || (pcpu->owner != ARM64_CORE_PMU_OWNER_VCPU) ||
		(pcpu->owner_vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}

	token->cycles = read_pmccntr_el0();
	if (instruction_counter != ARM64_CORE_PMU_INVALID_COUNTER) {
		token->instructions =
			arm64_core_pmu_read_event_counter(instruction_counter);
		token->instructions_valid = true;
	}
	token->epoch = pcpu->epoch;
	token->owner_generation = pcpu->owner_generation;
	token->pcpu_id = pcpu_id;
	token->vm_id = pcpu->owner_vm_id;
	token->vcpu_id = pcpu->owner_vcpu_id;
	token->valid = true;
}

void arm64_core_pmu_path_end(enum arm64_core_pmu_path path,
	struct arm64_core_pmu_path_token *token)
{
	struct arm64_core_pmu_pcpu *pcpu;
	struct arm64_core_pmu_path_values *values;
	uint8_t instruction_counter;
	uint64_t cycles;
	uint64_t instructions;

	if ((token == NULL) || !token->valid ||
		(path >= ARM64_CORE_PMU_PATH_NUM) ||
		(token->pcpu_id != get_pcpu_id()) ||
		(token->pcpu_id >= MAX_PCPU_NUM) ||
		(token->vm_id >= CONFIG_MAX_VM_NUM)) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[token->pcpu_id];
	values = &pcpu->vcpu[token->vm_id].path[path];
	instruction_counter =
		pcpu->capability.event_counter[ARM64_CORE_PMU_INSTRUCTIONS];
	if (!pcpu->running || (pcpu->epoch != token->epoch) ||
		(pcpu->owner_generation != token->owner_generation) ||
		(pcpu->owner != ARM64_CORE_PMU_OWNER_VCPU) ||
		(pcpu->owner_vm_id != token->vm_id) ||
		(pcpu->owner_vcpu_id != token->vcpu_id)) {
		if (pcpu->epoch == token->epoch) {
			values->dropped = arm64_core_pmu_saturating_add(values->dropped, 1UL);
		}
		token->valid = false;
		return;
	}

	cycles = arm64_core_pmu_counter_delta(read_pmccntr_el0(), token->cycles,
		pcpu->capability.cycle_width);
	values->cycles = arm64_core_pmu_saturating_add(values->cycles, cycles);
	if (token->instructions_valid &&
		(instruction_counter != ARM64_CORE_PMU_INVALID_COUNTER)) {
		instructions = arm64_core_pmu_counter_delta(
			arm64_core_pmu_read_event_counter(instruction_counter),
			token->instructions, pcpu->capability.event_width);
		values->instructions = arm64_core_pmu_saturating_add(
			values->instructions, instructions);
		values->instruction_calls = arm64_core_pmu_saturating_add(
			values->instruction_calls, 1UL);
	}
	values->calls = arm64_core_pmu_saturating_add(values->calls, 1UL);
	token->valid = false;
}

/* [20260717] Core PMU STR epochs
 *
 * running counters -> final local delta -> hardware disabled
 *       -> platform retention -> probe/program -> fresh baseline
 *
 * Key rules:
 *   - the retained object owns software totals, never physical counter images;
 *   - suspended time is excluded from enabled/running time;
 *   - resume failure disables only this pCPU PMU and never fails host STR.
 */
void arm64_core_pmu_suspend_cpu(uint64_t epoch)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;

	if ((epoch == 0UL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	if (!pcpu->initialized || pcpu->suspended) {
		return;
	}
	pcpu->resume_running = pcpu->running;
	if (pcpu->running) {
		arm64_core_pmu_account_now(pcpu);
		pcpu->running = false;
		arm64_core_pmu_hw_disable(pcpu);
	}
	arm64_core_pmu_delete_poll_timer(pcpu);
	pcpu->suspend_epoch = epoch;
	pcpu->suspended = true;
	pcpu->owner_generation++;
}

void arm64_core_pmu_resume_cpu(uint64_t epoch)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;
	bool resume_running;

	if ((epoch == 0UL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	if (!pcpu->suspended || (pcpu->suspend_epoch != epoch)) {
		return;
	}
	resume_running = pcpu->resume_running && arm64_core_pmu_requested_running;
	arm64_core_pmu_probe(pcpu);
	arm64_core_pmu_configure_mdcr(pcpu);
	if (pcpu->capability.available) {
		arm64_core_pmu_hw_program(pcpu);
	} else if (arm64_core_pmu_present(pcpu)) {
		arm64_core_pmu_hw_disable(pcpu);
	}
	if (resume_running && pcpu->capability.available) {
		if (arm64_core_pmu_add_poll_timer(pcpu) == 0) {
			arm64_core_pmu_hw_enable(pcpu);
			arm64_core_pmu_set_baseline(pcpu);
			pcpu->running = true;
		} else {
			LOG_ERR("PMU:    CPU%hu disabled after STR poll timer failure", pcpu_id);
		}
	}
	if (resume_running && !pcpu->capability.available) {
		LOG_WRN("PMU:    CPU%hu unavailable after STR, monitoring disabled", pcpu_id);
	}
	pcpu->resume_running = false;
	pcpu->suspend_epoch = 0UL;
	pcpu->suspended = false;
	pcpu->owner_generation++;
}

void arm64_core_pmu_init_pcpu(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_core_pmu_pcpu *pcpu;

	if (pcpu_id >= MAX_PCPU_NUM) {
		return;
	}
	pcpu = &arm64_core_pmu_pcpus[pcpu_id];
	(void)memset(pcpu, 0U, sizeof(*pcpu));
	pcpu->pcpu_id = pcpu_id;
	pcpu->epoch = arm64_core_pmu_epoch;
	pcpu->owner = ARM64_CORE_PMU_OWNER_HOST;
	pcpu->owner_vm_id = CONFIG_MAX_VM_NUM;
	pcpu->owner_vcpu_id = MAX_VCPUS_PER_VM;
	initialize_timer(&pcpu->poll_timer, arm64_core_pmu_poll, pcpu, 0UL, 0UL);
	arm64_core_pmu_probe(pcpu);
	arm64_core_pmu_configure_mdcr(pcpu);
	if (pcpu->capability.available) {
		arm64_core_pmu_hw_program(pcpu);
		LOG_INF("PMU:    CPU%hu PMUVer:%u counters:%u width:%u/%u events:0x%x stopped",
			pcpu_id, pcpu->capability.pmuver,
			pcpu->capability.counter_num, pcpu->capability.cycle_width,
			pcpu->capability.event_width, pcpu->capability.event_mask);
#if defined(CONFIG_PLATFORM_QEMU)
		if ((pcpu->capability.event_mask &
			(1U << ARM64_CORE_PMU_INSTRUCTIONS)) == 0U) {
			LOG_WRN("PMU:    CPU%hu QEMU cycles-only; use --pmu-icount only for INST_RETIRED validation",
				pcpu_id);
		}
#endif
	} else {
		if (arm64_core_pmu_present(pcpu)) {
			arm64_core_pmu_hw_disable(pcpu);
		}
		LOG_WRN("PMU:    CPU%hu disabled PMUVer:%u counters:%u inst-supported:%s",
			pcpu_id, pcpu->capability.pmuver,
			pcpu->capability.counter_num,
			arm64_core_pmu_event_supported(&pcpu->capability,
				ARM64_PMU_EVENT_INST_RETIRED) ? "yes" : "no");
	}
	pcpu->initialized = true;
}

void arm64_core_pmu_init_workers(void)
{
	struct sched_params params = { 0U };
	uint16_t pcpu_id;

	if (arm64_core_pmu_workers_initialized) {
		return;
	}
	params.prio = PRIO_HIGH;
	params.bvt_weight = ARM64_CORE_PMU_WORKER_BVT_WEIGHT;
	params.bvt_warp_value = ARM64_CORE_PMU_WORKER_BVT_WARP_VALUE;
	params.bvt_warp_limit = ARM64_CORE_PMU_WORKER_BVT_WARP_LIMIT;
	params.bvt_unwarp_period = ARM64_CORE_PMU_WORKER_BVT_UNWARP_PERIOD;
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		struct arm64_core_pmu_pcpu *pcpu = &arm64_core_pmu_pcpus[pcpu_id];

		(void)snprintf(pcpu->worker.name, sizeof(pcpu->worker.name),
			"pmu-ctl-%hu", pcpu_id);
		pcpu->worker.pcpu_id = pcpu_id;
		pcpu->worker.sched_ctl = &per_cpu(sched_ctl, pcpu_id);
		pcpu->worker.thread_entry = arm64_core_pmu_worker;
		pcpu->worker.switch_out = NULL;
		pcpu->worker.switch_in = NULL;
		pcpu->worker.host_sp = arch_setup_thread_stack(&pcpu->worker,
			pcpu->worker_stack, CONFIG_STACK_SIZE);
		init_thread_data(&pcpu->worker, &params);
	}
	arm64_core_pmu_workers_initialized = true;
}

const char *arm64_core_pmu_event_name(enum arm64_core_pmu_event event)
{
	static const char *const names[ARM64_CORE_PMU_EVENT_NUM] = {
		"cycles", "instructions", "stall-frontend", "stall-backend",
		"l1d-refill", "dtlb-walk", "branch-mispred",
	};

	return (event < ARM64_CORE_PMU_EVENT_NUM) ? names[event] : "unknown";
}

const char *arm64_core_pmu_path_name(enum arm64_core_pmu_path path)
{
	static const char *const names[ARM64_CORE_PMU_PATH_NUM] = {
		"mmio", "vgic", "virtio",
	};

	return (path < ARM64_CORE_PMU_PATH_NUM) ? names[path] : "unknown";
}
