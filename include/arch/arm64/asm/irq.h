/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_IRQ_H
#define ARM64_IRQ_H

#include <types.h>
#include <cpu.h>

#define IPI_NOTIFY_CPU		0U
#define EXCEPTION_INVALID	0xffffffffffffffffUL

#define ARM64_IRQ_SRC_VIRTUAL_TIMER	0U
#define ARM64_IRQ_SRC_IPI		1U
#define ARM64_IRQ_SRC_EXTERNAL		2U
#define IRQ_NUM_CPU_DOMAIN		16U

#define ARM64_GIC_SGI_SMP_CALL		0U
#define ARM64_GIC_PPI_VGIC_MAINTENANCE	25U
#define ARM64_GIC_PPI_HYPERVISOR_TIMER	26U
#define ARM64_GIC_PPI_PHYSICAL_TIMER	30U
#define ARM64_GIC_PPI_VIRTUAL_TIMER	27U
#define ARM64_GIC_SPURIOUS_INTID	1023U
#define ARM64_GIC_PRIORITY_DEFAULT	0x80U
#define ARM64_GIC_PRIORITY_MASKED	0xffU

/*
 * ARM64 interrupt numbers are sparse in hardware but dense in the common IRQ
 * core. SPI/PPI/SGI INTIDs live below 1024; ITS LPIs begin at 8192. Keep them
 * as separate domains so the common IRQ bitmap does not need an 8K hole.
 */
#define IRQ_NUM_GIC_DOMAIN		1024U
#define IRQ_NUM_GIC_LPI_DOMAIN		1024U
#define ARM64_GIC_FIRST_LPI		8192U
#define NR_IRQS				(IRQ_NUM_CPU_DOMAIN + IRQ_NUM_GIC_DOMAIN + IRQ_NUM_GIC_LPI_DOMAIN)

struct arm64_irq_data {
	uint32_t acrn_irq;
};

struct arm64_gicv3_local_irq_state {
	uint32_t enabled;
	uint32_t pending;
	uint32_t active;
	uint32_t group;
	uint32_t priority;
	bool valid;
};

struct arm64_gicv3_msi_msg {
	uint64_t addr;
	uint32_t data;
};

struct arm64_gicv3_its_stats {
	uint64_t base;
	uint64_t size;
	uint64_t typer;
	uint64_t target;
	uint32_t cmdq_writer;
	uint32_t vector_capacity;
	uint32_t vectors_used;
	uint32_t vectors_programmed;
	uint32_t devices_used;
	uint32_t alloc_msi_ok;
	uint32_t alloc_msi_fail;
	uint32_t alloc_msix_ok;
	uint32_t alloc_msix_fail;
	uint32_t release_msi;
	uint32_t release_msix;
	uint32_t map_event_ok;
	uint32_t map_event_fail;
	uint32_t unmap_event_ok;
	uint32_t unmap_event_fail;
	uint32_t cmd_issued;
	uint32_t cmd_errors;
	uint32_t cmd_timeouts;
	uint32_t cmd_stalls;
	int32_t last_ret;
	bool ready;
};

struct intr_excp_ctx {
	struct cpu_regs regs;
};

#define ARM64_IRQD_CPU		"cpu-intc"
#define ARM64_IRQD_GIC		"gicv3"
#define ARM64_IRQD_GIC_LPI	"gicv3-lpi"

bool arm64_register_irq_domain(const char *name, uint32_t irq_num);
uint32_t arm64_domain_get_acrn_irq(const char *name, uint32_t src_id);
bool arm64_is_valid_acrn_irq(uint32_t irq);
void arm64_gicv3_init_early(void);
void arm64_gicv3_init_its(void);
void arm64_gicv3_log_boot_info(void);
uint64_t arm64_gicv3_redist_base(uint16_t pcpu_id);
void arm64_gicv3_init(uint16_t pcpu_id);
uint32_t arm64_gicv3_ack_irq(void);
void arm64_gicv3_eoi_irq(uint32_t intid);
void arm64_gicv3_enable_irq(uint32_t intid);
void arm64_gicv3_unmask_irq(uint32_t intid);
void arm64_gicv3_disable_irq(uint32_t intid);
void arm64_gicv3_clear_irq(uint32_t intid);
void arm64_gicv3_set_irq_priority(uint32_t intid, uint8_t priority);
bool arm64_gicv3_set_irq_affinity(uint32_t intid, uint16_t pcpu_id);
bool arm64_gicv3_has_its(void);
bool arm64_gicv3_map_spi_msi(uint32_t intid, uint64_t *addr, uint32_t *data);
int32_t arm64_gicv3_its_alloc_msi(uint32_t dev_id, uint32_t count,
	uint32_t *first_lpi, struct arm64_gicv3_msi_msg *msgs);
void arm64_gicv3_its_release_msi(uint32_t dev_id, uint32_t first_lpi,
	uint32_t count);
int32_t arm64_gicv3_its_alloc_msix(uint32_t dev_id, uint32_t vector,
	uint32_t *lpi, struct arm64_gicv3_msi_msg *msg);
void arm64_gicv3_its_release_msix(uint32_t dev_id, uint32_t lpi);
int32_t arm64_gicv3_its_map_msi(uint32_t lpi, struct arm64_gicv3_msi_msg *msg);
int32_t arm64_gicv3_its_map_lpi_event(uint32_t dev_id, uint32_t event_id,
	uint32_t lpi, struct arm64_gicv3_msi_msg *msg);
int32_t arm64_gicv3_its_unmap_lpi_event(uint32_t dev_id, uint32_t event_id,
	uint32_t lpi);
void arm64_gicv3_its_get_stats(struct arm64_gicv3_its_stats *stats);
void arm64_gicv3_set_local_irq_active(uint16_t pcpu_id, uint32_t intid);
void arm64_gicv3_clear_local_irq_active(uint16_t pcpu_id, uint32_t intid);
void arm64_gicv3_get_local_irq_state(uint16_t pcpu_id, uint32_t intid,
	struct arm64_gicv3_local_irq_state *state);
void arm64_gicv3_send_sgi(uint16_t pcpu_id, uint32_t sgi_id);

#endif /* ARM64_IRQ_H */
