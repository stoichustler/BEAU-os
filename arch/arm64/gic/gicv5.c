/*-
 * Copyright (c) 2015-2016 The FreeBSD Foundation
 * Copyright (c) 2025 Arm Ltd
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
#include <per_cpu.h>
#include <logmsg.h>
#include <rtl.h>
#include <barrier.h>
#include <asm/irq.h>
#include <asm/platform.h>
#include <asm/sysreg.h>

#include "gicv5_reg.h"
#include "gicv5_var.h"

#define	GICV5_PPI_COUNT		128U
#define	GICV5_PPI_REG_BITS	64U
#define	GICV5_LPI_BASE		GICV5_LPI_ID_BASE
#define	GICV5_LPI_IPI_BASE	0U

static uint16_t gicv5_iaffids[MAX_PCPU_NUM];
static bool gicv5_initialized;

static inline uint64_t gicv5_sysl(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)
{
	uint64_t val = 0UL;

	if ((op1 == GICR_CDIA_op1) && (crn == GICR_CDIA_CRn) &&
		(crm == GICR_CDIA_CRm) && (op2 == GICR_CDIA_op2)) {
		val = arm64_sysl_read(0, 12, 3, 0);
	}

	return val;
}

static inline void gicv5_sys(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)
{
	if ((op1 == GIC_CDEOI_op1) && (crn == GIC_CDEOI_CRn) &&
		(crm == GIC_CDEOI_CRm) && (op2 == GIC_CDEOI_op2)) {
		arm64_sys(0, 12, 1, 7);
	}
}

static inline void gicv5_sys_arg(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2,
	uint64_t val)
{
	if ((op1 == GIC_CDDI_op1) && (crn == GIC_CDDI_CRn) &&
		(crm == GIC_CDDI_CRm) && (op2 == GIC_CDDI_op2)) {
		arm64_sys_write(0, 12, 2, 0, val);
	} else if ((op1 == GIC_CDDIS_op1) && (crn == GIC_CDDIS_CRn) &&
		(crm == GIC_CDDIS_CRm) && (op2 == GIC_CDDIS_op2)) {
		arm64_sys_write(0, 12, 1, 0, val);
	} else if ((op1 == GIC_CDEN_op1) && (crn == GIC_CDEN_CRn) &&
		(crm == GIC_CDEN_CRm) && (op2 == GIC_CDEN_op2)) {
		arm64_sys_write(0, 12, 1, 1, val);
	} else if ((op1 == GIC_CDPRI_op1) && (crn == GIC_CDPRI_CRn) &&
		(crm == GIC_CDPRI_CRm) && (op2 == GIC_CDPRI_op2)) {
		arm64_sys_write(0, 12, 1, 2, val);
	} else if ((op1 == GIC_CDAFF_op1) && (crn == GIC_CDAFF_CRn) &&
		(crm == GIC_CDAFF_CRm) && (op2 == GIC_CDAFF_op2)) {
		arm64_sys_write(0, 12, 1, 3, val);
	} else if ((op1 == GIC_CDPEND_op1) && (crn == GIC_CDPEND_CRn) &&
		(crm == GIC_CDPEND_CRm) && (op2 == GIC_CDPEND_op2)) {
		arm64_sys_write(0, 12, 1, 4, val);
	}
}

static inline void gicv5_gsb_ack(void)
{
	arm64_sys(0, 12, 0, 1);
}

static inline void gicv5_gsb_sys(void)
{
	arm64_sys(0, 12, 0, 0);
}

static inline uint64_t gicv5_read_icc_iaffidr_el1(void)
{
	uint64_t val;

	val = arm64_sysreg_read(s3_0_c12_c10_5);
	return val;
}

static inline void gicv5_write_icc_cr0_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_1_c12_c0_1, val);
}

static inline void gicv5_write_icc_pcr_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_1_c12_c0_2, val);
}

static inline void gicv5_write_icc_ppi_enabler0_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c10_6, val);
}

static inline void gicv5_write_icc_ppi_enabler1_el1(uint64_t val)
{
	arm64_sysreg_write_sync(s3_0_c12_c10_7, val);
}

static inline void gicv5_write_icc_ppi_priorityr(uint32_t reg, uint64_t val)
{
	switch (reg) {
	case 0U:
		arm64_sysreg_write(s3_0_c12_c14_0, val);
		break;
	case 1U:
		arm64_sysreg_write(s3_0_c12_c14_1, val);
		break;
	case 2U:
		arm64_sysreg_write(s3_0_c12_c14_2, val);
		break;
	case 3U:
		arm64_sysreg_write(s3_0_c12_c14_3, val);
		break;
	case 4U:
		arm64_sysreg_write(s3_0_c12_c14_4, val);
		break;
	case 5U:
		arm64_sysreg_write(s3_0_c12_c14_5, val);
		break;
	case 6U:
		arm64_sysreg_write(s3_0_c12_c14_6, val);
		break;
	case 7U:
		arm64_sysreg_write(s3_0_c12_c14_7, val);
		break;
	case 8U:
		arm64_sysreg_write(s3_0_c12_c15_0, val);
		break;
	case 9U:
		arm64_sysreg_write(s3_0_c12_c15_1, val);
		break;
	case 10U:
		arm64_sysreg_write(s3_0_c12_c15_2, val);
		break;
	case 11U:
		arm64_sysreg_write(s3_0_c12_c15_3, val);
		break;
	case 12U:
		arm64_sysreg_write(s3_0_c12_c15_4, val);
		break;
	case 13U:
		arm64_sysreg_write(s3_0_c12_c15_5, val);
		break;
	case 14U:
		arm64_sysreg_write(s3_0_c12_c15_6, val);
		break;
	case 15U:
		arm64_sysreg_write(s3_0_c12_c15_7, val);
		break;
	default:
		break;
	}
}

static bool gicv5_is_lpi(uint32_t intid)
{
	return intid >= GICV5_LPI_BASE;
}

static bool gicv5_is_spi(uint32_t intid)
{
	return (intid >= GICV5_PPI_COUNT) && !gicv5_is_lpi(intid);
}

static uint32_t gicv5_hw_irq(uint32_t intid)
{
	if (gicv5_is_lpi(intid)) {
		return intid - GICV5_LPI_BASE;
	}
	if (gicv5_is_spi(intid)) {
		return intid - GICV5_PPI_COUNT;
	}

	return intid;
}

static void gicv5_set_intid_priority(uint32_t intid, uint8_t priority)
{
	uint32_t type;
	uint64_t cmd;

	if (intid < GICV5_PPI_COUNT) {
		uint32_t reg = intid / 8U;
		uint32_t shift = (intid % 8U) * 8U;
		uint64_t val = ((uint64_t)((priority >> 3U) & GICV5_PRI_LOWEST)) << shift;

		gicv5_write_icc_ppi_priorityr(reg, val);
		gicv5_gsb_sys();
		return;
	}

	type = gicv5_is_lpi(intid) ? GIC_CDPRI_TYPE_LPI : GIC_CDPRI_TYPE_SPI;
	cmd = GIC_CDPRI_PRORITY((priority >> 3U) & GICV5_PRI_LOWEST) |
		type | GIC_CDPRI_ID(gicv5_hw_irq(intid));

	gicv5_sys_arg(GIC_CDPRI_op1, GIC_CDPRI_CRn, GIC_CDPRI_CRm, GIC_CDPRI_op2, cmd);
	gicv5_gsb_sys();
}

static void gicv5_set_intid_affinity(uint32_t intid, uint16_t pcpu_id)
{
	uint32_t type;
	uint64_t cmd;

	if ((intid < GICV5_PPI_COUNT) || (pcpu_id >= MAX_PCPU_NUM)) {
		return;
	}

	type = gicv5_is_lpi(intid) ? GIC_CDAFF_TYPE_LPI : GIC_CDAFF_TYPE_SPI;
	cmd = GIC_CDAFF_IAFFID(gicv5_iaffids[pcpu_id]) | type |
		GIC_CDAFF_IRM_TARGETED | GIC_CDAFF_ID(gicv5_hw_irq(intid));
	gicv5_sys_arg(GIC_CDAFF_op1, GIC_CDAFF_CRn, GIC_CDAFF_CRm, GIC_CDAFF_op2, cmd);
	gicv5_gsb_sys();
}

static void gicv5_cpu_init(void)
{
	uint64_t priority = ICC_PPI_PRIORITYR_PRIORITY_ALL(GICV5_PRI_LOWEST);
	uint32_t i;

	gicv5_iaffids[get_pcpu_id()] =
		(uint16_t)ICC_IAFFIDR_IAFFID_VAL(gicv5_read_icc_iaffidr_el1());

	for (i = 0U; i < 16U; i++) {
		gicv5_write_icc_ppi_priorityr(i, priority);
	}

	gicv5_write_icc_ppi_enabler0_el1(ICC_PPI_ENABLER_NONE);
	gicv5_write_icc_ppi_enabler1_el1(ICC_PPI_ENABLER_NONE);
	gicv5_write_icc_pcr_el1(ICC_PCR_PRIORITY_LOWEST);
	gicv5_write_icc_cr0_el1(ICC_CR0_EN);
	gicv5_gsb_sys();
}

void arm64_gicv3_init_early(void)
{
	uint32_t i;

	if (gicv5_initialized) {
		return;
	}

	for (i = 0U; i < MAX_PCPU_NUM; i++) {
		gicv5_iaffids[i] = 0U;
	}
	gicv5_cpu_init();
	beau_gicv5_its_init(beau_config.gits_base, beau_config.gits_size);
	beau_gicv5_iwb_init(0UL, 0UL);
	gicv5_initialized = true;
}

void arm64_gicv3_init(uint16_t pcpu_id)
{
	if (pcpu_id == BSP_CPU_ID) {
		arm64_gicv3_init_early();
	} else {
		gicv5_cpu_init();
	}

	arm64_gicv3_enable_irq(ARM64_GIC_SGI_SMP_CALL);
	arm64_gicv3_enable_irq(ARM64_GIC_PPI_VGIC_MAINTENANCE);
	arm64_gicv3_enable_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
}

uint32_t arm64_gicv3_ack_irq(void)
{
	uint64_t hppi = gicv5_sysl(GICR_CDIA_op1, GICR_CDIA_CRn, GICR_CDIA_CRm,
		GICR_CDIA_op2);
	uint32_t type;
	uint32_t irq;

	gicv5_gsb_ack();

	if ((hppi & ICC_HPPIR_HPPIV) == 0UL) {
		return ARM64_GIC_SPURIOUS_INTID;
	}

	type = (uint32_t)(hppi & ICC_HPPIR_TYPE_MASK);
	irq = (uint32_t)((hppi & ICC_HPPIR_ID_MASK) >> ICC_HPPIR_ID_SHIFT);

	if (type == ICC_HPPIR_TYPE_PPI) {
		return irq;
	}
	if (type == ICC_HPPIR_TYPE_SPI) {
		return irq + GICV5_PPI_COUNT;
	}
	if (type == ICC_HPPIR_TYPE_LPI) {
		return irq + GICV5_LPI_BASE;
	}

	return ARM64_GIC_SPURIOUS_INTID;
}

void arm64_gicv3_eoi_irq(uint32_t intid)
{
	uint32_t type;
	uint64_t cmd;

	if (intid < GICV5_PPI_COUNT) {
		type = GIC_CDDI_Type_PPI;
	} else if (gicv5_is_lpi(intid)) {
		type = GIC_CDDI_Type_LPI;
	} else {
		type = GIC_CDDI_Type_SPI;
	}
	cmd = (uint64_t)type | (uint64_t)gicv5_hw_irq(intid);

	cpu_memory_barrier();
	gicv5_sys_arg(GIC_CDDI_op1, GIC_CDDI_CRn, GIC_CDDI_CRm, GIC_CDDI_op2, cmd);
	gicv5_sys(GIC_CDEOI_op1, GIC_CDEOI_CRn, GIC_CDEOI_CRm, GIC_CDEOI_op2);
}

void arm64_gicv3_enable_irq(uint32_t intid)
{
	uint32_t type;
	uint64_t cmd;

	if (intid < GICV5_PPI_COUNT) {
		uint64_t reg = ICC_PPI_ENABLER_EN(intid);

		if (intid < GICV5_PPI_REG_BITS) {
			gicv5_write_icc_ppi_enabler0_el1(reg);
		} else {
			gicv5_write_icc_ppi_enabler1_el1(reg);
		}
	} else {
		type = gicv5_is_lpi(intid) ? GIC_CDEN_TYPE_LPI : GIC_CDEN_TYPE_SPI;
		cmd = (uint64_t)type | (uint64_t)gicv5_hw_irq(intid);
		gicv5_sys_arg(GIC_CDEN_op1, GIC_CDEN_CRn, GIC_CDEN_CRm, GIC_CDEN_op2, cmd);
	}
	gicv5_gsb_sys();
}

void arm64_gicv3_unmask_irq(uint32_t intid)
{
	arm64_gicv3_enable_irq(intid);
}

void arm64_gicv3_disable_irq(uint32_t intid)
{
	uint32_t type;
	uint64_t cmd;

	if (intid < GICV5_PPI_COUNT) {
		uint64_t reg = ICC_PPI_ENABLER_DIS(intid);

		if (intid < GICV5_PPI_REG_BITS) {
			gicv5_write_icc_ppi_enabler0_el1(reg);
		} else {
			gicv5_write_icc_ppi_enabler1_el1(reg);
		}
	} else {
		type = gicv5_is_lpi(intid) ? GIC_CDDIS_TYPE_LPI : GIC_CDDIS_TYPE_SPI;
		cmd = (uint64_t)type | (uint64_t)gicv5_hw_irq(intid);
		gicv5_sys_arg(GIC_CDDIS_op1, GIC_CDDIS_CRn, GIC_CDDIS_CRm, GIC_CDDIS_op2, cmd);
	}
	gicv5_gsb_sys();
}

void arm64_gicv3_clear_irq(uint32_t intid)
{
	uint32_t type;
	uint32_t irq;
	uint64_t cmd;

	if (intid < GICV5_PPI_COUNT) {
		return;
	}

	type = gicv5_is_lpi(intid) ? GIC_CDPEND_TYPE_LPI : GIC_CDPEND_TYPE_SPI;
	irq = gicv5_hw_irq(intid);
	cmd = (uint64_t)type | GIC_CDPEND_PENDING_CLEAR | GIC_CDPEND_ID(irq);
	gicv5_sys_arg(GIC_CDPEND_op1, GIC_CDPEND_CRn, GIC_CDPEND_CRm, GIC_CDPEND_op2, cmd);
	gicv5_gsb_sys();
}

void arm64_gicv3_set_irq_priority(uint32_t intid, uint8_t priority)
{
	gicv5_set_intid_priority(intid, priority);
}

bool arm64_gicv3_has_its(void)
{
	return beau_gicv5_its_present();
}

bool arm64_gicv3_map_spi_msi(uint32_t intid, uint64_t *addr, uint32_t *data)
{
	if ((addr == NULL) || (data == NULL) || (intid < GICV5_PPI_COUNT) ||
		(intid >= GICV5_LPI_BASE)) {
		return false;
	}

	*addr = 0UL;
	*data = gicv5_hw_irq(intid);
	return true;
}

void arm64_gicv3_set_local_irq_active(uint16_t pcpu_id __unused, uint32_t intid __unused)
{
}

void arm64_gicv3_clear_local_irq_active(uint16_t pcpu_id __unused, uint32_t intid __unused)
{
}

void arm64_gicv3_get_local_irq_state(uint16_t pcpu_id __unused, uint32_t intid __unused,
	struct arm64_gicv3_local_irq_state *state)
{
	if (state != NULL) {
		(void)memset(state, 0U, sizeof(*state));
	}
}

void arm64_gicv3_send_sgi(uint16_t pcpu_id __unused, uint32_t sgi_id __unused)
{
	uint32_t irq;
	uint64_t cmd;

	if ((pcpu_id >= MAX_PCPU_NUM) || (sgi_id != ARM64_GIC_SGI_SMP_CALL)) {
		return;
	}

	irq = GICV5_LPI_IPI_BASE + pcpu_id;
	gicv5_set_intid_priority(GICV5_LPI_BASE + irq, ARM64_GIC_PRIORITY_DEFAULT);
	gicv5_set_intid_affinity(GICV5_LPI_BASE + irq, pcpu_id);
	cmd = GIC_CDPEND_TYPE_LPI | GIC_CDPEND_PENDING_SET | GIC_CDPEND_ID(irq);
	cpu_memory_barrier();
	gicv5_sys_arg(GIC_CDPEND_op1, GIC_CDPEND_CRn, GIC_CDPEND_CRm, GIC_CDPEND_op2, cmd);
	gicv5_gsb_sys();
}
