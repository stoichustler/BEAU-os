/*
 * Copyright (c) 2021 Google Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <arch/arm64/mmu.h>
#include <arch/ops.h>
#include <assert.h>
#include <kernel/vm.h>
#include <lk/compiler.h>
#include <panic.h>
#include <sys/types.h>

#if ARM64_BOOT_PROTOCOL_X0_DTB
#include <lib/device_tree/libfdt_helpers.h>
#endif

static uint get_aspace_flags(void) {
    uint aspace_flags = ARCH_ASPACE_FLAG_KERNEL;

#ifdef KERNEL_BTI_ENABLED
    if (arch_bti_supported()) {
        aspace_flags |= ARCH_ASPACE_FLAG_BTI;
    }
#endif

    return aspace_flags;
}

/* trampoline translation table */
extern pte_t tt_trampoline[MMU_PAGE_TABLE_ENTRIES_IDENT];

/* the main translation table */
pte_t arm64_kernel_translation_table[MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP]
    __ALIGNED(MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP * 8);

static void* early_mmu_paddr_to_kvaddr(paddr_t paddr) {
    return (void*)paddr;
}

static int alloc_page_table(paddr_t* paddrp, uint page_size_shift) {
    const size_t size = 1UL << page_size_shift;
    paddr_t paddr = (paddr_t)boot_alloc_memalign(size, size);
    *paddrp = paddr;
    return 0;
}

static void free_page_table(void* vaddr,
                            paddr_t paddr,
                            uint page_size_shift) {
    /* If we get here then we can't boot, so halt */
    panic("reached free_page_table during early boot\n");
}

/*
 * Override paddr_to_kvaddr since it's implemented in kernel/vm.c
 * and we don't want to change that.
 */
#define paddr_to_kvaddr early_mmu_paddr_to_kvaddr
#define EARLY_MMU
#include "mmu.inc"
#undef paddr_to_kvaddr

void arch_mmu_map_early(vaddr_t vaddr,
                        paddr_t paddr,
                        size_t size,
                        uint flags) {
    pte_t attr;
    bool res = mmu_flags_to_pte_attr(get_aspace_flags(), flags, &attr);
    ASSERT(res);
    const uintptr_t vaddr_top_mask = ~0UL << MMU_KERNEL_SIZE_SHIFT;
    ASSERT((vaddr & vaddr_top_mask) == vaddr_top_mask);
    int ret = arm64_mmu_map_pt(vaddr, vaddr ^ vaddr_top_mask, paddr, size, attr,
                               MMU_KERNEL_TOP_SHIFT, MMU_KERNEL_PAGE_SIZE_SHIFT,
                               arm64_kernel_translation_table,
                               MMU_ARM64_GLOBAL_ASID, false);
    ASSERT(!ret);
}

#if ARM64_BOOT_PROTOCOL_X0_MEMSIZE
static inline void map_trampoline(paddr_t paddr, size_t size) {}

ulong arm64_get_ram_size(ulong ram_size_or_dtb_addr, paddr_t kernel_paddr) {
    return ram_size_or_dtb_addr;
}
#elif ARM64_BOOT_PROTOCOL_X0_DTB
static void map_trampoline(paddr_t paddr, size_t size) {
    paddr_t end = paddr + (size - 1);
    paddr_t i = paddr >> MMU_IDENT_TOP_SHIFT;
    paddr_t end_i = end >> MMU_IDENT_TOP_SHIFT;
    pte_t attrs = arm64_tagging_supported() ? MMU_PTE_IDENT_FLAGS_TAGGED
                                            : MMU_PTE_IDENT_FLAGS;

    /*
     * Remove MMU_PTE_IDENT_DESCRIPTOR since arm64_mmu_map_pt will select this
     * on its own.
     */
    attrs &= ~MMU_PTE_IDENT_DESCRIPTOR;

    for (; i <= end_i; i++) {
        if (!tt_trampoline[i]) {
            int ret = arm64_mmu_map_pt(
                    i << MMU_IDENT_TOP_SHIFT, i << MMU_IDENT_TOP_SHIFT,
                    i << MMU_IDENT_TOP_SHIFT, 1 << MMU_IDENT_TOP_SHIFT, attrs,
                    MMU_IDENT_TOP_SHIFT, MMU_IDENT_PAGE_SIZE_SHIFT,
                    tt_trampoline, MMU_ARM64_GLOBAL_ASID, false);
            ASSERT(!ret);
        }
    }
}

ulong arm64_get_ram_size(ulong ram_size_or_dtb_addr, paddr_t kernel_paddr) {
    const void *fdt = (const void *)ram_size_or_dtb_addr;
    int offset;
    paddr_t mem_base, mem_size;

    /* Make sure device-tree is mapped */
    map_trampoline(ram_size_or_dtb_addr, FDT_V1_SIZE);
    if (fdt_magic(fdt) != FDT_MAGIC) {
        panic("No device tree found at %p\n", fdt);
    }
    map_trampoline(ram_size_or_dtb_addr, fdt_totalsize(fdt));

    offset = fdt_node_offset_by_prop_value(fdt, 0, "device_type", "memory", 7);
    if (fdt_helper_get_reg(fdt, offset, 0, &mem_base, &mem_size)) {
        panic("No memory node found in device tree\n");
    }

    if ((kernel_paddr >= mem_base) && (kernel_paddr - mem_base < mem_size)) {
        /*
         * TODO: Allow using memory below kernel base. For now subtract this
         * from mem_size and ignore this memory.
         */
        return mem_size - (kernel_paddr - mem_base);
    }

    panic("kernel_paddr, 0x%" PRIxPADDR ", not in memory range: 0x%" PRIxPADDR
          ", size 0x%" PRIxPADDR "\n",
          kernel_paddr, mem_base, mem_size);
}
#else
#error "Unknown ARM64_BOOT_PROTOCOL"
#endif

void arm64_early_mmu_init(ulong ram_size_or_dtb_addr, uintptr_t* relr_start,
                          uintptr_t* relr_end, paddr_t kernel_paddr) {
    const uintptr_t kernel_initial_vaddr = KERNEL_BASE + KERNEL_LOAD_OFFSET;
    uintptr_t virt_offset = kernel_initial_vaddr - kernel_paddr;
    update_relocation_entries(relr_start, relr_end, virt_offset);

    /* Relocate the kernel to its physical address */
    relocate_kernel(relr_start, relr_end, kernel_initial_vaddr, kernel_paddr);

#if BOOT_ALLOC_RELOCATE_EARLY
    /* [20260728] Non-PIE boot allocator relocation
     *
     * linked virtual _end -> physical boot allocation -> final kernel mapping
     *
     * Key rule:
     *   - page-table pages are owned by the boot allocator before the final
     *     high virtual mapping exists;
     *   - convert both allocator bounds before allocation, then restore their
     *     final virtual form before VM accounting observes them;
     *   - this prevents an early allocator write through an unmapped linked
     *     virtual address when the selected linker cannot emit RELR records.
     */
    boot_alloc_relocate(kernel_initial_vaddr, kernel_paddr);
#endif

    ulong ram_size = arm64_get_ram_size(ram_size_or_dtb_addr, kernel_paddr);

    /* Map any ram not already mapped in trampoline page table */
    map_trampoline(kernel_paddr, ram_size);

    vm_assign_initial_dynamic(kernel_paddr, ram_size);
    vaddr_t kernel_final_vaddr =
        aslr_randomize_kernel_base(kernel_initial_vaddr);
    vm_map_initial_mappings();

#if BOOT_ALLOC_RELOCATE_EARLY
    boot_alloc_relocate(kernel_paddr, kernel_final_vaddr);
#endif

    /* Relocate the kernel to its final virtual address */
    relocate_kernel(relr_start, relr_end, kernel_paddr, kernel_final_vaddr);
}
