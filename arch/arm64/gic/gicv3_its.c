/*-
 * Copyright (c) 2015-2016 The FreeBSD Foundation
 * Copyright (c) 2023 Arm Ltd
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
#include <cpu.h>
#include <errno.h>
#include <io.h>
#include <logmsg.h>
#include <pgtable.h>
#include <rtl.h>
#include <spinlock.h>
#include <util.h>
#include <asm/mmu.h>
#include <asm/page.h>
#include <asm/irq.h>

/* [20260714] Physical ITS and MSI/MSI-X remap framework
 *
 *   PCI requester writes MSI/MSI-X
 *       |
 *       v
 *   GITS_TRANSLATER(event_id)
 *       |
 *       v
 *   ITS tables: Device -> ITT -> Collection
 *       |
 *       v
 *   LPI INTID -> Redistributor pending table -> EL2 IRQ domain
 *       |
 *       +--> host handler
 *       +--> passthrough/vPCI path -> vGIC injection
 *
 * Ownership boundary:
 *   - this file owns physical ITS tables, command queue, LPI allocation, and
 *     the physical MSI message programmed into a device;
 *   - guest-visible virtual ITS state belongs to arch/arm64/guest/vgicv3_its.c;
 *   - vPCI/passthrough callers receive remapped messages, not permission to
 *     expose arbitrary physical ITS state.
 *
 * Key rule:
 *   - publish backing tables before enabling LPIs/ITS;
 *   - issue ITS commands under the ITS lock and wait for command completion
 *     before software marks an event programmed;
 *   - fail closed when device, event, or LPI space is exhausted or inconsistent.
 */

#ifndef PAGE_SIZE_64K
#define	PAGE_SIZE_64K		0x10000UL
#endif

#include "gicv3_reg.h"

#define	GITS_CTLR_QUIESCENT	(1U << 31U)
#define	GITS_WAIT_RETRIES	1000000U
#define	BEAU_GICV3_ITS_MAX_VECTORS	1024U
#define	BEAU_GICV3_ITS_CMDQ_ENTRIES	(PAGE_SIZE / GITS_CMD_SIZE)
#define	BEAU_GICV3_ITS_TABLE_SIZE	PAGE_SIZE_64K
#define	BEAU_GICV3_ITS_MAX_DEVS	16U
#define	BEAU_GICV3_ITS_MAX_EVENTS	128U
#define	BEAU_GICV3_ITS_ITT_ENTRY_MAX	32U
#define	BEAU_GICV3_ITS_LPI_IDBITS	14U
#define	BEAU_GICV3_ITS_LPI_CFG_SIZE	(1UL << BEAU_GICV3_ITS_LPI_IDBITS)
#define	BEAU_GICV3_ITS_LPI_PEND_SIZE	PAGE_SIZE_64K
#define	BEAU_GICV3_ITS_COLLECTION_ID	0U

#define	GITS_CMD_MAPD		0x08U
#define	GITS_CMD_MAPC		0x09U
#define	GITS_CMD_MAPTI		0x0aU
#define	GITS_CMD_INV		0x0cU
#define	GITS_CMD_SYNC		0x05U
#define	GITS_CMD_DISCARD	0x0fU
#define	GITS_CMD_VALID		(1UL << 63U)
#define	GITS_CMD_ITT_ADDR_MASK	0x000fffffffffff00UL
#define	GITS_CMD_TARGET_MASK	0x0000ffffffff0000UL

static uint64_t beau_gicv3_its_base;
static uint64_t beau_gicv3_its_size;
static uint64_t beau_gicv3_its_typer;
static uint64_t beau_gicv3_its_collection_target;
static uint32_t beau_gicv3_its_cmdq_writer;
static uint32_t beau_gicv3_its_itt_entry_size = 16U;
static bool beau_gicv3_its_ready;
static uint32_t beau_gicv3_its_alloc_msi_ok;
static uint32_t beau_gicv3_its_alloc_msi_fail;
static uint32_t beau_gicv3_its_alloc_msix_ok;
static uint32_t beau_gicv3_its_alloc_msix_fail;
static uint32_t beau_gicv3_its_release_msi;
static uint32_t beau_gicv3_its_release_msix;
static uint32_t beau_gicv3_its_map_event_ok;
static uint32_t beau_gicv3_its_map_event_fail;
static uint32_t beau_gicv3_its_unmap_event_ok;
static uint32_t beau_gicv3_its_unmap_event_fail;
static uint32_t beau_gicv3_its_cmd_issued;
static uint32_t beau_gicv3_its_cmd_errors;
static uint32_t beau_gicv3_its_cmd_timeouts;
static uint32_t beau_gicv3_its_cmd_stalls;
static int32_t beau_gicv3_its_last_ret;

struct beau_gicv3_its_irqsrc {
	bool		used;
	bool		msix;
	bool		programmed;
	uint32_t	dev_id;
	uint32_t	event_id;
	uint32_t	lpi;
};

struct beau_gicv3_its_device {
	bool		used;
	uint32_t	dev_id;
	uint32_t	event_count;
};

static spinlock_t beau_gicv3_its_lock = { .head = 0U, .tail = 0U };
static struct beau_gicv3_its_irqsrc beau_gicv3_its_irqs[BEAU_GICV3_ITS_MAX_VECTORS];
static struct beau_gicv3_its_device beau_gicv3_its_devs[BEAU_GICV3_ITS_MAX_DEVS];
static uint64_t beau_gicv3_its_cmdq[BEAU_GICV3_ITS_CMDQ_ENTRIES][4] __aligned(PAGE_SIZE);
static uint8_t beau_gicv3_its_device_table[BEAU_GICV3_ITS_TABLE_SIZE] __aligned(PAGE_SIZE_64K);
static uint8_t beau_gicv3_its_collection_table[BEAU_GICV3_ITS_TABLE_SIZE] __aligned(PAGE_SIZE_64K);
static uint8_t beau_gicv3_its_lpi_cfg[BEAU_GICV3_ITS_LPI_CFG_SIZE] __aligned(PAGE_SIZE);
static uint8_t beau_gicv3_its_lpi_pending[BEAU_GICV3_ITS_LPI_PEND_SIZE] __aligned(PAGE_SIZE_64K);
static uint8_t beau_gicv3_its_itt[BEAU_GICV3_ITS_MAX_DEVS]
	[BEAU_GICV3_ITS_MAX_EVENTS * BEAU_GICV3_ITS_ITT_ENTRY_MAX] __aligned(256);

struct beau_gicv3_its_pm_state {
	uint64_t suspend_epoch;
	uint64_t cbaser;
	uint64_t cwriter;
	uint64_t baser[GITS_BASER_NUM];
	uint64_t propbaser;
	uint64_t pendbaser;
	uint32_t ctlr;
	bool active;
	bool valid;
};

static struct beau_gicv3_its_pm_state beau_gicv3_its_pm;

static inline void *beau_gits_addr(uint32_t off)
{
	return (void *)(beau_gicv3_its_base + off);
}

static inline uint32_t beau_gits_read_4(uint32_t off)
{
	return mmio_read32(beau_gits_addr(off));
}

static inline uint64_t beau_gits_read_8(uint32_t off)
{
	return mmio_read64(beau_gits_addr(off));
}

static inline void beau_gits_write_4(uint32_t off, uint32_t val)
{
	mmio_write32(val, beau_gits_addr(off));
}

static inline void beau_gits_write_8(uint32_t off, uint64_t val)
{
	mmio_write64(val, beau_gits_addr(off));
}

static inline void *beau_gicr_addr(uint64_t rdist, uint32_t off)
{
	return (void *)(rdist + off);
}

static inline uint32_t beau_gicr_read_4(uint64_t rdist, uint32_t off)
{
	return mmio_read32(beau_gicr_addr(rdist, off));
}

static inline uint64_t beau_gicr_read_8(uint64_t rdist, uint32_t off)
{
	return mmio_read64(beau_gicr_addr(rdist, off));
}

static inline void beau_gicr_write_4(uint64_t rdist, uint32_t off, uint32_t val)
{
	mmio_write32(val, beau_gicr_addr(rdist, off));
}

static inline void beau_gicr_write_8(uint64_t rdist, uint32_t off, uint64_t val)
{
	mmio_write64(val, beau_gicr_addr(rdist, off));
}

static uint32_t beau_gicv3_its_log2_u32(uint32_t value)
{
	uint32_t log = 0U;

	while (value > 1U) {
		value >>= 1U;
		log++;
	}

	return log;
}

static uint32_t beau_gicv3_its_event_count(uint32_t event_id)
{
	uint32_t count = 2U;

	while ((event_id >= count) && (count < BEAU_GICV3_ITS_MAX_EVENTS)) {
		count <<= 1U;
	}

	return count;
}

static bool beau_gicv3_its_dev_id_valid(uint32_t dev_id)
{
	uint32_t devbits = (uint32_t)GITS_TYPER_DEVB(beau_gicv3_its_typer);

	return (devbits >= 32U) || (dev_id < (1U << devbits));
}

static uint32_t beau_gicv3_its_find_baser(uint32_t type, uint64_t *baser)
{
	uint32_t idx;

	for (idx = 0U; idx < GITS_BASER_NUM; idx++) {
		uint64_t val = beau_gits_read_8(GITS_BASER(idx));

		if (GITS_BASER_TYPE(val) == type) {
			if (baser != NULL) {
				*baser = val;
			}
			return idx;
		}
	}

	return GITS_BASER_NUM;
}

static int32_t beau_gicv3_its_wait_creadr(uint32_t target)
{
	uint32_t retries;

	for (retries = GITS_WAIT_RETRIES; retries > 0U; retries--) {
		uint64_t creadr = beau_gits_read_8(GITS_CREADR);

		if ((creadr & GITS_CREADR_STALL) != 0UL) {
			LOG_ERR("gicv3 its command stalled creadr=0x%lx", creadr);
			return -EIO;
		}
		if ((uint32_t)GITS_CMD_OFFSET(creadr) == target) {
			return 0;
		}
		cpu_relax();
	}

	LOG_ERR("gicv3 its command timeout creadr=0x%lx target=0x%x",
		beau_gits_read_8(GITS_CREADR), target);
	return -ETIMEDOUT;
}

static int32_t beau_gicv3_its_issue_cmd(const uint64_t cmd[4])
{
	uint32_t writer = beau_gicv3_its_cmdq_writer;
	uint32_t next = writer + GITS_CMD_SIZE;
	uint32_t idx = writer / GITS_CMD_SIZE;
	int32_t ret;

	if (idx >= BEAU_GICV3_ITS_CMDQ_ENTRIES) {
		return -EINVAL;
	}
	if (next >= PAGE_SIZE) {
		next = 0U;
	}

	beau_gicv3_its_cmdq[idx][0] = cmd[0];
	beau_gicv3_its_cmdq[idx][1] = cmd[1];
	beau_gicv3_its_cmdq[idx][2] = cmd[2];
	beau_gicv3_its_cmdq[idx][3] = cmd[3];
	flush_cache_range(beau_gicv3_its_cmdq[idx], sizeof(beau_gicv3_its_cmdq[idx]));

	beau_gits_write_8(GITS_CWRITER, next);
	beau_gicv3_its_cmd_issued++;
	ret = beau_gicv3_its_wait_creadr(next);
	if (ret == 0) {
		beau_gicv3_its_cmdq_writer = next;
	} else {
		beau_gicv3_its_cmd_errors++;
		if (ret == -ETIMEDOUT) {
			beau_gicv3_its_cmd_timeouts++;
		} else if (ret == -EIO) {
			beau_gicv3_its_cmd_stalls++;
		}
	}
	beau_gicv3_its_last_ret = ret;

	return ret;
}

static int32_t beau_gicv3_its_cmd_mapc(void)
{
	uint64_t cmd[4] = { 0UL, 0UL, 0UL, 0UL };

	cmd[0] = GITS_CMD_MAPC;
	cmd[2] = (beau_gicv3_its_collection_target & GITS_CMD_TARGET_MASK) |
		BEAU_GICV3_ITS_COLLECTION_ID | GITS_CMD_VALID;

	return beau_gicv3_its_issue_cmd(cmd);
}

static int32_t beau_gicv3_its_cmd_sync(void)
{
	uint64_t cmd[4] = { 0UL, 0UL, 0UL, 0UL };

	cmd[0] = GITS_CMD_SYNC;
	cmd[2] = beau_gicv3_its_collection_target & GITS_CMD_TARGET_MASK;

	return beau_gicv3_its_issue_cmd(cmd);
}

static int32_t beau_gicv3_its_cmd_mapd(uint32_t dev_id, uint32_t dev_idx,
	uint32_t event_count)
{
	uint64_t cmd[4] = { 0UL, 0UL, 0UL, 0UL };
	uint64_t itt = hva2hpa(beau_gicv3_its_itt[dev_idx]);
	uint32_t event_bits = beau_gicv3_its_log2_u32(event_count);

	if ((dev_idx >= BEAU_GICV3_ITS_MAX_DEVS) ||
		(event_bits == 0U) || (event_count > BEAU_GICV3_ITS_MAX_EVENTS)) {
		return -EINVAL;
	}

	cmd[0] = GITS_CMD_MAPD | ((uint64_t)dev_id << 32U);
	cmd[1] = event_bits - 1U;
	cmd[2] = (itt & GITS_CMD_ITT_ADDR_MASK) | GITS_CMD_VALID;

	return beau_gicv3_its_issue_cmd(cmd);
}

static int32_t beau_gicv3_its_cmd_mapti(uint32_t dev_id, uint32_t event_id,
	uint32_t lpi)
{
	uint64_t cmd[4] = { 0UL, 0UL, 0UL, 0UL };

	cmd[0] = GITS_CMD_MAPTI | ((uint64_t)dev_id << 32U);
	cmd[1] = event_id | ((uint64_t)lpi << 32U);
	cmd[2] = BEAU_GICV3_ITS_COLLECTION_ID;

	return beau_gicv3_its_issue_cmd(cmd);
}

static int32_t beau_gicv3_its_cmd_inv(uint32_t dev_id, uint32_t event_id)
{
	uint64_t cmd[4] = { 0UL, 0UL, 0UL, 0UL };

	cmd[0] = GITS_CMD_INV | ((uint64_t)dev_id << 32U);
	cmd[1] = event_id;

	return beau_gicv3_its_issue_cmd(cmd);
}

static int32_t beau_gicv3_its_cmd_discard(uint32_t dev_id, uint32_t event_id)
{
	uint64_t cmd[4] = { 0UL, 0UL, 0UL, 0UL };

	cmd[0] = GITS_CMD_DISCARD | ((uint64_t)dev_id << 32U);
	cmd[1] = event_id;

	return beau_gicv3_its_issue_cmd(cmd);
}

static void beau_gicv3_its_set_lpi_enabled(uint32_t lpi, bool enabled)
{
	if ((lpi >= GIC_FIRST_LPI) &&
		((lpi - GIC_FIRST_LPI) < BEAU_GICV3_ITS_LPI_CFG_SIZE)) {
		uint32_t idx = lpi - GIC_FIRST_LPI;
		uint8_t conf = (uint8_t)((uint32_t)ARM64_GIC_PRIORITY_DEFAULT &
			LPI_CONF_PRIO_MASK);

		conf |= LPI_CONF_GROUP1;
		if (enabled) {
			conf |= LPI_CONF_ENABLE;
		}
		beau_gicv3_its_lpi_cfg[idx] = conf;
		flush_cache_range(&beau_gicv3_its_lpi_cfg[idx],
			sizeof(beau_gicv3_its_lpi_cfg[idx]));
	}
}

static int32_t beau_gicv3_its_program_baser(uint32_t type, void *table,
	uint64_t size)
{
	uint64_t old;
	uint64_t val;
	uint32_t idx = beau_gicv3_its_find_baser(type, &old);

	if ((idx >= GITS_BASER_NUM) || (table == NULL) ||
		(size == 0UL) || ((size % PAGE_SIZE_64K) != 0UL)) {
		return -EINVAL;
	}

	(void)memset(table, 0U, (size_t)size);
	flush_cache_range(table, size);

	val = old & (GITS_BASER_TYPE_MASK | GITS_BASER_ESIZE_MASK);
	val |= GITS_BASER_VALID;
	val |= (hva2hpa(table) & GITS_BASER_PA_MASK);
	val |= (((size / PAGE_SIZE_64K) - 1UL) & GITS_BASER_SIZE_MASK);
	val |= GITS_BASER_PSZ_64K << GITS_BASER_PSZ_SHIFT;
	val |= GITS_BASER_SHARE_IS << GITS_BASER_SHARE_SHIFT;
	val |= GITS_BASER_CACHE_RAWAWB << GITS_BASER_CACHE_SHIFT;

	beau_gits_write_8(GITS_BASER(idx), val);
	if ((beau_gits_read_8(GITS_BASER(idx)) & GITS_BASER_VALID) == 0UL) {
		LOG_ERR("gicv3 its BASER%u type%u rejected", idx, type);
		return -EIO;
	}

	return 0;
}

static int32_t beau_gicv3_its_program_cmdq(void)
{
	uint64_t cbaser;

	(void)memset(beau_gicv3_its_cmdq, 0U, sizeof(beau_gicv3_its_cmdq));
	flush_cache_range(beau_gicv3_its_cmdq, sizeof(beau_gicv3_its_cmdq));
	beau_gicv3_its_cmdq_writer = 0U;

	cbaser = GITS_CBASER_VALID;
	cbaser |= hva2hpa(beau_gicv3_its_cmdq) & GITS_CBASER_PA_MASK;
	cbaser |= GITS_CBASER_SHARE_IS << GITS_CBASER_SHARE_SHIFT;
	cbaser |= GITS_CBASER_CACHE_NIRAWAWB << GITS_CBASER_CACHE_SHIFT;
	beau_gits_write_8(GITS_CBASER, cbaser);
	beau_gits_write_8(GITS_CWRITER, 0UL);

	if ((beau_gits_read_8(GITS_CBASER) & GITS_CBASER_VALID) == 0UL) {
		LOG_ERR("gicv3 its CBASER rejected");
		return -EIO;
	}

	return 0;
}

static int32_t beau_gicv3_its_enable_lpis(void)
{
	uint64_t rdist = arm64_gicv3_redist_base(BSP_CPU_ID);
	uint64_t typer;
	uint64_t propbaser;
	uint64_t pendbaser;
	uint32_t ctlr;
	uint32_t retries;

	if (rdist == 0UL) {
		return -ENODEV;
	}

	typer = beau_gicr_read_8(rdist, GICR_TYPER);
	if ((typer & GICR_TYPER_PLPIS) == 0UL) {
		LOG_ERR("gicv3 redistributor has no physical LPI support");
		return -ENODEV;
	}

	(void)memset(beau_gicv3_its_lpi_cfg,
		ARM64_GIC_PRIORITY_DEFAULT | LPI_CONF_GROUP1,
		sizeof(beau_gicv3_its_lpi_cfg));
	(void)memset(beau_gicv3_its_lpi_pending, 0U,
		sizeof(beau_gicv3_its_lpi_pending));
	flush_cache_range(beau_gicv3_its_lpi_cfg, sizeof(beau_gicv3_its_lpi_cfg));
	flush_cache_range(beau_gicv3_its_lpi_pending, sizeof(beau_gicv3_its_lpi_pending));

	propbaser = hva2hpa(beau_gicv3_its_lpi_cfg) & GICR_PROPBASER_PA_MASK;
	propbaser |= (BEAU_GICV3_ITS_LPI_IDBITS - 1UL) & GICR_PROPBASER_IDBITS_MASK;
	propbaser |= GICR_PROPBASER_SHARE_IS << GICR_PROPBASER_SHARE_SHIFT;
	propbaser |= GICR_PROPBASER_CACHE_NIRAWAWB << GICR_PROPBASER_CACHE_SHIFT;
	propbaser |= GICR_PROPBASER_CACHE_NIRAWAWB << GICR_PROPBASER_OUTER_CACHE_SHIFT;
	beau_gicr_write_8(rdist, GICR_PROPBASER, propbaser);

	pendbaser = hva2hpa(beau_gicv3_its_lpi_pending) & GICR_PENDBASER_PA_MASK;
	pendbaser |= GICR_PENDBASER_SHARE_IS << GICR_PENDBASER_SHARE_SHIFT;
	pendbaser |= GICR_PENDBASER_CACHE_NIRAWAWB << GICR_PENDBASER_CACHE_SHIFT;
	pendbaser |= GICR_PENDBASER_CACHE_NIRAWAWB << GICR_PENDBASER_OUTER_CACHE_SHIFT;
	beau_gicr_write_8(rdist, GICR_PENDBASER, pendbaser);

	ctlr = beau_gicr_read_4(rdist, GICR_CTLR);
	beau_gicr_write_4(rdist, GICR_CTLR, ctlr | GICR_CTLR_LPI_ENABLE);

	for (retries = GITS_WAIT_RETRIES; retries > 0U; retries--) {
		if ((beau_gicr_read_4(rdist, GICR_CTLR) & GICR_CTLR_RWP) == 0U) {
			if ((beau_gicv3_its_typer & GITS_TYPER_PTA) != 0UL) {
				beau_gicv3_its_collection_target = rdist;
			} else {
				beau_gicv3_its_collection_target =
					GICR_TYPER_CPUNUM(typer) << 16U;
			}
			return 0;
		}
		cpu_relax();
	}

	LOG_ERR("gicv3 redistributor LPI enable timeout rdist=0x%lx ctlr=0x%x",
		rdist, beau_gicr_read_4(rdist, GICR_CTLR));
	return -ETIMEDOUT;
}

static int32_t beau_gicv3_its_setup_hw(void)
{
	int32_t ret;

	ret = beau_gicv3_its_enable_lpis();
	if (ret != 0) {
		return ret;
	}

	ret = beau_gicv3_its_program_baser(GITS_BASER_TYPE_DEV,
		beau_gicv3_its_device_table, sizeof(beau_gicv3_its_device_table));
	if (ret != 0) {
		return ret;
	}

	ret = beau_gicv3_its_program_baser(GITS_BASER_TYPE_IC,
		beau_gicv3_its_collection_table, sizeof(beau_gicv3_its_collection_table));
	if (ret != 0) {
		return ret;
	}

	ret = beau_gicv3_its_program_cmdq();
	if (ret != 0) {
		return ret;
	}

	beau_gits_write_4(GITS_CTLR, GITS_CTLR_EN);

	ret = beau_gicv3_its_cmd_mapc();
	if (ret == 0) {
		ret = beau_gicv3_its_cmd_sync();
	}

	return ret;
}

static struct beau_gicv3_its_device *beau_gicv3_its_find_dev(uint32_t dev_id)
{
	uint32_t idx;

	for (idx = 0U; idx < BEAU_GICV3_ITS_MAX_DEVS; idx++) {
		if (beau_gicv3_its_devs[idx].used &&
			(beau_gicv3_its_devs[idx].dev_id == dev_id)) {
			return &beau_gicv3_its_devs[idx];
		}
	}

	return NULL;
}

static struct beau_gicv3_its_device *beau_gicv3_its_alloc_dev(uint32_t dev_id)
{
	uint32_t idx;

	for (idx = 0U; idx < BEAU_GICV3_ITS_MAX_DEVS; idx++) {
		if (!beau_gicv3_its_devs[idx].used) {
			beau_gicv3_its_devs[idx].used = true;
			beau_gicv3_its_devs[idx].dev_id = dev_id;
			beau_gicv3_its_devs[idx].event_count = 0U;
			(void)memset(beau_gicv3_its_itt[idx], 0U,
				sizeof(beau_gicv3_its_itt[idx]));
			flush_cache_range(beau_gicv3_its_itt[idx],
				sizeof(beau_gicv3_its_itt[idx]));
			return &beau_gicv3_its_devs[idx];
		}
	}

	return NULL;
}

static uint32_t beau_gicv3_its_dev_index(const struct beau_gicv3_its_device *dev)
{
	uint32_t idx = BEAU_GICV3_ITS_MAX_DEVS;

	if (dev != NULL) {
		idx = (uint32_t)(dev - beau_gicv3_its_devs);
		if (idx >= BEAU_GICV3_ITS_MAX_DEVS) {
			idx = BEAU_GICV3_ITS_MAX_DEVS;
		}
	}

	return idx;
}

static int32_t beau_gicv3_its_ensure_dev(uint32_t dev_id, uint32_t event_id)
{
	struct beau_gicv3_its_device *dev;
	uint32_t event_count;
	uint32_t dev_idx;
	int32_t ret = 0;
	bool new_dev = false;

	if (!beau_gicv3_its_dev_id_valid(dev_id) ||
		(event_id >= BEAU_GICV3_ITS_MAX_EVENTS)) {
		return -EINVAL;
	}

	dev = beau_gicv3_its_find_dev(dev_id);
	if (dev == NULL) {
		dev = beau_gicv3_its_alloc_dev(dev_id);
		new_dev = dev != NULL;
	}
	if (dev == NULL) {
		return -ENOMEM;
	}

	event_count = beau_gicv3_its_event_count(event_id);
	if (event_count > BEAU_GICV3_ITS_MAX_EVENTS) {
		return -EINVAL;
	}

	if (dev->event_count < event_count) {
		dev_idx = beau_gicv3_its_dev_index(dev);
		ret = beau_gicv3_its_cmd_mapd(dev_id, dev_idx, event_count);
		if (ret == 0) {
			dev->event_count = event_count;
		} else if (new_dev) {
			(void)memset(dev, 0U, sizeof(*dev));
		}
	}

	return ret;
}

static int32_t beau_gicv3_its_map_event(uint32_t dev_id, uint32_t event_id,
	uint32_t lpi)
{
	int32_t ret;

	ret = beau_gicv3_its_ensure_dev(dev_id, event_id);
	if (ret != 0) {
		return ret;
	}

	beau_gicv3_its_set_lpi_enabled(lpi, true);
	ret = beau_gicv3_its_cmd_mapti(dev_id, event_id, lpi);
	if (ret == 0) {
		ret = beau_gicv3_its_cmd_inv(dev_id, event_id);
	}
	if (ret == 0) {
		ret = beau_gicv3_its_cmd_sync();
	}
	if (ret != 0) {
		beau_gicv3_its_set_lpi_enabled(lpi, false);
	}

	return ret;
}

static int32_t beau_gicv3_its_unmap_event(uint32_t dev_id, uint32_t event_id)
{
	int32_t ret;

	ret = beau_gicv3_its_cmd_discard(dev_id, event_id);
	if (ret == 0) {
		ret = beau_gicv3_its_cmd_sync();
	}

	return ret;
}

static int32_t beau_gicv3_its_program_irq(struct beau_gicv3_its_irqsrc *irq)
{
	int32_t ret;

	irq->programmed = false;
	ret = beau_gicv3_its_map_event(irq->dev_id, irq->event_id, irq->lpi);
	if (ret != 0) {
		return ret;
	} else {
		irq->programmed = true;
	}

	return ret;
}

static void beau_gicv3_its_unprogram_irq(struct beau_gicv3_its_irqsrc *irq)
{
	if (irq == NULL) {
		return;
	}

	if (irq->programmed) {
		(void)beau_gicv3_its_unmap_event(irq->dev_id, irq->event_id);
	}
	beau_gicv3_its_set_lpi_enabled(irq->lpi, false);
	irq->used = false;
	irq->programmed = false;
}

void beau_gicv3_its_init(uint64_t base, uint64_t size)
{
	uint32_t retries;
	int32_t ret;

	beau_gicv3_its_base = base;
	beau_gicv3_its_size = size;
	beau_gicv3_its_typer = 0UL;
	beau_gicv3_its_collection_target = 0UL;
	beau_gicv3_its_ready = false;
	beau_gicv3_its_cmdq_writer = 0U;
	(void)memset(beau_gicv3_its_irqs, 0U, sizeof(beau_gicv3_its_irqs));
	(void)memset(beau_gicv3_its_devs, 0U, sizeof(beau_gicv3_its_devs));
	beau_gicv3_its_alloc_msi_ok = 0U;
	beau_gicv3_its_alloc_msi_fail = 0U;
	beau_gicv3_its_alloc_msix_ok = 0U;
	beau_gicv3_its_alloc_msix_fail = 0U;
	beau_gicv3_its_release_msi = 0U;
	beau_gicv3_its_release_msix = 0U;
	beau_gicv3_its_map_event_ok = 0U;
	beau_gicv3_its_map_event_fail = 0U;
	beau_gicv3_its_unmap_event_ok = 0U;
	beau_gicv3_its_unmap_event_fail = 0U;
	beau_gicv3_its_cmd_issued = 0U;
	beau_gicv3_its_cmd_errors = 0U;
	beau_gicv3_its_cmd_timeouts = 0U;
	beau_gicv3_its_cmd_stalls = 0U;
	beau_gicv3_its_last_ret = 0;

	if ((base == 0UL) || (size == 0UL)) {
		return;
	}

	beau_gicv3_its_typer = beau_gits_read_8(GITS_TYPER);
	beau_gicv3_its_itt_entry_size =
		(uint32_t)GITS_TYPER_ITTES(beau_gicv3_its_typer);
	if ((beau_gicv3_its_itt_entry_size == 0U) ||
		(beau_gicv3_its_itt_entry_size > BEAU_GICV3_ITS_ITT_ENTRY_MAX)) {
		LOG_ERR("gicv3 its unsupported ITT entry size %u",
			beau_gicv3_its_itt_entry_size);
		return;
	}

	beau_gits_write_4(GITS_CTLR, 0U);
	for (retries = GITS_WAIT_RETRIES; retries > 0U; retries--) {
		uint32_t ctlr = beau_gits_read_4(GITS_CTLR);

		if ((ctlr & GITS_CTLR_QUIESCENT) != 0U) {
			ret = beau_gicv3_its_setup_hw();
			if (ret == 0) {
				beau_gicv3_its_ready = true;
				LOG_INF("GICv3:  ITS enabled base:0x%08lx target:0x%08lx",
					base, beau_gicv3_its_collection_target);
			} else {
				LOG_ERR("GICv3:  ITS setup failed ret=%d", ret);
			}
			return;
		}
		cpu_relax();
	}

	LOG_ERR("GICv3:  ITS quiesce timeout base=0x%lx size=0x%lx ctlr=0x%x",
		base, size, beau_gits_read_4(GITS_CTLR));
}

int32_t arm64_gicv3_its_pm_suspend(uint64_t epoch)
{
	uint64_t rdist = arm64_gicv3_redist_base(BSP_CPU_ID);
	uint64_t flags;
	uint32_t idx;
	int32_t ret = 0;

	if (epoch == 0UL) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	if (beau_gicv3_its_pm.valid) {
		ret = (beau_gicv3_its_pm.suspend_epoch == epoch) ? 0 : -EBUSY;
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return ret;
	}

	beau_gicv3_its_pm.suspend_epoch = epoch;
	beau_gicv3_its_pm.active = beau_gicv3_its_ready;
	if (beau_gicv3_its_pm.active) {
		ret = beau_gicv3_its_cmd_sync();
		if (ret == 0) {
			beau_gicv3_its_pm.ctlr = beau_gits_read_4(GITS_CTLR);
			beau_gicv3_its_pm.cbaser = beau_gits_read_8(GITS_CBASER);
			beau_gicv3_its_pm.cwriter = beau_gits_read_8(GITS_CWRITER);
			for (idx = 0U; idx < GITS_BASER_NUM; idx++) {
				beau_gicv3_its_pm.baser[idx] =
					beau_gits_read_8(GITS_BASER(idx));
			}
			beau_gicv3_its_pm.propbaser =
				beau_gicr_read_8(rdist, GICR_PROPBASER);
			beau_gicv3_its_pm.pendbaser =
				beau_gicr_read_8(rdist, GICR_PENDBASER);
		}
	}
	if (ret == 0) {
		beau_gicv3_its_pm.valid = true;
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return ret;
}

int32_t arm64_gicv3_its_pm_resume(uint64_t epoch)
{
	uint64_t rdist = arm64_gicv3_redist_base(BSP_CPU_ID);
	uint64_t flags;
	uint32_t idx;
	int32_t ret = 0;
	bool hardware_lost;

	if (epoch == 0UL) {
		return -EINVAL;
	}
	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	if (!beau_gicv3_its_pm.valid) {
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return 0;
	}
	if (beau_gicv3_its_pm.suspend_epoch != epoch) {
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return -EINVAL;
	}
	if (!beau_gicv3_its_pm.active) {
		beau_gicv3_its_pm.valid = false;
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return 0;
	}

	hardware_lost = ((beau_gits_read_4(GITS_CTLR) & GITS_CTLR_EN) == 0U) ||
		((beau_gits_read_8(GITS_CBASER) & GITS_CBASER_VALID) == 0UL);
	if (hardware_lost) {
		beau_gits_write_4(GITS_CTLR, 0U);
		for (idx = 0U; idx < GITS_BASER_NUM; idx++) {
			beau_gits_write_8(GITS_BASER(idx), beau_gicv3_its_pm.baser[idx]);
		}
		beau_gicr_write_8(rdist, GICR_PROPBASER,
			beau_gicv3_its_pm.propbaser);
		beau_gicr_write_8(rdist, GICR_PENDBASER,
			beau_gicv3_its_pm.pendbaser);
		ret = beau_gicv3_its_program_cmdq();
		if (ret == 0) {
			beau_gits_write_4(GITS_CTLR, beau_gicv3_its_pm.ctlr);
			ret = beau_gicv3_its_cmd_mapc();
		}
		for (idx = 0U; (ret == 0) &&
			(idx < BEAU_GICV3_ITS_MAX_DEVS); idx++) {
			const struct beau_gicv3_its_device *dev =
				&beau_gicv3_its_devs[idx];

			if (dev->used) {
				ret = beau_gicv3_its_cmd_mapd(dev->dev_id, idx,
					dev->event_count);
			}
		}
		for (idx = 0U; (ret == 0) &&
			(idx < BEAU_GICV3_ITS_MAX_VECTORS); idx++) {
			const struct beau_gicv3_its_irqsrc *irq =
				&beau_gicv3_its_irqs[idx];

			if (irq->used && irq->programmed) {
				ret = beau_gicv3_its_cmd_mapti(irq->dev_id,
					irq->event_id, irq->lpi);
				if (ret == 0) {
					ret = beau_gicv3_its_cmd_inv(irq->dev_id,
						irq->event_id);
				}
			}
		}
		if (ret == 0) {
			ret = beau_gicv3_its_cmd_sync();
		}
	}
	if (ret == 0) {
		beau_gicv3_its_ready = true;
		beau_gicv3_its_pm.suspend_epoch = 0UL;
		beau_gicv3_its_pm.active = false;
		beau_gicv3_its_pm.valid = false;
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return ret;
}

bool beau_gicv3_its_present(void)
{
	return beau_gicv3_its_ready;
}

void arm64_gicv3_its_get_stats(struct arm64_gicv3_its_stats *stats)
{
	uint64_t flags;
	uint32_t i;

	if (stats == NULL) {
		return;
	}

	(void)memset(stats, 0U, sizeof(*stats));
	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	stats->base = beau_gicv3_its_base;
	stats->size = beau_gicv3_its_size;
	stats->typer = beau_gicv3_its_typer;
	stats->target = beau_gicv3_its_collection_target;
	stats->cmdq_writer = beau_gicv3_its_cmdq_writer;
	stats->vector_capacity = BEAU_GICV3_ITS_MAX_VECTORS;
	stats->alloc_msi_ok = beau_gicv3_its_alloc_msi_ok;
	stats->alloc_msi_fail = beau_gicv3_its_alloc_msi_fail;
	stats->alloc_msix_ok = beau_gicv3_its_alloc_msix_ok;
	stats->alloc_msix_fail = beau_gicv3_its_alloc_msix_fail;
	stats->release_msi = beau_gicv3_its_release_msi;
	stats->release_msix = beau_gicv3_its_release_msix;
	stats->map_event_ok = beau_gicv3_its_map_event_ok;
	stats->map_event_fail = beau_gicv3_its_map_event_fail;
	stats->unmap_event_ok = beau_gicv3_its_unmap_event_ok;
	stats->unmap_event_fail = beau_gicv3_its_unmap_event_fail;
	stats->cmd_issued = beau_gicv3_its_cmd_issued;
	stats->cmd_errors = beau_gicv3_its_cmd_errors;
	stats->cmd_timeouts = beau_gicv3_its_cmd_timeouts;
	stats->cmd_stalls = beau_gicv3_its_cmd_stalls;
	stats->last_ret = beau_gicv3_its_last_ret;
	stats->ready = beau_gicv3_its_ready;
	for (i = 0U; i < BEAU_GICV3_ITS_MAX_VECTORS; i++) {
		if (beau_gicv3_its_irqs[i].used) {
			stats->vectors_used++;
			if (beau_gicv3_its_irqs[i].programmed) {
				stats->vectors_programmed++;
			}
		}
	}
	for (i = 0U; i < BEAU_GICV3_ITS_MAX_DEVS; i++) {
		if (beau_gicv3_its_devs[i].used) {
			stats->devices_used++;
		}
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
}

static int32_t beau_gicv3_its_find_free_run(uint32_t count, uint32_t *first)
{
	uint32_t run = 0U;
	uint32_t start = 0U;
	uint32_t i;

	if ((count == 0U) || (count > BEAU_GICV3_ITS_MAX_VECTORS) || (first == NULL)) {
		return -EINVAL;
	}

	for (i = 0U; i < BEAU_GICV3_ITS_MAX_VECTORS; i++) {
		if (!beau_gicv3_its_irqs[i].used) {
			if (run == 0U) {
				start = i;
			}
			run++;
			if (run == count) {
				*first = start;
				return 0;
			}
		} else {
			run = 0U;
		}
	}

	return -ENOMEM;
}

static struct beau_gicv3_its_irqsrc *beau_gicv3_its_find_irq(uint32_t lpi)
{
	uint32_t idx;

	if ((lpi < GIC_FIRST_LPI) ||
		(lpi >= (GIC_FIRST_LPI + BEAU_GICV3_ITS_MAX_VECTORS))) {
		return NULL;
	}

	idx = lpi - GIC_FIRST_LPI;
	return beau_gicv3_its_irqs[idx].used ? &beau_gicv3_its_irqs[idx] : NULL;
}

static bool beau_gicv3_its_event_busy(uint32_t dev_id, uint32_t event_id)
{
	uint32_t i;

	for (i = 0U; i < BEAU_GICV3_ITS_MAX_VECTORS; i++) {
		if (beau_gicv3_its_irqs[i].used &&
			(beau_gicv3_its_irqs[i].dev_id == dev_id) &&
			(beau_gicv3_its_irqs[i].event_id == event_id)) {
			return true;
		}
	}

	return false;
}

static int32_t beau_gicv3_its_fill_msg_event(uint32_t event_id,
	struct arm64_gicv3_msi_msg *msg)
{
	if (msg == NULL) {
		return -EINVAL;
	}
	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}

	msg->addr = beau_gicv3_its_base + GITS_TRANSLATER;
	msg->data = event_id;

	return 0;
}

static int32_t beau_gicv3_its_fill_msg(const struct beau_gicv3_its_irqsrc *irq,
	struct arm64_gicv3_msi_msg *msg)
{
	if (irq == NULL) {
		return -EINVAL;
	}

	return beau_gicv3_its_fill_msg_event(irq->event_id, msg);
}

int32_t arm64_gicv3_its_alloc_msi(uint32_t dev_id, uint32_t count, uint32_t *first_lpi,
	struct arm64_gicv3_msi_msg *msgs)
{
	uint32_t first;
	uint32_t i;
	uint32_t j;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}
	if ((count == 0U) || (first_lpi == NULL)) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	ret = beau_gicv3_its_find_free_run(count, &first);
	if (ret != 0) {
		beau_gicv3_its_alloc_msi_fail++;
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return ret;
	}

	for (i = 0U; i < count; i++) {
		struct beau_gicv3_its_irqsrc *irq = &beau_gicv3_its_irqs[first + i];

		irq->used = true;
		irq->msix = false;
		irq->dev_id = dev_id;
		irq->event_id = i;
		irq->lpi = GIC_FIRST_LPI + first + i;
		ret = beau_gicv3_its_program_irq(irq);
		if (ret != 0) {
			for (j = 0U; j <= i; j++) {
				beau_gicv3_its_unprogram_irq(&beau_gicv3_its_irqs[first + j]);
			}
			beau_gicv3_its_alloc_msi_fail++;
			spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
			return ret;
		}
		if (msgs != NULL) {
			ret = beau_gicv3_its_fill_msg(irq, &msgs[i]);
			if (ret != 0) {
				for (j = 0U; j <= i; j++) {
					beau_gicv3_its_unprogram_irq(&beau_gicv3_its_irqs[first + j]);
				}
				beau_gicv3_its_alloc_msi_fail++;
				spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
				return ret;
			}
		}
	}

	*first_lpi = GIC_FIRST_LPI + first;
	beau_gicv3_its_alloc_msi_ok++;
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return 0;
}

void arm64_gicv3_its_release_msi(uint32_t dev_id, uint32_t first_lpi, uint32_t count)
{
	uint32_t i;
	uint64_t flags;

	if ((count == 0U) || (first_lpi < GIC_FIRST_LPI)) {
		return;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	for (i = 0U; i < count; i++) {
		struct beau_gicv3_its_irqsrc *irq = beau_gicv3_its_find_irq(first_lpi + i);

		if ((irq != NULL) && !irq->msix && (irq->dev_id == dev_id)) {
			beau_gicv3_its_unprogram_irq(irq);
			beau_gicv3_its_release_msi++;
		}
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
}

int32_t arm64_gicv3_its_alloc_msix(uint32_t dev_id, uint32_t vector, uint32_t *lpi,
	struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint32_t first;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}
	if (lpi == NULL) {
		return -EINVAL;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	if (beau_gicv3_its_event_busy(dev_id, vector)) {
		beau_gicv3_its_alloc_msix_fail++;
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return -EBUSY;
	}

	ret = beau_gicv3_its_find_free_run(1U, &first);
	if (ret != 0) {
		beau_gicv3_its_alloc_msix_fail++;
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return ret;
	}

	irq = &beau_gicv3_its_irqs[first];
	irq->used = true;
	irq->msix = true;
	irq->dev_id = dev_id;
	irq->event_id = vector;
	irq->lpi = GIC_FIRST_LPI + first;
	ret = beau_gicv3_its_program_irq(irq);
	if (ret != 0) {
		irq->used = false;
		beau_gicv3_its_alloc_msix_fail++;
		spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
		return ret;
	}

	if (msg != NULL) {
		ret = beau_gicv3_its_fill_msg(irq, msg);
		if (ret != 0) {
			beau_gicv3_its_unprogram_irq(irq);
			beau_gicv3_its_alloc_msix_fail++;
			spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
			return ret;
		}
	}

	*lpi = irq->lpi;
	beau_gicv3_its_alloc_msix_ok++;
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return 0;
}

void arm64_gicv3_its_release_msix(uint32_t dev_id, uint32_t lpi)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint64_t flags;

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	irq = beau_gicv3_its_find_irq(lpi);
	if ((irq != NULL) && irq->msix && (irq->dev_id == dev_id)) {
		beau_gicv3_its_unprogram_irq(irq);
		beau_gicv3_its_release_msix++;
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);
}

int32_t arm64_gicv3_its_map_msi(uint32_t lpi, struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint64_t flags;
	int32_t ret;

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	irq = beau_gicv3_its_find_irq(lpi);
	ret = beau_gicv3_its_fill_msg(irq, msg);
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return ret;
}

int32_t arm64_gicv3_its_map_lpi_event(uint32_t dev_id, uint32_t event_id,
	uint32_t lpi, struct arm64_gicv3_msi_msg *msg)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	irq = beau_gicv3_its_find_irq(lpi);
	if ((irq == NULL) || !irq->programmed) {
		ret = -EINVAL;
	} else if ((irq->dev_id == dev_id) && (irq->event_id == event_id)) {
		ret = (msg == NULL) ? 0 : beau_gicv3_its_fill_msg(irq, msg);
	} else if (beau_gicv3_its_event_busy(dev_id, event_id)) {
		ret = -EBUSY;
	} else {
		ret = beau_gicv3_its_map_event(dev_id, event_id, lpi);
		if ((ret == 0) && (msg != NULL)) {
			ret = beau_gicv3_its_fill_msg_event(event_id, msg);
		}
	}
	if (ret == 0) {
		beau_gicv3_its_map_event_ok++;
	} else {
		beau_gicv3_its_map_event_fail++;
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return ret;
}

int32_t arm64_gicv3_its_unmap_lpi_event(uint32_t dev_id, uint32_t event_id,
	uint32_t lpi)
{
	struct beau_gicv3_its_irqsrc *irq;
	uint64_t flags;
	int32_t ret;

	if (!beau_gicv3_its_ready) {
		return -ENODEV;
	}

	spinlock_irqsave_obtain(&beau_gicv3_its_lock, &flags);
	irq = beau_gicv3_its_find_irq(lpi);
	if (irq == NULL) {
		ret = -EINVAL;
	} else if ((irq->dev_id == dev_id) && (irq->event_id == event_id)) {
		ret = -EPERM;
	} else {
		ret = beau_gicv3_its_unmap_event(dev_id, event_id);
	}
	if (ret == 0) {
		beau_gicv3_its_unmap_event_ok++;
	} else {
		beau_gicv3_its_unmap_event_fail++;
	}
	spinlock_irqrestore_release(&beau_gicv3_its_lock, flags);

	return ret;
}
