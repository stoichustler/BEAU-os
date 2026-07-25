/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <reloc.h>
#include <sprintf.h>
#include <util.h>
#include <vm.h>
#include <vconfig.h>
#include <asm/boot/ld_sym.h>
#include <asm/guest/stage2.h>
#include <asm/mmu.h>
#include <asm/platform.h>

#include "shell_cmds.h"

#define SHELL_MEM_KB_BYTES	1024UL
#define SHELL_MEM_MB_BYTES	(SHELL_MEM_KB_BYTES * 1024UL)
#define SHELL_MEM_GB_BYTES	(SHELL_MEM_MB_BYTES * 1024UL)

static void shell_print_mem_header(void)
{
	shell_item_line("domain      type       attr                    address range (size)");
	shell_item_line("──────────  ─────────  ──────────────────────  ─────────────────────────────────────────────────────");
}

static uint64_t shell_mem_range_end(uint64_t start, uint64_t size)
{
	return (size == 0UL) ? start : (start + size - 1UL);
}

static void shell_print_mem_map(const char *domain, const char *type,
	const char *attr, uint64_t addr, uint64_t size)
{
	uint64_t unit_bytes;
	uint64_t size_whole;
	uint64_t size_fraction;
	const char *unit;

	if (size >= SHELL_MEM_GB_BYTES) {
		unit_bytes = SHELL_MEM_GB_BYTES;
		unit = "GB";
	} else if (size >= SHELL_MEM_MB_BYTES) {
		unit_bytes = SHELL_MEM_MB_BYTES;
		unit = "MB";
	} else {
		unit_bytes = SHELL_MEM_KB_BYTES;
		unit = "KB";
	}
	size_whole = size / unit_bytes;
	size_fraction = ((size % unit_bytes) * 1000UL) / unit_bytes;

	shell_item_line("%-10s  %-9s  %-22s  [0x%016lx,0x%016lx] (%4lu.%03lu %s)",
		domain, type, attr, addr, shell_mem_range_end(addr, size),
		size_whole, size_fraction, unit);
	shell_output_checkpoint();
}

static const char *shell_memory_type_name(enum arm64_memory_type type)
{
	const char *name;

	switch (type) {
	case ARM64_MEMORY_NORMAL:
		name = "Normal Memory";
		break;
	case ARM64_MEMORY_DEVICE_GRE:
		name = "Device-GRE";
		break;
	case ARM64_MEMORY_DEVICE_nGRE:
		name = "Device-nGRE";
		break;
	case ARM64_MEMORY_DEVICE_nGnRE:
		name = "Device-nGnRE";
		break;
	case ARM64_MEMORY_DEVICE_nGnRnE:
		name = "Device-nGnRnE";
		break;
	case ARM64_MEMORY_UNMAPPED:
		name = "Unmapped";
		break;
	case ARM64_MEMORY_UNKNOWN:
	default:
		name = "Unknown";
		break;
	}

	return name;
}

static void shell_format_memory_attr(char *buf, size_t size,
	const struct arm64_memory_attr *attr, bool stage2)
{
	const char *name;
	uint8_t encoding;

	if ((buf == NULL) || (size == 0U) || (attr == NULL)) {
		return;
	}
	if (attr->type == ARM64_MEMORY_UNMAPPED) {
		(void)snprintf(buf, size, "Unmapped (%s)",
			stage2 ? "IPA->HPA" : "VA->HPA");
		return;
	}

	name = shell_memory_type_name(attr->type);
	encoding = attr->encoding & 0x0fU;
	(void)snprintf(buf, size, "%-13s [0b%u%u%u%u]", name,
		(encoding >> 3U) & 1U, (encoding >> 2U) & 1U,
		(encoding >> 1U) & 1U, encoding & 1U);
}

static void shell_print_host_mem_map(const char *type, uint64_t addr,
	uint64_t size)
{
	struct arm64_memory_attr attr = { 0U };
	char attr_text[32U];

	if (!arm64_get_hv_s1_memory_attr(addr, &attr)) {
		attr.type = ARM64_MEMORY_UNKNOWN;
	}
	(void)memset(attr_text, 0U, sizeof(attr_text));
	shell_format_memory_attr(attr_text, sizeof(attr_text), &attr, false);
	shell_print_mem_map("HOST s1", type, attr_text, addr, size);
}

static void shell_print_stage2_mem_map(struct acrn_vm *vm, const char *domain,
	const char *type, uint64_t addr, uint64_t size)
{
	struct arm64_memory_attr attr = { 0U };
	char attr_text[32U];

	if (!arm64_get_stage2_memory_attr(vm, addr, &attr)) {
		attr.type = ARM64_MEMORY_UNKNOWN;
	}
	(void)memset(attr_text, 0U, sizeof(attr_text));
	shell_format_memory_attr(attr_text, sizeof(attr_text), &attr, true);
	shell_print_mem_map(domain, type, attr_text, addr, size);
}

static void shell_print_host_maps(void)
{
	const struct arm64_mem_region *mmio_regions;
	const struct mem_region *rsvd_regions;
	uint32_t mmio_count;
	uint32_t rsvd_count;
	uint32_t idx;
	uint64_t hv_base = get_hv_image_base();

	mmio_regions = arm64_get_platform_mmio_regions(&mmio_count);
	for (idx = 0U; idx < mmio_count; idx++) {
		shell_print_host_mem_map("MMIO",
			mmio_regions[idx].base, mmio_regions[idx].size);
	}

	shell_print_host_mem_map("RAM",
		arm64_get_phys_mem_start(), arm64_get_phys_mem_size());
	shell_print_host_mem_map("HV", hv_base, get_hv_image_size());

	rsvd_regions = arm64_get_reserved_mem_regions(&rsvd_count);
	for (idx = 0U; idx < rsvd_count; idx++) {
		shell_print_host_mem_map("RSVD",
			rsvd_regions[idx].addr, rsvd_regions[idx].size);
	}
}

static void shell_print_vm_stage2_maps(struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char domain[16];

	snprintf(domain, sizeof(domain), "VM-%u s2", vm->vm_id);
	shell_print_stage2_mem_map(vm, domain, "RAM",
		arch_config->guest_ram_start,
		arch_config->guest_ram_size);
	shell_print_stage2_mem_map(vm, domain, "vGICD",
		arch_config->guest_gicd_base,
		arch_config->guest_gicd_size);
	shell_print_stage2_mem_map(vm, domain, "vGICR",
		arch_config->guest_gicr_base,
		arch_config->guest_gicr_size);
	shell_print_stage2_mem_map(vm, domain, "vPL011",
		arch_config->guest_uart_base,
		arch_config->guest_uart_size);
}

/* [20260717] devmap memory attributes:
 *
 *   host VA -> stage-1 leaf -> AttrIdx -> MAIR_EL2 -> type + low 4 bits
 *   guest IPA -> locked stage-2 leaf -> MemAttr[3:0] -> type + 4 bits
 *
 * The command prints the EL2 stage-1 host view first, then each VM's stage-2
 * view. VIO rows intentionally have no stage-2 leaf and report Unmapped.
 *
 * Key rule:
 *   - names and encodings come from live descriptors instead of range labels;
	*   - Stage-2 reads serialize with dynamic map/unmap in the architecture layer;
	*   - unmapped VIO is distinguished from unknown or reserved attributes.
	*
	* Output fields: domain selects host S1 or VM S2; type identifies the mapping
	* role; attr is decoded memory type/permissions; range is inclusive; size uses
	* the largest exact KB, MB, or GB unit.
	*/
int32_t shell_list_mem(__unused int32_t argc, __unused char **argv)
{
	uint16_t vm_id;

	shell_item_begin("memory mappings:");
	shell_print_mem_header();
	shell_print_host_maps();

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (vm->root_stg2ptp != NULL) {
			shell_print_vm_stage2_maps(vm);
		}
	}
	shell_item_end();

	return 0;
}

static void shell_memstat_format_usage(char *buf, size_t size,
	const struct page_pool_stats *stats)
{
	uint64_t permille = 0UL;

	if ((stats != NULL) && (stats->total_pages != 0UL)) {
		permille = (stats->used_pages * 1000UL) / stats->total_pages;
	}
	(void)snprintf(buf, size, "%02lu.%01lu%%", permille / 10UL, permille % 10UL);
}

static void shell_memstat_print_pool(const char *name,
	const struct page_pool_stats *stats)
{
	char usage[16U];

	shell_memstat_format_usage(usage, sizeof(usage), stats);
	shell_item_line("%-6s  %-5lu  %-5lu  %-5lu  %s",
		name, stats->total_pages, stats->used_pages, stats->free_pages, usage);
}

static bool shell_memstat_print_vm(uint16_t vm_id, uint64_t *accounted,
	uint64_t *malformed)
{
	struct acrn_vm *vm = get_vm_from_vmid(vm_id);
	struct arm64_stage2_vm_stats stats;

	if (!arm64_get_stage2_vm_stats(vm, &stats)) {
		return false;
	}

	shell_item_line("vm%-2hu  %-9s  0x%016lx  %-3lu  %-3lu  %-3lu  %-3lu  %-5lu  %-9lu",
		vm_id, shell_vm_state_to_str(vm->state), stats.root_address,
		stats.level3_pages, stats.level2_pages, stats.level1_pages,
		stats.level0_pages, stats.total_pages, stats.malformed_entries);
	*accounted += stats.total_pages;
	*malformed += stats.malformed_entries;
	return true;
}

/*
 * memstat reports fixed page-table memory, not a general heap. hv-s1 is the
 * EL2 translation-table pool; vm-s2 is shared by all VM stage-2 roots. The
 * ownership walk counts only table pages reachable from each VM root, making
 * the all-VM difference useful for spotting orphaned allocations.
 */
int32_t shell_memstat(int32_t argc, __unused char **argv)
{
	struct page_pool_stats hv_s1 = { 0U };
	struct page_pool_stats vm_s2 = { 0U };
	uint64_t accounted = 0UL;
	uint64_t malformed = 0UL;
	uint64_t unowned;
	uint64_t overaccounted;
	bool found = false;

	if (argc != 1) {
		shell_puts("usage: memstat\r\n");
		return -EINVAL;
	}

	arm64_get_hv_s1_page_pool_stats(&hv_s1);
	arm64_get_stage2_page_pool_stats(&vm_s2);

	shell_item_begin("memstat");
	/* Pool rows are total/used/free table pages. Ownership rows count reachable
	 * L3..L0 tables per VM; mismatch fields identify allocation accounting faults.
	 */
	shell_item_line("page-size:%uB hv-image:0x%016lx+0x%016lx",
		PAGE_SIZE, get_hv_image_base(), get_hv_image_size());
	shell_item_section("Page-table pools");
	shell_item_line("pool    total  used   free   usage");
	shell_item_line("──────  ─────  ─────  ─────  ──────");
	shell_memstat_print_pool("HV-s1", &hv_s1);
	shell_memstat_print_pool("VM-s2", &vm_s2);

	shell_item_section("Stage-2 ownership");
	shell_item_line("vm    state      root                L3   L2   L1   L0   total  malformed");
	shell_item_line("────  ─────────  ──────────────────  ───  ───  ───  ───  ─────  ─────────");
	for (uint16_t vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		found |= shell_memstat_print_vm(vm_id, &accounted, &malformed);
		shell_output_checkpoint();
	}

	if (!found) {
		shell_item_line("no stage-2 roots");
		shell_item_end();
		return 0;
	}

	unowned = (vm_s2.used_pages > accounted) ?
		(vm_s2.used_pages - accounted) : 0UL;
	overaccounted = (accounted > vm_s2.used_pages) ?
		(accounted - vm_s2.used_pages) : 0UL;

	shell_item_line("accounted:%lu unowned:%lu overaccounted:%lu malformed:%lu",
		accounted, unowned, overaccounted, malformed);
	shell_item_end();

	return 0;
}

static bool shell_s2walk_parse_vmid(const char *text, uint16_t *vmid)
{
	uint32_t value = 0U;
	uint32_t index;

	if ((text == NULL) || (text[0] == '\0') || (vmid == NULL)) {
		return false;
	}
	for (index = 0U; text[index] != '\0'; index++) {
		uint32_t digit;

		if ((text[index] < '0') || (text[index] > '9')) {
			return false;
		}
		digit = (uint32_t)(text[index] - '0');
		if (value > ((UINT32_MAX - digit) / 10U)) {
			return false;
		}
		value = (value * 10U) + digit;
		if (value >= CONFIG_MAX_VM_NUM) {
			return false;
		}
	}
	*vmid = (uint16_t)value;

	return true;
}

static bool shell_s2walk_parse_ipa(const char *text, uint64_t *ipa)
{
	uint64_t value = 0UL;
	uint32_t index = 0U;

	if ((text == NULL) || (ipa == NULL)) {
		return false;
	}
	if ((text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X'))) {
		index = 2U;
	}
	if (text[index] == '\0') {
		return false;
	}
	for (; text[index] != '\0'; index++) {
		uint64_t digit;

		if ((text[index] >= '0') && (text[index] <= '9')) {
			digit = (uint64_t)(text[index] - '0');
		} else if ((text[index] >= 'a') && (text[index] <= 'f')) {
			digit = (uint64_t)(text[index] - 'a') + 10UL;
		} else if ((text[index] >= 'A') && (text[index] <= 'F')) {
			digit = (uint64_t)(text[index] - 'A') + 10UL;
		} else {
			return false;
		}
		if (value > ((UINT64_MAX - digit) / 16UL)) {
			return false;
		}
		value = (value * 16UL) + digit;
	}
	*ipa = value;

	return true;
}

static const char *shell_s2walk_result_name(enum arm64_stage2_walk_result result)
{
	const char *name;

	switch (result) {
	case ARM64_STAGE2_WALK_MAPPED:
		name = "mapped";
		break;
	case ARM64_STAGE2_WALK_UNMAPPED:
		name = "unmapped";
		break;
	case ARM64_STAGE2_WALK_MALFORMED:
	default:
		name = "malformed";
		break;
	}

	return name;
}

static const char *shell_s2walk_shareability(uint64_t descriptor)
{
	const char *name;

	switch ((descriptor >> 8U) & 0x3UL) {
	case 0U:
		name = "nonshareable";
		break;
	case 2U:
		name = "outer";
		break;
	case 3U:
		name = "inner";
		break;
	case 1U:
	default:
		name = "reserved";
		break;
	}

	return name;
}

static enum arm64_memory_type shell_s2walk_memory_type(uint64_t descriptor)
{
	enum arm64_memory_type type;

	switch (descriptor & PAGE_S2_MEMATTR_MASK) {
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

static void shell_s2walk_print_permissions(const struct arm64_stage2_walk *walk)
{
	const uint64_t descriptor = walk->step[walk->step_count - 1U].descriptor;
	struct arm64_memory_attr attr = {
		.type = ARM64_MEMORY_UNKNOWN,
		.encoding = (uint8_t)((descriptor & PAGE_S2_MEMATTR_MASK) >> 2U),
	};
	char attr_text[32U];

	attr.type = shell_s2walk_memory_type(descriptor);
	shell_format_memory_attr(attr_text, sizeof(attr_text), &attr, true);
	shell_item_line("attr:%s perm:%c%c%c sh:%s af:%c",
		attr_text,
		((descriptor & PAGE_S2_S2AP_READ) != 0UL) ? 'R' : '-',
		((descriptor & PAGE_S2_S2AP_WRITE) != 0UL) ? 'W' : '-',
		((descriptor & PAGE_S2_XN) == 0UL) ? 'X' : '-',
		shell_s2walk_shareability(descriptor),
		((descriptor & PAGE_S2_AF) != 0UL) ? 'Y' : 'N');
}

int32_t shell_s2walk(int32_t argc, char **argv)
{
	struct arm64_stage2_walk walk;
	struct acrn_vm *vm;
	uint16_t vmid;
	uint32_t index;
	uint64_t ipa;
	int32_t ret;

	if ((argc != 3) || !shell_s2walk_parse_vmid(argv[1], &vmid) ||
		!shell_s2walk_parse_ipa(argv[2], &ipa)) {
		shell_puts("usage: walkpt <vmid> <ipa>\r\n");
		return -EINVAL;
	}
	vm = get_vm_from_vmid(vmid);
	ret = arm64_stage2_walk(vm, ipa, &walk);
	if (ret == -EBUSY) {
		shell_puts("walkpt: stage-2 update in progress; retry\r\n");
		return ret;
	}
	if (ret == -EFAULT) {
		shell_puts("walkpt: invalid stage-2 root\r\n");
		return ret;
	}
	if (ret == -ENODEV) {
		shell_puts("walkpt: VM has no stage-2 root\r\n");
		return ret;
	}
	if (ret != 0) {
		shell_puts("walkpt: invalid VM or IPA\r\n");
		return ret;
	}

	shell_item_begin("walkpt vm%hu", vmid);
	/* ipa is the requested guest address; L rows give table HPA/index/raw
	 * descriptor; result and leaf identify the terminating translation state.
	 */
	shell_item_line("ipa:0x%016lx vttbr:0x%016lx", walk.ipa, walk.vttbr);
	for (index = 0U; index < walk.step_count; index++) {
		const struct arm64_stage2_walk_step *step = &walk.step[index];

		shell_item_line("L%u table:0x%016lx index:0x%03x desc:0x%016lx",
			step->level, step->table_hpa, step->index, step->descriptor);
	}
	shell_item_line("result:%s stop:L%u", shell_s2walk_result_name(walk.result),
		walk.leaf_level);
	if (walk.result == ARM64_STAGE2_WALK_MAPPED) {
		shell_item_line("leaf:L%u ipa:[0x%016lx-0x%016lx] hpa:0x%016lx",
			walk.leaf_level, walk.range_start,
			walk.range_start + walk.range_size - 1UL, walk.hpa);
		shell_s2walk_print_permissions(&walk);
	}
	shell_item_end();

	return 0;
}

static void shell_kusg_print_usage(const char *name, const char *attribute,
	uint64_t bytes)
{
	uint64_t whole = bytes / SHELL_MEM_KB_BYTES;
	uint64_t fraction = ((bytes % SHELL_MEM_KB_BYTES) * 1000UL) /
		SHELL_MEM_KB_BYTES;

	shell_item_line("%-12s  %-15s  %8lu.%03lu KB",
		name, attribute, whole, fraction);
}

static void shell_kusg_print_section(const char *name, uint64_t start,
	uint64_t end)
{
	char attribute[4U] = { '?', '?', '?', '\0' };
	uint8_t access;

	if (arm64_get_hv_s1_memory_access(start, &access)) {
		attribute[0] = ((access & ARM64_S1_ACCESS_READ) != 0U) ? 'R' : '-';
		attribute[1] = ((access & ARM64_S1_ACCESS_WRITE) != 0U) ? 'W' : '-';
		attribute[2] = ((access & ARM64_S1_ACCESS_EXECUTE) != 0U) ? 'X' : '-';
	}

	shell_kusg_print_usage(name, attribute, end - start);
}

int32_t shell_kusg(int32_t argc, __unused char **argv)
{
	if (argc != 1) {
		return -EINVAL;
	}

	shell_item_begin("BEAU OS static memory usage");
	shell_item_line("section       attribute        usage");
	shell_item_line("────────────  ───────────────  ───────────────");
	shell_kusg_print_section(".TEXT",
		(uint64_t)&_text_start, (uint64_t)&_text_end);
	shell_kusg_print_section(".RODATA",
		(uint64_t)&_rodata_start, (uint64_t)&_rodata_end);
	shell_kusg_print_section(".DATA",
		(uint64_t)&_data_start, (uint64_t)&_data_end);
	shell_kusg_print_section(".BSS",
		(uint64_t)&_bss_start, (uint64_t)&_bss_end);
	shell_kusg_print_section(".BOOT.STACK",
		(uint64_t)&_boot_stack_start, (uint64_t)&_boot_stack_end);
	shell_kusg_print_usage(".IMAGE", "MIXED",
		(uint64_t)&ld_image_end - (uint64_t)&ld_ram_start);
	shell_kusg_print_usage("RAM", "MIXED",
		(uint64_t)&ld_ram_end - (uint64_t)&ld_ram_start);
	shell_item_end();

	return 0;
}
