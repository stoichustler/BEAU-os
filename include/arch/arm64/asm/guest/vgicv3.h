/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VGICV3_H
#define ARM64_GUEST_VGICV3_H

#include <types.h>
#include <spinlock.h>
#include <asm/irq.h>

#ifndef CONFIG_IRQSTAT_LATENCY
#define CONFIG_IRQSTAT_LATENCY		0
#endif

#define ARM64_VGIC_MAX_LRS		8U
#define ARM64_VGIC_SGI_NUM		16U
#define ARM64_VGIC_PPI_NUM		16U
#define ARM64_VGIC_LOCAL_IRQ_NUM	(ARM64_VGIC_SGI_NUM + ARM64_VGIC_PPI_NUM)
#define ARM64_VGIC_SPI_NUM		(IRQ_NUM_GIC_DOMAIN - ARM64_VGIC_LOCAL_IRQ_NUM)
#define ARM64_VGIC_IRQ_NUM		IRQ_NUM_GIC_DOMAIN
#define ARM64_VGIC_WORDS		(ARM64_VGIC_IRQ_NUM / 32U)
#define ARM64_VGIC_MAX_VCPUS		MAX_PCPU_NUM
#define ARM64_VGIC_LPI_BASE		8192U
#define ARM64_VGIC_LPI_NUM		256U
#define ARM64_VGIC_LPI_IDBITS		14U
#define ARM64_VGIC_LPI_WORDS		(ARM64_VGIC_LPI_NUM / 32U)
#define ARM64_VGIC_ITS_DEVICE_NUM	16U
#define ARM64_VGIC_ITS_EVENT_NUM	32U
#define ARM64_VGIC_ITS_COLLECTION_NUM	ARM64_VGIC_MAX_VCPUS
#define ARM64_VGIC_ITS_BASER_NUM	8U
#define ARM64_VGIC_IRQSTAT_MAX		256U
#define ARM64_VITS_CTLR_ENABLE		(1U << 0U)
#define ARM64_VITS_CBASER_VALID		(1UL << 63U)

#define ARM64_VGIC_MAINTENANCE_INTID	ARM64_GIC_PPI_VGIC_MAINTENANCE

#define ARM64_VGIC_SYSREG_ICC_PMR_EL1		0U
#define ARM64_VGIC_SYSREG_ICC_CTLR_EL1		1U
#define ARM64_VGIC_SYSREG_ICC_SRE_EL1		2U
#define ARM64_VGIC_SYSREG_ICC_IGRPEN1_EL1	3U
#define ARM64_VGIC_SYSREG_ICC_DIR_EL1		4U
#define ARM64_VGIC_SYSREG_ICC_RPR_EL1		5U

struct acrn_vm;
struct acrn_vcpu;
struct io_request;

/*
 * Latency accumulator exported by irqstat. Values are converted to
 * microseconds at snapshot time so the hot vGIC path can keep raw ticks.
 */
struct arm64_vgic_irq_latency_stats {
	uint64_t count;		/* Number of samples included in this range. */
	uint64_t min_us;	/* Minimum observed latency in microseconds. */
	uint64_t avg_us;	/* Average observed latency in microseconds. */
	uint64_t max_us;	/* Maximum observed latency in microseconds. */
};

/*
 * Guest-visible virtual IRQ lifecycle counters. irqstat consumes this sparse
 * snapshot; one entry is created only after a VM/target-vCPU/INTID actually
 * sees activity.
 */
struct arm64_vgic_irq_stats {
	uint16_t vm_id;		/* VM that owns the virtual interrupt. */
	uint16_t vcpu_id;	/* Target virtual CPU for this interrupt. */
	uint32_t virq;		/* Guest-visible GIC INTID. */
	bool level;		/* True for level-triggered source semantics. */
	bool in_flight;		/* True while an assert is waiting for completion. */
	uint64_t assert_count;	/* Source asserted or injected the vIRQ. */
	uint64_t deassert_count;	/* Source lowered a level-triggered vIRQ. */
	uint64_t lr_count;	/* vGIC published the vIRQ into an LR. */
	uint64_t eoi_count;	/* Guest completion observed by sync/EOI/DIR. */
	struct arm64_vgic_irq_latency_stats raise_to_lr;
	struct arm64_vgic_irq_latency_stats lr_to_eoi;
};

/*
 * Per-vCPU hardware virtualization context. List registers are the hardware
 * slots used by GICv3 to present pending/active virtual interrupts to EL1; the
 * AP/VMCR/SRE/PMR fields mirror the EL1-visible interrupt-controller state.
 */
struct arm64_vgicv3_vcpu_ctx {
	uint64_t lr[ARM64_VGIC_MAX_LRS];
	uint64_t ap0r0;
	uint64_t ap1r0;
	uint64_t vmcr;
	uint64_t hcr;
	uint64_t sre;
	uint64_t pmr;
	uint64_t ctlr;
	uint8_t used_lrs;
};

/*
 * Software model for a virtual INTID as observed by one target vCPU. The model
 * is intentionally per-vCPU so SGIs/PPIs are naturally private, while the
 * current SPI implementation can still target a selected vCPU during bring-up.
 */
struct arm64_vgic_irq {
	uint16_t virq;
	uint16_t pirq;
	uint8_t priority;
	uint8_t target_vcpu;
	bool enabled;
	bool pending;
	bool active;
	bool level;
	bool group1;
	bool groupmod;
	bool hw;
};

struct arm64_vits_collection {
	uint16_t collection_id;
	uint16_t target_vcpu;
	bool valid;
};

struct arm64_vits_device {
	uint32_t device_id;
	uint64_t itt_addr;
	uint16_t event_count;
	bool valid;
};

struct arm64_vits_event {
	uint32_t device_id;
	uint32_t event_id;
	uint32_t lpi;
	uint16_t collection_id;
	bool valid;
};

struct arm64_vits_stats {
	uint64_t cbaser;
	uint64_t cwriter;
	uint64_t creadr;
	uint64_t cmd_processed;
	uint64_t cmd_invalid;
	uint64_t cmd_unsupported;
	uint64_t cmd_queue_errors;
	uint64_t cmd_copy_fail;
	uint64_t cmd_budget_exhausted;
	uint64_t mmio_read;
	uint64_t mmio_write;
	uint64_t translater_write;
	uint64_t inject_ok;
	uint64_t inject_fail;
	uint64_t inject_no_event;
	uint64_t inject_bad_target;
	uint64_t config_update_ok;
	uint64_t config_update_fail;
	uint32_t devices;
	uint32_t events;
	uint32_t collections;
	uint32_t last_opcode;
	uint32_t last_device;
	uint32_t last_event;
	uint32_t last_lpi;
	uint16_t last_collection;
	uint16_t last_target;
	int32_t last_status;
	bool cbaser_valid;
	bool ctlr_enabled;
};

struct arm64_vits {
	uint32_t ctlr;
	uint64_t typer;
	uint64_t cbaser;
	uint64_t cwriter;
	uint64_t creadr;
	uint64_t baser[ARM64_VGIC_ITS_BASER_NUM];
	struct arm64_vits_collection collection[ARM64_VGIC_ITS_COLLECTION_NUM];
	struct arm64_vits_device device[ARM64_VGIC_ITS_DEVICE_NUM];
	struct arm64_vits_event event[ARM64_VGIC_ITS_DEVICE_NUM][ARM64_VGIC_ITS_EVENT_NUM];
	struct arm64_vits_stats stats;
};

/*
 * Per-VM vGIC state. Guest MMIO updates this model, physical events mark IRQs
 * pending here, and flush/sync operations reconcile it with the hardware list
 * registers of the running vCPU.
 */
struct arm64_vgicv3 {
	spinlock_t lock;
	bool initialized;
	bool its_enabled;
	uint16_t vcpu_count;
	uint16_t rdist_count;
	uint32_t lr_count;
	uint32_t vmcr;
	uint32_t gicd_ctlr;
	uint32_t gicd_typer;
	uint32_t gicd_pidr2;
	uint16_t rdist_vcpu[ARM64_VGIC_MAX_VCPUS];
	uint32_t gicr_waker[ARM64_VGIC_MAX_VCPUS];
	uint64_t spi_router[ARM64_VGIC_SPI_NUM];
	uint32_t pending_bitmap[ARM64_VGIC_MAX_VCPUS][ARM64_VGIC_WORDS];
	uint32_t lpi_pending_bitmap[ARM64_VGIC_MAX_VCPUS][ARM64_VGIC_LPI_WORDS];
	struct arm64_vgic_irq irq[ARM64_VGIC_MAX_VCPUS][ARM64_VGIC_IRQ_NUM];
	struct arm64_vgic_irq lpi[ARM64_VGIC_MAX_VCPUS][ARM64_VGIC_LPI_NUM];
	uint32_t gicr_ctlr[ARM64_VGIC_MAX_VCPUS];
	uint64_t gicr_propbaser[ARM64_VGIC_MAX_VCPUS];
	uint64_t gicr_pendbaser[ARM64_VGIC_MAX_VCPUS];
	struct arm64_vits its;
};

void arm64_vgicv3_global_init(void);
void arm64_vgicv3_init_vm(struct acrn_vm *vm, uint64_t cpu_affinity);
void arm64_vgicv3_init_vcpu(struct acrn_vcpu *vcpu);
void arm64_vgicv3_reset_vcpu(struct acrn_vcpu *vcpu);
void arm64_vgicv3_reset_vcpu_boot_state(struct acrn_vcpu *vcpu);
void arm64_vtimer_init_vcpu(struct acrn_vcpu *vcpu);
void arm64_vtimer_load_current(struct acrn_vcpu *vcpu);
void arm64_vtimer_save_current(struct acrn_vcpu *vcpu);
void arm64_vtimer_disable_current(void);
void arm64_vtimer_trace_switch(struct acrn_vcpu *vcpu, uint32_t event);
void arm64_vtimer_set_host_mask(struct acrn_vcpu *vcpu, bool masked);
bool arm64_vtimer_sample_current(struct acrn_vcpu *vcpu);
bool arm64_vtimer_guest_expired(const struct acrn_vcpu *vcpu);
bool arm64_vtimer_sysreg(uint64_t sysreg);
int32_t arm64_vtimer_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg);
void arm64_vgicv3_arm_cntv_timer(struct acrn_vcpu *vcpu);
void arm64_vgicv3_cancel_cntv_timer(struct acrn_vcpu *vcpu);
void arm64_vtimer_cancel_all(struct acrn_vcpu *vcpu);
void arm64_vgicv3_load_vcpu(struct acrn_vcpu *vcpu);
void arm64_vgicv3_save_vcpu(struct acrn_vcpu *vcpu);
int32_t arm64_vgicv3_inject_irq(struct acrn_vcpu *vcpu, uint32_t virq, bool level);
int32_t arm64_vgicv3_inject_msi(struct acrn_vm *vm, uint32_t device_id, uint32_t event_id);
bool arm64_vgicv3_get_its_stats(struct acrn_vm *vm, struct arm64_vits_stats *stats);
int32_t arm64_vgicv3_clear_irq(struct acrn_vcpu *vcpu, uint32_t virq);
int32_t arm64_vgicv3_deassert_irq(struct acrn_vcpu *vcpu, uint32_t virq);
int32_t arm64_vgicv3_handle_cpuif_sysreg(struct acrn_vcpu *vcpu, uint32_t sysreg,
	bool read, uint64_t *reg);
int32_t arm64_vgicv3_handle_sgi1r(struct acrn_vcpu *vcpu, uint64_t value);
void arm64_vgicv3_flush_vcpu(struct acrn_vcpu *vcpu);
void arm64_vgicv3_sync_vcpu(struct acrn_vcpu *vcpu);
void arm64_vgicv3_flush_current_vcpu(struct acrn_vcpu *vcpu);
void arm64_vgicv3_flush_current_vcpu_with_lock(struct acrn_vcpu *vcpu);
void arm64_vgicv3_sync_current_vcpu(struct acrn_vcpu *vcpu);
int32_t arm64_vgicv3_suspend_vm(struct acrn_vm *vm, uint64_t epoch);
int32_t arm64_vgicv3_resume_vm(struct acrn_vm *vm, uint64_t epoch);
void arm64_vgicv3_refresh_current_level_irq(struct acrn_vm *vm, uint32_t virq);
void arm64_vgicv3_complete_wfi_irqs(struct acrn_vcpu *vcpu);
bool arm64_vgicv3_has_pending_irq(struct acrn_vcpu *vcpu);
bool arm64_vgicv3_pending_irq_blocks_reschedule(struct acrn_vcpu *vcpu);
void arm64_vgicv3_poll_current_vtimer(struct acrn_vcpu *vcpu);
void arm64_vgicv3_update_current_vtimer(struct acrn_vcpu *vcpu);
void arm64_vgicv3_sync_current_timer_line(struct acrn_vcpu *vcpu, bool level);
void arm64_vgicv3_maintenance_irq_handler(uint32_t irq, void *data);
void arm64_vgicv3_virtual_timer_irq_handler(uint32_t irq, void *data);
int32_t arm64_vgicv3_mmio_handler(struct io_request *io_req, void *handler_private_data);
uint16_t arm64_vgicv3_get_irq_stats(struct arm64_vgic_irq_stats *stats, uint16_t max);

#endif /* ARM64_GUEST_VGICV3_H */
