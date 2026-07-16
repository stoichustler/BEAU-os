/*-
 * Copyright (c) 2015-2016 The FreeBSD Foundation
 * Copyright (c) 2026 Hustler Lo
 *
 * This software was developed by Andrew Turner under
 * the sponsorship of the FreeBSD Foundation.
 *
 * This software was developed by Semihalf under
 * the sponsorship of the FreeBSD Foundation.
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
#include <errno.h>
#include <cpu.h>
#include <per_cpu.h>
#include <io.h>
#include <logmsg.h>
#include <rtl.h>
#include <barrier.h>
#include <asm/irq.h>
#include <asm/platform.h>
#include <asm/sysreg.h>

/* [20260714] Physical GICv3 host interrupt framework
 *
 *   platform.dts
 *       |
 *       v
 *   beau_config.{gicd,gicr,gits}
 *       |
 *       +--> boot pCPU: Distributor, Redistributor discovery, global SPI policy
 *       +--> each pCPU: local Redistributor and ICC_* CPU interface
 *       |
 *       v
 *   hardware INTID -> ARM64 IRQ domain -> common IRQ handler -> GIC EOI
 *
 *   physical GICv3 (this file)          guest interrupt model
 *       owns EL2 host IRQ delivery  ->   arch/arm64/guest/vgicv3*.c
 *       programs GICD/GICR/ICC_*         owns guest-visible IRQ/LR state
 *
 * Key rule:
 *   - accept only INTIDs inside the discovered hardware range;
 *   - program SPIs through the Distributor and SGIs/PPIs through the current
 *     pCPU Redistributor;
 *   - keep physical GIC state separate from guest vGIC state so host IRQ
 *     routing and guest injection cannot silently share ownership.
 */

#ifndef PAGE_SIZE_64K
#define	PAGE_SIZE_64K		0x10000UL
#endif

#include "gicv3_reg.h"

#define	GIC_FIRST_SGI		0U
#define	GIC_LAST_SGI		15U
#define	GIC_FIRST_PPI		16U
#define	GIC_LAST_PPI		31U
#define	GIC_FIRST_SPI		32U

#define	GICD_CTLR		0x0000U
#define	GICD_TYPER		0x0004U
#define	GICD_I_PER_ISENABLERn	32U
#define	GICD_I_PER_ICFGRn	16U
#define	GICD_IPRIORITYR_BASE	0x0400U

#define	GICD_IGROUPR(n)		(0x0080U + (((n) / 32U) * 4U))
#define	GICD_ISENABLER(n)	(0x0100U + (((n) / 32U) * 4U))
#define	GICD_ICENABLER(n)	(0x0180U + (((n) / 32U) * 4U))
#define	GICD_ISPENDR(n)		(0x0200U + (((n) / 32U) * 4U))
#define	GICD_ICPENDR(n)		(0x0280U + (((n) / 32U) * 4U))
#define	GICD_ISACTIVER(n)	(0x0300U + (((n) / 32U) * 4U))
#define	GICD_ICACTIVER(n)	(0x0380U + (((n) / 32U) * 4U))
#define	GICD_ICFGR(n)		(0x0c00U + (((n) / 16U) * 4U))
#define	GICD_I_MASK(n)		(1U << ((n) % 32U))
#define	GICD_TYPER_I_NUM(n)	((uint32_t)((((n) & 0x1fU) + 1U) * 32U))

#define	GIC_SPECIAL_INTID_BASE	1020U
#define	GIC_DEFAULT_PRIORITY	ARM64_GIC_PRIORITY_DEFAULT
#define	GIC_LOWEST_PRIORITY	ARM64_GIC_PRIORITY_MASKED
#define	GIC_WAIT_RETRIES	1000000U

void beau_gicv3_its_init(uint64_t base, uint64_t size);
bool beau_gicv3_its_present(void);
int32_t arm64_gicv3_its_pm_suspend(uint64_t epoch);
int32_t arm64_gicv3_its_pm_resume(uint64_t epoch);

struct beau_gic_v3_softc {
	uint64_t	gic_dist;
	uint64_t	gic_dist_size;
	uint64_t	gic_redist;
	uint64_t	gic_redist_stride;
	uint64_t	gic_redist_size;
	uint64_t	gic_its;
	uint64_t	gic_its_size;
	uint64_t	gic_redist_bases[MAX_PCPU_NUM];
	uint32_t	gic_nirqs;
	bool		gic_initialized;
};

typedef int32_t (*gic_v3_initseq_t)(struct beau_gic_v3_softc *sc);

#define GIC_PM_DIST_WORDS	((IRQ_NUM_GIC_DOMAIN + 31U) / 32U)
#define GIC_PM_CFG_WORDS	((IRQ_NUM_GIC_DOMAIN + 15U) / 16U)

struct arm64_gicv3_pm_cpu_state {
	uint64_t suspend_epoch;
	uint64_t sre;
	uint64_t pmr;
	uint64_t bpr1;
	uint64_t ctlr;
	uint64_t igrpen1;
	bool valid;
};

struct arm64_gicv3_pm_redist_state {
	uint32_t group;
	uint32_t enabled;
	uint32_t pending;
	uint32_t active;
	uint32_t config[2U];
	uint8_t priority[32U];
};

struct arm64_gicv3_pm_state {
	uint64_t suspend_epoch;
	uint64_t route[IRQ_NUM_GIC_DOMAIN - GIC_FIRST_SPI];
	uint32_t group[GIC_PM_DIST_WORDS];
	uint32_t enabled[GIC_PM_DIST_WORDS];
	uint32_t pending[GIC_PM_DIST_WORDS];
	uint32_t active[GIC_PM_DIST_WORDS];
	uint32_t config[GIC_PM_CFG_WORDS];
	uint8_t priority[IRQ_NUM_GIC_DOMAIN];
	struct arm64_gicv3_pm_redist_state redist[MAX_PCPU_NUM];
	uint32_t ctlr;
	bool valid;
};

static struct arm64_gicv3_pm_cpu_state gic_pm_cpu[MAX_PCPU_NUM];
static struct arm64_gicv3_pm_state gic_pm_state;

static struct beau_gic_v3_softc gic_v3_sc;

static uint64_t gic_range_end(uint64_t base, uint64_t size)
{
	if (size == 0UL) {
		return base;
	}
	if ((size - 1UL) > (UINT64_MAX - base)) {
		return UINT64_MAX;
	}
	return base + size - 1UL;
}

static inline void *gic_mmio(uint64_t base, uint32_t off)
{
	return (void *)(base + off);
}

static inline uint32_t gic_d_read_4(const struct beau_gic_v3_softc *sc, uint32_t off)
{
	return mmio_read32(gic_mmio(sc->gic_dist, off));
}

static inline uint64_t gic_d_read_8(const struct beau_gic_v3_softc *sc, uint32_t off)
{
	return mmio_read64(gic_mmio(sc->gic_dist, off));
}

static inline void gic_d_write_4(const struct beau_gic_v3_softc *sc, uint32_t off, uint32_t val)
{
	mmio_write32(val, gic_mmio(sc->gic_dist, off));
}

static inline void gic_d_write_8(const struct beau_gic_v3_softc *sc, uint32_t off, uint64_t val)
{
	mmio_write64(val, gic_mmio(sc->gic_dist, off));
}

static inline uint32_t gic_r_read_4(const struct beau_gic_v3_softc *sc, uint16_t pcpu_id,
	uint32_t off)
{
	return mmio_read32(gic_mmio(sc->gic_redist_bases[pcpu_id], off));
}

static inline uint64_t gic_r_read_8(const struct beau_gic_v3_softc *sc, uint16_t pcpu_id,
	uint32_t off)
{
	return mmio_read64(gic_mmio(sc->gic_redist_bases[pcpu_id], off));
}

static inline void gic_r_write_4(const struct beau_gic_v3_softc *sc, uint16_t pcpu_id,
	uint32_t off, uint32_t val)
{
	mmio_write32(val, gic_mmio(sc->gic_redist_bases[pcpu_id], off));
}

static inline uint32_t gic_v3_intid_reg(uint32_t intid)
{
	return (intid / GICD_I_PER_ISENABLERn) * 4U;
}

static inline uint32_t gic_v3_intid_mask(uint32_t intid)
{
	return GICD_I_MASK(intid);
}

static uint32_t gic_v3_intid_mask_until(uint32_t base_intid, uint32_t limit_intid)
{
	uint32_t count = limit_intid - base_intid;

	return (count >= GICD_I_PER_ISENABLERn) ? UINT32_MAX : ((1U << count) - 1U);
}

static uint32_t gic_v3_icfgr_mask_until(uint32_t base_intid, uint32_t limit_intid)
{
	uint32_t count = limit_intid - base_intid;

	return (count >= GICD_I_PER_ICFGRn) ? UINT32_MAX : ((1U << (count * 2U)) - 1U);
}

static bool gic_v3_is_programmable_spi(const struct beau_gic_v3_softc *sc, uint32_t intid)
{
	return (intid >= GIC_FIRST_SPI) && (intid < sc->gic_nirqs) &&
		(intid < GIC_SPECIAL_INTID_BASE);
}

static uint64_t gic_v3_mpidr_to_affinity(uint64_t mpidr)
{
	uint64_t aff0 = mpidr & 0xffUL;
	uint64_t aff1 = (mpidr >> 8U) & 0xffUL;
	uint64_t aff2 = (mpidr >> 16U) & 0xffUL;
	uint64_t aff3 = (mpidr >> 32U) & 0xffUL;

	return aff0 | (aff1 << 8U) | (aff2 << 16U) | (aff3 << 24U);
}

static void gic_v3_wait_for_rwp(const struct beau_gic_v3_softc *sc, bool redist)
{
	uint16_t pcpu_id = get_pcpu_id();
	uint32_t ctlr_off = redist ? GICR_CTLR : GICD_CTLR;
	uint32_t rwp_mask = redist ? GICR_CTLR_RWP : GICD_CTLR_RWP;
	uint32_t retries;

	for (retries = GIC_WAIT_RETRIES; retries > 0U; retries--) {
		uint32_t ctlr = redist ? gic_r_read_4(sc, pcpu_id, ctlr_off) :
			gic_d_read_4(sc, ctlr_off);

		if ((ctlr & rwp_mask) == 0U) {
			return;
		}
		cpu_relax();
	}

	if (redist) {
		panic("gicr rwp timeout pcpu%hu base=0x%lx ctlr=0x%x", pcpu_id,
			sc->gic_redist_bases[pcpu_id], gic_r_read_4(sc, pcpu_id, ctlr_off));
	}

	panic("gicd rwp timeout base=0x%lx ctlr=0x%x", sc->gic_dist,
		gic_d_read_4(sc, ctlr_off));
}

static void gic_v3_set_dist_priority(const struct beau_gic_v3_softc *sc, uint32_t intid,
	uint8_t priority)
{
	mmio_write8(priority, gic_mmio(sc->gic_dist, GICD_IPRIORITYR_BASE + intid));
}

static void gic_v3_set_redist_priority(const struct beau_gic_v3_softc *sc, uint16_t pcpu_id,
	uint32_t intid, uint8_t priority)
{
	mmio_write8(priority, gic_mmio(sc->gic_redist_bases[pcpu_id],
		GICR_SGI_BASE + GICD_IPRIORITYR_BASE + intid));
}

static int32_t gic_v3_redist_find_all(struct beau_gic_v3_softc *sc)
{
	uint64_t rdist = sc->gic_redist;
	uint32_t frame_count;
	uint32_t frame;
	uint16_t pcpu_id;

	if ((sc->gic_redist == 0UL) || (sc->gic_redist_size == 0UL) ||
		(sc->gic_redist_stride == 0UL)) {
		panic("invalid gic redistributor range base=0x%lx size=0x%lx stride=0x%lx",
			sc->gic_redist, sc->gic_redist_size, sc->gic_redist_stride);
	}

	frame_count = (uint32_t)(sc->gic_redist_size / sc->gic_redist_stride);
	if (frame_count > MAX_PCPU_NUM) {
		frame_count = MAX_PCPU_NUM;
	}
	if (frame_count == 0U) {
		panic("empty gic redistributor range base=0x%lx size=0x%lx stride=0x%lx",
			sc->gic_redist, sc->gic_redist_size, sc->gic_redist_stride);
	}

	(void)memset(sc->gic_redist_bases, 0U, sizeof(sc->gic_redist_bases));

	for (frame = 0U; frame < frame_count; frame++) {
		uint64_t typer = mmio_read64(gic_mmio(rdist, GICR_TYPER));
		uint64_t aff = GICR_TYPER_AFF(typer);

		for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
			if (gic_v3_mpidr_to_affinity(per_cpu(arch.mpidr, pcpu_id)) == aff) {
				sc->gic_redist_bases[pcpu_id] = rdist;
				break;
			}
		}

		if ((typer & GICR_TYPER_LAST) != 0UL) {
			break;
		}

		rdist += sc->gic_redist_stride;
	}

	for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
		if (sc->gic_redist_bases[pcpu_id] == 0UL) {
			panic("missing gic redistributor for pcpu%hu mpidr=0x%lx",
				pcpu_id, per_cpu(arch.mpidr, pcpu_id));
		}
	}

	return 0;
}

static int32_t gic_v3_dist_init(struct beau_gic_v3_softc *sc)
{
	uint32_t typer;
	uint32_t i;
	uint32_t line_limit;

	gic_d_write_4(sc, GICD_CTLR, 0U);
	gic_v3_wait_for_rwp(sc, false);

	gic_d_write_4(sc, GICD_CTLR, GICD_CTLR_ARE_NS);
	gic_v3_wait_for_rwp(sc, false);

	typer = gic_d_read_4(sc, GICD_TYPER);
	sc->gic_nirqs = GICD_TYPER_I_NUM(typer);
	if (sc->gic_nirqs > IRQ_NUM_GIC_DOMAIN) {
		sc->gic_nirqs = IRQ_NUM_GIC_DOMAIN;
	}

	line_limit = sc->gic_nirqs;
	if (line_limit > GIC_SPECIAL_INTID_BASE) {
		line_limit = GIC_SPECIAL_INTID_BASE;
	}

	for (i = GIC_FIRST_SPI; i < line_limit; i += GICD_I_PER_ISENABLERn) {
		uint32_t mask = gic_v3_intid_mask_until(i, line_limit);

		gic_d_write_4(sc, GICD_ICENABLER(i), mask);
		gic_d_write_4(sc, GICD_ICPENDR(i), mask);
		gic_d_write_4(sc, GICD_ICACTIVER(i), mask);
		gic_d_write_4(sc, GICD_IGROUPR(i), gic_d_read_4(sc, GICD_IGROUPR(i)) | mask);
	}

	for (i = GIC_FIRST_SPI; i < line_limit; i++) {
		gic_v3_set_dist_priority(sc, i, GIC_DEFAULT_PRIORITY);
		gic_d_write_8(sc, GICD_IROUTER(i),
			gic_v3_mpidr_to_affinity(per_cpu(arch.mpidr, BSP_CPU_ID)));
	}

	for (i = GIC_FIRST_SPI; i < line_limit; i += GICD_I_PER_ICFGRn) {
		uint32_t mask = gic_v3_icfgr_mask_until(i, line_limit);

		gic_d_write_4(sc, GICD_ICFGR(i), gic_d_read_4(sc, GICD_ICFGR(i)) & ~mask);
	}

	gic_d_write_4(sc, GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_G1A | GICD_CTLR_G1);
	gic_v3_wait_for_rwp(sc, false);

	return 0;
}

static int32_t gic_v3_its_init(struct beau_gic_v3_softc *sc)
{
	beau_gicv3_its_init(sc->gic_its, sc->gic_its_size);
	return 0;
}

static void gic_v3_redist_wake(const struct beau_gic_v3_softc *sc, uint16_t pcpu_id)
{
	uint32_t val = gic_r_read_4(sc, pcpu_id, GICR_WAKER);
	uint32_t retries;

	val &= ~GICR_WAKER_PS;
	gic_r_write_4(sc, pcpu_id, GICR_WAKER, val);

	for (retries = GIC_WAIT_RETRIES; retries > 0U; retries--) {
		if ((gic_r_read_4(sc, pcpu_id, GICR_WAKER) & GICR_WAKER_CA) == 0U) {
			return;
		}
		cpu_relax();
	}

	panic("gicr wake timeout pcpu%hu rdist=0x%lx waker=0x%x", pcpu_id,
		sc->gic_redist_bases[pcpu_id], gic_r_read_4(sc, pcpu_id, GICR_WAKER));
}

static int32_t gic_v3_redist_init(struct beau_gic_v3_softc *sc)
{
	uint16_t pcpu_id = get_pcpu_id();
	uint32_t i;

	gic_v3_redist_wake(sc, pcpu_id);

	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICENABLER(0U), UINT32_MAX);
	gic_v3_wait_for_rwp(sc, true);
	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICPENDR(0U), UINT32_MAX);
	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICACTIVER(0U), UINT32_MAX);
	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_IGROUPR(0U), UINT32_MAX);

	for (i = GIC_FIRST_SGI; i <= GIC_LAST_PPI; i++) {
		gic_v3_set_redist_priority(sc, pcpu_id, i, GIC_DEFAULT_PRIORITY);
	}

	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISENABLER(ARM64_GIC_SGI_SMP_CALL),
		gic_v3_intid_mask(ARM64_GIC_SGI_SMP_CALL));
	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISENABLER(ARM64_GIC_PPI_VGIC_MAINTENANCE),
		gic_v3_intid_mask(ARM64_GIC_PPI_VGIC_MAINTENANCE));
	gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISENABLER(ARM64_GIC_PPI_VIRTUAL_TIMER),
		gic_v3_intid_mask(ARM64_GIC_PPI_VIRTUAL_TIMER));
	gic_v3_wait_for_rwp(sc, true);

	return 0;
}

static int32_t gic_v3_cpu_init(struct beau_gic_v3_softc *sc __unused)
{
	uint64_t sre;

	write_icc_sre_el2(ICC_SRE_ENABLE | ICC_SRE_DIB | ICC_SRE_DFB | ICC_SRE_SRE);
	write_icc_sre_el1(ICC_SRE_SRE);

	sre = read_icc_sre_el1();
	if ((sre & ICC_SRE_SRE) == 0UL) {
		panic("gicv3 sre enable failed");
	}

	write_icc_pmr_el1(GIC_LOWEST_PRIORITY);
	write_icc_bpr1_el1(0U);
	write_icc_ctlr_el1(0U);
	write_icc_igrpen1_el1(1UL);

	return 0;
}

static gic_v3_initseq_t gic_v3_primary_init[] = {
	gic_v3_redist_find_all,
	gic_v3_dist_init,
	NULL
};

static gic_v3_initseq_t gic_v3_secondary_init[] = {
	gic_v3_redist_init,
	gic_v3_cpu_init,
	NULL
};

static void gic_v3_run_init_sequence(struct beau_gic_v3_softc *sc, gic_v3_initseq_t *seq)
{
	uint32_t i;

	for (i = 0U; seq[i] != NULL; i++) {
		int32_t ret = seq[i](sc);

		if (ret != 0) {
			panic("gicv3 init sequence failed step%u ret=%d", i, ret);
		}
	}
}

void arm64_gicv3_init_early(void)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;

	if (sc->gic_initialized) {
		return;
	}

	sc->gic_dist = beau_config.gicd_base;
	sc->gic_dist_size = beau_config.gicd_size;
	sc->gic_redist = beau_config.gicr_base;
	sc->gic_redist_stride = beau_config.gicr_stride;
	sc->gic_redist_size = beau_config.gicr_size;
	sc->gic_its = beau_config.gits_base;
	sc->gic_its_size = beau_config.gits_size;

	if ((sc->gic_dist == 0UL) || (sc->gic_dist_size == 0UL)) {
		panic("invalid gic distributor base=0x%lx size=0x%lx",
			sc->gic_dist, sc->gic_dist_size);
	}

	gic_v3_run_init_sequence(sc, gic_v3_primary_init);
	sc->gic_initialized = true;
}

void arm64_gicv3_init_its(void)
{
	(void)gic_v3_its_init(&gic_v3_sc);
}

void arm64_gicv3_log_boot_info(void)
{
	const struct beau_gic_v3_softc *sc = &gic_v3_sc;

	LOG_INF("GICv3:  GICR      [0x%016lx-0x%016lx] (0x%08lx)",
		sc->gic_redist, gic_range_end(sc->gic_redist, sc->gic_redist_size),
		sc->gic_redist_size);
	LOG_INF("GICv3:  GICD      [0x%016lx-0x%016lx] (0x%08lx)",
		sc->gic_dist, gic_range_end(sc->gic_dist, sc->gic_dist_size),
		sc->gic_dist_size);
	if (sc->gic_its_size != 0UL) {
		LOG_INF("GICv3:  ITS       [0x%016lx-0x%016lx] (0x%08lx)",
			sc->gic_its, gic_range_end(sc->gic_its, sc->gic_its_size),
			sc->gic_its_size);
	}
}

uint64_t arm64_gicv3_redist_base(uint16_t pcpu_id)
{
	return (pcpu_id < MAX_PCPU_NUM) ? gic_v3_sc.gic_redist_bases[pcpu_id] : 0UL;
}

void arm64_gicv3_init(uint16_t pcpu_id)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;

	if (pcpu_id >= MAX_PCPU_NUM) {
		panic("invalid pcpu%hu for gic init", pcpu_id);
	}

	if (pcpu_id == BSP_CPU_ID) {
		arm64_gicv3_init_early();
	}

	if (sc->gic_redist_bases[pcpu_id] == 0UL) {
		panic("no gic redistributor for pcpu%hu mpidr=0x%lx", pcpu_id,
			per_cpu(arch.mpidr, pcpu_id));
	}

	gic_v3_run_init_sequence(sc, gic_v3_secondary_init);
}

static uint64_t gic_pm_read_bpr1(void)
{
	return arm64_sysreg_read(icc_bpr1_el1);
}

int32_t arm64_gicv3_pm_suspend_cpu(uint64_t epoch)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_gicv3_pm_cpu_state *state;

	if ((epoch == 0UL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return -EINVAL;
	}
	state = &gic_pm_cpu[pcpu_id];
	if (state->valid) {
		return (state->suspend_epoch == epoch) ? 0 : -EBUSY;
	}

	state->suspend_epoch = epoch;
	state->sre = read_icc_sre_el1();
	state->pmr = read_icc_pmr_el1();
	state->bpr1 = gic_pm_read_bpr1();
	state->ctlr = read_icc_ctlr_el1();
	state->igrpen1 = read_icc_igrpen1_el1();
	state->valid = true;

	return 0;
}

int32_t arm64_gicv3_pm_resume_cpu(uint64_t epoch)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct arm64_gicv3_pm_cpu_state *state;

	if ((epoch == 0UL) || (pcpu_id >= MAX_PCPU_NUM)) {
		return -EINVAL;
	}
	state = &gic_pm_cpu[pcpu_id];
	if (!state->valid) {
		return 0;
	}
	if (state->suspend_epoch != epoch) {
		return -EINVAL;
	}

	write_icc_sre_el1(state->sre | ICC_SRE_SRE);
	write_icc_pmr_el1(state->pmr);
	write_icc_bpr1_el1(state->bpr1);
	write_icc_ctlr_el1(state->ctlr);
	write_icc_igrpen1_el1(state->igrpen1);
	state->suspend_epoch = 0UL;
	state->valid = false;

	return 0;
}

int32_t arm64_gicv3_pm_suspend(uint64_t epoch)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint32_t intid;
	uint16_t pcpu_id;
	int32_t ret;

	if ((epoch == 0UL) || !sc->gic_initialized) {
		return -EINVAL;
	}
	if (gic_pm_state.valid) {
		return (gic_pm_state.suspend_epoch == epoch) ? 0 : -EBUSY;
	}

	ret = arm64_gicv3_pm_suspend_cpu(epoch);
	if (ret != 0) {
		return ret;
	}
	ret = arm64_gicv3_its_pm_suspend(epoch);
	if (ret != 0) {
		(void)arm64_gicv3_pm_resume_cpu(epoch);
		return ret;
	}

	gic_pm_state.suspend_epoch = epoch;
	gic_pm_state.ctlr = gic_d_read_4(sc, GICD_CTLR);
	for (intid = GIC_FIRST_SPI; intid < sc->gic_nirqs; intid += 32U) {
		uint32_t word = intid / 32U;

		gic_pm_state.group[word] = gic_d_read_4(sc, GICD_IGROUPR(intid));
		gic_pm_state.enabled[word] = gic_d_read_4(sc, GICD_ISENABLER(intid));
		gic_pm_state.pending[word] = gic_d_read_4(sc, GICD_ISPENDR(intid));
		gic_pm_state.active[word] = gic_d_read_4(sc, GICD_ISACTIVER(intid));
	}
	for (intid = GIC_FIRST_SPI; intid < sc->gic_nirqs; intid += 16U) {
		gic_pm_state.config[intid / 16U] =
			gic_d_read_4(sc, GICD_ICFGR(intid));
	}
	for (intid = GIC_FIRST_SPI; intid < sc->gic_nirqs; intid++) {
		gic_pm_state.priority[intid] = mmio_read8(gic_mmio(sc->gic_dist,
			GICD_IPRIORITYR_BASE + intid));
		gic_pm_state.route[intid - GIC_FIRST_SPI] =
			gic_d_read_8(sc, GICD_IROUTER(intid));
	}
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		struct arm64_gicv3_pm_redist_state *state =
			&gic_pm_state.redist[pcpu_id];

		state->group = gic_r_read_4(sc, pcpu_id,
			GICR_SGI_BASE + GICD_IGROUPR(0U));
		state->enabled = gic_r_read_4(sc, pcpu_id,
			GICR_SGI_BASE + GICD_ISENABLER(0U));
		state->pending = gic_r_read_4(sc, pcpu_id,
			GICR_SGI_BASE + GICD_ISPENDR(0U));
		state->active = gic_r_read_4(sc, pcpu_id,
			GICR_SGI_BASE + GICD_ISACTIVER(0U));
		state->config[0U] = gic_r_read_4(sc, pcpu_id,
			GICR_SGI_BASE + GICD_ICFGR(0U));
		state->config[1U] = gic_r_read_4(sc, pcpu_id,
			GICR_SGI_BASE + GICD_ICFGR(16U));
		for (intid = 0U; intid < 32U; intid++) {
			state->priority[intid] = mmio_read8(gic_mmio(
				sc->gic_redist_bases[pcpu_id], GICR_SGI_BASE +
				GICD_IPRIORITYR_BASE + intid));
		}
	}
	gic_pm_state.valid = true;

	return 0;
}

int32_t arm64_gicv3_pm_resume(uint64_t epoch)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint32_t intid;
	uint16_t pcpu_id;
	int32_t ret;

	if (epoch == 0UL) {
		return -EINVAL;
	}
	if (!gic_pm_state.valid) {
		return 0;
	}
	if (gic_pm_state.suspend_epoch != epoch) {
		return -EINVAL;
	}

	gic_d_write_4(sc, GICD_CTLR, 0U);
	gic_v3_wait_for_rwp(sc, false);
	for (intid = GIC_FIRST_SPI; intid < sc->gic_nirqs; intid += 32U) {
		uint32_t word = intid / 32U;

		gic_d_write_4(sc, GICD_ICENABLER(intid), UINT32_MAX);
		gic_d_write_4(sc, GICD_ICPENDR(intid), UINT32_MAX);
		gic_d_write_4(sc, GICD_ICACTIVER(intid), UINT32_MAX);
		gic_d_write_4(sc, GICD_IGROUPR(intid), gic_pm_state.group[word]);
		gic_d_write_4(sc, GICD_ISPENDR(intid), gic_pm_state.pending[word]);
		gic_d_write_4(sc, GICD_ISACTIVER(intid), gic_pm_state.active[word]);
		gic_d_write_4(sc, GICD_ISENABLER(intid), gic_pm_state.enabled[word]);
	}
	for (intid = GIC_FIRST_SPI; intid < sc->gic_nirqs; intid += 16U) {
		gic_d_write_4(sc, GICD_ICFGR(intid),
			gic_pm_state.config[intid / 16U]);
	}
	for (intid = GIC_FIRST_SPI; intid < sc->gic_nirqs; intid++) {
		mmio_write8(gic_pm_state.priority[intid], gic_mmio(sc->gic_dist,
			GICD_IPRIORITYR_BASE + intid));
		gic_d_write_8(sc, GICD_IROUTER(intid),
			gic_pm_state.route[intid - GIC_FIRST_SPI]);
	}
	for (pcpu_id = 0U; pcpu_id < get_pcpu_nums(); pcpu_id++) {
		const struct arm64_gicv3_pm_redist_state *state =
			&gic_pm_state.redist[pcpu_id];

		gic_v3_redist_wake(sc, pcpu_id);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ICENABLER(0U), UINT32_MAX);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ICPENDR(0U), UINT32_MAX);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ICACTIVER(0U), UINT32_MAX);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_IGROUPR(0U), state->group);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ICFGR(0U), state->config[0U]);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ICFGR(16U), state->config[1U]);
		for (intid = 0U; intid < 32U; intid++) {
			mmio_write8(state->priority[intid], gic_mmio(
				sc->gic_redist_bases[pcpu_id], GICR_SGI_BASE +
				GICD_IPRIORITYR_BASE + intid));
		}
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ISPENDR(0U), state->pending);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ISACTIVER(0U), state->active);
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE +
			GICD_ISENABLER(0U), state->enabled);
	}
	gic_d_write_4(sc, GICD_CTLR, gic_pm_state.ctlr);
	gic_v3_wait_for_rwp(sc, false);

	ret = arm64_gicv3_its_pm_resume(epoch);
	if (ret == 0) {
		ret = arm64_gicv3_pm_resume_cpu(epoch);
	}
	if (ret == 0) {
		gic_pm_state.suspend_epoch = 0UL;
		gic_pm_state.valid = false;
	}

	return ret;
}

uint32_t arm64_gicv3_ack_irq(void)
{
	return (uint32_t)(read_icc_iar1_el1() & 0x00ffffffU);
}

void arm64_gicv3_eoi_irq(uint32_t intid)
{
	cpu_memory_barrier();
	write_icc_eoir1_el1(intid);
}

void arm64_gicv3_enable_irq(uint32_t intid)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint16_t pcpu_id = get_pcpu_id();

	if (intid < GIC_FIRST_SPI) {
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICPENDR(intid),
			gic_v3_intid_mask(intid));
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICACTIVER(intid),
			gic_v3_intid_mask(intid));
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISENABLER(intid),
			gic_v3_intid_mask(intid));
		gic_v3_wait_for_rwp(sc, true);
	} else if (gic_v3_is_programmable_spi(sc, intid)) {
		gic_d_write_4(sc, GICD_ICPENDR(intid), gic_v3_intid_mask(intid));
		gic_d_write_4(sc, GICD_ICACTIVER(intid), gic_v3_intid_mask(intid));
		gic_d_write_4(sc, GICD_ISENABLER(intid), gic_v3_intid_mask(intid));
		gic_v3_wait_for_rwp(sc, false);
	}
}

void arm64_gicv3_unmask_irq(uint32_t intid)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint16_t pcpu_id = get_pcpu_id();

	if (intid < GIC_FIRST_SPI) {
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISENABLER(intid),
			gic_v3_intid_mask(intid));
		gic_v3_wait_for_rwp(sc, true);
	} else if (gic_v3_is_programmable_spi(sc, intid)) {
		gic_d_write_4(sc, GICD_ISENABLER(intid), gic_v3_intid_mask(intid));
		gic_v3_wait_for_rwp(sc, false);
	}
}

void arm64_gicv3_disable_irq(uint32_t intid)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint16_t pcpu_id = get_pcpu_id();

	if (intid < GIC_FIRST_SPI) {
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICENABLER(intid),
			gic_v3_intid_mask(intid));
		gic_v3_wait_for_rwp(sc, true);
	} else if (gic_v3_is_programmable_spi(sc, intid)) {
		gic_d_write_4(sc, GICD_ICENABLER(intid), gic_v3_intid_mask(intid));
		gic_v3_wait_for_rwp(sc, false);
	}
}

void arm64_gicv3_clear_irq(uint32_t intid)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint16_t pcpu_id = get_pcpu_id();

	if (intid < GIC_FIRST_SPI) {
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICPENDR(intid),
			gic_v3_intid_mask(intid));
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICACTIVER(intid),
			gic_v3_intid_mask(intid));
	} else if (gic_v3_is_programmable_spi(sc, intid)) {
		gic_d_write_4(sc, GICD_ICPENDR(intid), gic_v3_intid_mask(intid));
		gic_d_write_4(sc, GICD_ICACTIVER(intid), gic_v3_intid_mask(intid));
	}
}

void arm64_gicv3_set_irq_priority(uint32_t intid, uint8_t priority)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint16_t pcpu_id = get_pcpu_id();

	if (intid < GIC_FIRST_SPI) {
		gic_v3_set_redist_priority(sc, pcpu_id, intid, priority);
	} else if (gic_v3_is_programmable_spi(sc, intid)) {
		gic_v3_set_dist_priority(sc, intid, priority);
	}
}

bool arm64_gicv3_set_irq_affinity(uint32_t intid, uint16_t pcpu_id)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	bool ret = false;

	/*
	 * Only SPIs are programmable through GICD_IROUTER. SGIs/PPIs are
	 * redistributor-local by definition, and LPIs are routed by ITS
	 * collections rather than this distributor register. Rejecting all
	 * non-programmable INTIDs keeps passthrough affinity policy limited to
	 * the physical interrupt class that has a single well-defined owner pCPU.
	 */
	if ((pcpu_id < get_pcpu_nums()) && gic_v3_is_programmable_spi(sc, intid) &&
		(sc->gic_redist_bases[pcpu_id] != 0UL)) {
		gic_d_write_8(sc, GICD_IROUTER(intid),
			gic_v3_mpidr_to_affinity(per_cpu(arch.mpidr, pcpu_id)));
		gic_v3_wait_for_rwp(sc, false);
		ret = true;
	}

	return ret;
}

void arm64_gicv3_set_local_irq_active(uint16_t pcpu_id, uint32_t intid)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;

	if ((pcpu_id < MAX_PCPU_NUM) && (intid < GIC_FIRST_SPI) &&
		(sc->gic_redist_bases[pcpu_id] != 0UL)) {
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISACTIVER(intid),
			gic_v3_intid_mask(intid));
		cpu_memory_barrier();
	}
}

void arm64_gicv3_clear_local_irq_active(uint16_t pcpu_id, uint32_t intid)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;

	if ((pcpu_id < MAX_PCPU_NUM) && (intid < GIC_FIRST_SPI) &&
		(sc->gic_redist_bases[pcpu_id] != 0UL)) {
		gic_r_write_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ICACTIVER(intid),
			gic_v3_intid_mask(intid));
		cpu_memory_barrier();
	}
}

bool arm64_gicv3_has_its(void)
{
	return beau_gicv3_its_present();
}

bool arm64_gicv3_map_spi_msi(uint32_t intid, uint64_t *addr, uint32_t *data)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;

	if ((addr == NULL) || (data == NULL) || !gic_v3_is_programmable_spi(sc, intid)) {
		return false;
	}

	*addr = sc->gic_dist + GICD_SETSPI_NSR;
	*data = intid;

	return true;
}

void arm64_gicv3_get_local_irq_state(uint16_t pcpu_id, uint32_t intid,
	struct arm64_gicv3_local_irq_state *state)
{
	struct beau_gic_v3_softc *sc = &gic_v3_sc;
	uint32_t reg;
	uint32_t mask;

	if (state == NULL) {
		return;
	}

	(void)memset(state, 0U, sizeof(*state));
	if ((pcpu_id >= MAX_PCPU_NUM) || (intid >= GIC_FIRST_SPI) ||
		(sc->gic_redist_bases[pcpu_id] == 0UL)) {
		return;
	}

	reg = gic_v3_intid_reg(intid);
	mask = gic_v3_intid_mask(intid);

	state->enabled = gic_r_read_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISENABLER(0U) + reg) & mask;
	state->pending = gic_r_read_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISPENDR(0U) + reg) & mask;
	state->active = gic_r_read_4(sc, pcpu_id, GICR_SGI_BASE + GICD_ISACTIVER(0U) + reg) & mask;
	state->group = gic_r_read_4(sc, pcpu_id, GICR_SGI_BASE + GICD_IGROUPR(0U) + reg) & mask;
	state->priority = mmio_read8(gic_mmio(sc->gic_redist_bases[pcpu_id],
		GICR_SGI_BASE + GICD_IPRIORITYR_BASE + intid));
	state->valid = true;
}

void arm64_gicv3_send_sgi(uint16_t pcpu_id, uint32_t sgi_id)
{
	uint64_t mpidr;
	uint64_t aff1;
	uint64_t aff2;
	uint64_t aff3;
	uint64_t target_list;
	uint64_t sgi;

	if ((pcpu_id >= MAX_PCPU_NUM) || (sgi_id > GIC_LAST_SGI)) {
		return;
	}

	mpidr = per_cpu(arch.mpidr, pcpu_id);
	aff1 = (mpidr >> 8U) & 0xffUL;
	aff2 = (mpidr >> 16U) & 0xffUL;
	aff3 = (mpidr >> 32U) & 0xffUL;
	target_list = 1UL << (mpidr & 0xfUL);

	sgi = (aff3 << 48U) | (aff2 << 32U) | (sgi_id << 24U) | (aff1 << 16U) |
		target_list;
	cpu_memory_barrier();
	write_icc_sgi1r_el1(sgi);
}
