/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <atomic.h>
#include <delay.h>
#include <errno.h>
#include <vm.h>
#include <vcpu.h>
#include <vconfig.h>
#include <cpu.h>
#include <mmu.h>
#include <pgtable.h>
#include <per_cpu.h>
#include <bsp/vfdt.h>
#include <bsp/vpci.h>
#include <logmsg.h>
#include <acrn_hv_defs.h>
#include <bsp/io_req.h>
#include <asm/sysreg.h>
#include <asm/mte.h>
#include <asm/guest/vcpu_priv.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vpl011.h>
#include <asm/guest/vipc.h>
#include <asm/guest/vrproc.h>
#include <asm/guest/vsmmu.h>
#include <asm/vtd.h>
#include <vhost_console.h>
#include <virtio_proxy.h>
#include <vsock_proxy.h>
#include <debug/ramlog.h>

/* [20260630] VM/stage-2 principle:
 *
 * VM memory virtualization is intentionally split from host MMU setup:
 * host stage-1 maps the hypervisor world, while each VM owns a stage-2 root
 * that translates guest IPAs into configured host physical memory. The QEMU
 * RTOS layout keeps the RAM window identity-mapped by design.
 *
 * Device windows such as vGIC and vPL011 are not mapped as RAM. They are left
 * unmapped at stage-2 so guest accesses trap into the common vio MMIO path.
 *
 *   VM config RAM window -> stage-2 identity map -> EL1 normal memory
 *   VM config device IPA -> no stage-2 leaf map  -> data abort -> vio MMIO
 *
 * [20260710] ARM64 VM initialization framework:
 *
 *   common create_vm()
 *          |
 *          v
 *   arch_init_vm()
 *     - init_stage2_identity_map()
 *     - arm64_vgicv3_init_vm()
 *     - virtio/vPL011 backend state
 *     - register_arm64_vio_mmio()
 *          |
 *          v
 *   arch_init_vcpu()
 *          |
 *          v
 *   arch_vm_prepare_bsp()
 *          |
 *          v
 *   arm64_prepare_linux_vcpu_context()
 *
 * Stage-2 controls isolation, while the MMIO registration table controls
 * emulation. A device window must be either mapped as guest RAM or registered
 * as trapped MMIO, never both.
 */
#define ARM64_STAGE2_PAGES_PER_VM	64UL
#define ARM64_STAGE2_PAGE_NUM		(CONFIG_MAX_VM_NUM * ARM64_STAGE2_PAGES_PER_VM)
#define ARM64_STAGE2_RETIRED_WORDS	((ARM64_STAGE2_PAGE_NUM + 63UL) >> 6U)
#define ARM64_STAGE2_SYNC_TIMEOUT_US	100000U
#define ARM64_STAGE2_OWNER_RETRY_US	10U
#define ARM64_STAGE2_OWNER_RETRIES	(ARM64_STAGE2_SYNC_TIMEOUT_US / ARM64_STAGE2_OWNER_RETRY_US)
#define ARM64_STAGE2_ADDRESS_LIMIT	(1UL << 48U)
#define ARM64_STAGE2_MAP_FLAG_MASK	(ARM64_STAGE2_MAP_READ | ARM64_STAGE2_MAP_WRITE | \
	ARM64_STAGE2_MAP_DEVICE | ARM64_STAGE2_MAP_NORMAL)
#define ARM64_STAGE2_STATIC_RAM_LEAVES	(PGTABLE_LEAF_LVL2 | PGTABLE_LEAF_LVL1 | \
	PGTABLE_LEAF_LVL0)
#define ARM64_STAGE2_DYNAMIC_LEAVES	(PGTABLE_LEAF_LVL1 | PGTABLE_LEAF_LVL0)
#define ARM64_STAGE2_PAGE_LEAVES	PGTABLE_LEAF_LVL0

/* A single static pool backs all per-VM stage-2 page tables for QEMU bring-up. */
static struct page_pool stage2_page_pool;
DEFINE_MTE_PAGE_TABLES(stage2_pages, ARM64_STAGE2_PAGE_NUM);
DEFINE_PAGE_TABLE(stage2_pages_bitmap);
static uint8_t stage2_page_tag_states[ARM64_STAGE2_PAGE_NUM];
static uint8_t stage2_zero_page[PAGE_SIZE] __aligned(PAGE_SIZE);
static spinlock_t stage2_page_pool_init_lock;
static bool stage2_page_pool_initialized;
static bool arm64_boot_vdev_logged;

struct arm64_stage2_walk_context {
	struct arm64_stage2_vm_stats *stats;
	uint64_t visited[(ARM64_STAGE2_PAGE_NUM + 63UL) >> 6U];
};

static bool arm64_vm_boot_log_enabled(uint16_t vm_id)
{
	return vm_id <= 2U;
}

static uint64_t arm64_vm_range_end(uint64_t base, uint64_t size)
{
	if (size == 0UL) {
		return base;
	}
	if ((size - 1UL) > (UINT64_MAX - base)) {
		return UINT64_MAX;
	}
	return base + size - 1UL;
}

static bool stage2_large_page_support(enum _page_table_level level, __unused uint64_t prot)
{
	/* [20260807] Stage-2 static RAM leaf selection
	 *
	 * validated identity RAM -> aligned 1 GiB range -> L2 block leaf
	 *                                  |
	 *                                  +--> otherwise use existing L1/L0 leaves
	 *
	 * Key rule:
	 *   - static RAM validation owns the HPA/IPA identity guarantee;
	 *   - the generic walker must prove complete block alignment before publish;
	 *   - dynamic maps remain limited to L1/L0 so BAR and MSI-X trap boundaries
	 *     retain their required isolation granularity.
	 */
	return (level == PGT_LVL2) || (level == PGT_LVL1);
}

static void stage2_flush_cache_pagewalk(const void *entry)
{
	clean_cacheline(entry);
}

static uint64_t stage2_pgentry_present(uint64_t pte)
{
	return pte & PAGE_DESC_VALID;
}

static inline uint64_t stage2_leaf_desc_type(enum _page_table_level level)
{
	return (level == PGT_LVL0) ? PAGE_PAGE_DESC : PAGE_BLOCK_DESC;
}

static uint64_t arm64_stage2_vmid(const struct acrn_vm *vm)
{
	uint64_t vmid = (uint64_t)vm->vm_id + 1UL;

	if (vmid > ARM64_STAGE2_VMID_MASK) {
		panic("vm-%u exceeds arm64 stage-2 vmid range", vm->vm_id);
	}

	return vmid;
}

uint64_t arm64_stage2_vttbr(const struct acrn_vm *vm)
{
	return hva2hpa(vm->root_stg2ptp) |
		(arm64_stage2_vmid(vm) << ARM64_STAGE2_VMID_SHIFT);
}

int32_t arm64_stage2_invalidate_vm_tlb(struct acrn_vm *vm)
{
	uint64_t target_vttbr;
	uint64_t old_vttbr;

	if ((vm == NULL) || (vm->root_stg2ptp == NULL) || (vm->hw.cpu_affinity == 0UL)) {
		return -EINVAL;
	}

	target_vttbr = arm64_stage2_vttbr(vm);
	old_vttbr = read_vttbr_el2();
	write_vttbr_el2(target_vttbr);
	arm64_dsb_ishst();
	arm64_tlbi(vmalls12e1is);
	arm64_dsb_ish();
	arm64_isb();
	write_vttbr_el2(old_vttbr);

	return 0;
}

static int32_t arm64_stage2_sync_translation(struct acrn_vm *vm)
{
	int32_t status;

	status = arm64_stage2_invalidate_vm_tlb(vm);
	if (status == 0) {
		status = arm_smmu_sync_vm_stage2(vm->vm_id,
			hva2hpa(vm->root_stg2ptp));
	}

	return status;
}

static uint64_t arm64_stage2_update_owner(const struct acrn_vm *vm)
{
	return __atomic_load_n(&vm->arch_vm.stage2_update_owner, __ATOMIC_ACQUIRE);
}

static uint64_t arm64_stage2_acquire_update(struct acrn_vm *vm,
	uint64_t ipa, uint64_t size)
{
	uint64_t owner = (uint64_t)get_pcpu_id() + 1UL;
	uint64_t previous = 0UL;
	uint32_t retry;

	for (retry = 0U; retry < ARM64_STAGE2_OWNER_RETRIES; retry++) {
		previous = atomic_cmpxchg64(&vm->arch_vm.stage2_update_owner, 0UL, owner);
		if (previous == 0UL) {
			return owner;
		}
		if (previous == owner) {
			panic("vm-%u reentrant stage-2 update ipa=0x%lx size=0x%lx",
				vm->vm_id, ipa, size);
		}
		udelay(ARM64_STAGE2_OWNER_RETRY_US);
	}

	panic("vm-%u stage-2 update owner timeout ipa=0x%lx size=0x%lx owner:%lu",
		vm->vm_id, ipa, size, previous);
	return owner;
}

static void arm64_stage2_release_update(struct acrn_vm *vm, uint64_t owner)
{
	if (atomic_cmpxchg64(&vm->arch_vm.stage2_update_owner, owner, 0UL) != owner) {
		panic("vm-%u stage-2 update owner corruption", vm->vm_id);
	}
}

static void arm64_stage2_sync_or_panic(struct acrn_vm *vm, uint64_t ipa,
	uint64_t size, const char *phase)
{
	int32_t status = arm64_stage2_sync_translation(vm);

	if (status != 0) {
		panic("vm-%u stage-2 %s sync failed ipa=0x%lx size=0x%lx status:%d",
			vm->vm_id, phase, ipa, size, status);
	}
}

/* [20260720] Stage-2 break-before-make replacement
 *
 *   valid block/page descriptor
 *       -> invalidate and clean while stg2pt_lock is held
 *       -> release stg2pt_lock
 *       -> CPU VMID TLBI and bound SMMU VMID TLBI/CMD_SYNC
 *       -> reacquire stg2pt_lock and publish the replacement
 *
 * Key rule:
 *   - stage2_update_owner excludes a second writer while the lock is released;
 *   - readers either observe the old descriptor, the invalid gap, or the final
 *     descriptor, but never a partially initialized child table;
 *   - synchronization failure leaves the new table allocated and fails closed.
 */
static void stage2_replace_pgentry(uint64_t *pte, uint64_t new_pte,
	uint64_t ipa, uint64_t size, const struct pgtable *table)
{
	struct acrn_vm *vm = table->private_data;
	uint64_t owner = (uint64_t)get_pcpu_id() + 1UL;

	if ((vm == NULL) || (arm64_stage2_update_owner(vm) != owner)) {
		panic("invalid stage-2 BBM owner ipa=0x%lx size=0x%lx", ipa, size);
	}

	sanitize_pte_entry(pte, table);
	spinlock_release(&vm->stg2pt_lock);
	arm64_stage2_sync_or_panic(vm, ipa, size, "BBM");
	spinlock_obtain(&vm->stg2pt_lock);
	if (table->pgentry_present(*pte)) {
		panic("vm-%u stage-2 BBM descriptor republished early ipa=0x%lx",
			vm->vm_id, ipa);
	}
	*pte = new_pte;
	table->flush_cache_pagewalk(pte);
}

/*
 * Stage-2 descriptors use the ARM S2AP and memory-attribute encoding, which
 * differs from EL2 stage-1 descriptors. The common walker handles allocation
 * and splitting; this callback supplies the ARM64-specific leaf descriptor.
 * In stage-2 calls, the walker's "virtual address" argument is the guest IPA
 * and the "physical address" argument is the host PA stored into the stage-2
 * output-address field.
 */
static void stage2_set_pgentry(uint64_t *pte, uint64_t page, uint64_t prot,
	enum _page_table_level level, bool is_leaf, const struct pgtable *table)
{
	uint64_t prot_tmp;

	if (!is_leaf) {
		prot_tmp = PAGE_TABLE_DESC;
	} else {
		prot_tmp = (prot & ~PAGE_DESC_TYPE_MASK) | stage2_leaf_desc_type(level) | PAGE_S2_AF;
		if ((prot_tmp & PAGE_S2_MEMATTR_MASK) == 0UL) {
			prot_tmp |= PAGE_S2_ATTR_NORMAL;
		}
	}

	make_pgentry(pte, page, prot_tmp, table);
}

static void init_stage2_page_pool(void)
{
	int32_t status = 0;

	spinlock_obtain(&stage2_page_pool_init_lock);
	if (!stage2_page_pool_initialized) {
		init_page_pool(&stage2_page_pool, (uint64_t *)stage2_pages,
			(uint64_t *)stage2_pages_bitmap, ARM64_STAGE2_PAGE_NUM);
		status = arm64_mte_register_page_pool(&stage2_page_pool,
			stage2_pages, ARM64_STAGE2_PAGE_NUM, stage2_page_tag_states);
		if (status == 0) {
			stage2_page_pool_initialized = true;
		}
	}
	spinlock_release(&stage2_page_pool_init_lock);
	if (status != 0) {
		panic("MTE: failed to register stage-2 page pool status:%d", status);
	}
}

void arm64_get_stage2_page_pool_stats(struct page_pool_stats *stats)
{
	page_pool_get_stats(&stage2_page_pool, stats);
}

static bool arm64_stage2_page_index(const uint64_t *page, uint64_t *page_index)
{
	uint64_t address = arm64_mte_untag_address((uint64_t)page);
	uint64_t pool_start = (uint64_t)stage2_pages;
	uint64_t pool_end = pool_start + sizeof(stage2_pages);
	bool allocated;

	if ((page == NULL) || (page_index == NULL) ||
		((address & (PAGE_SIZE - 1UL)) != 0UL) ||
		(address < pool_start) || (address >= pool_end)) {
		return false;
	}

	*page_index = (address - pool_start) / PAGE_SIZE;
	spinlock_obtain(&stage2_page_pool.lock);
	allocated = bitmap_test(*page_index, stage2_page_pool.bitmap);
	spinlock_release(&stage2_page_pool.lock);
	return allocated;
}

static void arm64_stage2_count_table(struct arm64_stage2_walk_context *context,
	uint64_t *table, enum _page_table_level level)
{
	uint64_t page_index;
	uint64_t entry_index;

	if ((context == NULL) || (context->stats == NULL) ||
		(level > PGT_LVL0) ||
		!arm64_stage2_page_index(table, &page_index)) {
		if ((context != NULL) && (context->stats != NULL)) {
			context->stats->malformed_entries++;
		}
		return;
	}
	if (bitmap_test(page_index, context->visited)) {
		context->stats->malformed_entries++;
		return;
	}
	bitmap_set_non_atomic(page_index & 0x3fUL,
		&context->visited[page_index >> 6U]);

	switch (level) {
	case PGT_LVL3:
		context->stats->level3_pages++;
		break;
	case PGT_LVL2:
		context->stats->level2_pages++;
		break;
	case PGT_LVL1:
		context->stats->level1_pages++;
		break;
	case PGT_LVL0:
		context->stats->level0_pages++;
		break;
	default:
		break;
	}
	context->stats->total_pages++;

	if (level == PGT_LVL0) {
		return;
	}
	for (entry_index = 0UL; entry_index < PTRS_PER_PGTL0E; entry_index++) {
		uint64_t entry = table[entry_index];

		if (!stage2_pgentry_present(entry) || (is_pgtl_large(entry) != 0UL)) {
			continue;
		}
		arm64_stage2_count_table(context, page_addr(entry),
			(enum _page_table_level)((uint32_t)level + 1U));
	}
}

bool arm64_get_stage2_vm_stats(struct acrn_vm *vm,
	struct arm64_stage2_vm_stats *stats)
{
	struct arm64_stage2_walk_context context;

	if ((vm == NULL) || (stats == NULL) || (vm->root_stg2ptp == NULL)) {
		return false;
	}

	(void)memset(&context, 0U, sizeof(context));
	(void)memset(stats, 0U, sizeof(*stats));
	context.stats = stats;
	stats->root_address = (uint64_t)vm->root_stg2ptp;

	spinlock_obtain(&vm->stg2pt_lock);
	if (arm64_stage2_update_owner(vm) != 0UL) {
		spinlock_release(&vm->stg2pt_lock);
		return false;
	}
	arm64_stage2_count_table(&context, vm->root_stg2ptp, PGT_LVL3);
	spinlock_release(&vm->stg2pt_lock);

	return stats->total_pages != 0UL;
}

static enum arm64_memory_type arm64_stage2_memory_type(uint64_t memattr)
{
	enum arm64_memory_type type;

	switch (memattr) {
	case PAGE_S2_MEMATTR_DEVICE_nGnRnE:
		type = ARM64_MEMORY_DEVICE_nGnRnE;
		break;
	case PAGE_S2_MEMATTR_DEVICE_nGnRE:
		type = ARM64_MEMORY_DEVICE_nGnRE;
		break;
	case PAGE_S2_MEMATTR_DEVICE_nGRE:
		type = ARM64_MEMORY_DEVICE_nGRE;
		break;
	case PAGE_S2_MEMATTR_DEVICE_GRE:
		type = ARM64_MEMORY_DEVICE_GRE;
		break;
	default:
		type = ARM64_MEMORY_NORMAL;
		break;
	}

	return type;
}

/* [20260717] Stage-2 memory-type query
 *
 *   guest IPA -> locked stage-2 leaf -> MemAttr[3:0] -> memory type
 *
 * Key rule:
 *   - stg2pt_lock keeps the lookup coherent with dynamic device map/unmap;
 *   - only the copied leaf is decoded after releasing the lock;
 *   - an absent leaf is a valid Unmapped result used by emulated MMIO ranges.
 */
bool arm64_get_stage2_memory_attr(struct acrn_vm *vm, uint64_t ipa,
	struct arm64_memory_attr *attr)
{
	const uint64_t *entry;
	uint64_t pg_size = 0UL;
	uint64_t descriptor = 0UL;
	uint64_t memattr;
	bool mapped;

	if ((vm == NULL) || (attr == NULL) || (vm->root_stg2ptp == NULL)) {
		return false;
	}
	attr->type = ARM64_MEMORY_UNKNOWN;
	attr->encoding = 0U;

	spinlock_obtain(&vm->stg2pt_lock);
	if (arm64_stage2_update_owner(vm) != 0UL) {
		spinlock_release(&vm->stg2pt_lock);
		return false;
	}
	entry = pgtable_lookup_entry((uint64_t *)vm->root_stg2ptp, ipa,
		&pg_size, &vm->stg2_pgtable);
	mapped = entry != NULL;
	if (mapped) {
		descriptor = *entry;
	}
	spinlock_release(&vm->stg2pt_lock);

	if (!mapped) {
		attr->type = ARM64_MEMORY_UNMAPPED;
		return true;
	}

	memattr = descriptor & PAGE_S2_MEMATTR_MASK;
	attr->type = arm64_stage2_memory_type(memattr);
	attr->encoding = (uint8_t)(memattr >> 2U);

	return true;
}

static uint64_t arm64_stage2_walk_index(enum _page_table_level level,
	uint64_t ipa)
{
	uint64_t index;

	switch (level) {
	case PGT_LVL3:
		index = pgtl3e_index(ipa);
		break;
	case PGT_LVL2:
		index = pgtl2e_index(ipa);
		break;
	case PGT_LVL1:
		index = pgtl1e_index(ipa);
		break;
	case PGT_LVL0:
		index = pgtl0e_index(ipa);
		break;
	default:
		index = 0UL;
		break;
	}

	return index;
}

static bool arm64_stage2_walk_child_table(uint64_t descriptor,
	uint64_t **table)
{
	uint64_t page_index;
	uint64_t hpa = arch_pgtl_page_paddr(descriptor);
	uint64_t *next = hpa2hva(hpa);

	if ((next == NULL) || !arm64_stage2_page_index(next, &page_index)) {
		return false;
	}
	*table = next;

	return true;
}

static bool arm64_stage2_walk_set_leaf(struct arm64_stage2_walk *walk,
	enum _page_table_level level, uint64_t descriptor)
{
	uint64_t range_size = get_level_size(level);
	uint64_t hpa_base = arch_pgtl_page_paddr(descriptor);
	uint64_t offset;

	if ((range_size == 0UL) || (hpa_base >= ARM64_STAGE2_ADDRESS_LIMIT) ||
		(range_size > (ARM64_STAGE2_ADDRESS_LIMIT - hpa_base))) {
		return false;
	}
	offset = walk->ipa & (range_size - 1UL);
	if (offset > (UINT64_MAX - hpa_base)) {
		return false;
	}
	walk->leaf_level = (uint8_t)level;
	walk->range_size = range_size;
	walk->range_start = walk->ipa & ~(range_size - 1UL);
	walk->hpa = hpa_base + offset;
	walk->result = ARM64_STAGE2_WALK_MAPPED;

	return true;
}

/* [20260722] Read-only Stage-2 walk snapshot
 *
 * shell IPA request -> stg2pt_lock -> copy validated L3..L0 descriptors
 *                                      |
 *                                      +--> invalid/unallocated child: malformed
 *                                      |
 *                                      v
 *                              unlock -> format private snapshot
 *
 * Key rule:
 *   - the VM owns root_stg2ptp and stg2pt_lock serializes the descriptor view;
 *   - a dynamic map/unmap owner prevents a shell walk from sampling a
 *     break-before-make invalidation window;
 *   - every child HPA must resolve to an allocated Stage-2 pool page before it
 *     is dereferenced, so a corrupt descriptor cannot redirect EL2 reads.
 */
int32_t arm64_stage2_walk(struct acrn_vm *vm, uint64_t ipa,
	struct arm64_stage2_walk *walk)
{
	uint64_t page_index;
	uint64_t *table;
	enum _page_table_level level;
	int32_t ret = 0;

	if ((vm == NULL) || (walk == NULL) || (ipa >= ARM64_STAGE2_ADDRESS_LIMIT)) {
		return -EINVAL;
	}
	if (vm->root_stg2ptp == NULL) {
		return -ENODEV;
	}
	(void)memset(walk, 0U, sizeof(*walk));
	walk->ipa = ipa;
	walk->result = ARM64_STAGE2_WALK_MALFORMED;

	spinlock_obtain(&vm->stg2pt_lock);
	if (arm64_stage2_update_owner(vm) != 0UL) {
		ret = -EBUSY;
		goto out;
	}
	table = (uint64_t *)vm->root_stg2ptp;
	if (!arm64_stage2_page_index(table, &page_index)) {
		ret = -EFAULT;
		goto out;
	}
	walk->vttbr = arm64_stage2_vttbr(vm);

	for (level = PGT_LVL3; level <= PGT_LVL0;
		level = (enum _page_table_level)((uint32_t)level + 1U)) {
		struct arm64_stage2_walk_step *step =
			&walk->step[walk->step_count];
		uint64_t descriptor;
		uint64_t type;

		step->table_hpa = hva2hpa(table);
		step->index = (uint16_t)arm64_stage2_walk_index(level, ipa);
		step->level = (uint8_t)level;
		descriptor = table[step->index];
		step->descriptor = descriptor;
		walk->step_count++;
		walk->leaf_level = (uint8_t)level;

		if (!stage2_pgentry_present(descriptor)) {
			walk->leaf_level = (uint8_t)level;
			walk->result = ARM64_STAGE2_WALK_UNMAPPED;
			break;
		}
		type = descriptor & PAGE_DESC_TYPE_MASK;
		if (level == PGT_LVL0) {
			if ((type != PAGE_PAGE_DESC) ||
				!arm64_stage2_walk_set_leaf(walk, level, descriptor)) {
				walk->result = ARM64_STAGE2_WALK_MALFORMED;
			}
			break;
		}
		if (type == PAGE_BLOCK_DESC) {
			if ((level == PGT_LVL3) ||
				!arm64_stage2_walk_set_leaf(walk, level, descriptor)) {
				walk->result = ARM64_STAGE2_WALK_MALFORMED;
			}
			break;
		}
		if ((type != PAGE_TABLE_DESC) ||
			!arm64_stage2_walk_child_table(descriptor, &table)) {
			walk->result = ARM64_STAGE2_WALK_MALFORMED;
			break;
		}
	}

out:
	spinlock_release(&vm->stg2pt_lock);
	return ret;
}

static bool arm64_vm_uses_virtio_console(const struct acrn_vm_config *vm_config)
{
	/*
	 * Console transport policy:
	 *
	 *   RTOS  -> vPL011 serial frontend
	 *   Linux -> virtio-console frontend when the platform provides it
	 *
	 * The guest-facing frontend is strict, while both paths share the BEAU
	 * console ring and vsh backend after bytes leave the device model.
	 */
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_console_size != 0UL);
}

static bool arm64_vm_uses_virtio_proxy(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_proxy_num != 0U);
}

static bool arm64_vm_uses_vpci(const struct acrn_vm_config *vm_config)
{
	return (vm_config->pci_devs != NULL) && (vm_config->pci_dev_num != 0U);
}

static void arm64_vm_log_boot_vdevs(const struct acrn_vm *vm,
	const struct acrn_vm_config *vm_config)
{
	const struct arch_vm_config *arch_config = &vm_config->arch;

	if (arm64_boot_vdev_logged || !arm64_vm_boot_log_enabled(vm->vm_id)) {
		return;
	}

	arm64_boot_vdev_logged = true;
	LOG_INF("vGICv3: GICR         [0x%016lx-0x%016lx] (0x%08lx)",
		arch_config->guest_gicr_base,
		arm64_vm_range_end(arch_config->guest_gicr_base, arch_config->guest_gicr_size),
		arch_config->guest_gicr_size);
	LOG_INF("vGICv3: GICD         [0x%016lx-0x%016lx] (0x%08lx)",
		arch_config->guest_gicd_base,
		arm64_vm_range_end(arch_config->guest_gicd_base, arch_config->guest_gicd_size),
		arch_config->guest_gicd_size);
	if (arch_config->guest_its_size != 0UL) {
		LOG_INF("vGICv3: ITS          [0x%016lx-0x%016lx] (0x%08lx)",
			arch_config->guest_its_base,
			arm64_vm_range_end(arch_config->guest_its_base,
				arch_config->guest_its_size),
			arch_config->guest_its_size);
	}
	if (!arm64_vm_uses_virtio_console(vm_config) && (arch_config->guest_uart_size != 0UL)) {
		LOG_INF("vUART:               [0x%016lx-0x%016lx] (0x%08lx)",
			arch_config->guest_uart_base,
			arm64_vm_range_end(arch_config->guest_uart_base,
				arch_config->guest_uart_size),
			arch_config->guest_uart_size);
	}
}

static void validate_stage2_ram_identity(const struct acrn_vm *vm, uint64_t mem_start,
	uint64_t mem_hpa, uint64_t mem_size)
{
	/*
	 * The QEMU static RTOS layout keeps stage-2 simple: IPA and PA are a
	 * 1:1 mapping for the configured RAM window. Multiple RTOS VMs can use
	 * the same physical window only because the current setup is a bring-up
	 * target; Linux/post-launch memory ownership must move to a real loader
	 * and non-overlapping configured regions.
	 */
	if (mem_hpa != mem_start) {
		panic("vm-%u stage-2 ram is not 1:1 ipa=0x%lx pa=0x%lx",
			vm->vm_id, mem_start, mem_hpa);
	}
	if ((mem_size == 0UL) || (mem_start >= ARM64_STAGE2_ADDRESS_LIMIT) ||
		(mem_hpa >= ARM64_STAGE2_ADDRESS_LIMIT) ||
		(mem_size > (ARM64_STAGE2_ADDRESS_LIMIT - mem_start)) ||
		(mem_size > (ARM64_STAGE2_ADDRESS_LIMIT - mem_hpa))) {
		panic("vm-%u has invalid stage-2 ram window", vm->vm_id);
	}
	if (!mem_aligned_check(mem_start, PAGE_SIZE) ||
		!mem_aligned_check(mem_hpa, PAGE_SIZE) ||
		!mem_aligned_check(mem_size, PAGE_SIZE)) {
		panic("vm-%u stage-2 ram is not page aligned ipa=0x%lx pa=0x%lx size=0x%lx",
			vm->vm_id, mem_start, mem_hpa, mem_size);
	}
}

static void arm64_stage2_complete_update(struct acrn_vm *vm, uint64_t ipa,
	uint64_t size, struct pgtable_update *update);

static void arm64_stage2_add_map(struct acrn_vm *vm, uint64_t hpa, uint64_t ipa,
	uint64_t size, uint64_t prot, uint32_t allowed_leaf_levels,
	struct pgtable_update *update)
{
	const struct pgtable_map_request request = {
		.paddr_base = hpa,
		.vaddr_base = ipa,
		.size = size,
		.prot = prot,
		.allowed_leaf_levels = allowed_leaf_levels,
	};
	int32_t status;

	status = pgtable_add_map_deferred((uint64_t *)vm->root_stg2ptp, &request,
		&vm->stg2_pgtable, update);
	if (status != 0) {
		panic("vm-%u stage-2 map request failed ipa=0x%lx size=0x%lx status:%d",
			vm->vm_id, ipa, size, status);
	}
}

static void init_stage2_identity_map(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t mem_start = arch_config->guest_ram_start;
	uint64_t mem_size = arch_config->guest_ram_size;
	uint64_t mem_hpa = arch_config->guest_ram_hpa;

	static const struct pgtable stage2_pgtable_template = {
		.pool = &stage2_page_pool,
		.large_page_support = stage2_large_page_support,
		.pgentry_present = stage2_pgentry_present,
		.flush_cache_pagewalk = stage2_flush_cache_pagewalk,
		.set_pgentry = stage2_set_pgentry,
		.replace_pgentry = stage2_replace_pgentry,
	};

	init_stage2_page_pool();

	__atomic_store_n(&vm->arch_vm.stage2_update_owner, 0UL, __ATOMIC_RELEASE);
	vm->stg2_pgtable = stage2_pgtable_template;
	vm->stg2_pgtable.private_data = vm;
	vm->root_stg2ptp = pgtable_create_root(&vm->stg2_pgtable);
	if (vm->root_stg2ptp == NULL) {
		panic("failed to create arm64 stage-2 root page table");
	}

	validate_stage2_ram_identity(vm, mem_start, mem_hpa, mem_size);

	/*
	 * The current QEMU layout gives each guest a simple RAM IPA window. KISS:
	 * map guest-visible IPA to the same physical address and leave device
	 * windows unmapped so they trap into vio emulation.
	 *
	 * The platform contract is:
	 *   guest_ram_start = first guest IPA/GPA
	 *   guest_ram_hpa   = first host physical address
	 *   guest_ram_size  = mapped byte length
	 *
	 * validate_stage2_ram_identity() enforces guest_ram_hpa == guest_ram_start.
	 * Therefore the common walker call below receives identical HPA and IPA
	 * bases and emits stage-2 block/page descriptors whose output address is
	 * the same number the guest uses as its IPA. That is the VM RAM 1:1 map.
	 */
	arm64_stage2_add_map(vm, mem_hpa, mem_start, mem_size,
		PAGE_S2_ATTR_NORMAL | PAGE_BLOCK_DESC, ARM64_STAGE2_STATIC_RAM_LEAVES,
		NULL);

	arm64_stage2_add_map(vm, hva2hpa(stage2_zero_page), 0UL, PAGE_SIZE,
		PAGE_S2_MEMATTR_NORMAL | PAGE_S2_S2AP_READ | PAGE_S2_SH_INNER |
		PAGE_S2_AF, ARM64_STAGE2_PAGE_LEAVES, NULL);

	/*
	 * Device IPA ranges stay unmapped at stage-2 and are registered below as
	 * vio MMIO. The absence of a stage-2 leaf mapping is what makes EL1 device
	 * accesses exit to EL2 for emulation.
	 */
}

static uint64_t arm64_stage2_map_prot(uint32_t flags)
{
	uint64_t prot = PAGE_S2_AF | PAGE_S2_SH_INNER | PAGE_S2_XN;

	prot |= ((flags & ARM64_STAGE2_MAP_DEVICE) != 0U) ?
		PAGE_S2_MEMATTR_DEVICE : PAGE_S2_MEMATTR_NORMAL;
	if ((flags & ARM64_STAGE2_MAP_READ) != 0U) {
		prot |= PAGE_S2_S2AP_READ;
	}
	if ((flags & ARM64_STAGE2_MAP_WRITE) != 0U) {
		prot |= PAGE_S2_S2AP_WRITE;
	}

	return prot | PAGE_BLOCK_DESC;
}

static void arm64_stage2_validate_range(const struct acrn_vm *vm,
	uint64_t hpa, uint64_t ipa, uint64_t size, bool has_hpa)
{
	if ((vm == NULL) || (vm->root_stg2ptp == NULL) || (size == 0UL) ||
		!mem_aligned_check(ipa, PAGE_SIZE) ||
		!mem_aligned_check(size, PAGE_SIZE) ||
		(ipa >= ARM64_STAGE2_ADDRESS_LIMIT) ||
		(size > (ARM64_STAGE2_ADDRESS_LIMIT - ipa))) {
		panic("invalid arm64 stage-2 range ipa=0x%lx size=0x%lx", ipa, size);
	}
	if (has_hpa && (!mem_aligned_check(hpa, PAGE_SIZE) ||
		(hpa >= ARM64_STAGE2_ADDRESS_LIMIT) ||
		(size > (ARM64_STAGE2_ADDRESS_LIMIT - hpa)))) {
		panic("invalid arm64 stage-2 output range hpa=0x%lx size=0x%lx",
			hpa, size);
	}
}

void arm64_stage2_map(struct acrn_vm *vm, uint64_t hpa, uint64_t ipa,
	uint64_t size, uint32_t flags)
{
	uint64_t retired_bitmap[ARM64_STAGE2_RETIRED_WORDS];
	struct pgtable_update update;
	uint64_t owner;
	uint64_t prot;

	arm64_stage2_validate_range(vm, hpa, ipa, size, true);
	if (((flags & (ARM64_STAGE2_MAP_DEVICE | ARM64_STAGE2_MAP_NORMAL)) == 0U) ||
		((flags & (ARM64_STAGE2_MAP_DEVICE | ARM64_STAGE2_MAP_NORMAL)) ==
			(ARM64_STAGE2_MAP_DEVICE | ARM64_STAGE2_MAP_NORMAL)) ||
		((flags & (ARM64_STAGE2_MAP_READ | ARM64_STAGE2_MAP_WRITE)) == 0U) ||
		((flags & ~ARM64_STAGE2_MAP_FLAG_MASK) != 0U)) {
		panic("invalid arm64 stage-2 map flags 0x%x", flags);
	}

	prot = arm64_stage2_map_prot(flags);
	owner = arm64_stage2_acquire_update(vm, ipa, size);
	pgtable_update_init(&update, retired_bitmap, ARRAY_SIZE(retired_bitmap),
		&vm->stg2_pgtable);
	spinlock_obtain(&vm->stg2pt_lock);
	arm64_stage2_add_map(vm, hpa, ipa, size, prot, ARM64_STAGE2_DYNAMIC_LEAVES,
		&update);
	spinlock_release(&vm->stg2pt_lock);
	arm64_stage2_complete_update(vm, ipa, size, &update);
	arm64_stage2_release_update(vm, owner);
}

/* [20260726] Stage-2 update completion
 *
 *   VM_CREATING descriptor update
 *       -> clean page-walk memory only
 *       -> no vCPU VTTBR or DMA domain is published
 *
 *   VM_CREATED/RUNNING descriptor update
 *       |
 *       v
 *   CPU VMID TLBI + SMMU TLBI/CMD_SYNC
 *       |
 *       +--> fail closed: retain detached table and stop
 *       |
 *       v
 *   return retired table page to the shared pool
 *
 * Key rule:
 *   - the update owner retains exclusive write access until synchronization ends;
 *   - only a published VM requires CPU and DMA translation synchronization;
 *   - boot-time maps stay private until vCPU and device ownership publication;
 *   - no detached table can be retagged or reused after a failed sync.
 */
static bool arm64_stage2_sync_required(const struct acrn_vm *vm)
{
	return (vm->lifecycle.phase != VM_LIFECYCLE_CREATING) ||
		(vm->hw.created_vcpus != 0U);
}

static void arm64_stage2_complete_update(struct acrn_vm *vm, uint64_t ipa,
	uint64_t size, struct pgtable_update *update)
{
	if (update == NULL) {
		panic("vm-%u stage-2 unmap has no update batch", vm->vm_id);
	}
	if ((update->retired_pages != 0UL) && !update->changed) {
		panic("vm-%u stage-2 unmap retire without descriptor update ipa=0x%lx pages:%lu",
			vm->vm_id, ipa, update->retired_pages);
	}
	if (update->changed && arm64_stage2_sync_required(vm)) {
		arm64_stage2_sync_or_panic(vm, ipa, size, "unmap");
	}
	if (update->retired_pages != 0UL) {
		pgtable_free_retired_pages(update, &vm->stg2_pgtable);
	}
}

void arm64_stage2_unmap(struct acrn_vm *vm, uint64_t ipa, uint64_t size)
{
	uint64_t retired_bitmap[ARM64_STAGE2_RETIRED_WORDS];
	struct pgtable_update update;
	uint64_t owner;

	arm64_stage2_validate_range(vm, 0UL, ipa, size, false);
	owner = arm64_stage2_acquire_update(vm, ipa, size);
	pgtable_update_init(&update, retired_bitmap, ARRAY_SIZE(retired_bitmap),
		&vm->stg2_pgtable);
	spinlock_obtain(&vm->stg2pt_lock);
	pgtable_modify_or_del_map_deferred((uint64_t *)vm->root_stg2ptp,
		ipa, size, 0UL, 0UL, &vm->stg2_pgtable, MR_DEL, &update);
	spinlock_release(&vm->stg2pt_lock);
	arm64_stage2_complete_update(vm, ipa, size, &update);
	arm64_stage2_release_update(vm, owner);
}

static void register_arm64_vio_mmio(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t gicd_base = arch_config->guest_gicd_base;
	uint64_t gicr_base = arch_config->guest_gicr_base;
	uint64_t its_base = arch_config->guest_its_base;
	uint64_t its_size = arch_config->guest_its_size;
	uint64_t uart_base = arch_config->guest_uart_base;
	uint64_t vhost_console_base = arch_config->guest_virtio_console_base;
	uint64_t smmu_base = arch_config->guest_smmu_base;

	/*
	 * The common IO request layer owns dispatch by GPA range. ARM64 registers
	 * the guest interrupt-controller and UART windows here so data abort exits
	 * can be converted into device-specific emulation callbacks.
	 *
	 *   guest load/store device IPA
	 *              |
	 *              v
	 *   stage-2 no-map data abort
	 *              |
	 *              v
	 *   vcpu_exit.c builds MMIO ioreq
	 *              |
	 *              v
	 *   io_req.c range lookup
	 *              |
	 *              v
	 *   vGIC / vPL011 / virtio handler
	 */
	register_mmio_emul_handler(vm, arm64_vgicv3_mmio_handler,
		gicd_base, gicd_base + arch_config->guest_gicd_size,
		&vm->arch_vm.vgic, false);
	register_mmio_emul_handler(vm, arm64_vgicv3_mmio_handler,
		gicr_base, gicr_base + arch_config->guest_gicr_size,
		&vm->arch_vm.vgic, false);
	if (its_size != 0UL) {
		register_mmio_emul_handler(vm, arm64_vgicv3_mmio_handler,
			its_base, its_base + its_size, &vm->arch_vm.vgic, false);
	}
	if (arm64_vsmmu_available(vm)) {
		register_mmio_emul_handler(vm, arm64_vsmmu_mmio_handler,
			smmu_base, smmu_base + arch_config->guest_smmu_size,
			vm, false);
	}
	if (arm64_vm_uses_virtio_console(get_vm_config(vm->vm_id))) {
		register_mmio_emul_handler(vm, vhost_console_mmio_handler,
			vhost_console_base,
			vhost_console_base + arch_config->guest_virtio_console_size,
			vm, false);
	} else {
		register_mmio_emul_handler(vm, arm64_vpl011_mmio_handler,
			uart_base, uart_base + arch_config->guest_uart_size,
			vm, false);
	}
	if (arm64_vm_uses_virtio_proxy(get_vm_config(vm->vm_id))) {
		for (uint16_t i = 0U; i < arch_config->guest_virtio_proxy_num; i++) {
			void *proxy;
			uint64_t base = arch_config->guest_virtio_proxy[i].base;
			uint64_t size = arch_config->guest_virtio_proxy[i].size;

			if (arch_config->guest_virtio_proxy[i].device_id ==
				VIRTIO_DEVICE_ID_VSOCK) {
				proxy = vsock_proxy_get_dev(vm);
				register_mmio_emul_handler(vm, vsock_proxy_mmio_handler,
					base, base + size, proxy, false);
			} else {
				proxy = virtio_proxy_get_dev(vm, i);
				register_mmio_emul_handler(vm, virtio_proxy_mmio_handler,
					base, base + size, proxy, false);
			}
		}
	}
}

uint64_t vcpu_get_vmpidr(struct acrn_vcpu *vcpu)
{
	return vcpu->vcpu_id;
}

struct acrn_vcpu *vcpu_from_vmpidr(struct acrn_vm *vm, uint64_t vmpidr)
{
	uint16_t vcpu_id = (uint16_t)(vmpidr & MPIDR_AFFINITY_MASK);

	if (vcpu_id >= vm->hw.created_vcpus) {
		return NULL;
	}

	return vcpu_from_vid(vm, vcpu_id);
}

int32_t arch_init_vm(struct acrn_vm *vm, struct acrn_vm_config *vm_config)
{
	(void)vm_config;

	/*
	 * Initialization order matters: stage-2 creates the trap boundaries first,
	 * then virtual devices create their software state and register MMIO
	 * handlers against those trapped ranges.
	 */
	init_stage2_identity_map(vm);
	arm64_vgicv3_init_vm(vm, vm_config->cpu_affinity);
	if ((vm_config->arch.guest_smmu_size == 0UL) ||
		arm_smmu_assignment_ready()) {
		arm64_vsmmu_init_vm(vm);
	} else {
		LOG_WRN("vSMMUv3: hide vm%u instance: physical S2 isolation unavailable",
			vm->vm_id);
	}
	arm64_vipc_init_vm(vm);
	arm64_vrproc_init_vm(vm);
	if (arm64_vm_uses_virtio_console(vm_config)) {
		vhost_console_init_vm(vm);
	} else {
		arm64_vpl011_init_vm(vm);
	}
	if (arm64_vm_uses_virtio_proxy(vm_config)) {
		virtio_proxy_init_vm(vm);
		vsock_proxy_init_vm(vm);
	}
	if (arm64_vm_uses_vpci(vm_config) && (init_vpci(vm) != 0)) {
		panic("failed to initialize arm64 vPCI for vm%u", vm->vm_id);
	}
	register_arm64_vio_mmio(vm);
	arm64_vrproc_register_mmio(vm);
	arm64_vm_log_boot_vdevs(vm, vm_config);

	if (is_static_configured_vm(vm) && (vm_config->fdt_config.fdt_mod_tag[0] == '\0')) {
		init_service_vm_vfdt(vm);
	}

	vm->arch_vm.time_delta = -(int64_t)cpu_ticks();
	return 0;
}

int32_t arch_deinit_vm(struct acrn_vm *vm)
{
	arm64_vsmmu_deinit_vm(vm);
	if (arm64_vm_uses_vpci(get_vm_config(vm->vm_id))) {
		deinit_vpci(vm);
	}
	virtio_proxy_release_vm(vm);
	vsock_proxy_release_vm(vm);
	return 0;
}

int32_t arch_reset_vm(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu = NULL;
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	int32_t ret;

	/* [20260801] ARM64 VM reset coherency boundary
	 *
	 * A VM reset must retire translations from the previous VMID instance before
	 * clearing per-vCPU execution state and per-VM device state. Otherwise a new
	 * kernel can inherit stale EL1 translations, pending interrupts, virtqueue
	 * indices, or outstanding IO request slots.
	 *
	 *   pause all vCPUs (common)
	 *          |
	 *          v
	 *   broadcast invalidation for the old VMID translation regime
	 *          |
	 *          +--> failure: keep VM stopped
	 *          |
	 *          v
	 *   reset ioreq + vGIC + virtio transport state
	 *          |
	 *          v
	 *   reset each vCPU boot context
	 *          |
	 *          v
	 *   start_vm() prepares and wakes BSP
	 *
	 * Key rule:
	 *   - the VM lifecycle lock owns the reset while every vCPU is quiesced;
	 *   - old EL1/Stage-2 translations retire before reset state is published;
	 *   - the inner-shareable broadcast completes without waiting for a target
	 *     pCPU callback that may itself be running another guest.
	 */
	(void)ramlog_capture_vm_pstore(vm);
	ret = arm64_stage2_invalidate_vm_tlb(vm);
	if (ret != 0) {
		LOG_ERR("VM%u: reset TLB invalidation failed ret=%d", vm->vm_id, ret);
		return ret;
	}
	virtio_proxy_release_vm(vm);
	vsock_proxy_release_vm(vm);
	reset_vm_ioreqs(vm);
	arm64_vgicv3_init_vm(vm, vm_config->cpu_affinity);
	if ((vm_config->arch.guest_smmu_size == 0UL) ||
		arm_smmu_assignment_ready()) {
		arm64_vsmmu_reset_vm(vm);
	} else {
		arm64_vsmmu_deinit_vm(vm);
	}
	if (arm64_vm_uses_virtio_console(vm_config)) {
		vhost_console_reset_vm(vm);
	} else {
		arm64_vpl011_reset_vm(vm);
	}
	if (arm64_vm_uses_virtio_proxy(vm_config)) {
		virtio_proxy_reset_vm(vm);
		vsock_proxy_reset_vm(vm);
	}
	vm->arch_vm.time_delta = -(int64_t)cpu_ticks();

	foreach_vcpu(i, vm, vcpu) {
		reset_vcpu(vcpu);
	}

	return 0;
}

void arch_vm_prepare_bsp(struct acrn_vcpu *vcpu)
{
	struct acrn_vm *vm = vcpu->vm;
	uint64_t entry = (uint64_t)vm->sw.kernel_info.kernel_entry_addr;

	/*
	 * GUEST_FLAG_NO_FW only means no external ACPI/FDT module is required.
	 * Static QEMU raw images still consume the synthetic vFDT boot ABI.
	 */
#if CONFIG_STATIC_VFDT
	arm64_prepare_linux_vcpu_context(vcpu, entry, (uint64_t)vm->sw.fdt_info.load_addr);
#else
	arm64_prepare_linux_vcpu_context(vcpu, entry, vcpu_get_vmpidr(vcpu));
	vcpu->arch.regs.x0 = vcpu_get_vmpidr(vcpu);
	vcpu->arch.regs.x1 = (uint64_t)vm->sw.fdt_info.load_addr;
#endif
}

void arch_trigger_level_intr(struct acrn_vm *vm, uint32_t irq, bool assert)
{
	struct acrn_vcpu *vcpu;
	uint16_t idx;

	if (assert) {
		vcpu = vcpu_from_vid(vm, BSP_CPU_ID);
		if ((vcpu != NULL) && (vcpu_get_state(vcpu) != VCPU_OFFLINE)) {
			(void)arm64_vgicv3_inject_irq(vcpu, irq, true);
		}
	} else {
		foreach_vcpu(idx, vm, vcpu) {
			(void)arm64_vgicv3_deassert_irq(vcpu, irq);
		}
	}
}
