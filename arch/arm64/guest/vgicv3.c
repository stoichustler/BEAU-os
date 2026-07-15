/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (C) 2018 Alexandru Elisei <alexandru.elisei@gmail.com>
 * Copyright (C) 2020-2022 Andrew Turner
 * Copyright (C) 2023 Arm Ltd
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
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bits.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <bsp/io_req.h>
#include <irq.h>
#include <logmsg.h>
#include <rtl.h>
#include <guest_memory.h>
#include <ticks.h>
#include <asm/platform.h>
#include <asm/sysreg.h>
#include <asm/trap.h>
#include <asm/guest/vgicv3.h>

#include "vgicv3_its.h"

/* [20260630] interrupt virtualization coverage:
 *
 * vGICv3 virtualization principle:
 *
 * The guest sees a GIC Distributor, Redistributors, CPU interface, and optional
 * ITS. EL2 backs that view with software descriptors and uses hardware list
 * registers only as a short-lived delivery cache for the currently loaded vCPU.
 * Arm names the CPU-interface register groups by owner:
 *
 *   ICC_* : physical CPU interface used by EL2 for host IRQs
 *   ICH_* : EL2 virtualization controls, LR state, VMCR/AP state
 *   ICV_* : EL1 virtual CPU interface selected by HCR_EL2 routing
 *
 * Guest code normally uses the ICC_* instruction encodings. With HCR_EL2.IMO
 * and HCR_EL2.FMO set, eligible EL1 accesses reach the ICV view instead, while
 * BEAU traps only the SGI/control paths that need software policy.
 *
 *   authoritative software model              hardware delivery cache
 *   +-----------------------------+   flush   +-----------------------------+
 *   | irq descriptor              | --------> | ICH_LR<n>                   |
 *   | - enabled / pending         |           | - virq / priority           |
 *   | - active / level            |           | - pending / active bits     |
 *   | - priority / target vCPU    |           +-----------------------------+
 *   +--------------+--------------+                         |
 *                  ^                                        |
 *                  | sync / maintenance / guest exit        |
 *                  +----------------------------------------+
 *
 * Pending bitmaps are indexes, not authority:
 *
 *   event source / guest GIC write
 *              |
 *              v
 *   +-------------------+     set/clear      +------------------+
 *   | irq descriptor    | -----------------> | pending bitmap   |
 *   +-------------------+                    +------------------+
 *              |
 *              v
 *   flush scans bitmap, checks descriptor rules, and fills free LRs
 *
 * Normal interrupt lifecycle:
 *
 *   device/MSI/SGI/timer event
 *        -> descriptor.pending = true
 *        -> optional wake/reschedule of target vCPU
 *        -> vgicv3_flush_vcpu() publishes deliverable IRQs into LRs
 *        -> EL1 reads IAR and later EOIs/deactivates
 *        -> maintenance IRQ or vgicv3_sync_vcpu() copies LR evidence back
 *        -> descriptor pending/active state becomes authoritative again
 *
 * This split keeps scheduling and interrupt delivery independent. A vCPU can
 * be switched out with in-flight virtual interrupts because sync saves LR
 * evidence into software, and a later flush reconstructs the guest-visible CPU
 * interface when that vCPU runs again.
 *
 * Guest MMIO/system-register accesses mutate only the software model first:
 *
 *   GICD/GICR/GITS MMIO or ICC_* trap
 *        -> sync current LRs
 *        -> emulate register access against descriptors/ITS tables
 *        -> flush if the guest-visible delivery state changed
 *
 * vtimer virtualization flow:
 *
 *   guest CNTV sysreg write or live CNTV PPI
 *        -> CNTV shadow + live CNTV sample
 *        -> PPI27 descriptor and pending bitmap
 *        -> LR delivery to EL1
 *        -> guest reprogram/EOI lowers or completes the level source
 *
 * Guest CNTP is software-emulated in vtimer.c and injects PPI30 as an ordinary
 * level IRQ.
 *
 * Linux SMP boot depends on a precise edge-SGI lifecycle. A representative
 * failure was VM2 reaching the PL011 console and then stalling with CPU0 in
 * smp_call_function_many_cond(), waiting for CPU2 to unlock a synchronous CSD
 * queued by kick_all_cpus_sync(). dumpstat showed that vCPU requests had been
 * consumed, SGI1 had been targeted and flushed into a list register, and the
 * SGI was no longer pending/active in the saved vGIC state; however the Linux
 * CSD flags stayed locked. Symbolication of the CSD function identified the
 * work item as do_nothing(), so the missing event was not a timer callback but
 * the normal IPI_CALL_FUNC handler running on the target CPU.
 *
 * The target CPUs were stopped around cpu_do_idle(): WFI had returned, but the
 * saved guest PSTATE still had IRQs masked because Linux enters WFI with local
 * interrupts disabled and later calls local_irq_enable() in the idle loop. The
 * virtual SGI may therefore wake the CPU before EL1 is ready to acknowledge and
 * dispatch a normal IRQ. If EL2 observes a pending-only edge LR disappear and
 * clears the software pending bit without EOI evidence, Linux loses the SGI
 * before local_irq_enable() can take it; the CSD remains locked and the boot CPU
 * spins forever. For this reason software-backed edge IRQs, especially SGIs,
 * request EOI maintenance even from pending-only state and are kept pending
 * until an explicit LR EOI/deactivation path proves that the guest completed
 * the interrupt.
 *
 * The Linux virtual timer has a related post-boot diagnostic signature: EL2 can
 * show an expired, host-masked CNTV source and a pending-only timer LR while
 * Linux reports that RCU timer wakeups did not happen. The CNTV physical PPI is
 * therefore treated only as an EL2 event source: EL2 samples and masks it, then
 * vGIC delivers PPI27 as a normal software level IRQ. The live CNTV mask follows
 * the sampled source level rather than a hardware LR lifecycle.
 */
#define BIT32(n)			(1U << (n))

#define VGICD_CTLR			0x0000U
#define VGICD_TYPER			0x0004U
#define VGICD_IIDR			0x0008U
#define VGICD_IGROUPR			0x0080U
#define VGICD_ISENABLER		0x0100U
#define VGICD_ICENABLER		0x0180U
#define VGICD_ISPENDR			0x0200U
#define VGICD_ICPENDR			0x0280U
#define VGICD_ISACTIVER		0x0300U
#define VGICD_ICACTIVER		0x0380U
#define VGICD_IPRIORITYR		0x0400U
#define VGICD_ITARGETSR		0x0800U
#define VGICD_ICFGR			0x0C00U
#define VGICD_IGRPMODR		0x0D00U
#define VGICD_IROUTER			0x6000U
#define VGICD_PIDR2			0xFFE8U

#define VGICR_CTLR			0x0000U
#define VGICR_IIDR			0x0004U
#define VGICR_TYPER			0x0008U
#define VGICR_WAKER			0x0014U
#define VGICR_PROPBASER		0x0070U
#define VGICR_PENDBASER		0x0078U
#define VGICR_INVLPIR			0x00A0U
#define VGICR_INVALLR			0x00B0U
#define VGICR_SYNCR			0x00C0U
#define VGICR_PIDR2			0xFFE8U
#define VGICR_SGI_BASE			0x10000U
#define VGICR_SGI_END			(VGICR_SGI_BASE + 0x10000U)

#define VGICR_CTLR_ENABLE_LPIS	BIT32(0U)
#define VGICR_CTLR_RWP		BIT32(3U)
#define VGIC_CTLR_ENABLE_G1		BIT32(1U)
#define VGIC_CTLR_ARE_NS		BIT32(5U)
#define VGICD_CTLR_RWP		BIT32(31U)
#define VGICD_TYPER_LPIS		BIT32(17U)
#define VGICD_TYPER_NUM_LPIS_SHIFT	11U
#define VGICD_TYPER_IDBITS_SHIFT	19U
#define VGICR_TYPER_LAST		(1UL << 4U)
#define VGICR_TYPER_PLPIS		(1UL << 0U)
#define VGICR_TYPER_PROCNUM_SHIFT	8U
#define VGICR_TYPER_AFF_SHIFT		32U
#define VGICR_WAKER_PROCESSOR_SLEEP	BIT32(1U)
#define VGICR_WAKER_CHILDREN_ASLEEP	BIT32(2U)
#define VGIC_VMCR_GROUP_ENABLES		ICH_VMCR_VENG1
#define VGICR_WAKER_READ_MASK		(VGICR_WAKER_PROCESSOR_SLEEP | VGICR_WAKER_CHILDREN_ASLEEP)

#define ICC_SGI1R_TARGET_LIST_MASK	0xffffUL
#define ICC_SGI1R_AFF1_SHIFT		16U
#define ICC_SGI1R_INTID_SHIFT		24U
#define ICC_SGI1R_INTID_MASK		0xfUL
#define ICC_SGI1R_IRM			(1UL << 40U)
#define ICC_SGI1R_AFF2_SHIFT		32U
#define ICC_SGI1R_AFF3_SHIFT		48U
#define ICC_SGI1R_AFF_MASK		0xffUL

#define GIC_IROUTER_IRM		(1UL << 31U)
#define GIC_IROUTER_AFF0_MASK		0xffUL
#define GIC_IROUTER_AFF1_SHIFT		8U
#define GIC_IROUTER_AFF2_SHIFT		16U
#define GIC_IROUTER_AFF3_SHIFT		32U
#define GIC_IROUTER_AFF_MASK		0xffUL
#define GIC_IROUTER_LOW_MASK		0xffffffffUL
#define GIC_PRIORITY_LOCAL		0x00U
#define GIC_PRIORITY_DEFAULT		0x80U
#define GIC_PRIORITY_LOWEST		0xffU
#define GIC_PIDR2_ARCH_GICV3		(3U << 4U)
#define VGIC_ICFGR_IRQS_PER_REG	16U
#define VGIC_INVALID_VCPU_ID		0xffffU
#define VGIC_INVALID_AFFINITY		0xffUL

static uint32_t vgic_lr_count;
static bool vgic_global_initialized;
static spinlock_t vgic_irqstat_lock = { .head = 0U, .tail = 0U };

/* [20260715] vGIC IRQ lifecycle statistics
 *
 * Framework:
 *
 *   physical IRQ / timer / SGI / emulated device
 *                 |
 *                 v
 *   vgic_irqstat_record_assert()
 *       last_assert_tick = now
 *                 |
 *                 v
 *   vgicv3_flush_vcpu()
 *       chooses a pending descriptor and writes an ICH_LR<n> slot
 *                 |
 *                 v
 *   vgic_irqstat_record_lr(new_lr = true)
 *       raise_to_lr += now - last_assert_tick
 *       last_lr_tick = now
 *                 |
 *                 v
 *   guest handles interrupt and writes EOIR/DIR, or EL2 sync observes EOI
 *                 |
 *                 v
 *   vgic_irqstat_record_eoi()
 *       lr_to_eoi += now - last_lr_tick
 *
 * Flow interpretation:
 *
 *   raise-to-LR:
 *       Source became runnable/pending, but EL2 had not yet exposed it through
 *       a hardware list register. High values point at scheduler latency,
 *       target-vCPU not running, no free LR, or vGIC flush path contention.
 *
 *   LR-to-EOI:
 *       The vIRQ was visible to the guest CPU interface, but completion had
 *       not been observed. High values point at guest interrupt masking,
 *       ISR runtime, guest scheduling, or lost/late EOI synchronization.
 *
 * Principle:
 *   Keep the hot path accounting small: one spinlock-protected sparse table,
 *   raw tick accumulation, and conversion to microseconds only when irqstat
 *   snapshots the data. The counters are diagnostic and must not decide vGIC
 *   delivery semantics.
 */
#if CONFIG_IRQSTAT_LATENCY
struct vgic_irqstat_latency_accum {
	uint64_t count;
	uint64_t min_ticks;
	uint64_t max_ticks;
	uint64_t sum_ticks;
};
#endif

struct vgic_irqstat_entry {
	bool valid;
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint32_t virq;
	bool level;
	bool in_flight;
	uint64_t assert_count;
	uint64_t deassert_count;
	uint64_t lr_count;
	uint64_t eoi_count;
#if CONFIG_IRQSTAT_LATENCY
	uint64_t last_assert_tick;
	uint64_t last_lr_tick;
	struct vgic_irqstat_latency_accum raise_to_lr;
	struct vgic_irqstat_latency_accum lr_to_eoi;
#endif
};

static struct vgic_irqstat_entry vgic_irqstats[ARM64_VGIC_IRQSTAT_MAX];

static void vgicv3_sync_vcpu(struct acrn_vcpu *vcpu, bool is_current);
static void vgicv3_flush_vcpu(struct acrn_vcpu *vcpu, bool is_current);
static int32_t vgic_inject_locked(struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level);
static uint32_t vgic_lpi_index(uint32_t lpi);
static bool vgic_irq_is_lpi(uint32_t virq);

#if CONFIG_IRQSTAT_LATENCY
static void vgic_irqstat_accum(struct vgic_irqstat_latency_accum *accum,
	uint64_t delta_ticks)
{
	if (accum->count == 0UL) {
		accum->min_ticks = delta_ticks;
		accum->max_ticks = delta_ticks;
	} else {
		if (delta_ticks < accum->min_ticks) {
			accum->min_ticks = delta_ticks;
		}
		if (delta_ticks > accum->max_ticks) {
			accum->max_ticks = delta_ticks;
		}
	}
	if (accum->sum_ticks <= (UINT64_MAX - delta_ticks)) {
		accum->sum_ticks += delta_ticks;
	} else {
		accum->sum_ticks = UINT64_MAX;
	}
	if (accum->count != UINT64_MAX) {
		accum->count++;
	}
}

static void vgic_irqstat_export_latency(
	const struct vgic_irqstat_latency_accum *src,
	struct arm64_vgic_irq_latency_stats *dst)
{
	dst->count = src->count;
	if (src->count == 0UL) {
		dst->min_us = 0UL;
		dst->avg_us = 0UL;
		dst->max_us = 0UL;
	} else {
		dst->min_us = ticks_to_us(src->min_ticks);
		dst->avg_us = ticks_to_us(src->sum_ticks / src->count);
		dst->max_us = ticks_to_us(src->max_ticks);
	}
}
#endif

static bool vgic_irqstat_enabled(const struct vgic_irqstat_entry *entry)
{
	const struct acrn_vm *vm;
	const struct arm64_vgicv3 *vgic;
	bool enabled = false;

	if ((entry->vm_id >= CONFIG_MAX_VM_NUM) ||
		(entry->vcpu_id >= ARM64_VGIC_MAX_VCPUS)) {
		return false;
	}

	vm = get_vm_from_vmid(entry->vm_id);
	vgic = &vm->arch_vm.vgic;
	if (is_poweroff_vm(vm) || !vgic->initialized ||
		(entry->vcpu_id >= vgic->vcpu_count)) {
		return false;
	}

	if (entry->virq < ARM64_VGIC_IRQ_NUM) {
		enabled = vgic->irq[entry->vcpu_id][entry->virq].enabled;
	} else if (vgic->its_enabled && vgic_irq_is_lpi(entry->virq)) {
		enabled = vgic->lpi[entry->vcpu_id][vgic_lpi_index(entry->virq)].enabled;
	}

	return enabled;
}

static struct vgic_irqstat_entry *vgic_irqstat_find_locked(uint16_t vm_id,
	uint16_t vcpu_id, uint32_t virq, bool create)
{
	struct vgic_irqstat_entry *free_entry = NULL;
	uint16_t idx;

	for (idx = 0U; idx < ARM64_VGIC_IRQSTAT_MAX; idx++) {
		struct vgic_irqstat_entry *entry = &vgic_irqstats[idx];

		if (entry->valid) {
			if ((entry->vm_id == vm_id) && (entry->vcpu_id == vcpu_id) &&
				(entry->virq == virq)) {
				return entry;
			}
		} else if (free_entry == NULL) {
			free_entry = entry;
		}
	}

	if (create && (free_entry != NULL)) {
		(void)memset(free_entry, 0U, sizeof(*free_entry));
		free_entry->valid = true;
		free_entry->vm_id = vm_id;
		free_entry->vcpu_id = vcpu_id;
		free_entry->virq = virq;
		return free_entry;
	}

	return NULL;
}

static void vgic_irqstat_record_assert(const struct acrn_vcpu *vcpu,
	uint32_t virq, bool level)
{
	struct vgic_irqstat_entry *entry;
	uint64_t flags;

	if ((vcpu != NULL) && (vcpu->vm != NULL)) {
		spinlock_irqsave_obtain(&vgic_irqstat_lock, &flags);
		entry = vgic_irqstat_find_locked(vcpu->vm->vm_id, vcpu->vcpu_id,
			virq, true);

		if (entry != NULL) {
			entry->level = level;
			entry->assert_count++;
#if CONFIG_IRQSTAT_LATENCY
			if (!entry->in_flight) {
				/*
				 * Start a new lifecycle sample only for the first raise while
				 * no previous delivery is in flight. Repeated level assertions
				 * update counters but do not restart the clock, otherwise a
				 * noisy device would hide the original delivery delay.
				 */
				entry->last_assert_tick = cpu_ticks();
				entry->last_lr_tick = 0UL;
			}
#endif
			entry->in_flight = true;
		}
		spinlock_irqrestore_release(&vgic_irqstat_lock, flags);
	}
}

static void vgic_irqstat_record_deassert(const struct acrn_vcpu *vcpu,
	uint32_t virq)
{
	struct vgic_irqstat_entry *entry;
	uint64_t flags;

	if ((vcpu != NULL) && (vcpu->vm != NULL)) {
		spinlock_irqsave_obtain(&vgic_irqstat_lock, &flags);
		entry = vgic_irqstat_find_locked(vcpu->vm->vm_id, vcpu->vcpu_id,
			virq, true);

		if (entry != NULL) {
			entry->deassert_count++;
		}
		spinlock_irqrestore_release(&vgic_irqstat_lock, flags);
	}
}

static void vgic_irqstat_record_lr(const struct acrn_vcpu *vcpu,
	const struct arm64_vgic_irq *desc, bool new_lr)
{
	struct vgic_irqstat_entry *entry;
	uint64_t flags;

#if !CONFIG_IRQSTAT_LATENCY
	(void)new_lr;
#endif
	if ((vcpu != NULL) && (vcpu->vm != NULL) && (desc != NULL)) {
#if CONFIG_IRQSTAT_LATENCY
		uint64_t now = cpu_ticks();
#endif

		spinlock_irqsave_obtain(&vgic_irqstat_lock, &flags);
		entry = vgic_irqstat_find_locked(vcpu->vm->vm_id, vcpu->vcpu_id,
			desc->virq, true);

		if (entry != NULL) {
			entry->level = desc->level;
			entry->lr_count++;
#if CONFIG_IRQSTAT_LATENCY
			if (new_lr && entry->in_flight && (entry->last_assert_tick != 0UL) &&
				(now >= entry->last_assert_tick)) {
				/*
				 * new_lr is true only when a pending vIRQ was actually
				 * published into an LR. Sync/readback paths may account an
				 * already-existing LR, but those must not double-count
				 * raise-to-LR latency.
				 */
				vgic_irqstat_accum(&entry->raise_to_lr,
					now - entry->last_assert_tick);
				entry->last_lr_tick = now;
			}
#endif
		}
		spinlock_irqrestore_release(&vgic_irqstat_lock, flags);
	}
}

static void vgic_irqstat_record_eoi(const struct acrn_vcpu *vcpu,
	uint32_t virq)
{
	struct vgic_irqstat_entry *entry;
	uint64_t flags;

	if ((vcpu != NULL) && (vcpu->vm != NULL)) {
#if CONFIG_IRQSTAT_LATENCY
		uint64_t now = cpu_ticks();
#endif

		spinlock_irqsave_obtain(&vgic_irqstat_lock, &flags);
		entry = vgic_irqstat_find_locked(vcpu->vm->vm_id, vcpu->vcpu_id,
			virq, true);

		if (entry != NULL) {
			entry->eoi_count++;
#if CONFIG_IRQSTAT_LATENCY
			if (entry->in_flight && (entry->last_lr_tick != 0UL) &&
				(now >= entry->last_lr_tick)) {
				/*
				 * LR-to-EOI measures only guest-visible completion. If no LR
				 * timestamp exists, the EOI came from a partially synchronized
				 * or reset lifecycle and is counted as an EOI event without a
				 * latency sample.
				 */
				vgic_irqstat_accum(&entry->lr_to_eoi,
					now - entry->last_lr_tick);
			}
			entry->last_assert_tick = 0UL;
			entry->last_lr_tick = 0UL;
#endif
			entry->in_flight = false;
		}
		spinlock_irqrestore_release(&vgic_irqstat_lock, flags);
	}
}

uint16_t arm64_vgicv3_get_irq_stats(struct arm64_vgic_irq_stats *stats, uint16_t max)
{
	uint64_t flags;
	uint16_t copied = 0U;
	uint16_t idx;

	if ((stats == NULL) || (max == 0U)) {
		return 0U;
	}

	spinlock_irqsave_obtain(&vgic_irqstat_lock, &flags);
	for (idx = 0U; (idx < ARM64_VGIC_IRQSTAT_MAX) && (copied < max); idx++) {
		const struct vgic_irqstat_entry *src = &vgic_irqstats[idx];
		struct arm64_vgic_irq_stats *dst = &stats[copied];

		if (!src->valid) {
			continue;
		}
		if (!vgic_irqstat_enabled(src)) {
			continue;
		}

		(void)memset(dst, 0U, sizeof(*dst));
		dst->vm_id = src->vm_id;
		dst->vcpu_id = src->vcpu_id;
		dst->virq = src->virq;
		dst->level = src->level;
		dst->in_flight = src->in_flight;
		dst->assert_count = src->assert_count;
		dst->deassert_count = src->deassert_count;
		dst->lr_count = src->lr_count;
		dst->eoi_count = src->eoi_count;
#if CONFIG_IRQSTAT_LATENCY
		vgic_irqstat_export_latency(&src->raise_to_lr, &dst->raise_to_lr);
		vgic_irqstat_export_latency(&src->lr_to_eoi, &dst->lr_to_eoi);
#endif
		copied++;
	}
	spinlock_irqrestore_release(&vgic_irqstat_lock, flags);

	return copied;
}

static void vgic_irqstat_reset_vm(uint16_t vm_id)
{
	uint64_t flags;
	uint16_t idx;

	spinlock_irqsave_obtain(&vgic_irqstat_lock, &flags);
	for (idx = 0U; idx < ARM64_VGIC_IRQSTAT_MAX; idx++) {
		struct vgic_irqstat_entry *entry = &vgic_irqstats[idx];

		if (entry->valid && (entry->vm_id == vm_id)) {
			entry->in_flight = false;
#if CONFIG_IRQSTAT_LATENCY
			entry->last_assert_tick = 0UL;
			entry->last_lr_tick = 0UL;
#endif
		}
	}
	spinlock_irqrestore_release(&vgic_irqstat_lock, flags);
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static bool addr_in_range(uint64_t addr, uint64_t base, uint64_t size)
{
	return (size != 0UL) && (addr >= base) && ((addr - base) < size);
}

static uint16_t arm64_vcpu_count_from_affinity(uint64_t affinity)
{
	uint16_t count = 0U;

	while (affinity != 0UL) {
		affinity &= affinity - 1UL;
		count++;
	}

	return count;
}

static struct arm64_vgic_irq *vgic_irq_desc(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = NULL;
	struct arm64_vgicv3 *vgic;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
		vgic = &vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			if (virq < ARM64_VGIC_IRQ_NUM) {
				desc = &vgic->irq[vcpu->vcpu_id][virq];
			} else if (vgic->its_enabled && vgic_irq_is_lpi(virq)) {
				desc = &vgic->lpi[vcpu->vcpu_id][vgic_lpi_index(virq)];
			}
		}
	}

	return desc;
}

static void vgic_record_irq_injection(struct acrn_vcpu *source_vcpu,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level, int32_t status)
{
	struct arm64_vcpu_last_irq *last;

	if (target_vcpu != NULL) {
		last = &target_vcpu->arch.debug.last_irq;
		last->virq = virq;
		last->status = status;
		last->source_vcpu_id = (source_vcpu != NULL) ?
			source_vcpu->vcpu_id : ARM64_VCPU_DEBUG_INVALID_VCPU_ID;
		last->target_vcpu_id = target_vcpu->vcpu_id;
		last->level = level;
		last->tsc = cpu_ticks();
	}
}

static uint32_t vgic_irq_word(uint32_t virq)
{
	return virq / 32U;
}

static uint32_t vgic_irq_bit(uint32_t virq)
{
	return virq % 32U;
}

static uint32_t vgic_lpi_index(uint32_t lpi)
{
	return lpi - ARM64_VGIC_LPI_BASE;
}

static bool vgic_irq_is_spi(uint32_t virq)
{
	return (virq >= ARM64_VGIC_LOCAL_IRQ_NUM) && (virq < ARM64_VGIC_IRQ_NUM);
}

static bool vgic_irq_is_lpi(uint32_t virq)
{
	return (virq >= ARM64_VGIC_LPI_BASE) &&
		(virq < (ARM64_VGIC_LPI_BASE + ARM64_VGIC_LPI_NUM));
}

static void vgic_set_pending(struct arm64_vgicv3 *vgic, uint16_t vcpu_id,
	struct arm64_vgic_irq *desc, bool pending)
{
	uint32_t virq = desc->virq;
	uint32_t word;
	uint32_t mask;

	/*
	 * The descriptor is the authoritative software state. The bitmap is only
	 * the fast scan index used by flush and WFI wake checks.
	 */
	desc->pending = pending;
	if (vgic_irq_is_lpi(virq)) {
		virq -= ARM64_VGIC_LPI_BASE;
		word = vgic_irq_word(virq);
		mask = BIT32(vgic_irq_bit(virq));
		if (pending) {
			vgic->lpi_pending_bitmap[vcpu_id][word] |= mask;
		} else {
			vgic->lpi_pending_bitmap[vcpu_id][word] &= ~mask;
		}
		return;
	}

	word = vgic_irq_word(virq);
	mask = BIT32(vgic_irq_bit(virq));
	if (pending) {
		vgic->pending_bitmap[vcpu_id][word] |= mask;
	} else {
		vgic->pending_bitmap[vcpu_id][word] &= ~mask;
	}
}

void arm64_vgicv3_set_pending_locked(struct arm64_vgicv3 *vgic,
	uint16_t vcpu_id, struct arm64_vgic_irq *desc, bool pending)
{
	vgic_set_pending(vgic, vcpu_id, desc, pending);
}

static bool vgic_is_vtimer_irq(__unused const struct acrn_vcpu *vcpu, uint32_t virq)
{
	/*
	 * PPI27 is sourced from the live CNTV comparator. EL2 masks the physical
	 * source after sampling it, but the guest-visible timer interrupt is still
	 * modeled as a software level line in the vGIC.
	 */
	return virq == ARM64_GIC_PPI_VIRTUAL_TIMER;
}

static bool vgic_irq_configurable(uint32_t virq)
{
	/* [20260627] vGIC/vtimer invariant:
	 *
	 *   guest GICR_ICFGR write
	 *          |
	 *          +--> PPI27: keep CNTV line-level semantics
	 *          |
	 *          +--> other non-SGI IRQs: use guest configuration
	 *
	 * The virtual timer interrupt is not an ordinary guest-selected PPI. Its
	 * source is the CNTV comparator level sampled by EL2; allowing ICFGR to
	 * make PPI27 edge-triggered breaks requeue/EOI handling for Linux timers.
	 */
	return (virq >= ARM64_VGIC_SGI_NUM) && (virq != ARM64_GIC_PPI_VIRTUAL_TIMER);
}

static struct acrn_vcpu *vgic_irq_target_vcpu(struct acrn_vcpu *vcpu,
	const struct arm64_vgicv3 *vgic, uint32_t virq)
{
	struct acrn_vcpu *target_vcpu = vcpu;

	if (vgic_irq_is_spi(virq)) {
		uint16_t target_vcpu_id = vgic->irq[0U][virq].target_vcpu;

		if (target_vcpu_id >= vcpu->vm->hw.created_vcpus) {
			target_vcpu_id = 0U;
		}
		target_vcpu = vcpu_from_vid(vcpu->vm, target_vcpu_id);
	}

	return target_vcpu;
}

static uint64_t make_lr_state(const struct arm64_vgic_irq *desc, bool pending, bool active)
{
	uint64_t lr = desc->virq & ICH_LR_VINTID_MASK;
	uint64_t state;

	/*
	 * The LR state encodes the guest-visible lifecycle. Edge interrupts are
	 * pending-only until accepted; level interrupts become active after being
	 * presented so the maintenance interrupt can later reconcile completion.
	 */
	if (active) {
		state = pending ? ICH_LR_STATE_ACTIVE_PENDING : ICH_LR_STATE_ACTIVE;
	} else {
		state = pending ? ICH_LR_STATE_PENDING : ICH_LR_STATE_INVALID;
	}

	lr |= ((uint64_t)desc->priority << ICH_LR_PRIORITY_SHIFT);
	lr |= state << ICH_LR_STATE_SHIFT;
	lr |= ICH_LR_GROUP1;

	if (desc->hw) {
		lr |= ((uint64_t)desc->pirq << ICH_LR_PINTID_SHIFT);
		lr |= ICH_LR_HW;
	} else if (active || !desc->level || (desc->virq == ARM64_GIC_PPI_VIRTUAL_TIMER)) {
		/*
		 * Software edge IRQs and the virtual timer must report completion even
		 * when they start as pending-only LRs:
		 *
		 *   pending PPI27 LR -> guest EOI -> maintenance -> resample CNTV
		 *
		 * Without EOI maintenance, EL2 can keep host PPI27 masked until an
		 * unrelated exit, starving the next Linux clockevent tick.
		 */
		lr |= ICH_LR_EOI;
	}

	return lr;
}

static bool vgic_irq_deliverable(const struct arm64_vgicv3 *vgic,
	const struct acrn_vcpu *vcpu, const struct arm64_vgic_irq *desc)
{
	bool enabled = ((vgic->gicd_ctlr & VGIC_CTLR_ENABLE_G1) != 0U) &&
		((vcpu->arch.vgic.vmcr & ICH_VMCR_VENG1) != 0UL) && desc->enabled;

	if (enabled && vgic_irq_is_lpi(desc->virq)) {
		enabled = (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS) &&
			((vgic->gicr_ctlr[vcpu->vcpu_id] & VGICR_CTLR_ENABLE_LPIS) != 0U);
	}

	return enabled;
}

static uint64_t make_lr(const struct arm64_vgic_irq *desc)
{
	return make_lr_state(desc, desc->pending, desc->active);
}

static uint32_t lr_vintid(uint64_t lr)
{
	return (uint32_t)(lr & ICH_LR_VINTID_MASK);
}

static uint32_t lr_state(uint64_t lr)
{
	return (uint32_t)((lr >> ICH_LR_STATE_SHIFT) & 0x3UL);
}

static int32_t find_lr_for_virq(const struct acrn_vcpu *vcpu, uint32_t virq)
{
	uint32_t idx;

	for (idx = 0U; idx < vcpu->arch.vgic.used_lrs; idx++) {
		if ((lr_state(vcpu->arch.vgic.lr[idx]) != ICH_LR_STATE_INVALID) &&
			(lr_vintid(vcpu->arch.vgic.lr[idx]) == virq)) {
			return (int32_t)idx;
		}
	}

	return -1;
}

static void remove_lr(struct acrn_vcpu *vcpu, uint32_t idx)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t last;

	if (idx < ctx->used_lrs) {
		last = ctx->used_lrs - 1U;
		if (idx != last) {
			ctx->lr[idx] = ctx->lr[last];
		}
		ctx->lr[last] = 0UL;
		ctx->used_lrs--;
	}
}

static void vgic_update_irq_lr(struct acrn_vcpu *vcpu, const struct arm64_vgic_irq *desc)
{
	int32_t lr_idx = find_lr_for_virq(vcpu, desc->virq);

	if (lr_idx >= 0) {
		bool lr_pending = desc->enabled && desc->pending;
		bool lr_active = desc->active;

		/*
		 * Guest MMIO may change enable, pending, active, priority, or route
		 * while an LR is already loaded. Refresh or remove the existing LR so
		 * the CPU interface mirrors the descriptor before the next guest entry.
		 */
		if (lr_pending || lr_active) {
			vcpu->arch.vgic.lr[lr_idx] = make_lr_state(desc, lr_pending, lr_active);
			vgic_irqstat_record_lr(vcpu, desc, false);
		} else {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
	}
}

static void vgic_update_irq_row_lr(struct acrn_vm *vm, uint16_t vcpu_id,
	const struct arm64_vgic_irq *desc)
{
	struct acrn_vcpu *target_vcpu;

	if (vcpu_id < vm->hw.created_vcpus) {
		target_vcpu = vcpu_from_vid(vm, vcpu_id);
		if (target_vcpu != NULL) {
			vgic_update_irq_lr(target_vcpu, desc);
		}
	}
}

void arm64_vgicv3_update_irq_row_lr_locked(struct acrn_vm *vm,
	uint16_t vcpu_id, const struct arm64_vgic_irq *desc)
{
	vgic_update_irq_row_lr(vm, vcpu_id, desc);
}

static uint16_t vgic_valid_target_vcpu(const struct arm64_vgicv3 *vgic, uint16_t target_vcpu_id)
{
	return (target_vcpu_id < vgic->vcpu_count) ? target_vcpu_id : 0U;
}

static void vgic_set_spi_target(struct acrn_vm *vm, uint32_t virq, uint16_t target_vcpu_id)
{
	struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	uint16_t old_target;
	uint16_t new_target;
	uint16_t vcpu_id;

	if (!vgic_irq_is_spi(virq)) {
		return;
	}

	old_target = vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu);
	new_target = vgic_valid_target_vcpu(vgic, target_vcpu_id);
	if (old_target != new_target) {
		struct arm64_vgic_irq *old_desc = &vgic->irq[old_target][virq];
		struct arm64_vgic_irq *new_desc = &vgic->irq[new_target][virq];

		if (old_desc->pending) {
			vgic_set_pending(vgic, old_target, old_desc, false);
			vgic_set_pending(vgic, new_target, new_desc, true);
		}
		if (old_desc->active) {
			old_desc->active = false;
			new_desc->active = true;
		}
		vgic_update_irq_row_lr(vm, old_target, old_desc);
		vgic_update_irq_row_lr(vm, new_target, new_desc);
	}

	for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
		vgic->irq[vcpu_id][virq].target_vcpu = (uint8_t)new_target;
	}
}

static void vgicv3_write_lrs(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	uint32_t idx;
	uint32_t count = min_u32(vgic_lr_count, ARM64_VGIC_MAX_LRS);

	/*
	 * List registers do not name a target vCPU. Hardware treats every LR as
	 * belonging to the vPE currently scheduled on this physical PE, so LR
	 * load/save is part of the vCPU context switch rather than a global routing
	 * operation.
	 */
	for (idx = 0U; idx < count; idx++) {
		write_ich_lr_el2((uint8_t)idx, (idx < ctx->used_lrs) ? ctx->lr[idx] : 0UL);
	}
}

static uint64_t vgicv3_control_hcr(uint64_t hcr)
{
	/* [20260630] vGICv3 CPU-interface boundary:
	 *
	 *   EL1 IAR/EOIR/PMR/IGRPEN -> ICV view backed by ICH_VMCR/AP/LRs
	 *   EL1 SGI sysreg          -> sysreg trap -> software SGI inject
	 *   maintenance PPI25       -> ICH_HCR condition reported in ICH_MISR
	 *
	 * ICH_HCR_EL2 controls the virtual CPU interface for the current pPE.
	 * BEAU leaves TC clear so common guest ICC accesses stay on the hardware
	 * virtual interface, then syncs VMCR/AP/LR state at explicit boundaries.
	 * EOIcount is hardware-reported completion evidence, not persistent control
	 * state, so it must not be replayed.
	 */
	return (hcr & ~(ICH_HCR_EOICOUNT_MASK | ICH_HCR_TC)) | ICH_HCR_EN;
}

static void vgicv3_write_cpuif(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	write_icc_pmr_el1(ctx->pmr);
	write_ich_vmcr_el2(ctx->vmcr);
	write_ich_ap0r0_el2(ctx->ap0r0);
	write_ich_ap1r0_el2(ctx->ap1r0);
	vgicv3_write_lrs(ctx);
	write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
}

static void vgicv3_write_lrs_hcr(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	/*
	 * Runtime flushes only publish pending virtual interrupts and maintenance
	 * controls. VMCR/AP are guest CPU-interface state; replaying old snapshots
	 * while the vCPU is running can resurrect stale active-priority state.
	 */
	vgicv3_write_lrs(ctx);
	write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
}

static void vgicv3_requeue_lr(struct acrn_vcpu *vcpu, const struct arm64_vgic_irq *desc,
	bool loaded)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint8_t lr_idx;

	if (!desc->pending || !vgic_irq_deliverable(vgic, vcpu, desc) ||
		(find_lr_for_virq(vcpu, desc->virq) >= 0)) {
		return;
	}

	if (ctx->used_lrs >= vgic->lr_count) {
		ctx->hcr |= ICH_HCR_UIE;
		if (loaded) {
			write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
		}
		return;
	}

	/* [20260627] pending restore:
	 *
	 *   LR disappeared without EOI
	 *          |
	 *          v
	 *   descriptor.pending still high -> rebuild one LR now
	 *
	 * This is intentionally narrower than vgicv3_flush_vcpu(). During sync we
	 * are still reconciling old hardware LR evidence; scanning every pending
	 * IRQ here could consume freshly queued edge IRQs before EL1 sees them.
	 */
	lr_idx = ctx->used_lrs;
	ctx->lr[lr_idx] = make_lr(desc);
	ctx->used_lrs++;
	vgic_irqstat_record_lr(vcpu, desc, true);
	if (loaded) {
		write_ich_lr_el2(lr_idx, ctx->lr[lr_idx]);
		write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
	}
}

static void vgicv3_write_cpuif_control(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	/*
	 * Trapped ICC control writes update VMCR/SRE immediately for the running
	 * vCPU. This keeps guest priority/group enables live without replaying AP
	 * state, which is owned by the hardware interrupt acknowledge/EOI path.
	 */
	write_icc_sre_el1(ctx->sre | ICC_SRE_SRE);
	write_icc_pmr_el1(ctx->pmr);
	write_ich_vmcr_el2(ctx->vmcr);
}

static void vgicv3_read_lrs(struct arm64_vgicv3_vcpu_ctx *ctx)
{
	uint32_t idx;
	uint32_t count = min_u32(vgic_lr_count, ARM64_VGIC_MAX_LRS);

	/*
	 * Hardware may clear an LR after the guest acknowledges/deactivates an
	 * interrupt. Keep used_lrs as the highest non-empty slot so later code can
	 * compact the software copy without scanning unused hardware capacity.
	 */
	ctx->used_lrs = 0U;
	for (idx = 0U; idx < count; idx++) {
		ctx->lr[idx] = read_ich_lr_el2((uint8_t)idx);
		if (lr_state(ctx->lr[idx]) != ICH_LR_STATE_INVALID) {
			ctx->used_lrs = (uint8_t)(idx + 1U);
		}
	}
}

static void vgicv3_sync_live_cpuif(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint64_t vmcr;

	/*
	 * With ICH_HCR.TC clear, Linux can update ICC_PMR/IGRPEN/CTL without an
	 * EL2 sysreg trap. The live virtual CPU interface is authoritative until
	 * the vCPU is switched out, so sync the pieces BEAU uses for delivery
	 * decisions before evaluating pending IRQs.
	 *
	 *   guest ICC_* write -> live ICH_VMCR_EL2
	 *             -> sync point -> ctx shadow -> vGIC deliverability checks
	 */
	vmcr = read_ich_vmcr_el2();
	ctx->vmcr = vmcr;
	ctx->pmr = (vmcr & ICH_VMCR_PRIORITY_MASK) >> ICH_VMCR_PRIORITY_SHIFT;
	ctx->ctlr = ((vmcr & ICH_VMCR_EOIM) != 0UL) ? ICC_CTLR_EL1_EOIMODE : 0UL;
	ctx->sre = read_icc_sre_el1();
}

static bool vgicv3_vcpu_is_loaded(const struct acrn_vcpu *vcpu)
{
	return get_running_vcpu(get_pcpu_id()) == vcpu;
}

static bool vgicv3_vcpu_is_running_remote(const struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);

	return (pcpu_id != get_pcpu_id()) && (get_running_vcpu(pcpu_id) == vcpu);
}

static void vgicv3_record_cpuif_snapshot(struct acrn_vcpu *vcpu,
	struct arm64_vcpu_last_vgic *last, uint32_t source)
{
	uint32_t count = last->count + 1U;

	/*
	 * ICH_MISR/EISR/ELRSR are the hardware-visible maintenance contract.
	 * Recording them before software consumes LRs preserves the evidence that
	 * explains why EL2 was entered and whether EOI state was available.
	 */
	last->tsc = cpu_ticks();
	last->misr = read_ich_misr_el2();
	last->eisr = read_ich_eisr_el2();
	last->elrsr = read_ich_elrsr_el2();
	last->hcr = read_ich_hcr_el2();
	last->vmcr = read_ich_vmcr_el2();
	last->ap0r0 = read_ich_ap0r0_el2();
	last->ap1r0 = read_ich_ap1r0_el2();
	last->lr0 = (vgic_lr_count > 0U) ? read_ich_lr_el2(0U) : 0UL;
	last->lr1 = (vgic_lr_count > 1U) ? read_ich_lr_el2(1U) : 0UL;
	last->source = source;
	last->count = count;
	last->used_lrs = vcpu->arch.vgic.used_lrs;
}

static void vgicv3_read_loaded_lrs(struct acrn_vcpu *vcpu, bool is_current)
{
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_read_lrs(&vcpu->arch.vgic);
	}
}

static bool vgicv3_lrs_changed(const struct arm64_vgicv3_vcpu_ctx *ctx,
	const uint64_t old_lrs[ARM64_VGIC_MAX_LRS], uint8_t old_used_lrs, uint64_t old_hcr)
{
	bool changed = (ctx->used_lrs != old_used_lrs) || (ctx->hcr != old_hcr);
	uint32_t idx;

	for (idx = 0U; !changed && (idx < ARM64_VGIC_MAX_LRS); idx++) {
		changed = (ctx->lr[idx] != old_lrs[idx]);
	}

	return changed;
}

static void vgicv3_complete_eoi_lrs(struct acrn_vcpu *vcpu, uint64_t eoi_lrs,
	const uint64_t *old_lrs, uint8_t old_used_lrs)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t idx = min_u32(vgic_lr_count, ARM64_VGIC_MAX_LRS);

	while (idx > 0U) {
		uint32_t lr_idx = idx - 1U;
		uint64_t lr;
		uint32_t virq;
		uint32_t state;
		struct arm64_vgic_irq *desc;

		idx--;
		if ((eoi_lrs & (1UL << lr_idx)) == 0UL) {
			continue;
		}

		lr = (lr_idx < ctx->used_lrs) ? ctx->lr[lr_idx] : 0UL;
		state = lr_state(lr);
		if ((state == ICH_LR_STATE_INVALID) &&
			(old_lrs != NULL) && (lr_idx < old_used_lrs)) {
			/*
			 * EISR is an event bitmap, while LR readback is stateful. Some
			 * implementations can report the EOI after the LR slot has already
			 * become invalid, so use the pre-sync software LR as the identity
			 * source to clear the matching virtual IRQ active state.
			 */
			lr = old_lrs[lr_idx];
			state = lr_state(lr);
		}
		if (state == ICH_LR_STATE_INVALID) {
			continue;
		}

		virq = lr_vintid(lr);
		desc = vgic_irq_desc(vcpu, virq);
		if (desc != NULL) {
			bool lr_pending = ((state & ICH_LR_STATE_PENDING) != 0U);
			bool lr_active = ((state & ICH_LR_STATE_ACTIVE) != 0U);
			bool keep_pending = desc->level ?
				(lr_pending || desc->pending) : (lr_pending && lr_active);

			/*
			 * The virtual timer is level-triggered by CNTV, not by the
			 * cached pending bit alone. On EOI maintenance, the guest may
			 * already have moved CNTV_CVAL to a future deadline without an
			 * intervening EL2 sysreg trap, so resample CNTV before deciding
			 * whether to requeue the timer LR.
			 */
			if (vgic_is_vtimer_irq(vcpu, virq) && vgicv3_vcpu_is_loaded(vcpu)) {
				/*
				 * The timer source is the live CNTV line. Once the guest
				 * completes a timer LR, immediately resample the line. If the
				 * deadline is still due, keep the virtual line asserted instead
				 * of depending on another host PPI edge to recreate it.
				 */
				keep_pending = arm64_vtimer_sample_current(vcpu);
			}

			vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, keep_pending);
			desc->active = false;
			vgic_irqstat_record_eoi(vcpu, virq);
			if (vgic_is_vtimer_irq(vcpu, virq)) {
				arm64_vtimer_set_host_mask(vcpu, keep_pending);
			}
		}
		if (lr_idx < ctx->used_lrs) {
			remove_lr(vcpu, lr_idx);
		}
	}
}

static void vgicv3_deactivate_irq_locked(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc;
	int32_t lr_idx;
	bool keep_pending = false;

	if (virq >= ARM64_VGIC_IRQ_NUM) {
		return;
	}

	/*
	 * ICC_DIR_EL1 is the explicit deactivation path when a guest uses split
	 * priority-drop/deactivate semantics. Treating a DIR write as idempotent
	 * is safe for the current combined-EOI model and prevents a guest-visible
	 * active LR from sticking if Linux takes the split path anyway.
	 */
	vgicv3_sync_vcpu(vcpu, true);
	desc = vgic_irq_desc(vcpu, virq);
	if (desc == NULL) {
		return;
	}

	if (vgic_is_vtimer_irq(vcpu, virq) && vgicv3_vcpu_is_loaded(vcpu)) {
		keep_pending = arm64_vtimer_sample_current(vcpu);
	} else {
		keep_pending = desc->level && desc->pending;
	}

	lr_idx = find_lr_for_virq(vcpu, virq);
	if (lr_idx >= 0) {
		remove_lr(vcpu, (uint32_t)lr_idx);
	}
	desc->active = false;
	vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, keep_pending);
	vgic_irqstat_record_eoi(vcpu, virq);
	if (vgic_is_vtimer_irq(vcpu, virq)) {
		arm64_vtimer_set_host_mask(vcpu, keep_pending);
	}
}

static void vgicv3_write_loaded_lrs(const struct acrn_vcpu *vcpu, bool is_current)
{
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_write_lrs_hcr(&vcpu->arch.vgic);
	}
}

void arm64_vgicv3_global_init(void)
{
	if (!vgic_global_initialized) {
		/*
		 * ICH_VTR_EL2 reports the hardware LR count minus one. The value is a
		 * property of the GICv3 CPU interface implemented by the platform or CPU
		 * model, not a BEAU policy knob. QEMU virt currently reports ListRegs=3,
		 * so BEAU prints "4 list registers":
		 *
		 *     ICH_VTR_EL2.ListRegs = 3
		 *                |
		 *                v
		 *     usable LR slots = ListRegs + 1 = 4
		 *
		 * Four LRs only limit how many virtual interrupts can be resident in
		 * hardware at one time. Extra pending IRQs stay in the software
		 * descriptor/pending-bitmap model and are flushed into LRs after sync or
		 * maintenance frees a slot. Clamp to the static context array size so the
		 * save/restore path has a fixed bound even on larger GIC implementations.
		 */
		vgic_lr_count = (uint32_t)((read_ich_vtr_el2() & 0x1fUL) + 1UL);
		if (vgic_lr_count > ARM64_VGIC_MAX_LRS) {
			vgic_lr_count = ARM64_VGIC_MAX_LRS;
		}
		if (vgic_lr_count == 0U) {
			panic("gicv3 virtualization has no list registers");
		}
		vgic_global_initialized = true;
	}
}

void arm64_vgicv3_init_vm(struct acrn_vm *vm, uint64_t cpu_affinity)
{
	struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	uint32_t idx;
	uint32_t spi;
	uint16_t vcpu_count = arm64_vcpu_count_from_affinity(cpu_affinity);

	/*
	 * The virtual distributor advertises a compact GICv3 with affinity routing
	 * and Group-1 interrupts enabled. Guest redistributor frames are indexed by
	 * guest vCPU ID, so guest MPIDR affinity and GICR_TYPER affinity stay in
	 * the same compact namespace even when the VM runs on sparse pCPU IDs.
	 */
	arm64_vgicv3_global_init();
	vgic_irqstat_reset_vm(vm->vm_id);

	(void)memset(vgic, 0U, sizeof(*vgic));
	spinlock_init(&vgic->lock);
	vgic->vcpu_count = (uint16_t)min_u32((uint32_t)vcpu_count, ARM64_VGIC_MAX_VCPUS);
	if (vgic->vcpu_count == 0U) {
		vgic->vcpu_count = 1U;
	}
	vgic->rdist_count = vgic->vcpu_count;
	vgic->lr_count = vgic_lr_count;
	vgic->vmcr = (uint32_t)(VGIC_VMCR_GROUP_ENABLES | ICH_VMCR_DEFAULT_MASK);
	vgic->gicd_ctlr = VGIC_CTLR_ARE_NS | VGIC_CTLR_ENABLE_G1;
	vgic->its_enabled = (get_vm_config(vm->vm_id)->arch.guest_its_size != 0UL);
	vgic->gicd_typer = (((ARM64_VGIC_IRQ_NUM / 32U) - 1U) & 0x1fU) |
		(((uint32_t)(vgic->vcpu_count - 1U) & 0x7U) << 5U);
	if (vgic->its_enabled) {
		vgic->gicd_typer |= (12U << VGICD_TYPER_NUM_LPIS_SHIFT) |
			VGICD_TYPER_LPIS |
			((ARM64_VGIC_LPI_IDBITS - 1U) << VGICD_TYPER_IDBITS_SHIFT);
	}
	vgic->gicd_pidr2 = GIC_PIDR2_ARCH_GICV3;

	for (idx = 0U; idx < ARM64_VGIC_MAX_VCPUS; idx++) {
		uint32_t virq;

		vgic->rdist_vcpu[idx] = VGIC_INVALID_VCPU_ID;
		for (virq = 0U; virq < ARM64_VGIC_IRQ_NUM; virq++) {
			struct arm64_vgic_irq *desc = &vgic->irq[idx][virq];

			desc->virq = (uint16_t)virq;
			desc->pirq = 0xffffU;
			desc->priority = GIC_PRIORITY_DEFAULT;
			desc->target_vcpu = (virq < ARM64_VGIC_LOCAL_IRQ_NUM) ? (uint8_t)idx : 0U;
			desc->level = (virq >= ARM64_VGIC_SGI_NUM);
			desc->group1 = true;
			desc->groupmod = false;
		}
		for (virq = 0U; virq < ARM64_VGIC_LPI_NUM; virq++) {
			struct arm64_vgic_irq *desc = &vgic->lpi[idx][virq];

			desc->virq = (uint16_t)(ARM64_VGIC_LPI_BASE + virq);
			desc->pirq = 0xffffU;
			desc->priority = GIC_PRIORITY_DEFAULT;
			desc->target_vcpu = (uint8_t)idx;
			desc->enabled = false;
			desc->level = false;
			desc->group1 = true;
			desc->groupmod = false;
		}
	}

	for (spi = 0U; spi < ARM64_VGIC_SPI_NUM; spi++) {
		vgic->spi_router[spi] = 0UL;
	}

	if (vgic->its_enabled) {
		arm64_vgicv3_its_init(vgic);
	}
	vgic->initialized = true;
}

void arm64_vgicv3_init_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint16_t rdist_id = vcpu->vcpu_id;

	(void)memset(ctx, 0U, sizeof(*ctx));
	/*
	 * vGICv3 model: keep common ICC registers on the hardware virtual CPU
	 * interface and let EL2 sync VMCR/LRs at trap boundaries. SGI
	 * generation still traps through HCR_EL2.IMO/FMO and is handled in software.
	 */
	ctx->hcr = ICH_HCR_EN;
	ctx->vmcr = VGIC_VMCR_GROUP_ENABLES | ICH_VMCR_DEFAULT_MASK;
	ctx->sre = ICC_SRE_SRE;
	ctx->pmr = GIC_PRIORITY_LOWEST;
	ctx->ctlr = 0UL;
	arm64_vtimer_init_vcpu(vcpu);

	if ((rdist_id < ARM64_VGIC_MAX_VCPUS) && (vcpu->vcpu_id < vgic->vcpu_count)) {
		vgic->rdist_vcpu[rdist_id] = vcpu->vcpu_id;
	}
}

int32_t arm64_vgicv3_handle_cpuif_sysreg(struct acrn_vcpu *vcpu, uint32_t sysreg,
	bool read, uint64_t *reg)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	struct arm64_vcpu_last_cpuif *last;
	uint64_t flags;
	uint64_t value = 0UL;
	uint64_t access_value;
	bool control_changed = false;
	bool flush_needed = false;
	int32_t ret = 0;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (reg == NULL)) {
		return -EINVAL;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	last = &vcpu->arch.debug.last_cpuif;
	access_value = read ? 0UL : *reg;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	switch (sysreg) {
	case ARM64_VGIC_SYSREG_ICC_SRE_EL1:
		if (read) {
			value = ctx->sre | ICC_SRE_SRE;
		} else {
			ctx->sre = *reg | ICC_SRE_SRE;
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_PMR_EL1:
		if (read) {
			value = ctx->pmr;
		} else {
			ctx->pmr = *reg & 0xffUL;
			ctx->vmcr = (ctx->vmcr & ~(0xffUL << 24U)) | (ctx->pmr << 24U);
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_CTLR_EL1:
		if (read) {
			value = ctx->ctlr;
		} else {
			/*
			 * The current LR lifecycle completes software-backed virtual IRQs
			 * through EOIR maintenance. Keep EOImode as combined priority-drop
			 * plus deactivate until ICC_DIR_EL1 deactivation is fully modeled.
			 */
			ctx->ctlr = 0UL;
			ctx->vmcr &= ~ICH_VMCR_EOIM;
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_IGRPEN1_EL1:
		if (read) {
			value = ((ctx->vmcr & ICH_VMCR_VENG1) != 0UL) ? 1UL : 0UL;
		} else if ((*reg & 1UL) != 0UL) {
			ctx->vmcr |= ICH_VMCR_VENG1;
			control_changed = true;
		} else {
			ctx->vmcr &= ~ICH_VMCR_VENG1;
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_DIR_EL1:
		/*
		 * ICC_DIR_EL1 carries the INTID to deactivate. Linux should only need
		 * it when EOImode is split, but honoring the write is harmless for the
		 * combined model and prevents a stale software active bit if the guest
		 * still takes the explicit deactivate path.
		 */
		if (read) {
			value = 0UL;
		} else {
			vgicv3_deactivate_irq_locked(vcpu, (uint32_t)(*reg & ICH_LR_VINTID_MASK));
			flush_needed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_RPR_EL1:
		/*
		 * RPR is observational state. Returning the idle priority is sufficient
		 * for guests that probe it while virtual IRQ acknowledgement/deactivation
		 * remains in hardware through IAR/EOIR.
		 */
		if (read) {
			value = GIC_PRIORITY_LOWEST;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret == 0) {
		if (read) {
			*reg = value;
			access_value = value;
		}
		/*
		 * Record after emulation so reads capture the returned value and writes
		 * can be matched against the vGIC state produced by the access.
		 */
		last->sysreg = sysreg;
		last->value = access_value;
		last->status = ret;
		last->read = read;
		last->tsc = cpu_ticks();
		if (control_changed && vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_write_cpuif_control(ctx);
		}
		if (control_changed || flush_needed) {
			vgicv3_flush_vcpu(vcpu, true);
		}
	}
	spinlock_irqrestore_release(&vgic->lock, flags);

	return ret;
}

void arm64_vgicv3_reset_vcpu(struct acrn_vcpu *vcpu)
{
	arm64_vgicv3_init_vcpu(vcpu);
}

static void vgicv3_clear_vcpu_boot_irqs(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t virq;

	/*
	 * CPU_ON is a vCPU reset boundary. Clear private interrupt delivery state
	 * so a secondary CPU does not enter Linux with stale SGI/PPI/LPI state,
	 * while preserving distributor configuration and any shared SPI state.
	 */
	for (virq = 0U; virq < ARM64_VGIC_LOCAL_IRQ_NUM; virq++) {
		struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];

		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		desc->active = false;
	}
	for (virq = 0U; vgic->its_enabled && (virq < ARM64_VGIC_LPI_NUM); virq++) {
		struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][virq];

		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		desc->active = false;
	}
}

void arm64_vgicv3_reset_vcpu_boot_state(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return;
	}

	arm64_vgicv3_cancel_cntv_timer(vcpu);
	arm64_vgicv3_reset_vcpu(vcpu);
	vgic = &vcpu->vm->arch_vm.vgic;
	if (!vgic->initialized || (vcpu->vcpu_id >= ARM64_VGIC_MAX_VCPUS)) {
		return;
	}

	spinlock_irqsave_obtain(&vgic->lock, &flags);
	vgicv3_clear_vcpu_boot_irqs(vcpu);
	spinlock_irqrestore_release(&vgic->lock, flags);
}

void arm64_vgicv3_load_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;

	/*
	 * Program the EL1-visible virtual CPU interface and then enable ICH_HCR.
	 * Context switching a vPE includes VMCR, active priorities, and pending LR
	 * entries; LRs must be written before ICH_HCR.EN so pending virtual
	 * interrupts are observable as soon as the guest resumes.
	 */
	write_icc_sre_el1(ctx->sre | ICC_SRE_SRE);
	vgicv3_write_cpuif(ctx);
}

void arm64_vgicv3_save_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;

	/*
	 * EOIcount is consumed from the live CPU interface during sync. Saving it
	 * into the vCPU context would make later loads replay an old completion
	 * count as if it were a persistent HCR control bit.
	 */
	ctx->hcr = read_ich_hcr_el2() & ~(ICH_HCR_EOICOUNT_MASK | ICH_HCR_TC);
	vgicv3_sync_live_cpuif(vcpu);
	ctx->ap0r0 = read_ich_ap0r0_el2();
	ctx->ap1r0 = read_ich_ap1r0_el2();
	vgicv3_read_lrs(ctx);
	write_ich_hcr_el2(0UL);
}

static bool vgicv3_active_priority_empty(void)
{
	/*
	 * The virtual AP registers track guest active priority state. If both
	 * groups are empty, EL1 has returned from the interrupt handler far enough
	 * that a remaining Active LR is stale state rather than a handler still in
	 * progress.
	 */
	return (read_ich_ap0r0_el2() == 0UL) && (read_ich_ap1r0_el2() == 0UL);
}

static bool vgicv3_complete_timer_lr(struct acrn_vcpu *vcpu,
	struct arm64_vgic_irq *desc, uint32_t lr_idx, bool *removed)
{
	bool line_asserted;

	if (removed != NULL) {
		*removed = false;
	}
	if (!vgicv3_active_priority_empty()) {
		return false;
	}

	/*
	 * CNTV is the level source for the guest timer. Once AP state is empty the
	 * consumed LR no longer owns redelivery; remove it and immediately
	 * resample CNTV so an already-expired deadline stays asserted in the vGIC.
	 */
	line_asserted = arm64_vtimer_sample_current(vcpu);
	desc->active = false;
	vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, line_asserted);
	remove_lr(vcpu, lr_idx);
	vgic_irqstat_record_eoi(vcpu, desc->virq);
	if (removed != NULL) {
		*removed = true;
	}
	arm64_vtimer_set_host_mask(vcpu, line_asserted);
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_EOI, desc->virq,
		UINT32_MAX, UINT64_MAX, false, false);

	return true;
}

static void vgicv3_sync_timer_line_locked(struct acrn_vcpu *vcpu, bool is_current,
	bool line_asserted)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgic_irq *desc;
	uint32_t virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	int32_t lr_idx;

	vcpu->arch.gctx.timer_virq = virq;

	desc = vgic_irq_desc(vcpu, virq);
	if (desc == NULL) {
		return;
	}

	/*
	 * Timer line sync bridges vtimer and vGIC ownership:
	 *
	 *   CNTV sample -> descriptor pending bit -> timer LR -> live CNTV mask
	 *
	 * The timer interrupt is a level line whose source is CNTV while loaded
	 * and the saved CNTV deadline while offline. Lowering the line must remove
	 * stale pending-only LR state as well as the software pending bit, otherwise
	 * a consumed timer can be presented again after the guest has programmed a
	 * future deadline.
	 */
	desc->level = true;
	if (line_asserted && !desc->pending) {
		vgic_irqstat_record_assert(vcpu, virq, true);
	} else if (!line_asserted && desc->pending) {
		vgic_irqstat_record_deassert(vcpu, virq);
	}
	vgic_set_pending(vgic, vcpu->vcpu_id, desc, line_asserted);
	lr_idx = find_lr_for_virq(vcpu, virq);
	if (lr_idx >= 0) {
		uint32_t state = lr_state(vcpu->arch.vgic.lr[lr_idx]);
		bool lr_active = ((state & ICH_LR_STATE_ACTIVE) != 0U);

		if (line_asserted || lr_active) {
			vcpu->arch.vgic.lr[lr_idx] = make_lr_state(desc, line_asserted, lr_active);
		} else {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
	}
	/*
	 * CNTV line ownership:
	 *
	 *   line high -> vGIC owns PPI27, suppress host re-entry
	 *   line low  -> guest owns the next deadline, keep a backup tick armed
	 *
	 * The live CNTV PPI is the fast path, but the saved CNTV deadline is also
	 * mirrored into a host timer. This matches the vCPU-resume model where
	 * every return to EL1 first checks whether the saved timer is already due;
	 * it also prevents a loaded vCPU from running forever if the physical CNTV
	 * PPI does not produce another EL2 exit after a future deadline is set.
	 */
	arm64_vtimer_set_host_mask(vcpu, line_asserted);
	if (!line_asserted) {
		arm64_vgicv3_arm_cntv_timer(vcpu);
	} else {
		arm64_vgicv3_cancel_cntv_timer(vcpu);
	}
	vgicv3_flush_vcpu(vcpu, is_current);
}

void arm64_vgicv3_sync_current_timer_line(struct acrn_vcpu *vcpu, bool level)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || !vcpu->vm->arch_vm.vgic.initialized) {
		return;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	vgicv3_sync_vcpu(vcpu, true);
	vgicv3_sync_timer_line_locked(vcpu, true, level);
	spinlock_irqrestore_release(&vgic->lock, flags);
}

static void vgicv3_sync_vcpu(struct acrn_vcpu *vcpu, bool is_current)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint64_t old_lrs[ARM64_VGIC_MAX_LRS];
	uint8_t old_used_lrs;
	uint64_t eoi_lrs = 0UL;
	uint32_t idx = 0U;
	bool loaded;

	if (!is_current && vgicv3_vcpu_is_running_remote(vcpu)) {
		return;
	}
	loaded = is_current || vgicv3_vcpu_is_loaded(vcpu);

	/*
	 * Synchronization is the authoritative readback point from hardware LRs
	 * into the software IRQ descriptors. It is called before changing virtual
	 * IRQ state and from maintenance IRQs when hardware reports LR pressure or
	 * completion.
	 *
	 *   ICH_LR<n>/EISR/EOIcount -> descriptor pending/active bits
	 *        -> compact saved LRs -> next flush can refill free slots
	 */
	old_used_lrs = ctx->used_lrs;
	(void)memcpy(old_lrs, ctx->lr, sizeof(old_lrs));
	vgicv3_read_loaded_lrs(vcpu, is_current);
	if (loaded) {
		uint64_t live_hcr = read_ich_hcr_el2();

		eoi_lrs = read_ich_eisr_el2();
		vgicv3_record_cpuif_snapshot(vcpu, &vcpu->arch.debug.last_vgic_sync,
			ARM64_VCPU_DEBUG_VGIC_SYNC);
		vgicv3_sync_live_cpuif(vcpu);
		if ((live_hcr & ICH_HCR_EOICOUNT_MASK) != 0UL) {
			/*
			 * EOIcount reports guest EOIs that did not surface through EISR.
			 * Consume the count after recording it so later HCR writes do not
			 * keep rediscovering the same completed interrupt.
			 */
			ctx->hcr = live_hcr & ~(ICH_HCR_EOICOUNT_MASK | ICH_HCR_TC);
			write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
		} else {
			ctx->hcr &= ~(ICH_HCR_EOICOUNT_MASK | ICH_HCR_TC);
		}
	}
	if (((ctx->vmcr & ICH_VMCR_EOIM) == 0UL) && (eoi_lrs != 0UL)) {
		vgicv3_complete_eoi_lrs(vcpu, eoi_lrs, old_lrs, old_used_lrs);
	}

	/*
	 * Performance bottleneck: sync reconciles hardware LR evidence with the
	 * software descriptor model while the vGIC lock is normally held. Frequent
	 * timer/IPI exits pay this LR walk before new interrupts can be flushed.
	 */
	for (idx = 0U; idx < old_used_lrs; idx++) {
		uint64_t old_lr = old_lrs[idx];
		uint32_t virq;
		struct arm64_vgic_irq *desc;

		if (lr_state(old_lr) == ICH_LR_STATE_INVALID) {
			continue;
		}

		virq = lr_vintid(old_lr);
		if (find_lr_for_virq(vcpu, virq) >= 0) {
			continue;
		}

		desc = vgic_irq_desc(vcpu, virq);
		if (desc != NULL) {
			bool queued_level = desc->level && desc->pending;
			bool old_pending = ((lr_state(old_lr) & ICH_LR_STATE_PENDING) != 0U);
			bool eoi_reported = (idx < BITS_PER_LONG) &&
				((eoi_lrs & (1UL << idx)) != 0UL);
			bool timer_irq = vgic_is_vtimer_irq(vcpu, virq);
			bool timer_line_sampled = false;
			bool timer_line_asserted = false;

			if (loaded && desc->level && timer_irq && old_pending && !eoi_reported) {
				/*
				 * A pending-only timer LR can be consumed as a WFI wake event before
				 * Linux unmasks IRQs and acknowledges PPI27. The architectural source
				 * is still CNTV, so preserve the virtual line when the live timer is
				 * still asserted instead of treating the missing LR as completion.
				 */
				timer_line_asserted = arm64_vtimer_sample_current(vcpu);
				timer_line_sampled = true;
				vcpu->arch.debug.vtimer_diag.lost_pending_lr++;
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id,
					desc, timer_line_asserted);
				if (timer_line_asserted) {
					vcpu->arch.debug.vtimer_diag.pending_only_lr_preserve++;
					vgicv3_requeue_lr(vcpu, desc, loaded);
				} else {
					vcpu->arch.debug.vtimer_diag.pending_only_lr_drop++;
				}
				arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_LOST_LR,
					virq, UINT32_MAX, UINT64_MAX, false,
					timer_line_asserted);
			} else if (queued_level) {
				/* Keep the level source queued for the next flush pass. */
			} else if (!desc->level && old_pending && !eoi_reported) {
				/*
				 * A pending edge LR can wake an idle guest before EL1 has
				 * re-enabled IRQs and acknowledged the SGI. If it disappears
				 * without EOI evidence, treat the edge as not yet consumed.
				 */
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, true);
			} else {
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, false);
			}
			desc->active = false;
			if (timer_irq && (timer_line_sampled || !queued_level)) {
				arm64_vtimer_set_host_mask(vcpu,
					timer_line_sampled && timer_line_asserted);
			}
		}
	}

	idx = 0U;
	while (idx < ctx->used_lrs) {
		uint64_t lr = ctx->lr[idx];
		uint32_t virq = lr_vintid(lr);
		uint32_t state = lr_state(lr);
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);

		if (desc != NULL) {
			bool lr_pending = ((state & ICH_LR_STATE_PENDING) != 0U);
			bool lr_active = ((state & ICH_LR_STATE_ACTIVE) != 0U);
			bool removed = false;

			/*
			 * Remaining valid LRs are still owned by the virtual CPU interface.
			 * Sync copies their state back to descriptors without dropping a
			 * pending edge credit or a still-asserted level line.
			 */
			if (!desc->level && lr_active && !lr_pending) {
				/*
				 * Software edge IRQs remain active until an EOI maintenance
				 * event proves that the guest completed the interrupt. Clearing
				 * active-only SGIs here can lose an IPI that woke Linux from
				 * idle before local_irq_enable() ran the handler.
				 */
				desc->active = true;
				idx++;
				continue;
			}
			if (loaded && desc->level && vgic_is_vtimer_irq(vcpu, virq) &&
				lr_active &&
				vgicv3_complete_timer_lr(vcpu, desc, idx, &removed)) {
				if (!removed) {
					idx++;
				}
				continue;
			}
			if (loaded && desc->level && vgic_is_vtimer_irq(vcpu, virq) &&
				lr_pending && !lr_active) {
				bool line_asserted = arm64_vtimer_sample_current(vcpu);

				/* [20260626] vtimer/vGIC sync:
				 *
				 *   CNTV level high -> pending-only PPI27 LR -> wait for EOI
				 *
				 * PPI27 is a software level IRQ sourced by CNTV. Keep EOI
				 * maintenance armed so EL2 can unmask/re-sample the host source
				 * when Linux completes arch_timer_handler().
				 */
				vcpu->arch.debug.vtimer_diag.pending_only_lr_seen++;
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id,
					desc, line_asserted);
				desc->active = false;
				if (!line_asserted) {
					arm64_vtimer_set_host_mask(vcpu, false);
					vcpu->arch.debug.vtimer_diag.pending_only_lr_drop++;
					remove_lr(vcpu, idx);
				} else {
					arm64_vtimer_set_host_mask(vcpu, true);
					vcpu->arch.debug.vtimer_diag.pending_only_lr_preserve++;
					idx++;
				}
				arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_PENDING_LR,
					virq, UINT32_MAX, UINT64_MAX, false,
					line_asserted);
				continue;
			}

			/*
			 * Edge pending is a delivery credit and can be consumed once it is
			 * resident in an LR. Level pending is the sampled device line; keep
			 * it set until the device deassert path lowers the line.
			 */
			if (!desc->level &&
				(lr_pending || !lr_active || !desc->pending)) {
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, false);
			}
			desc->active = lr_active;
		}

		if ((state == ICH_LR_STATE_INVALID) || (state == 0U)) {
			remove_lr(vcpu, idx);
		} else {
			idx++;
		}
	}

	if (loaded) {
		struct arm64_vgic_irq *timer_desc =
			vgic_irq_desc(vcpu, ARM64_GIC_PPI_VIRTUAL_TIMER);

		if ((timer_desc != NULL) && timer_desc->level && timer_desc->pending) {
			vgicv3_requeue_lr(vcpu, timer_desc, true);
		}
	}
}

void arm64_vgicv3_sync_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_sync_vcpu(vcpu, false);
	}
}

void arm64_vgicv3_sync_current_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_sync_vcpu(vcpu, true);
	}
}

void arm64_vgicv3_refresh_current_level_irq(struct acrn_vm *vm, uint32_t virq)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());

	if ((vm != NULL) && (vcpu != NULL) && (vcpu->vm == vm) &&
		(virq < ARM64_VGIC_IRQ_NUM) && vm->arch_vm.vgic.initialized) {
		struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
		struct arm64_vgic_irq *desc;
		uint64_t flags;

		spinlock_irqsave_obtain(&vgic->lock, &flags);
		desc = vgic_irq_desc(vcpu, virq);
		if ((desc != NULL) && desc->level && desc->pending) {
			/*
			 * A device line that is still high may need an active LR to become
			 * active+pending after a guest MMIO exit, but it does not need a
			 * fresh vCPU event request or remote wakeup.
			 */
			vgicv3_sync_vcpu(vcpu, true);
			vgicv3_flush_vcpu(vcpu, true);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
	}
}

void arm64_vgicv3_complete_wfi_irqs(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	uint64_t flags;
	uint32_t idx = 0U;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || !vcpu->vm->arch_vm.vgic.initialized) {
		return;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	vgicv3_sync_vcpu(vcpu, true);
	while (idx < ctx->used_lrs) {
		uint64_t lr = ctx->lr[idx];
		uint32_t state = lr_state(lr);
		uint32_t virq = lr_vintid(lr);
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
		bool removed = false;

		if ((desc != NULL) && !desc->level && (state == ICH_LR_STATE_ACTIVE)) {
			/*
			 * SGIs are edge-triggered software IRQs. If Linux reaches WFI with
			 * an SGI LR still active-only, the interrupt handler has returned
			 * far enough that keeping the LR active only blocks later IPIs; a
			 * pending bit would have kept it deliverable instead.
			 */
			desc->active = false;
			remove_lr(vcpu, idx);
			continue;
		}
		if ((desc != NULL) && desc->level && vgic_is_vtimer_irq(vcpu, virq) &&
			((state & ICH_LR_STATE_ACTIVE) != 0U) &&
			vgicv3_complete_timer_lr(vcpu, desc, idx, &removed)) {
			if (!removed) {
				idx++;
			}
			continue;
		}
		idx++;
	}
	vgicv3_flush_vcpu(vcpu, true);
	spinlock_irqrestore_release(&vgic->lock, flags);
}

bool arm64_vgicv3_has_pending_irq(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	uint64_t flags;
	uint32_t idx;
	uint32_t word;
	bool pending = false;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || !vcpu->vm->arch_vm.vgic.initialized) {
		return false;
	}

	/*
	 * This query is used by the ARM64 guest-return path, not by the generic
	 * scheduler. It asks whether returning to the same EL1 context is part of
	 * interrupt completion, for example when Linux has a pending timer LR but
	 * still has DAIF.I set after WFI.
	 */
	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	if (vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_sync_live_cpuif(vcpu);
	}
	/*
	 * WFI must not sleep when a virtual interrupt is already visible through
	 * a list register. Active+Pending carries a redelivery after guest EOI, so
	 * treat any LR pending bit as enough work to re-enter EL1 immediately.
	 */
	for (idx = 0U; idx < ctx->used_lrs; idx++) {
		if ((lr_state(ctx->lr[idx]) & ICH_LR_STATE_PENDING) != 0U) {
			pending = true;
			break;
		}
	}

	/*
	 * Software pending bits for edge-like interrupts are delivery credits that
	 * still need an LR. Ordinary level pending mirrors the device line; it
	 * should wake WFI only while it still needs an LR slot, not while an active
	 * LR already owns guest-visible progress.
	 */
	for (word = 0U; !pending && (word < ARM64_VGIC_WORDS); word++) {
		uint32_t bits = vgic->pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t virq = (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];

			bits &= ~BIT32(bit);
			if (desc->pending && vgic_irq_deliverable(vgic, vcpu, desc) &&
				(!desc->level || (find_lr_for_virq(vcpu, virq) < 0))) {
				pending = true;
				break;
			}
		}
	}
	for (word = 0U; vgic->its_enabled && !pending && (word < ARM64_VGIC_LPI_WORDS); word++) {
		uint32_t bits = vgic->lpi_pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t lpi = ARM64_VGIC_LPI_BASE + (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][lpi - ARM64_VGIC_LPI_BASE];

			bits &= ~BIT32(bit);
			if (desc->pending && vgic_irq_deliverable(vgic, vcpu, desc)) {
				pending = true;
				break;
			}
		}
	}
	spinlock_irqrestore_release(&vgic->lock, flags);

	return pending;
}

static bool vgicv3_pending_lr_blocks_reschedule(struct acrn_vcpu *vcpu, uint64_t lr)
{
	uint32_t state = lr_state(lr);

	/*
	 * IRQ-routing progress window:
	 *
	 *   Pending LR -> guest has not acknowledged the IRQ yet
	 *   Active LR  -> guest handler/irq_exit path may still be running
	 *
	 * The reference scheduler model uses guest IRQ/preempt state when the guest
	 * exports it. BEAU does not have that ABI, so live active-priority state is
	 * the local approximation: avoid an immediate host reschedule while EL1 is
	 * likely between hardirq entry and bottom-half processing. A stale saved
	 * Active LR alone is not enough evidence, because that could unfairly pin a
	 * shared pCPU.
	 */
	if ((state & ICH_LR_STATE_PENDING) != 0U) {
		return true;
	}
	if (((state & ICH_LR_STATE_ACTIVE) != 0U) && vgicv3_vcpu_is_loaded(vcpu)) {
		return !vgicv3_active_priority_empty();
	}

	return false;
}

bool arm64_vgicv3_pending_irq_blocks_reschedule(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	uint64_t flags;
	uint32_t idx;
	uint32_t word;
	bool blocks = false;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || !vcpu->vm->arch_vm.vgic.initialized) {
		return false;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	if (vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_sync_live_cpuif(vcpu);
	}
	for (idx = 0U; idx < ctx->used_lrs; idx++) {
		if (vgicv3_pending_lr_blocks_reschedule(vcpu, ctx->lr[idx])) {
			blocks = true;
			break;
		}
	}
	if (!blocks && vgicv3_vcpu_is_loaded(vcpu) && !vgicv3_active_priority_empty()) {
		/*
		 * The live virtual CPU interface can still show an active guest IRQ
		 * even when LR sync no longer gives a clean per-INTID explanation. Treat
		 * that as the same IRQ-forward-progress window: return to EL1 so the
		 * guest can finish EOI/irq_exit and run any bottom halves it raised.
		 */
		blocks = true;
	}

	for (word = 0U; !blocks && (word < ARM64_VGIC_WORDS); word++) {
		uint32_t bits = vgic->pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t virq = (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];

			bits &= ~BIT32(bit);
			if (desc->pending && vgic_irq_deliverable(vgic, vcpu, desc) &&
				(!desc->level || (find_lr_for_virq(vcpu, virq) < 0))) {
				blocks = true;
				break;
			}
		}
	}
	for (word = 0U; vgic->its_enabled && !blocks && (word < ARM64_VGIC_LPI_WORDS); word++) {
		uint32_t bits = vgic->lpi_pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t lpi = ARM64_VGIC_LPI_BASE + (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][lpi - ARM64_VGIC_LPI_BASE];

			bits &= ~BIT32(bit);
			if (desc->pending && vgic_irq_deliverable(vgic, vcpu, desc)) {
				blocks = true;
				break;
			}
		}
	}
	spinlock_irqrestore_release(&vgic->lock, flags);

	return blocks;
}

static void vgicv3_flush_vcpu(struct acrn_vcpu *vcpu, bool is_current)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t word;

	if (!is_current && vgicv3_vcpu_is_running_remote(vcpu)) {
		return;
	}
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_sync_live_cpuif(vcpu);
	}

	/*
	 * Flush chooses pending, enabled virtual IRQs and materializes them into
	 * hardware LRs. If no LR is available, underflow/maintenance signaling is
	 * enabled so the guest exit path can retry after the guest consumes entries.
	 * The pending bitmap avoids scanning the full virtual INTID space when the
	 * common case has only a few pending interrupts.
	 *
	 *   pending bitmap -> deliverability check -> LR slot
	 *        -> consume software pending state or set UIE when LRs are full
	 */
	ctx->hcr &= ~ICH_HCR_UIE;

	/*
	 * Performance bottleneck: flush scans pending bitmap words, performs a
	 * descriptor deliverability check, and linearly searches existing LRs for
	 * each candidate. Interrupt-heavy guests are bounded by this lock-protected
	 * scan until enough LRs are free to publish pending virtual IRQs.
	 */
	for (word = 0U; word < ARM64_VGIC_WORDS; word++) {
		uint32_t bits = vgic->pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t virq = (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];
			int32_t lr_idx;

			bits &= ~BIT32(bit);
			if (!desc->pending) {
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				continue;
			}
			if (!vgic_irq_deliverable(vgic, vcpu, desc)) {
				continue;
			}

			lr_idx = find_lr_for_virq(vcpu, virq);
			if (lr_idx >= 0) {
				/*
				 * If the IRQ already has an LR slot, refresh it from descriptor
				 * state. Level IRQ state stays pending while the source line is
				 * asserted; the device deassert path clears it after the source
				 * lowers. The virtual timer has extra live-CNTV sampling, but it
				 * follows the same pending-as-level-line rule here.
				 */
				ctx->lr[lr_idx] = make_lr(desc);
				vgic_irqstat_record_lr(vcpu, desc, false);
				if (!desc->level) {
					vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				}
				continue;
			}

			if (ctx->used_lrs >= vgic->lr_count) {
				ctx->hcr |= ICH_HCR_UIE;
				break;
			}

			ctx->lr[ctx->used_lrs] = make_lr(desc);
			ctx->used_lrs++;
			vgic_irqstat_record_lr(vcpu, desc, true);
			if (!desc->level) {
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
			}
		}

		if ((ctx->hcr & ICH_HCR_UIE) != 0UL) {
			break;
		}
	}

	for (word = 0U; vgic->its_enabled && (word < ARM64_VGIC_LPI_WORDS); word++) {
		uint32_t bits = vgic->lpi_pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t lpi = ARM64_VGIC_LPI_BASE + (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][lpi - ARM64_VGIC_LPI_BASE];
			int32_t lr_idx;

			bits &= ~BIT32(bit);
			if (!desc->pending) {
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				continue;
			}
			if (!vgic_irq_deliverable(vgic, vcpu, desc)) {
				continue;
			}

			lr_idx = find_lr_for_virq(vcpu, lpi);
			if (lr_idx >= 0) {
				ctx->lr[lr_idx] = make_lr(desc);
				vgic_irqstat_record_lr(vcpu, desc, false);
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				continue;
			}
			if (ctx->used_lrs >= vgic->lr_count) {
				ctx->hcr |= ICH_HCR_UIE;
				break;
			}

			ctx->lr[ctx->used_lrs] = make_lr(desc);
			ctx->used_lrs++;
			vgic_irqstat_record_lr(vcpu, desc, true);
			vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		}

		if ((ctx->hcr & ICH_HCR_UIE) != 0UL) {
			break;
		}
	}

	vgicv3_write_loaded_lrs(vcpu, is_current);
}

void arm64_vgicv3_flush_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_flush_vcpu(vcpu, false);
	}
}

void arm64_vgicv3_flush_current_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_flush_vcpu(vcpu, true);
	}
}

void arm64_vgicv3_flush_current_vcpu_with_lock(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint64_t flags;

		/*
		 * Most vGIC flush callers already hold vgic->lock. The guest-return
		 * final-sync path deliberately sits outside those call chains: it only
		 * knows that the current vCPU must have any pending timer line made
		 * resident in live LRs before ERET. Keep the lock acquisition here so the
		 * caller does not need to know descriptor/LR ownership rules.
		 */
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		vgicv3_flush_vcpu(vcpu, true);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}
}

static void vgicv3_flush_target_vcpu(struct acrn_vcpu *current_vcpu,
	struct acrn_vcpu *target_vcpu)
{
	if (target_vcpu != NULL) {
		vgicv3_flush_vcpu(target_vcpu, target_vcpu == current_vcpu);
	}
}

static void vgicv3_flush_vm(struct acrn_vcpu *current_vcpu)
{
	struct acrn_vm *vm = current_vcpu->vm;
	struct acrn_vcpu *target_vcpu;
	uint16_t idx;

	foreach_vcpu(idx, vm, target_vcpu) {
		vgicv3_flush_target_vcpu(current_vcpu, target_vcpu);
	}
}

void arm64_vgicv3_flush_vm_locked(struct acrn_vcpu *current_vcpu)
{
	vgicv3_flush_vm(current_vcpu);
}

static int32_t vgic_inject_locked(struct arm64_vgicv3 *vgic, struct acrn_vcpu *target_vcpu,
	uint32_t virq, bool level)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(target_vcpu, virq);
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		/*
		 * Always sync before injection so a guest-completed LR does not
		 * get overwritten by stale software state. The event request wakes
		 * a blocked vCPU and makes the scheduler re-enter pre-guest
		 * request processing.
		 *
		 *   event source -> descriptor.pending -> optional live flush
		 *        -> vCPU event request -> scheduler/guest return path
		 */
		vgicv3_sync_vcpu(target_vcpu, false);
		desc->level = level;
		vgic_set_pending(vgic, target_vcpu->vcpu_id, desc, true);
		vgic_irqstat_record_assert(target_vcpu, virq, level);
		if (vgicv3_vcpu_is_loaded(target_vcpu)) {
			vgicv3_flush_vcpu(target_vcpu, false);
		}
		ret = 0;
	}

	return ret;
}

int32_t arm64_vgicv3_inject_irq_locked(struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level)
{
	return vgic_inject_locked(vgic, target_vcpu, virq, level);
}

static int32_t arm64_vgicv3_inject_irq_to(struct acrn_vcpu *source_vcpu,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if ((target_vcpu != NULL) && (target_vcpu->vm != NULL) && (virq < ARM64_VGIC_IRQ_NUM)) {
		vgic = &target_vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			spinlock_irqsave_obtain(&vgic->lock, &flags);
			ret = vgic_inject_locked(vgic, target_vcpu, virq, level);
			spinlock_irqrestore_release(&vgic->lock, flags);
			vgic_record_irq_injection(source_vcpu, target_vcpu, virq, level, ret);
			if (ret == 0) {
				vcpu_make_request(target_vcpu, ARM64_VCPU_REQUEST_EVENT);
				signal_event(&target_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
			}
		}
	}

	return ret;
}

int32_t arm64_vgicv3_inject_irq(struct acrn_vcpu *vcpu, uint32_t virq, bool level)
{
	struct arm64_vgicv3 *vgic;
	struct acrn_vcpu *target_vcpu;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && (virq < ARM64_VGIC_IRQ_NUM)) {
		vgic = &vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			spinlock_irqsave_obtain(&vgic->lock, &flags);
			target_vcpu = vgic_irq_target_vcpu(vcpu, vgic, virq);
			ret = vgic_inject_locked(vgic, target_vcpu, virq, level);
			spinlock_irqrestore_release(&vgic->lock, flags);
			vgic_record_irq_injection(vcpu, target_vcpu, virq, level, ret);
			if (ret == 0) {
				vcpu_make_request(target_vcpu, ARM64_VCPU_REQUEST_EVENT);
				signal_event(&target_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
			}
		}
	}

	return ret;
}

int32_t arm64_vgicv3_inject_msi(struct acrn_vm *vm, uint32_t device_id, uint32_t event_id)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if ((vm != NULL) && vm->arch_vm.vgic.initialized && vm->arch_vm.vgic.its_enabled) {
		vgic = &vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		ret = arm64_vgicv3_its_inject_msi(vm, vgic, device_id, event_id);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}

	return ret;
}

bool arm64_vgicv3_get_its_stats(struct acrn_vm *vm, struct arm64_vits_stats *stats)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vits *its;
	uint64_t flags;
	uint32_t dev_idx;
	uint32_t evt_idx;
	bool available = false;

	if ((vm == NULL) || (stats == NULL)) {
		return false;
	}

	(void)memset(stats, 0U, sizeof(*stats));
	vgic = &vm->arch_vm.vgic;
	if (!vgic->initialized || !vgic->its_enabled) {
		return false;
	}

	spinlock_irqsave_obtain(&vgic->lock, &flags);
	its = &vgic->its;
	*stats = its->stats;
	stats->cbaser = its->cbaser;
	stats->cwriter = its->cwriter;
	stats->creadr = its->creadr;
	stats->ctlr_enabled = ((its->ctlr & ARM64_VITS_CTLR_ENABLE) != 0U);
	stats->cbaser_valid = ((its->cbaser & ARM64_VITS_CBASER_VALID) != 0UL);
	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_COLLECTION_NUM; dev_idx++) {
		if (its->collection[dev_idx].valid) {
			stats->collections++;
		}
	}
	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid) {
			continue;
		}
		stats->devices++;
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			if (its->event[dev_idx][evt_idx].valid) {
				stats->events++;
			}
		}
	}
	available = true;
	spinlock_irqrestore_release(&vgic->lock, flags);

	return available;
}

int32_t arm64_vgicv3_clear_irq(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		int32_t lr_idx;

		vgic = &vcpu->vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		vgicv3_sync_vcpu(vcpu, false);
		lr_idx = find_lr_for_virq(vcpu, virq);
		if (lr_idx >= 0) {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		desc->active = false;
		if (vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_flush_vcpu(vcpu, false);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		ret = 0;
	}

	return ret;
}

int32_t arm64_vgicv3_deassert_irq(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		vgic = &vcpu->vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		vgicv3_sync_vcpu(vcpu, false);
		/*
		 * Device level deassertion lowers the input line; it is not a guest
		 * deactivate command. Preserve Active LRs until the guest EOI path
		 * completes them, but remove any pending redelivery for the line.
		 */
		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		vgic_update_irq_lr(vcpu, desc);
		vgic_irqstat_record_deassert(vcpu, virq);
		if (vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_flush_vcpu(vcpu, false);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		ret = 0;
	}

	return ret;
}

static bool sgi1r_targets_vcpu(uint64_t value, uint16_t source_vcpu_id,
	uint16_t target_vcpu_id)
{
	bool targeted;

	if ((value & ICC_SGI1R_IRM) != 0UL) {
		targeted = (target_vcpu_id != source_vcpu_id);
	} else {
		uint64_t aff1 = (value >> ICC_SGI1R_AFF1_SHIFT) & ICC_SGI1R_AFF_MASK;
		uint64_t aff2 = (value >> ICC_SGI1R_AFF2_SHIFT) & ICC_SGI1R_AFF_MASK;
		uint64_t aff3 = (value >> ICC_SGI1R_AFF3_SHIFT) & ICC_SGI1R_AFF_MASK;
		uint64_t target_list = value & ICC_SGI1R_TARGET_LIST_MASK;

		targeted = (aff1 == 0UL) && (aff2 == 0UL) && (aff3 == 0UL) &&
			((target_list & (1UL << target_vcpu_id)) != 0UL);
	}

	return targeted;
}

static void vgic_record_sgi(struct acrn_vcpu *source_vcpu, uint64_t value,
	uint32_t intid, uint16_t target_mask, uint16_t delivered_mask, int32_t status)
{
	struct arm64_vcpu_last_sgi *last = &source_vcpu->arch.debug.last_sgi;

	last->value = value;
	last->intid = intid;
	last->status = status;
	last->source_vcpu_id = source_vcpu->vcpu_id;
	last->target_mask = target_mask;
	last->delivered_mask = delivered_mask;
	last->tsc = cpu_ticks();
}

static void vgic_sgi_masks(const struct acrn_vcpu *vcpu, uint16_t *enabled,
	uint16_t *pending, uint16_t *active)
{
	const struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t intid;

	*enabled = 0U;
	*pending = 0U;
	*active = 0U;

	for (intid = 0U; intid < ARM64_VGIC_SGI_NUM; intid++) {
		const struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][intid];
		uint16_t bit = (uint16_t)BIT32(intid);

		if (desc->enabled) {
			*enabled |= bit;
		}
		if (desc->pending) {
			*pending |= bit;
		}
		if (desc->active) {
			*active |= bit;
		}
	}
}

static void vgic_record_sgi_target(struct acrn_vcpu *source_vcpu,
	struct acrn_vcpu *target_vcpu, uint64_t value, uint32_t intid, int32_t status)
{
	struct arm64_vcpu_last_sgi_target *last = &target_vcpu->arch.debug.last_sgi_target;
	const struct arm64_vgic_irq *desc = vgic_irq_desc(target_vcpu, intid);
	bool target_current = vgicv3_vcpu_is_loaded(target_vcpu);
	uint16_t enabled;
	uint16_t pending;
	uint16_t active;

	/*
	 * Remote target state can only be sampled from the saved vGIC context.
	 * Live ICH_* registers are read only when the target vCPU is the current
	 * guest on this pCPU; otherwise they would describe the source vCPU.
	 */
	vgic_sgi_masks(target_vcpu, &enabled, &pending, &active);
	last->source_value = value;
	last->intid = intid;
	last->status = status;
	last->source_vcpu_id = source_vcpu->vcpu_id;
	last->target_vcpu_id = target_vcpu->vcpu_id;
	last->local_enabled = enabled;
	last->local_pending = pending;
	last->local_active = active;
	last->used_lrs = target_vcpu->arch.vgic.used_lrs;
	last->request_pending = vcpu_has_pending_request(target_vcpu);
	last->target_running = is_vcpu_running(target_vcpu);
	last->target_current = target_current;
	last->desc_enabled = (desc != NULL) && desc->enabled;
	last->desc_pending = (desc != NULL) && desc->pending;
	last->desc_active = (desc != NULL) && desc->active;
	last->desc_level = (desc != NULL) && desc->level;
	if (target_current) {
		last->hcr = read_ich_hcr_el2();
		last->misr = read_ich_misr_el2();
		last->lr0 = read_ich_lr_el2(0U);
		last->lr1 = read_ich_lr_el2(1U);
	} else {
		last->hcr = target_vcpu->arch.vgic.hcr;
		last->misr = 0UL;
		last->lr0 = (target_vcpu->arch.vgic.used_lrs > 0U) ?
			target_vcpu->arch.vgic.lr[0U] : 0UL;
		last->lr1 = (target_vcpu->arch.vgic.used_lrs > 1U) ?
			target_vcpu->arch.vgic.lr[1U] : 0UL;
	}
	last->tsc = cpu_ticks();
}

int32_t arm64_vgicv3_handle_sgi1r(struct acrn_vcpu *vcpu, uint64_t value)
{
	uint32_t intid = (uint32_t)((value >> ICC_SGI1R_INTID_SHIFT) & ICC_SGI1R_INTID_MASK);
	uint16_t idx;
	uint16_t target_mask = 0U;
	uint16_t delivered_mask = 0U;
	struct acrn_vcpu *target_vcpu;
	int32_t last_status = 0;

	/*
	 * Guest SGI sends are trapped from ICC_SGI1R_EL1 and translated into
	 * per-target virtual IRQ injections. The current affinity model matches
	 * the QEMU vMPIDR layout where vCPU IDs occupy affinity level 0.
	 * Unlike LRs, ICC_SGI1R carries a target-list/affinity payload, so the
	 * hypervisor must decode it before choosing each target vCPU descriptor.
	 *
	 *   ICC_SGI1R_EL1 trap -> target mask -> per-target inject
	 *        -> kick running target vCPU
	 */
	foreach_vcpu(idx, vcpu->vm, target_vcpu) {
		if (sgi1r_targets_vcpu(value, vcpu->vcpu_id, target_vcpu->vcpu_id)) {
			target_mask |= BIT32(target_vcpu->vcpu_id);
		}
		if ((vcpu_get_state(target_vcpu) != VCPU_OFFLINE) &&
			((target_mask & BIT32(target_vcpu->vcpu_id)) != 0U)) {
			last_status = arm64_vgicv3_inject_irq_to(vcpu, target_vcpu, intid, false);
			vgic_record_sgi_target(vcpu, target_vcpu, value, intid, last_status);
			if (last_status == 0) {
				delivered_mask |= BIT32(target_vcpu->vcpu_id);
			}
			if (is_vcpu_running(target_vcpu)) {
				kick_vcpu(target_vcpu);
			}
		}
	}
	vgic_record_sgi(vcpu, value, intid, target_mask, delivered_mask, last_status);

	return 0;
}

void arm64_vgicv3_maintenance_irq_handler(__unused uint32_t irq, __unused void *data)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());

	if (vcpu != NULL) {
		struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint64_t flags;

		/*
		 * Maintenance IRQs are the hardware notification that LR state
		 * needs service. Read back guest-consumed state, then refill LRs
		 * from software pending state while still on the running pCPU.
		 * Arm reports this as a PPI; BEAU wires it as INTID 25 and uses
		 * ICH_MISR/EISR/ELRSR snapshots to decide what software state changed.
		 *
		 *   maintenance PPI -> CPU interface snapshot
		 *        -> sync LRs -> flush pending backlog
		 */
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		vgicv3_record_cpuif_snapshot(vcpu, &vcpu->arch.debug.last_vgic_maintenance,
			ARM64_VCPU_DEBUG_VGIC_MAINTENANCE);
		vgicv3_sync_vcpu(vcpu, true);
		vgicv3_flush_vcpu(vcpu, true);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}
}

static uint32_t reg_word_index(uint32_t offset, uint32_t base)
{
	return (offset - base) / 4U;
}

static bool mmio_access_in_range(uint32_t offset, uint64_t size, uint32_t start, uint32_t end)
{
	return (offset >= start) && (offset < end) && (size <= (uint64_t)(end - offset));
}

static bool vgic_priority_access(uint32_t offset, uint64_t size)
{
	/*
	 * Priority registers are byte-addressable by architecture, and guests may
	 * legally use byte, halfword, or word accesses. Other modeled registers
	 * are handled as aligned 32-bit words by vgic_word_access().
	 */
	return ((size == 1UL) || (size == 2UL) || (size == 4UL)) &&
		mmio_access_in_range(offset, size, VGICD_IPRIORITYR, VGICD_ITARGETSR);
}

static bool vgic_target_access(uint32_t offset, uint64_t size)
{
	return ((size == 1UL) || (size == 2UL) || (size == 4UL)) &&
		mmio_access_in_range(offset, size, VGICD_ITARGETSR, VGICD_ICFGR);
}

static bool vgic_word_access(uint32_t offset, uint64_t size)
{
	return (size == 4UL) && ((offset & 0x3U) == 0U);
}

struct vgic_irq_bit_reg {
	uint32_t selector;
	uint32_t irq_base;
};

struct vgic_irq_config_reg {
	uint32_t irq_base;
};

struct vgic_irouter_reg {
	uint32_t virq;
	uint32_t word;
};

struct vgicr_frame {
	uint16_t pcpu_id;
	uint16_t vcpu_id;
	uint32_t offset;
};

static bool vgic_decode_irq_bit_reg(uint32_t offset, bool local_bank,
	struct vgic_irq_bit_reg *reg)
{
	uint32_t selector = UINT32_MAX;
	uint32_t base = 0U;
	uint32_t max_words = local_bank ? 1U : ARM64_VGIC_WORDS;
	bool ret = true;

	/*
	 * The GIC enable/pending/active registers are arranged as parallel
	 * set/clear views over the same IRQ descriptor bits. Decode the offset
	 * once into the canonical selector and first INTID covered by the word.
	 *
	 * Redistributor SGI/PPI accesses use local_bank=true because the current
	 * model exposes only the first 32 local INTIDs for each target vCPU.
	 */
	switch (offset) {
	case VGICD_IGROUPR ... (VGICD_ISENABLER - 1U):
		selector = VGICD_IGROUPR;
		base = reg_word_index(offset, VGICD_IGROUPR) * 32U;
		break;
	case VGICD_ISENABLER ... (VGICD_ICENABLER - 1U):
		selector = VGICD_ISENABLER;
		base = reg_word_index(offset, VGICD_ISENABLER) * 32U;
		break;
	case VGICD_ICENABLER ... (VGICD_ISPENDR - 1U):
		selector = VGICD_ICENABLER;
		base = reg_word_index(offset, VGICD_ICENABLER) * 32U;
		break;
	case VGICD_ISPENDR ... (VGICD_ICPENDR - 1U):
		selector = VGICD_ISPENDR;
		base = reg_word_index(offset, VGICD_ISPENDR) * 32U;
		break;
	case VGICD_ICPENDR ... (VGICD_ISACTIVER - 1U):
		selector = VGICD_ICPENDR;
		base = reg_word_index(offset, VGICD_ICPENDR) * 32U;
		break;
	case VGICD_ISACTIVER ... (VGICD_ICACTIVER - 1U):
		selector = VGICD_ISACTIVER;
		base = reg_word_index(offset, VGICD_ISACTIVER) * 32U;
		break;
	case VGICD_ICACTIVER ... (VGICD_IPRIORITYR - 1U):
		selector = VGICD_ICACTIVER;
		base = reg_word_index(offset, VGICD_ICACTIVER) * 32U;
		break;
	default:
		if ((offset >= VGICD_IGRPMODR) &&
			(offset < (VGICD_IGRPMODR + (max_words * 4U)))) {
			selector = VGICD_IGRPMODR;
			base = reg_word_index(offset, VGICD_IGRPMODR) * 32U;
		} else {
			ret = false;
		}
		break;
	}

	if (ret && local_bank && (base != 0U)) {
		ret = false;
	}

	if (ret) {
		reg->selector = selector;
		reg->irq_base = local_bank ? 0U : base;
	}

	return ret;
}

static bool vgic_decode_irq_config_reg(uint32_t offset, bool local_bank,
	struct vgic_irq_config_reg *reg)
{
	uint32_t max_irqs = local_bank ? ARM64_VGIC_LOCAL_IRQ_NUM : ARM64_VGIC_IRQ_NUM;
	uint32_t end = VGICD_ICFGR + ((max_irqs / VGIC_ICFGR_IRQS_PER_REG) * 4U);
	uint32_t word;
	uint32_t irq_base;
	bool ret = false;

	if ((offset >= VGICD_ICFGR) && (offset < end)) {
		word = reg_word_index(offset, VGICD_ICFGR);
		irq_base = word * VGIC_ICFGR_IRQS_PER_REG;
		reg->irq_base = irq_base;
		ret = true;
	}

	return ret;
}

static bool vgic_decode_irouter_reg(uint32_t offset, struct vgic_irouter_reg *reg)
{
	uint32_t rel;
	uint32_t virq;
	bool ret = false;

	if (offset >= VGICD_IROUTER) {
		rel = offset - VGICD_IROUTER;
		virq = rel / 8U;
		if ((virq >= ARM64_VGIC_LOCAL_IRQ_NUM) && (virq < ARM64_VGIC_IRQ_NUM) &&
			((rel & 0x3U) == 0U)) {
			reg->virq = virq;
			reg->word = ((rel & 0x4U) != 0U) ? 1U : 0U;
			ret = true;
		}
	}

	return ret;
}

static uint64_t vgic_read_irq_priority(struct acrn_vcpu *vcpu, uint32_t irq_base, uint64_t size)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint64_t value = 0UL;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		struct arm64_vgic_irq *desc = vgic_irq_is_spi(virq) ?
			&vgic->irq[0U][virq] : vgic_irq_desc(vcpu, virq);
		uint8_t priority = (desc != NULL) ? desc->priority : GIC_PRIORITY_DEFAULT;

		value |= (uint64_t)priority << (byte * 8U);
	}

	return value;
}

static void vgic_write_irq_priority(struct acrn_vcpu *vcpu, uint32_t irq_base,
	uint64_t size, uint64_t value)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		uint8_t priority = (uint8_t)((value >> (byte * 8U)) & 0xffU);
		uint16_t vcpu_id;
		struct arm64_vgic_irq *desc;

		if (vgic_irq_is_spi(virq)) {
			for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
				desc = &vgic->irq[vcpu_id][virq];
				desc->priority = priority;
				vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
			}
		} else {
			desc = vgic_irq_desc(vcpu, virq);
			if (desc == NULL) {
				continue;
			}
			desc->priority = priority;
			vgic_update_irq_lr(vcpu, desc);
		}
	}
}

static struct arm64_vgic_irq *vgic_read_irq_bit_desc(struct acrn_vcpu *vcpu,
	uint32_t virq, uint32_t selector)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint16_t target_vcpu_id = vcpu->vcpu_id;

	if (virq >= ARM64_VGIC_IRQ_NUM) {
		return NULL;
	}

	if (vgic_irq_is_spi(virq)) {
		switch (selector) {
		case VGICD_ISPENDR:
		case VGICD_ICPENDR:
		case VGICD_ISACTIVER:
		case VGICD_ICACTIVER:
			target_vcpu_id = vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu);
			break;
		default:
			target_vcpu_id = 0U;
			break;
		}
	}

	return (target_vcpu_id < ARM64_VGIC_MAX_VCPUS) ? &vgic->irq[target_vcpu_id][virq] : NULL;
}

static uint32_t vgic_read_irq_bits(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t selector)
{
	uint32_t val = 0U;
	uint32_t bit;

	for (bit = 0U; bit < 32U; bit++) {
		struct arm64_vgic_irq *desc = vgic_read_irq_bit_desc(vcpu,
			irq_base + bit, selector);
		bool set = false;

		if (desc == NULL) {
			continue;
		}

		switch (selector) {
		case VGICD_IGROUPR:
			set = true;
			break;
		case VGICD_ISENABLER:
		case VGICD_ICENABLER:
			set = desc->enabled;
			break;
		case VGICD_ISPENDR:
		case VGICD_ICPENDR:
			set = desc->pending;
			break;
		case VGICD_ISACTIVER:
		case VGICD_ICACTIVER:
			set = desc->active;
			break;
		case VGICD_IGRPMODR:
			set = false;
			break;
		default:
			break;
		}

		if (set) {
			val |= BIT32(bit);
		}
	}

	return val;
}

static void vgic_write_irq_bits(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t selector, uint32_t val)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t bit;

	for (bit = 0U; bit < 32U; bit++) {
		struct arm64_vgic_irq *desc;
		uint32_t virq = irq_base + bit;
		uint16_t vcpu_id;
		uint16_t target_vcpu_id;

		if (virq >= ARM64_VGIC_IRQ_NUM) {
			continue;
		}

		if ((selector != VGICD_IGROUPR) && (selector != VGICD_IGRPMODR) &&
			((val & BIT32(bit)) == 0U)) {
			continue;
		}

		switch (selector) {
		case VGICD_IGROUPR:
			/* The current virtual GIC exposes a non-secure Group-1-only model. */
			break;
		case VGICD_ISENABLER:
			if (vgic_irq_is_spi(virq)) {
				for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
					desc = &vgic->irq[vcpu_id][virq];
					desc->enabled = true;
					vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
				}
			} else {
				desc = vgic_irq_desc(vcpu, virq);
				if (desc != NULL) {
					desc->enabled = true;
					vgic_update_irq_lr(vcpu, desc);
				}
			}
			break;
		case VGICD_ICENABLER:
			if (vgic_irq_is_spi(virq)) {
				for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
					desc = &vgic->irq[vcpu_id][virq];
					desc->enabled = false;
					vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
				}
			} else {
				desc = vgic_irq_desc(vcpu, virq);
				if (desc != NULL) {
					desc->enabled = false;
					vgic_update_irq_lr(vcpu, desc);
				}
			}
			break;
		case VGICD_ISPENDR:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			vgic_set_pending(vgic, target_vcpu_id, desc, true);
			vgic_irqstat_record_assert(vcpu_from_vid(vcpu->vm, target_vcpu_id),
				virq, desc->level);
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_ICPENDR:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			vgic_set_pending(vgic, target_vcpu_id, desc, false);
			vgic_irqstat_record_deassert(vcpu_from_vid(vcpu->vm, target_vcpu_id),
				virq);
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_ISACTIVER:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			desc->active = true;
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_ICACTIVER:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			desc->active = false;
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_IGRPMODR:
			/* Group modifiers are ignored in the Group-1-only model. */
			break;
		default:
			break;
		}
	}
}

static uint64_t vgic_read_irq_target(struct acrn_vcpu *vcpu, uint32_t irq_base, uint64_t size)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint64_t value = 0UL;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		uint8_t target = 0U;

		if (vgic_irq_is_spi(virq)) {
			uint16_t target_vcpu_id =
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu);

			target = (uint8_t)(1U << target_vcpu_id);
		}
		value |= (uint64_t)target << (byte * 8U);
	}

	return value;
}

static void vgic_write_irq_target(struct acrn_vcpu *vcpu, uint32_t irq_base,
	uint64_t size, uint64_t value)
{
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		uint8_t target_mask = (uint8_t)((value >> (byte * 8U)) & 0xffU);
		uint16_t target_vcpu_id = 0U;

		if (!vgic_irq_is_spi(virq)) {
			continue;
		}

		if (target_mask != 0U) {
			target_vcpu_id = ffs64((uint64_t)target_mask);
		}
		vgic_set_spi_target(vcpu->vm, virq, target_vcpu_id);
	}
}

static uint32_t vgic_read_irq_config(struct acrn_vcpu *vcpu, uint32_t irq_base)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t val = 0U;
	uint32_t idx;

	for (idx = 0U; idx < VGIC_ICFGR_IRQS_PER_REG; idx++) {
		uint32_t virq = irq_base + idx;
		struct arm64_vgic_irq *desc;

		if (virq >= ARM64_VGIC_IRQ_NUM) {
			continue;
		}

		desc = vgic_irq_is_spi(virq) ? &vgic->irq[0U][virq] : vgic_irq_desc(vcpu, virq);

		if ((desc != NULL) && !desc->level) {
			val |= BIT32((idx * 2U) + 1U);
		}
	}

	return val;
}

static void vgic_write_irq_config(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t val)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t idx;

	for (idx = 0U; idx < VGIC_ICFGR_IRQS_PER_REG; idx++) {
		uint32_t virq = irq_base + idx;
		uint16_t vcpu_id;
		struct arm64_vgic_irq *desc;

		if (virq >= ARM64_VGIC_IRQ_NUM) {
			continue;
		}

		if (!vgic_irq_configurable(virq)) {
			if (virq == ARM64_GIC_PPI_VIRTUAL_TIMER) {
				desc = vgic_irq_desc(vcpu, virq);
				if (desc != NULL) {
					desc->level = true;
					vgic_update_irq_lr(vcpu, desc);
				}
			}
			continue;
		}

		if (vgic_irq_is_spi(virq)) {
			for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
				desc = &vgic->irq[vcpu_id][virq];
				desc->level = ((val & BIT32((idx * 2U) + 1U)) == 0U);
				vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
			}
		} else {
			desc = vgic_irq_desc(vcpu, virq);
			if (desc == NULL) {
				continue;
			}
			desc->level = ((val & BIT32((idx * 2U) + 1U)) == 0U);
			vgic_update_irq_lr(vcpu, desc);
		}
	}
}

static uint16_t vgic_irouter_target_vcpu(const struct arm64_vgicv3 *vgic, uint64_t router)
{
	uint64_t aff0 = router & GIC_IROUTER_AFF0_MASK;
	uint64_t aff1 = (router >> GIC_IROUTER_AFF1_SHIFT) & GIC_IROUTER_AFF_MASK;
	uint64_t aff2 = (router >> GIC_IROUTER_AFF2_SHIFT) & GIC_IROUTER_AFF_MASK;
	uint64_t aff3 = (router >> GIC_IROUTER_AFF3_SHIFT) & GIC_IROUTER_AFF_MASK;
	uint16_t target_vcpu_id = 0U;

	if (((router & GIC_IROUTER_IRM) == 0UL) && (aff1 == 0UL) && (aff2 == 0UL) &&
		(aff3 == 0UL) && (aff0 < vgic->vcpu_count)) {
		target_vcpu_id = (uint16_t)aff0;
	}

	return target_vcpu_id;
}

static uint32_t vgic_read_irouter_word(const struct arm64_vgicv3 *vgic,
	const struct vgic_irouter_reg *reg)
{
	uint32_t spi = reg->virq - ARM64_VGIC_LOCAL_IRQ_NUM;
	uint64_t router = (spi < ARM64_VGIC_SPI_NUM) ? vgic->spi_router[spi] : 0UL;
	uint32_t value;

	switch (reg->word) {
	case 0U:
		value = (uint32_t)(router & GIC_IROUTER_LOW_MASK);
		break;
	case 1U:
		value = (uint32_t)(router >> 32U);
		break;
	default:
		value = 0U;
		break;
	}

	return value;
}

static void vgic_write_irouter_word(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	const struct vgic_irouter_reg *reg, uint32_t value)
{
	uint32_t spi = reg->virq - ARM64_VGIC_LOCAL_IRQ_NUM;
	uint64_t router = (spi < ARM64_VGIC_SPI_NUM) ? vgic->spi_router[spi] : 0UL;
	uint16_t target_vcpu_id;

	switch (reg->word) {
	case 0U:
		router = (router & ~GIC_IROUTER_LOW_MASK) | value;
		break;
	case 1U:
		router = (router & GIC_IROUTER_LOW_MASK) | ((uint64_t)value << 32U);
		break;
	default:
		break;
	}

	if (spi < ARM64_VGIC_SPI_NUM) {
		vgic->spi_router[spi] = router;
	}

	target_vcpu_id = vgic_irouter_target_vcpu(vgic, router);
	vgic_set_spi_target(vm, reg->virq, target_vcpu_id);
}

static void vgicd_mmio_read(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct vgic_irq_bit_reg bit_reg;
	struct vgic_irq_config_reg config_reg;
	struct vgic_irouter_reg irouter_reg;

	/*
	 * Distributor MMIO exposes the shared interrupt-controller programming
	 * model. For the current QEMU guests, the important pieces are enable,
	 * pending, active, priority, and basic identity registers.
	 */
	if (vgic_decode_irq_bit_reg(offset, false, &bit_reg)) {
		mmio->value = vgic_read_irq_bits(vcpu, bit_reg.irq_base, bit_reg.selector);
	} else if (vgic_decode_irq_config_reg(offset, false, &config_reg)) {
		mmio->value = vgic_read_irq_config(vcpu, config_reg.irq_base);
	} else {
		switch (offset) {
		case VGICD_CTLR:
			mmio->value = vgic->gicd_ctlr;
			break;
		case VGICD_TYPER:
			mmio->value = vgic->gicd_typer;
			break;
		case VGICD_IIDR:
			mmio->value = beau_config.gic_iidr;
			break;
		case VGICD_ITARGETSR ... (VGICD_ICFGR - 1U):
			mmio->value = vgic_read_irq_target(vcpu, offset - VGICD_ITARGETSR,
				mmio->size);
			break;
		case VGICD_IROUTER ... (VGICD_PIDR2 - 1U):
			if (vgic_decode_irouter_reg(offset, &irouter_reg)) {
				mmio->value = vgic_read_irouter_word(vgic, &irouter_reg);
			} else {
				mmio->value = 0UL;
			}
			break;
		case VGICD_PIDR2:
			mmio->value = vgic->gicd_pidr2;
			break;
		default:
			mmio->value = 0UL;
			break;
		}
	}
}

static bool vgicd_mmio_write(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	const struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct vgic_irq_bit_reg bit_reg;
	struct vgic_irq_config_reg config_reg;
	struct vgic_irouter_reg irouter_reg;
	uint32_t value = (uint32_t)mmio->value;
	bool flush = false;

	/*
	 * Only writes that can change pending/active/enable/priority state need
	 * to be flushed into LRs. Control or ignored register writes update the
	 * software model only and avoid unnecessary hardware LR traffic.
	 */
	if (vgic_decode_irq_bit_reg(offset, false, &bit_reg)) {
		vgic_write_irq_bits(vcpu, bit_reg.irq_base, bit_reg.selector, value);
		flush = true;
	} else if (vgic_decode_irq_config_reg(offset, false, &config_reg)) {
		vgic_write_irq_config(vcpu, config_reg.irq_base, value);
		flush = true;
	} else {
		switch (offset) {
		case VGICD_CTLR:
			vgic->gicd_ctlr = value & ~VGICD_CTLR_RWP;
			flush = true;
			break;
		case VGICD_ITARGETSR ... (VGICD_ICFGR - 1U):
			vgic_write_irq_target(vcpu, offset - VGICD_ITARGETSR,
				mmio->size, mmio->value);
			flush = true;
			break;
		case VGICD_IROUTER ... (VGICD_PIDR2 - 1U):
			if (vgic_decode_irouter_reg(offset, &irouter_reg)) {
				vgic_write_irouter_word(vcpu->vm, vgic, &irouter_reg, value);
				flush = true;
			}
			break;
		default:
			break;
		}
	}

	return flush;
}

static int32_t vgicd_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	uint32_t offset = (uint32_t)(mmio->address -
		get_vm_config(vcpu->vm->vm_id)->arch.guest_gicd_base);
	uint32_t irq_base;
	int32_t ret = 0;

	/*
	 * Dispatch order mirrors architectural access rules: priority registers
	 * accept sub-word accesses, while the rest of the modeled distributor
	 * block is restricted to aligned 32-bit accesses.
	 */
	if (vgic_priority_access(offset, mmio->size)) {
		irq_base = offset - VGICD_IPRIORITYR;
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			mmio->value = vgic_read_irq_priority(vcpu, irq_base, mmio->size);
			break;
		case ACRN_IOREQ_DIR_WRITE:
			vgic_write_irq_priority(vcpu, irq_base, mmio->size, mmio->value);
			vgicv3_flush_vm(vcpu);
			break;
		default:
			ret = -EINVAL;
			break;
		}
	} else if (vgic_target_access(offset, mmio->size)) {
		irq_base = offset - VGICD_ITARGETSR;
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			mmio->value = vgic_read_irq_target(vcpu, irq_base, mmio->size);
			break;
		case ACRN_IOREQ_DIR_WRITE:
			vgic_write_irq_target(vcpu, irq_base, mmio->size, mmio->value);
			vgicv3_flush_vm(vcpu);
			break;
		default:
			ret = -EINVAL;
			break;
		}
	} else if (!vgic_word_access(offset, mmio->size)) {
		ret = -EINVAL;
	} else {
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			vgicd_mmio_read(vcpu, vgic, mmio, offset);
			break;
		case ACRN_IOREQ_DIR_WRITE:
			if (vgicd_mmio_write(vcpu, vgic, mmio, offset)) {
				vgicv3_flush_vm(vcpu);
			}
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static uint16_t vgicr_pcpu_id(const struct acrn_vcpu *vcpu, uint64_t addr)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vcpu->vm->vm_id)->arch;

	return (uint16_t)((addr - arch_config->guest_gicr_base) /
		arch_config->guest_gicr_stride);
}

static uint32_t vgicr_offset(const struct acrn_vcpu *vcpu, uint64_t addr)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vcpu->vm->vm_id)->arch;

	return (uint32_t)((addr - arch_config->guest_gicr_base) %
		arch_config->guest_gicr_stride);
}

static bool vgicr_decode_frame(const struct acrn_vcpu *vcpu, const struct arm64_vgicv3 *vgic,
	uint64_t addr, struct vgicr_frame *frame)
{
	uint16_t pcpu_id = vgicr_pcpu_id(vcpu, addr);
	bool ret = false;

	if ((pcpu_id < ARM64_VGIC_MAX_VCPUS) && (pcpu_id < vgic->rdist_count)) {
		frame->pcpu_id = pcpu_id;
		frame->vcpu_id = vgic->rdist_vcpu[pcpu_id];
		if ((frame->vcpu_id != VGIC_INVALID_VCPU_ID) &&
			(frame->vcpu_id >= vcpu->vm->hw.created_vcpus)) {
			frame->vcpu_id = VGIC_INVALID_VCPU_ID;
		}
		frame->offset = vgicr_offset(vcpu, addr);
		ret = true;
	}

	return ret;
}

static uint64_t vgicr_typer_value(const struct arm64_vgicv3 *vgic, const struct vgicr_frame *frame)
{
	uint64_t affinity = (frame->vcpu_id == VGIC_INVALID_VCPU_ID) ?
		VGIC_INVALID_AFFINITY : frame->vcpu_id;
	uint64_t typer = ((uint64_t)frame->pcpu_id << VGICR_TYPER_PROCNUM_SHIFT) |
		(affinity << VGICR_TYPER_AFF_SHIFT);

	if (vgic->its_enabled) {
		typer |= VGICR_TYPER_PLPIS;
	}
	if (frame->pcpu_id == (vgic->rdist_count - 1U)) {
		typer |= VGICR_TYPER_LAST;
	}

	return typer;
}

static void vgicr_write_waker(struct arm64_vgicv3 *vgic, uint16_t pcpu_id, uint32_t value)
{
	uint32_t waker = 0U;

	if ((value & VGICR_WAKER_PROCESSOR_SLEEP) != 0U) {
		waker = VGICR_WAKER_PROCESSOR_SLEEP | VGICR_WAKER_CHILDREN_ASLEEP;
	}

	vgic->gicr_waker[pcpu_id] = waker;
}

static void vgicr_sgi_mmio_read(struct acrn_vcpu *target_vcpu,
	struct acrn_mmio_request *mmio, uint32_t sgi_offset)
{
	struct vgic_irq_bit_reg bit_reg;
	struct vgic_irq_config_reg config_reg;
	uint32_t irq_base;

	/*
	 * The SGI/PPI sub-frame reuses distributor-style register offsets, but the
	 * current model exposes only the target vCPU's local INTID bank.
	 */
	if (vgic_decode_irq_bit_reg(sgi_offset, true, &bit_reg)) {
		mmio->value = vgic_read_irq_bits(target_vcpu, bit_reg.irq_base, bit_reg.selector);
	} else if (vgic_decode_irq_config_reg(sgi_offset, true, &config_reg)) {
		mmio->value = vgic_read_irq_config(target_vcpu, config_reg.irq_base);
	} else {
		switch (sgi_offset) {
		case VGICD_IPRIORITYR ... (VGICD_ICFGR - 1U):
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			mmio->value = vgic_read_irq_priority(target_vcpu, irq_base, mmio->size);
			break;
		default:
			mmio->value = 0UL;
			break;
		}
	}
}

static bool vgicr_sgi_mmio_write(struct acrn_vcpu *target_vcpu,
	const struct acrn_mmio_request *mmio, uint32_t sgi_offset)
{
	struct vgic_irq_bit_reg bit_reg;
	struct vgic_irq_config_reg config_reg;
	uint32_t irq_base;
	uint32_t value = (uint32_t)mmio->value;
	bool flush = false;

	if (vgic_decode_irq_bit_reg(sgi_offset, true, &bit_reg)) {
		vgic_write_irq_bits(target_vcpu, bit_reg.irq_base, bit_reg.selector, value);
		flush = true;
	} else if (vgic_decode_irq_config_reg(sgi_offset, true, &config_reg)) {
		vgic_write_irq_config(target_vcpu, config_reg.irq_base, value);
		flush = true;
	} else {
		switch (sgi_offset) {
		case VGICD_IPRIORITYR ... (VGICD_ICFGR - 1U):
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			vgic_write_irq_priority(target_vcpu, irq_base, mmio->size, mmio->value);
			flush = true;
			break;
		default:
			break;
		}
	}

	return flush;
}

static void vgicr_mmio_read(struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, struct acrn_mmio_request *mmio,
	const struct vgicr_frame *frame)
{
	uint32_t offset = frame->offset;
	uint64_t typer;

	/*
	 * Guest redistributor frames are addressed by pCPU slot. A frame's
	 * GICR_TYPER still reports the guest vCPU affinity assigned to that pCPU
	 * slot, while SGI/PPI state is stored in the target vCPU's local IRQ row.
	 */
	switch (offset) {
	case VGICR_CTLR:
		mmio->value = vgic->gicr_ctlr[frame->pcpu_id] & ~VGICR_CTLR_RWP;
		break;
	case VGICR_IIDR:
		mmio->value = beau_config.gic_iidr;
		break;
	case VGICR_TYPER:
		typer = vgicr_typer_value(vgic, frame);
		mmio->value = typer & UINT32_MAX;
		break;
	case VGICR_TYPER + 4U:
		typer = vgicr_typer_value(vgic, frame);
		mmio->value = typer >> 32U;
		break;
	case VGICR_WAKER:
		mmio->value = vgic->gicr_waker[frame->pcpu_id] & VGICR_WAKER_READ_MASK;
		break;
	case VGICR_PROPBASER:
		mmio->value = vgic->gicr_propbaser[frame->pcpu_id] & UINT32_MAX;
		break;
	case VGICR_PROPBASER + 4U:
		mmio->value = vgic->gicr_propbaser[frame->pcpu_id] >> 32U;
		break;
	case VGICR_PENDBASER:
		mmio->value = vgic->gicr_pendbaser[frame->pcpu_id] & UINT32_MAX;
		break;
	case VGICR_PENDBASER + 4U:
		mmio->value = vgic->gicr_pendbaser[frame->pcpu_id] >> 32U;
		break;
	case VGICR_SYNCR:
		mmio->value = 0UL;
		break;
	case VGICR_PIDR2:
		mmio->value = GIC_PIDR2_ARCH_GICV3;
		break;
	case VGICR_SGI_BASE ... (VGICR_SGI_END - 1U):
		if (target_vcpu != NULL) {
			vgicr_sgi_mmio_read(target_vcpu, mmio, offset - VGICR_SGI_BASE);
		} else {
			mmio->value = 0UL;
		}
		break;
	default:
		mmio->value = 0UL;
		break;
	}
}

static bool vgicr_mmio_write(struct arm64_vgicv3 *vgic, struct acrn_vcpu *target_vcpu,
	const struct acrn_mmio_request *mmio, const struct vgicr_frame *frame)
{
	bool flush = false;

	switch (frame->offset) {
	case VGICR_CTLR:
		if (vgic->its_enabled) {
			bool was_enabled = ((vgic->gicr_ctlr[frame->pcpu_id] &
				VGICR_CTLR_ENABLE_LPIS) != 0U);
			bool now_enabled = (((uint32_t)mmio->value &
				VGICR_CTLR_ENABLE_LPIS) != 0U);

			vgic->gicr_ctlr[frame->pcpu_id] =
				(uint32_t)mmio->value & VGICR_CTLR_ENABLE_LPIS;
			if (!was_enabled && now_enabled && (target_vcpu != NULL)) {
				(void)arm64_vgicv3_its_invall_vcpu_locked(target_vcpu->vm,
					vgic, target_vcpu->vcpu_id);
			}
			flush = (target_vcpu != NULL);
		}
		break;
	case VGICR_WAKER:
		vgicr_write_waker(vgic, frame->pcpu_id, (uint32_t)mmio->value);
		break;
	case VGICR_PROPBASER:
		if (vgic->its_enabled) {
			vgic->gicr_propbaser[frame->pcpu_id] =
				(vgic->gicr_propbaser[frame->pcpu_id] & 0xffffffff00000000UL) |
				(mmio->value & UINT32_MAX);
		}
		break;
	case VGICR_PROPBASER + 4U:
		if (vgic->its_enabled) {
			vgic->gicr_propbaser[frame->pcpu_id] =
				(vgic->gicr_propbaser[frame->pcpu_id] & UINT32_MAX) |
				((mmio->value & UINT32_MAX) << 32U);
		}
		break;
	case VGICR_PENDBASER:
		if (vgic->its_enabled) {
			vgic->gicr_pendbaser[frame->pcpu_id] =
				(vgic->gicr_pendbaser[frame->pcpu_id] & 0xffffffff00000000UL) |
				(mmio->value & UINT32_MAX);
		}
		break;
	case VGICR_PENDBASER + 4U:
		if (vgic->its_enabled) {
			vgic->gicr_pendbaser[frame->pcpu_id] =
				(vgic->gicr_pendbaser[frame->pcpu_id] & UINT32_MAX) |
				((mmio->value & UINT32_MAX) << 32U);
		}
		break;
	case VGICR_INVLPIR:
		if (vgic->its_enabled && (target_vcpu != NULL)) {
			flush = arm64_vgicv3_its_inv_lpi_locked(target_vcpu->vm, vgic,
				target_vcpu->vcpu_id, (uint32_t)mmio->value);
		}
		break;
	case VGICR_INVLPIR + 4U:
		break;
	case VGICR_INVALLR:
		if (vgic->its_enabled && (target_vcpu != NULL)) {
			flush = arm64_vgicv3_its_invall_vcpu_locked(target_vcpu->vm,
				vgic, target_vcpu->vcpu_id);
		}
		break;
	case VGICR_INVALLR + 4U:
		break;
	case VGICR_SGI_BASE ... (VGICR_SGI_END - 1U):
		if (target_vcpu != NULL) {
			flush = vgicr_sgi_mmio_write(target_vcpu, mmio, frame->offset - VGICR_SGI_BASE);
		}
		break;
	default:
		break;
	}

	return flush;
}

static int32_t vgicr_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	struct vgicr_frame frame;
	struct acrn_vcpu *target_vcpu;
	uint32_t sgi_offset;
	uint32_t irq_base;
	int32_t ret = 0;

	/*
	 * The redistributor has two access domains: a top-level per-pCPU frame and
	 * an SGI/PPI sub-frame for the vCPU currently assigned to that pCPU slot.
	 * The sub-frame inherits distributor-style priority byte access; other
	 * modeled redistributor registers are 32-bit words.
	 */
	if (!vgicr_decode_frame(vcpu, vgic, mmio->address, &frame)) {
		ret = -EINVAL;
	} else {
		target_vcpu = (frame.vcpu_id == VGIC_INVALID_VCPU_ID) ?
			NULL : vcpu_from_vid(vcpu->vm, frame.vcpu_id);
		if ((frame.offset >= VGICR_SGI_BASE) &&
			vgic_priority_access(frame.offset - VGICR_SGI_BASE, mmio->size)) {
			sgi_offset = frame.offset - VGICR_SGI_BASE;
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			switch (mmio->direction) {
			case ACRN_IOREQ_DIR_READ:
				mmio->value = (target_vcpu != NULL) ?
					vgic_read_irq_priority(target_vcpu, irq_base, mmio->size) : 0UL;
				break;
			case ACRN_IOREQ_DIR_WRITE:
				if (target_vcpu != NULL) {
					vgic_write_irq_priority(target_vcpu, irq_base, mmio->size,
						mmio->value);
					vgicv3_flush_target_vcpu(vcpu, target_vcpu);
				}
				break;
			default:
				ret = -EINVAL;
				break;
			}
		} else if (!vgic_word_access(frame.offset, mmio->size)) {
			ret = -EINVAL;
		} else {
			switch (mmio->direction) {
			case ACRN_IOREQ_DIR_READ:
				vgicr_mmio_read(vgic, target_vcpu, mmio, &frame);
				break;
			case ACRN_IOREQ_DIR_WRITE:
				if (vgicr_mmio_write(vgic, target_vcpu, mmio, &frame)) {
					vgicv3_flush_target_vcpu(vcpu, target_vcpu);
				}
				break;
			default:
				ret = -EINVAL;
				break;
			}
		}
	}

	return ret;
}

static int32_t vgicv3_mmio_dispatch(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vcpu->vm->vm_id)->arch;
	int32_t ret = -ENODEV;

	if (addr_in_range(mmio->address, arch_config->guest_gicd_base,
		arch_config->guest_gicd_size)) {
		ret = vgicd_mmio_access(vcpu, vgic, mmio);
	} else if (addr_in_range(mmio->address, arch_config->guest_gicr_base,
		arch_config->guest_gicr_size)) {
		ret = vgicr_mmio_access(vcpu, vgic, mmio);
	} else if (vgic->its_enabled &&
		addr_in_range(mmio->address, arch_config->guest_its_base,
		arch_config->guest_its_size)) {
		ret = arm64_vgicv3_its_mmio_access(vcpu, vgic, mmio);
	}

	return ret;
}

static int32_t vgicv3_mmio_dispatch64(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vcpu->vm->vm_id)->arch;
	struct acrn_mmio_request low = *mmio;
	struct acrn_mmio_request high = *mmio;
	int32_t ret;

	if (vgic->its_enabled &&
		addr_in_range(mmio->address, arch_config->guest_its_base,
		arch_config->guest_its_size) &&
		arm64_vgicv3_its_mmio_access64(vcpu, vgic, mmio, &ret)) {
		return ret;
	}

	/*
	 * Some guest code issues 64-bit accesses to GIC registers even when the
	 * modeled register block is naturally 32-bit. Split them into two ordered
	 * word accesses so existing register handlers remain simple.
	 */
	low.size = 4UL;
	high.size = 4UL;
	high.address = mmio->address + 4UL;
	if (mmio->direction == ACRN_IOREQ_DIR_WRITE) {
		low.value = mmio->value & 0xffffffffUL;
		high.value = mmio->value >> 32U;
	}

	ret = vgicv3_mmio_dispatch(vcpu, vgic, &low);
	if (ret == 0) {
		ret = vgicv3_mmio_dispatch(vcpu, vgic, &high);
	}
	if ((ret == 0) && (mmio->direction == ACRN_IOREQ_DIR_READ)) {
		mmio->value = (low.value & 0xffffffffUL) | (high.value << 32U);
	}

	return ret;
}

static int32_t vgicv3_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	int32_t ret;

	switch (mmio->size) {
	case 1UL:
	case 2UL:
	case 4UL:
		ret = vgicv3_mmio_dispatch(vcpu, vgic, mmio);
		break;
	case 8UL:
		ret = vgicv3_mmio_dispatch64(vcpu, vgic, mmio);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int32_t arm64_vgicv3_mmio_handler(struct io_request *io_req, void *handler_private_data)
{
	struct arm64_vgicv3 *vgic = (struct arm64_vgicv3 *)handler_private_data;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());
	uint64_t flags;
	int32_t ret = -ENODEV;

	/*
	 * MMIO can race with host-side virtual IRQ injection. The vGIC lock makes
	 * guest register programming, LR readback, and software IRQ descriptors a
	 * single coherent state transition.
	 * GICD/GICR/GITS MMIO changes the guest-visible distributor model, not the
	 * live virtual CPU interface directly; sync first so guest-completed LR
	 * state is not overwritten by stale software descriptors.
	 *
	 *   data abort -> io_request -> vGIC lock -> sync
	 *        -> emulate GICD/GICR/GITS -> flush if LR state changed
	 */
	if ((vcpu != NULL) && (vgic != NULL)) {
		struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
		uint64_t old_lrs[ARM64_VGIC_MAX_LRS];
		uint64_t old_hcr;
		uint8_t old_used_lrs;

		spinlock_irqsave_obtain(&vgic->lock, &flags);
		old_used_lrs = ctx->used_lrs;
		old_hcr = ctx->hcr;
		(void)memcpy(old_lrs, ctx->lr, sizeof(old_lrs));
		vgicv3_sync_vcpu(vcpu, true);
		ret = vgicv3_mmio_access(vcpu, vgic, mmio);
		if (vgicv3_lrs_changed(ctx, old_lrs, old_used_lrs, old_hcr)) {
			vgicv3_flush_vcpu(vcpu, true);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
	}

	return ret;
}
