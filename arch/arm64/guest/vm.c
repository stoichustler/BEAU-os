/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vm.h>
#include <vcpu.h>
#include <vm_config.h>
#include <cpu.h>
#include <mmu.h>
#include <pgtable.h>
#include <per_cpu.h>
#include <bsp/vfdt.h>
#include <logmsg.h>
#include <sprintf.h>
#include <bsp/io_req.h>
#include <asm/sysreg.h>
#include <asm/guest/vcpu_priv.h>
#include <asm/guest/vgicv3.h>
#include <asm/guest/vpl011.h>
#include <virtio_console.h>
#include <virtio_proxy.h>

/*
 * 2026-06-30, VM/stage-2 principle:
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
 */
#define ARM64_STAGE2_PAGES_PER_VM	64UL
#define ARM64_STAGE2_PAGE_NUM		(CONFIG_MAX_VM_NUM * ARM64_STAGE2_PAGES_PER_VM)

/* A single static pool backs all per-VM stage-2 page tables for QEMU bring-up. */
static struct page_pool stage2_page_pool;
DEFINE_PAGE_TABLES(stage2_pages, ARM64_STAGE2_PAGE_NUM);
DEFINE_PAGE_TABLE(stage2_pages_bitmap);
static uint8_t stage2_zero_page[PAGE_SIZE] __aligned(PAGE_SIZE);
static bool stage2_page_pool_initialized;

#ifdef CONFIG_LOG_VERBOSE
static void log_stage2_map(const struct acrn_vm *vm, const char *name,
	uint64_t ipa, uint64_t pa, uint64_t size)
{
	LOG_INF("vm-%u stage-2 map (%6s) gpa[0x%08lx-0x%08lx]:hpa[0x%08lx-0x%08lx]",
		vm->vm_id, name, ipa, ipa + size, pa, pa + size);
}

static void log_stage2_vio(const struct acrn_vm *vm, const char *name,
	uint64_t ipa, uint64_t size)
{
	LOG_INF("vm-%u stage-2 vio (%6s) gpa[0x%08lx-0x%08lx]",
		vm->vm_id, name, ipa, ipa + size);
}
#else
#define log_stage2_map(vm, name, ipa, pa, size)
#define log_stage2_vio(vm, name, ipa, size)
#endif /* CONFIG_LOG_VERBOSE */

static bool stage2_large_page_support(enum _page_table_level level, __unused uint64_t prot)
{
	return (level == PGT_LVL1) || (level == PGT_LVL2);
}

static void stage2_flush_cache_pagewalk(const void *entry)
{
	flush_cacheline(entry);
}

static uint64_t stage2_pgentry_present(uint64_t pte)
{
	return pte & PAGE_DESC_VALID;
}

static inline uint64_t stage2_leaf_desc_type(enum _page_table_level level)
{
	return (level == PGT_LVL0) ? PAGE_PAGE_DESC : PAGE_BLOCK_DESC;
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
	if (!stage2_page_pool_initialized) {
		init_page_pool(&stage2_page_pool, (uint64_t *)stage2_pages,
			(uint64_t *)stage2_pages_bitmap, ARM64_STAGE2_PAGE_NUM);
		stage2_page_pool_initialized = true;
	}
}

static bool arm64_vm_uses_virtio_console(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_console_size != 0UL);
}

static bool arm64_vm_uses_virtio_proxy(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_proxy_num != 0U);
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
	if ((mem_size == 0UL) || ((mem_start + mem_size) <= mem_start)) {
		panic("vm-%u has invalid stage-2 ram window", vm->vm_id);
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
	};

	init_stage2_page_pool();

	vm->stg2_pgtable = stage2_pgtable_template;
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
	pgtable_add_map((uint64_t *)vm->root_stg2ptp, mem_hpa, mem_start,
		mem_size, PAGE_S2_ATTR_NORMAL | PAGE_BLOCK_DESC, &vm->stg2_pgtable);
	log_stage2_map(vm, "ram", mem_start, mem_hpa, mem_size);

	pgtable_add_map((uint64_t *)vm->root_stg2ptp, hva2hpa(stage2_zero_page), 0UL,
		PAGE_SIZE, PAGE_S2_MEMATTR_NORMAL | PAGE_S2_S2AP_READ |
		PAGE_S2_SH_INNER | PAGE_S2_AF, &vm->stg2_pgtable);
	log_stage2_map(vm, "zero", 0UL, hva2hpa(stage2_zero_page), PAGE_SIZE);

	/*
	 * Device IPA ranges are logged here but registered below as vio MMIO. The
	 * absence of a stage-2 leaf mapping is what makes EL1 device accesses exit
	 * to EL2 for emulation.
	 */
	log_stage2_vio(vm, "vgicd", arch_config->guest_gicd_base,
		arch_config->guest_gicd_size);
	log_stage2_vio(vm, "vgicr", arch_config->guest_gicr_base,
		arch_config->guest_gicr_size);
	if (arch_config->guest_its_size != 0UL) {
		log_stage2_vio(vm, "vits", arch_config->guest_its_base,
			arch_config->guest_its_size);
	}
	if (arm64_vm_uses_virtio_console(get_vm_config(vm->vm_id))) {
		log_stage2_vio(vm, "vcon", arch_config->guest_virtio_console_base,
			arch_config->guest_virtio_console_size);
	} else {
		log_stage2_vio(vm, "vpl011", arch_config->guest_uart_base,
			arch_config->guest_uart_size);
	}
	if (arm64_vm_uses_virtio_proxy(get_vm_config(vm->vm_id))) {
		for (uint16_t i = 0U; i < arch_config->guest_virtio_proxy_num; i++) {
			char name[8];

			snprintf(name, sizeof(name), "vprox%u", i);
			log_stage2_vio(vm, name, arch_config->guest_virtio_proxy[i].base,
				arch_config->guest_virtio_proxy[i].size);
		}
	}
}

static void register_arm64_vio_mmio(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t gicd_base = arch_config->guest_gicd_base;
	uint64_t gicr_base = arch_config->guest_gicr_base;
	uint64_t its_base = arch_config->guest_its_base;
	uint64_t its_size = arch_config->guest_its_size;
	uint64_t uart_base = arch_config->guest_uart_base;
	uint64_t virtio_console_base = arch_config->guest_virtio_console_base;

	/*
	 * The common IO request layer owns dispatch by GPA range. ARM64 registers
	 * the guest interrupt-controller and UART windows here so data abort exits
	 * can be converted into device-specific emulation callbacks.
	 */
	register_mmio_emulation_handler(vm, arm64_vgicv3_mmio_handler,
		gicd_base, gicd_base + arch_config->guest_gicd_size,
		&vm->arch_vm.vgic, false);
	register_mmio_emulation_handler(vm, arm64_vgicv3_mmio_handler,
		gicr_base, gicr_base + arch_config->guest_gicr_size,
		&vm->arch_vm.vgic, false);
	if (its_size != 0UL) {
		register_mmio_emulation_handler(vm, arm64_vgicv3_mmio_handler,
			its_base, its_base + its_size, &vm->arch_vm.vgic, false);
	}
	if (arm64_vm_uses_virtio_console(get_vm_config(vm->vm_id))) {
		register_mmio_emulation_handler(vm, virtio_console_mmio_handler,
			virtio_console_base,
			virtio_console_base + arch_config->guest_virtio_console_size,
			vm, false);
	} else {
		register_mmio_emulation_handler(vm, arm64_vpl011_mmio_handler,
			uart_base, uart_base + arch_config->guest_uart_size,
			vm, false);
	}
	if (arm64_vm_uses_virtio_proxy(get_vm_config(vm->vm_id))) {
		for (uint16_t i = 0U; i < arch_config->guest_virtio_proxy_num; i++) {
			struct virtio_proxy_dev *proxy = virtio_proxy_get_dev(vm, i);
			uint64_t base = arch_config->guest_virtio_proxy[i].base;
			uint64_t size = arch_config->guest_virtio_proxy[i].size;

			register_mmio_emulation_handler(vm, virtio_proxy_mmio_handler,
				base, base + size, proxy, false);
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
	if (arm64_vm_uses_virtio_console(vm_config)) {
		virtio_console_init_vm(vm);
	} else {
		arm64_vpl011_init_vm(vm);
	}
	if (arm64_vm_uses_virtio_proxy(vm_config)) {
		virtio_proxy_init_vm(vm);
	}
	register_arm64_vio_mmio(vm);

	if (is_static_configured_vm(vm) && (vm_config->fdt_config.fdt_mod_tag[0] == '\0')) {
		init_service_vm_vfdt(vm);
	}

	vm->arch_vm.time_delta = -(int64_t)cpu_ticks();
	return 0;
}

int32_t arch_deinit_vm(__unused struct acrn_vm *vm)
{
	return 0;
}

int32_t arch_reset_vm(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu = NULL;
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	/*
	 * 2026-07-08, ARM64 VM warm-reset boundary:
	 *
	 * A VM reset must clear both per-vCPU execution state and per-VM device
	 * state. Otherwise the new boot may inherit stale pending interrupts,
	 * virtqueue indices, or outstanding IO request slots from the previous
	 * kernel instance.
	 *
	 *   pause all vCPUs (common)
	 *          |
	 *          v
	 *   reset ioreq + vGIC + virtio transport state
	 *          |
	 *          v
	 *   reset each vCPU boot context
	 *          |
	 *          v
	 *   start_vm() prepares and wakes BSP
	 */
	reset_vm_ioreqs(vm);
	arm64_vgicv3_init_vm(vm, vm_config->cpu_affinity);
	if (arm64_vm_uses_virtio_console(vm_config)) {
		virtio_console_reset_vm(vm);
	} else {
		arm64_vpl011_reset_vm(vm);
	}
	if (arm64_vm_uses_virtio_proxy(vm_config)) {
		virtio_proxy_reset_vm(vm);
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
		if ((vcpu != NULL) && (vcpu->state != VCPU_OFFLINE)) {
			(void)arm64_vgicv3_inject_irq(vcpu, irq, true);
		}
	} else {
		foreach_vcpu(idx, vm, vcpu) {
			(void)arm64_vgicv3_deassert_irq(vcpu, irq);
		}
	}
}
