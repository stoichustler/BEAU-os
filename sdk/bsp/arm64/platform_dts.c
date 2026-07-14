/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bare.h>
#include <vm_config.h>
#include <schedule.h>
#include <libfdt.h>
#include <logmsg.h>
#include <rtl.h>
#include <bsp/pci.h>
#include <platform_acpi_info.h>
#include <passthrough.h>
#include <asm/irq.h>
#include <asm/platform.h>
#include <asm/sve.h>
#include <asm/guest/vipc.h>
#include <asm/vtd.h>
#include <arm64_platform_dts.h>
#include <virtio_proxy.h>

#define ARM64_DTS_MMIO_REGION_MAX	8U
#define ARM64_DTS_GIC_SPI		0U
#define ARM64_DTS_IRQ_TYPE_EDGE_RISING	1U
#define ARM64_DTS_IRQ_TYPE_LEVEL_HIGH	4U
#define ARM64_DTS_CBS_GANG_SKEW_US_DEFAULT	500U

static const struct arm64_platform_dts_vm_storage *dts_storage;
static uint16_t dts_bare_boot_option_count;
static struct arm64_mem_region dts_mmio_regions[ARM64_DTS_MMIO_REGION_MAX];
static uint32_t dts_mmio_region_count;

/* [20260714] ARM64 platform DTS ingestion
 *
 * The platform device tree is the static contract for the learning hypervisor:
 * it describes host hardware, VM layout, scheduler policy, boot images, and
 * passthrough ownership before any VM object exists.
 *
 *   platform.dts
 *        |
 *        v
 *   arm64_platform_dts.c
 *     - validate required nodes and properties
 *     - normalize aliases and legacy property names
 *     - populate vm_config[], boot_options[], PCI and MMIO policy
 *        |
 *        v
 *   common runtime
 *     - init_primary_pcpu() consumes host policy
 *     - create_vm() consumes VM policy
 *     - launch_vms() consumes boot module tags
 *     - sched_get_pcpu_pool_config() consumes scheduler policy
 *
 * Key rule:
 *   - DTS parsing fails closed on missing or ambiguous ownership;
 *   - parsed storage is plain runtime config, not live hardware state;
 *   - architecture and common code consume the normalized structs instead of
 *     re-reading the flattened tree.
 */

static void arm64_dts_panic(const char *op, int32_t ret)
{
	panic("failed to parse arm64 platform dts: %s ret=%d", op, ret);
}

static void dts_set_storage(const struct arm64_platform_dts_vm_storage *storage)
{
	if ((storage == NULL) || (storage->vm_configs == NULL) ||
		(storage->memory_regions == NULL) || (storage->pci_devs == NULL) ||
		(storage->boot_options == NULL) ||
		(storage->boot_option_count == NULL) || (storage->vm_config_count == 0U) ||
		(storage->boot_option_capacity == 0U)) {
		panic("invalid arm64 platform dts storage");
	}

	dts_storage = storage;
}

static uint64_t dts_read_cells(const fdt32_t *cells, int32_t count)
{
	uint64_t value = 0UL;
	int32_t i;

	if ((count <= 0) || (count > 2)) {
		arm64_dts_panic("bad address cells", -EINVAL);
	}

	for (i = 0; i < count; i++) {
		value = (value << 32U) | (uint64_t)fdt32_to_cpu(cells[i]);
	}

	return value;
}

static int32_t dts_child_by_name(const void *fdt, int32_t parent, const char *name)
{
	int32_t node;

	fdt_for_each_subnode(node, fdt, parent) {
		const char *node_name = fdt_get_name(fdt, node, NULL);

		if ((node_name != NULL) && (strcmp(node_name, name) == 0)) {
			return node;
		}
	}

	return -FDT_ERR_NOTFOUND;
}

static int32_t dts_child_by_unit_name(const void *fdt, int32_t parent, const char *name)
{
	int32_t node;

	fdt_for_each_subnode(node, fdt, parent) {
		const char *node_name = fdt_get_name(fdt, node, NULL);
		const char *unit;
		size_t len;

		if (node_name == NULL) {
			continue;
		}
		unit = strchr(node_name, '@');
		len = unit == NULL ? strlen(node_name) : (size_t)(unit - node_name);
		if ((strlen(name) == len) && (strncmp(node_name, name, len) == 0)) {
			return node;
		}
	}

	return -FDT_ERR_NOTFOUND;
}

static int32_t dts_child_by_any_name(const void *fdt, int32_t parent,
	const char *first, const char *second)
{
	int32_t node = dts_child_by_unit_name(fdt, parent, first);

	if ((node < 0) && (second != NULL)) {
		node = dts_child_by_unit_name(fdt, parent, second);
	}

	return node;
}

static bool dts_has_compatible(const void *fdt, int32_t node, const char *compat)
{
	return fdt_node_check_compatible(fdt, node, compat) == 0;
}

static bool dts_has_any_compatible(const void *fdt, int32_t node,
	const char *first, const char *second)
{
	return dts_has_compatible(fdt, node, first) ||
		((second != NULL) && dts_has_compatible(fdt, node, second));
}

static bool dts_is_virtio_proxy(const void *fdt, int32_t node)
{
	return dts_has_compatible(fdt, node, "beau,virtio-proxy") ||
		dts_has_compatible(fdt, node, "beau,virtio-fs") ||
		dts_has_compatible(fdt, node, "beau,virtio-rng");
}

static int32_t dts_child_compatible(const void *fdt, int32_t parent, const char *compat)
{
	int32_t node;

	fdt_for_each_subnode(node, fdt, parent) {
		if (dts_has_compatible(fdt, node, compat)) {
			return node;
		}
	}

	return -FDT_ERR_NOTFOUND;
}

static uint32_t dts_u32_prop(const void *fdt, int32_t node, const char *name,
	uint32_t default_value)
{
	const fdt32_t *prop;
	int32_t len;

	prop = fdt_getprop(fdt, node, name, &len);
	if (prop == NULL) {
		return default_value;
	}
	if (len < (int32_t)sizeof(fdt32_t)) {
		arm64_dts_panic(name, -EINVAL);
	}

	return fdt32_to_cpu(prop[0]);
}

static uint64_t dts_u64_prop(const void *fdt, int32_t node, const char *name,
	uint64_t default_value)
{
	const fdt32_t *prop;
	int32_t len;

	prop = fdt_getprop(fdt, node, name, &len);
	if (prop == NULL) {
		return default_value;
	}
	if (len == (int32_t)sizeof(fdt32_t)) {
		return fdt32_to_cpu(prop[0]);
	}
	if (len >= (int32_t)(2U * sizeof(fdt32_t))) {
		return dts_read_cells(prop, 2);
	}

	arm64_dts_panic(name, -EINVAL);
	return default_value;
}

static uint64_t dts_addr_prop(const void *fdt, int32_t node, const char *name,
	uint64_t default_value)
{
	const fdt32_t *prop;
	int32_t len;
	int32_t parent;
	int32_t addr_cells;

	prop = fdt_getprop(fdt, node, name, &len);
	if (prop == NULL) {
		return default_value;
	}

	parent = fdt_parent_offset(fdt, node);
	if (parent < 0) {
		arm64_dts_panic(name, parent);
	}
	addr_cells = fdt_address_cells(fdt, parent);
	if (addr_cells < 0) {
		arm64_dts_panic(name, addr_cells);
	}
	if (len < (int32_t)((uint32_t)addr_cells * sizeof(fdt32_t))) {
		arm64_dts_panic(name, -EINVAL);
	}

	return dts_read_cells(prop, addr_cells);
}

static void dts_reg_by_index(const void *fdt, int32_t node, uint32_t index,
	uint64_t *base, uint64_t *size)
{
	const fdt32_t *reg;
	int32_t len;
	int32_t parent;
	int32_t addr_cells;
	int32_t size_cells;
	uint32_t entry_cells;
	uint32_t cell;

	parent = fdt_parent_offset(fdt, node);
	if (parent < 0) {
		arm64_dts_panic("reg parent", parent);
	}

	addr_cells = fdt_address_cells(fdt, parent);
	size_cells = fdt_size_cells(fdt, parent);
	if ((addr_cells < 0) || (size_cells < 0)) {
		arm64_dts_panic("reg cells", addr_cells < 0 ? addr_cells : size_cells);
	}

	entry_cells = (uint32_t)addr_cells + (uint32_t)size_cells;
	cell = index * entry_cells;

	reg = fdt_getprop(fdt, node, "reg", &len);
	if ((reg == NULL) || (len < (int32_t)((cell + entry_cells) * sizeof(fdt32_t)))) {
		arm64_dts_panic("reg", reg == NULL ? len : -EINVAL);
	}

	*base = dts_read_cells(&reg[cell], addr_cells);
	*size = size_cells == 0 ? 0UL : dts_read_cells(&reg[cell + (uint32_t)addr_cells],
		size_cells);
}

static void dts_copy_string(char *dst, size_t dst_size, const char *src)
{
	if (src != NULL) {
		(void)strncpy_s(dst, dst_size, src, dst_size - 1U);
	}
}

static const char *dts_string_prop(const void *fdt, int32_t node, const char *name,
	const char *default_value)
{
	const char *value = fdt_getprop(fdt, node, name, NULL);

	return value != NULL ? value : default_value;
}

static const char *dts_required_string_prop(const void *fdt, int32_t node, const char *name)
{
	const char *value = fdt_getprop(fdt, node, name, NULL);

	if (value == NULL) {
		arm64_dts_panic(name, -FDT_ERR_NOTFOUND);
	}

	return value;
}

static bool dts_stringlist_contains(const void *fdt, int32_t node, const char *name,
	const char *value)
{
	const char *prop;
	int32_t len;

	prop = fdt_getprop(fdt, node, name, &len);
	return (prop != NULL) && (fdt_stringlist_contains(prop, len, value) != 0);
}

static int32_t dts_vm_generic_node(const void *fdt, int32_t vm_root)
{
	int32_t generic = dts_child_by_any_name(fdt, vm_root, "generic", "guest-defaults");

	if (generic < 0) {
		arm64_dts_panic("/vm/generic", generic);
	}

	return generic;
}

static uint32_t dts_optional_u32_from_node(const void *fdt, int32_t parent,
	const char *node_name, const char *prop_name, uint32_t default_value)
{
	int32_t node = dts_child_by_unit_name(fdt, parent, node_name);

	return node < 0 ? default_value : dts_u32_prop(fdt, node, prop_name, default_value);
}

static uint32_t dts_parse_virtio_proxy_throughput(const void *fdt, int32_t node)
{
	const char *throughput = dts_string_prop(fdt, node, "beau,throughput", "low");

	return (strcmp(throughput, "high") == 0) ?
		ARM64_VIRTIO_PROXY_THROUGHPUT_HIGH :
		ARM64_VIRTIO_PROXY_THROUGHPUT_LOW;
}

static void dts_parse_mmio_ranges(const void *fdt, int32_t platform)
{
	int32_t ranges;
	int32_t node;

	dts_mmio_region_count = 0U;
	ranges = dts_child_by_unit_name(fdt, platform, "mmio-ranges");
	if (ranges < 0) {
		return;
	}

	fdt_for_each_subnode(node, fdt, ranges) {
		if (dts_mmio_region_count >= ARRAY_SIZE(dts_mmio_regions)) {
			panic("too many arm64 platform mmio ranges");
		}
		dts_reg_by_index(fdt, node, 0U,
			&dts_mmio_regions[dts_mmio_region_count].base,
			&dts_mmio_regions[dts_mmio_region_count].size);
		dts_mmio_region_count++;
	}
}

static void dts_parse_pcie_host(const void *fdt, int32_t soc)
{
	struct pci_mmcfg_region region = {
		.address = DEFAULT_PCI_MMCFG_BASE,
		.start_bus = DEFAULT_PCI_MMCFG_START_BUS,
		.end_bus = DEFAULT_PCI_MMCFG_END_BUS,
	};
	const fdt32_t *bus_range;
	int32_t len;
	int32_t pcie;
	uint64_t ecam_base;
	uint64_t ecam_size;

	pcie = dts_child_compatible(fdt, soc, "pci-host-ecam-generic");
	if (pcie < 0) {
		return;
	}

	dts_reg_by_index(fdt, pcie, 0U, &ecam_base, &ecam_size);
	region.address = ecam_base;

	bus_range = fdt_getprop(fdt, pcie, "bus-range", &len);
	if (bus_range != NULL) {
		if (len < (int32_t)(2U * sizeof(fdt32_t))) {
			arm64_dts_panic("pcie bus-range", -EINVAL);
		}
		region.start_bus = (uint8_t)fdt32_to_cpu(bus_range[0]);
		region.end_bus = (uint8_t)fdt32_to_cpu(bus_range[1]);
	} else if (ecam_size >= 0x100000UL) {
		uint64_t buses = ecam_size >> 20U;

		region.start_bus = 0U;
		region.end_bus = (buses > 256UL) ? 0xffU : (uint8_t)(buses - 1UL);
	}

	set_mmcfg_region(&region);
}

static uint32_t dts_stream_id_from_iommus(const void *fdt, int32_t node)
{
	const fdt32_t *prop;
	int32_t len;
	int32_t smmu;
	uint32_t phandle;
	uint32_t stream_id;
	uint32_t iommu_cells;

	prop = fdt_getprop(fdt, node, "iommus", &len);
	if (prop == NULL) {
		return ARM_SMMU_STREAM_ID_INVALID;
	}
	if (len < (int32_t)(2U * sizeof(fdt32_t))) {
		arm64_dts_panic("iommus", -EINVAL);
	}

	phandle = fdt32_to_cpu(prop[0]);
	stream_id = fdt32_to_cpu(prop[1]);
	smmu = fdt_node_offset_by_phandle(fdt, phandle);
	if ((smmu < 0) || !dts_has_compatible(fdt, smmu, "arm,smmu-v3")) {
		arm64_dts_panic("iommus arm,smmu-v3", smmu < 0 ? smmu : -EINVAL);
	}

	iommu_cells = dts_u32_prop(fdt, smmu, "#iommu-cells", 0U);
	if (iommu_cells != 1U) {
		arm64_dts_panic("#iommu-cells", -EINVAL);
	}

	return stream_id;
}

static uint32_t dts_parse_pt_stream_id(const void *fdt, int32_t node)
{
	uint32_t stream_id = dts_stream_id_from_iommus(fdt, node);

	if (stream_id == ARM_SMMU_STREAM_ID_INVALID) {
		stream_id = dts_u32_prop(fdt, node, "beau,stream-id",
			ARM_SMMU_STREAM_ID_INVALID);
	}
	if (stream_id == ARM_SMMU_STREAM_ID_INVALID) {
		arm64_dts_panic("passthrough stream-id", -EINVAL);
	}

	return stream_id;
}

static bool dts_parse_pt_spi(const void *fdt, int32_t node,
	struct passthrough_spi_mapping *mapping)
{
	const fdt32_t *irq;
	int32_t len;
	uint32_t type;
	uint32_t number;
	uint32_t flags;
	uint32_t phys_spi;
	uint32_t virt_irq;

	irq = fdt_getprop(fdt, node, "interrupts", &len);
	if (irq == NULL) {
		return false;
	}
	if (len < (int32_t)(3U * sizeof(fdt32_t))) {
		arm64_dts_panic("passthrough interrupts", -EINVAL);
	}

	type = fdt32_to_cpu(irq[0]);
	number = fdt32_to_cpu(irq[1]);
	flags = fdt32_to_cpu(irq[2]);
	if (type != ARM64_DTS_GIC_SPI) {
		arm64_dts_panic("passthrough spi type", -EINVAL);
	}

	phys_spi = number + 32U;
	if ((phys_spi < 32U) || (phys_spi >= ARM64_GIC_SPURIOUS_INTID)) {
		arm64_dts_panic("passthrough phys spi", -EINVAL);
	}

	virt_irq = dts_u32_prop(fdt, node, "beau,guest-irq", UINT32_MAX);
	if ((virt_irq < 32U) || (virt_irq >= ARM64_GIC_SPURIOUS_INTID)) {
		arm64_dts_panic("passthrough guest irq", -EINVAL);
	}

	mapping->phys_spi = phys_spi;
	mapping->virt_irq = virt_irq;
	mapping->level = flags == ARM64_DTS_IRQ_TYPE_LEVEL_HIGH;
	if ((flags != ARM64_DTS_IRQ_TYPE_LEVEL_HIGH) &&
		(flags != ARM64_DTS_IRQ_TYPE_EDGE_RISING)) {
		arm64_dts_panic("passthrough irq flags", -EINVAL);
	}

	return true;
}

static void dts_parse_passthrough_device(const void *fdt, int32_t node)
{
	struct passthrough_spi_mapping mapping;
	uint32_t stream_id = dts_parse_pt_stream_id(fdt, node);
	uint32_t owner_prop = dts_u32_prop(fdt, node, "beau,owner-vm",
		ACRN_INVALID_VMID);
	uint16_t owner_vmid;
	const char *name = dts_string_prop(fdt, node, "beau,name", NULL);
	bool writable = fdt_getprop(fdt, node, "beau,writable", NULL) != NULL;
	int32_t ret;

	/* [20260709] ARM64 passthrough policy parse:
	 *
	 *   platform.dts -> StreamID owner policy -> BSP passthrough ledger
	 *                         |
	 *                         v
	 *                 later assignment must bind SMMU before MMIO
	 *
	 * This parser registers only static ownership policy. The SMMU driver
	 * still fails closed until real stream-table hardware is ready.
	 */
	if ((owner_prop != ACRN_INVALID_VMID) && (owner_prop >= CONFIG_MAX_VM_NUM)) {
		arm64_dts_panic("passthrough owner vm", -EINVAL);
	}
	owner_vmid = (uint16_t)owner_prop;

	ret = passthrough_register_device_owner(stream_id, name, writable, owner_vmid);
	if (ret != 0) {
		arm64_dts_panic("passthrough device", ret);
	}

	if (dts_parse_pt_spi(fdt, node, &mapping)) {
		ret = passthrough_register_spi(stream_id, &mapping);
		if (ret != 0) {
			arm64_dts_panic("passthrough spi", ret);
		}
	}
}

static void dts_parse_passthrough_policy(const void *fdt, int32_t platform)
{
	int32_t passthrough;
	int32_t node;

	passthrough = dts_child_by_unit_name(fdt, platform, "passthrough");
	if (passthrough < 0) {
		return;
	}

	fdt_for_each_subnode(node, fdt, passthrough) {
		if (dts_has_compatible(fdt, node, "beau,passthrough-device")) {
			dts_parse_passthrough_device(fdt, node);
		}
	}
}

static void dts_parse_ipc_channel(const void *fdt, int32_t node)
{
	struct arm64_vipc_channel_config config;
	const fdt32_t *endpoints;
	uint64_t channel_id;
	uint64_t ignored_size;
	int32_t len;
	int32_t ret;

	(void)memset(&config, 0U, sizeof(config));
	dts_reg_by_index(fdt, node, 0U, &channel_id, &ignored_size);
	if (channel_id > UINT32_MAX) {
		arm64_dts_panic("ipc channel id", -EINVAL);
	}
	config.channel_id = (uint32_t)channel_id;

	endpoints = fdt_getprop(fdt, node, "endpoint-vms", &len);
	if ((endpoints == NULL) || (len < (int32_t)(2U * sizeof(fdt32_t)))) {
		arm64_dts_panic("ipc endpoint-vms", -EINVAL);
	}
	config.endpoint_vmid[0] = (uint16_t)fdt32_to_cpu(endpoints[0]);
	config.endpoint_vmid[1] = (uint16_t)fdt32_to_cpu(endpoints[1]);
	config.gpa_base = dts_u64_prop(fdt, node, "gpa-base", 0UL);
	config.ring_size = dts_u32_prop(fdt, node, "ring-size",
		ARM64_VIPC_RING_SIZE_DEFAULT);
	config.notify_virq = dts_u32_prop(fdt, node, "notify-virq", 0U);

	ret = arm64_vipc_register_channel(&config);
	if (ret != 0) {
		arm64_dts_panic("ipc channel", ret);
	}
}

static void dts_parse_ipc_channels(const void *fdt, int32_t platform)
{
	int32_t channels;
	int32_t node;

	channels = dts_child_by_unit_name(fdt, platform, "ipc-channels");
	if (channels < 0) {
		return;
	}

	fdt_for_each_subnode(node, fdt, channels) {
		if (dts_has_compatible(fdt, node, "beau,ipc-channel")) {
			dts_parse_ipc_channel(fdt, node);
		}
	}
}

void arm64_platform_dts_parse_info(const void *fdt, struct arm64_platform_dts_info *info)
{
	int32_t platform;

	if (info == NULL) {
		panic("invalid arm64 platform dts info");
	}

	info->gic_iidr = 0x43bU;
	info->guest_cpu_compatible = "arm,cortex-a57";
	info->vfdt_model = "linux,dummy-virt";
	info->vfdt_compatible = "linux,dummy-virt";
	info->uart_clock_hz = 24000000U;
	info->uart_baud = 115200U;
	info->service_vm_initrd = false;
	dts_mmio_region_count = 0U;

	platform = fdt_path_offset(fdt, "/beau,platform");
	if (platform < 0) {
		return;
	}

	info->gic_iidr = dts_u32_prop(fdt, platform, "gic-iidr", info->gic_iidr);
	info->guest_cpu_compatible = dts_string_prop(fdt, platform,
		"guest-cpu-compatible", info->guest_cpu_compatible);
	info->vfdt_model = dts_string_prop(fdt, platform, "vfdt-model", info->vfdt_model);
	info->vfdt_compatible = dts_string_prop(fdt, platform, "vfdt-compatible",
		info->vfdt_compatible);
	info->service_vm_initrd = dts_u32_prop(fdt, platform, "service-vm-initrd", 0U) != 0U;
	info->uart_clock_hz = dts_optional_u32_from_node(fdt, platform, "uart",
		"clock-frequency", info->uart_clock_hz);
	info->uart_baud = dts_optional_u32_from_node(fdt, platform, "uart",
		"current-speed", info->uart_baud);
	dts_parse_mmio_ranges(fdt, platform);
	dts_parse_passthrough_policy(fdt, platform);
	dts_parse_ipc_channels(fdt, platform);
}

const struct arm64_mem_region *arm64_platform_dts_mmio_regions(uint32_t *count)
{
	*count = dts_mmio_region_count;
	return dts_mmio_regions;
}

static uint32_t dts_count_cpus(const void *fdt)
{
	int32_t cpus;
	int32_t cpu;
	uint32_t count = 0U;

	cpus = fdt_path_offset(fdt, "/cpus");
	if (cpus < 0) {
		arm64_dts_panic("/cpus", cpus);
	}

	fdt_for_each_subnode(cpu, fdt, cpus) {
		const char *device_type = fdt_getprop(fdt, cpu, "device_type", NULL);

		if ((device_type != NULL) && (strcmp(device_type, "cpu") == 0)) {
			count++;
		}
	}

	return count;
}

static void dts_parse_memory(const void *fdt, uint64_t *base, uint64_t *size)
{
	int32_t node;

	fdt_for_each_subnode(node, fdt, 0) {
		const char *device_type = fdt_getprop(fdt, node, "device_type", NULL);

		if ((device_type != NULL) && (strcmp(device_type, "memory") == 0)) {
			dts_reg_by_index(fdt, node, 0U, base, size);
			return;
		}
	}

	arm64_dts_panic("/memory", -FDT_ERR_NOTFOUND);
}

static void dts_validate_timer(const void *fdt, int32_t generic)
{
	int32_t timer = dts_child_by_name(fdt, generic, "timer");
	uint32_t htimer_ppi;
	uint32_t vtimer_ppi;
	uint32_t ptimer_ppi;

	if (timer < 0) {
		arm64_dts_panic("vm/generic/timer", timer);
	}

	htimer_ppi = dts_u32_prop(fdt, timer, "htimer-ppi",
		dts_u32_prop(fdt, timer, "host-timer-ppi", UINT32_MAX));
	vtimer_ppi = dts_u32_prop(fdt, timer, "vtimer-ppi",
		dts_u32_prop(fdt, timer, "guest-virtual-timer-ppi", UINT32_MAX));
	ptimer_ppi = dts_u32_prop(fdt, timer, "ptimer-ppi",
		dts_u32_prop(fdt, timer, "guest-physical-timer-ppi", UINT32_MAX));

	if ((htimer_ppi != ARM64_GIC_PPI_HYPERVISOR_TIMER) ||
		(vtimer_ppi != ARM64_GIC_PPI_VIRTUAL_TIMER) ||
		(ptimer_ppi != ARM64_GIC_PPI_PHYSICAL_TIMER)) {
		panic("arm64 timer ppi mismatch: h=%u v=%u p=%u",
			htimer_ppi, vtimer_ppi, ptimer_ppi);
	}
}

void arm64_platform_dts_parse_board(const void *fdt,
	const struct arm64_platform_dts_info *info)
{
	int32_t soc;
	int32_t gic;
	int32_t its;
	int32_t smmu;
	int32_t uart;
	int32_t vm;
	int32_t generic;
	uint64_t gicr_base;
	uint64_t gicr_total_size;
	uint32_t cpu_count;

	if (info == NULL) {
		panic("invalid arm64 platform dts info");
	}

	dts_parse_memory(fdt, &beau_config.ram_start, &beau_config.ram_size);

	soc = fdt_path_offset(fdt, "/soc");
	if (soc < 0) {
		arm64_dts_panic("/soc", soc);
	}
	dts_parse_pcie_host(fdt, soc);

	gic = dts_child_compatible(fdt, soc, "arm,gic-v3");
	if (gic < 0) {
		arm64_dts_panic("arm,gic-v3", gic);
	}
	dts_reg_by_index(fdt, gic, 0U, &beau_config.gicd_base,
		&beau_config.gicd_size);
	dts_reg_by_index(fdt, gic, 1U, &gicr_base, &gicr_total_size);

	cpu_count = dts_count_cpus(fdt);
	if ((cpu_count == 0U) || (cpu_count > MAX_PCPU_NUM)) {
		panic("invalid arm64 dts cpu count %u", cpu_count);
	}
	beau_config.gicr_base = gicr_base;
	beau_config.gicr_size = gicr_total_size;
	beau_config.gicr_stride = gicr_total_size / cpu_count;
	if (beau_config.gicr_stride == 0UL) {
		panic("invalid arm64 dts gicr size=0x%lx cpu_count=%u",
			gicr_total_size, cpu_count);
	}

	its = dts_child_compatible(fdt, gic, "arm,gic-v3-its");
	if (its >= 0) {
		dts_reg_by_index(fdt, its, 0U, &beau_config.gits_base,
			&beau_config.gits_size);
	}

	smmu = dts_child_compatible(fdt, soc, "arm,smmu-v3");
	if (smmu >= 0) {
		dts_reg_by_index(fdt, smmu, 0U, &beau_config.smmu_base,
			&beau_config.smmu_size);
	}

	uart = dts_child_compatible(fdt, soc, "arm,pl011");
	if (uart < 0) {
		arm64_dts_panic("arm,pl011", uart);
	}
	{
		uint64_t uart_size;

		dts_reg_by_index(fdt, uart, 0U, &beau_config.console_mmio_base,
			&uart_size);
	}

	vm = fdt_path_offset(fdt, "/vm");
	if (vm < 0) {
		arm64_dts_panic("/vm", vm);
	}
	generic = dts_vm_generic_node(fdt, vm);
	dts_validate_timer(fdt, generic);
	beau_config.gic_iidr = info->gic_iidr;
}

static uint64_t dts_parse_cpu_affinity(const void *fdt, int32_t node,
	uint16_t *order, uint16_t *order_num)
{
	const fdt32_t *prop;
	int32_t len;
	int32_t cells;
	int32_t i;
	uint32_t count;
	uint64_t affinity = 0UL;
	uint32_t pcpu_id;

	/* [20260708] preserve the authored pCPU order from DTS:
	 *
	 *   cpu-affinity = <1 6>
	 *        -> order[0] = 1 -> vcpu0
	 *        -> order[1] = 6 -> vcpu1
	 *
	 * The returned bitmap is only a set view for legacy common code.
	 */
	prop = fdt_getprop(fdt, node, "cpu-affinity", &len);
	if ((prop == NULL) || (len <= 0) || ((len % (int32_t)sizeof(fdt32_t)) != 0)) {
		arm64_dts_panic("cpu-affinity", prop == NULL ? len : -EINVAL);
	}

	cells = len / (int32_t)sizeof(fdt32_t);
	count = (uint32_t)cells;
	if (count > MAX_PCPU_NUM) {
		panic("too many arm64 dts cpu-affinity entries: %d", cells);
	}
	if ((order == NULL) || (order_num == NULL)) {
		arm64_dts_panic("cpu-affinity storage", -EINVAL);
	}

	*order_num = 0U;
	for (i = 0; i < cells; i++) {
		pcpu_id = fdt32_to_cpu(prop[i]);
		if (pcpu_id >= MAX_PCPU_NUM) {
			panic("invalid arm64 dts vm pcpu id %u", pcpu_id);
		}
		if ((affinity & AFFINITY_CPU(pcpu_id)) != 0UL) {
			panic("duplicate arm64 dts vm pcpu id %u", pcpu_id);
		}
		order[*order_num] = (uint16_t)pcpu_id;
		(*order_num)++;
		affinity |= AFFINITY_CPU(pcpu_id);
	}

	return affinity;
}

static uint64_t dts_parse_guest_flags(const void *fdt, int32_t node)
{
	uint64_t flags = 0UL;

	if (dts_stringlist_contains(fdt, node, "guest-flags", "static")) {
		flags |= GUEST_FLAG_STATIC_VM;
	}
	if (dts_stringlist_contains(fdt, node, "guest-flags", "no-fw")) {
		flags |= GUEST_FLAG_NO_FW;
	}
	if (dts_stringlist_contains(fdt, node, "guest-flags", "rt")) {
		flags |= GUEST_FLAG_RT;
	}

	return flags;
}

static void dts_parse_load_order(const void *fdt, int32_t node,
	struct acrn_vm_config *vm_config)
{
	const char *load_order = dts_string_prop(fdt, node, "load-order", "");

	if (strcmp(load_order, "service") == 0) {
		vm_config->load_order = SERVICE_VM;
		vm_config->severity = SEVERITY_SERVICE_VM;
	} else if ((strcmp(load_order, "pre-std") == 0) ||
		(strcmp(load_order, "pre-launched") == 0)) {
		vm_config->load_order = PRE_LAUNCHED_VM;
		vm_config->severity = SEVERITY_STANDARD_VM;
	} else if (strcmp(load_order, "pre-rt") == 0) {
		vm_config->load_order = PRE_LAUNCHED_VM;
		vm_config->severity = SEVERITY_RTVM;
	} else {
		panic("unknown arm64 dts vm load-order '%s'", load_order);
	}
}

static enum vm_os_family dts_parse_os_family(const char *family)
{
	enum vm_os_family ret = VM_OS_BAREMETAL;

	if (strcmp(family, "rtos") == 0) {
		ret = VM_OS_RTOS;
	} else if (strcmp(family, "linux") == 0) {
		ret = VM_OS_LINUX;
	} else if (strcmp(family, "baremetal") != 0) {
		panic("unknown arm64 dts vm os family '%s'", family);
	}

	return ret;
}

static bool dts_is_vboot_node(const void *fdt, int32_t node)
{
	return dts_has_any_compatible(fdt, node, "beau,vboot", "beau,module");
}

static int32_t dts_find_module(const void *fdt, int32_t vm_node, const char *compat)
{
	int32_t node;

	fdt_for_each_subnode(node, fdt, vm_node) {
		if (dts_is_vboot_node(fdt, node) && dts_has_compatible(fdt, node, compat)) {
			return node;
		}
	}

	return -FDT_ERR_NOTFOUND;
}

static uint64_t dts_module_addr(const void *fdt, int32_t node,
	const struct arm64_platform_dts_ops *ops)
{
	const char *source = dts_string_prop(fdt, node, "source",
		dts_string_prop(fdt, node, "src-type", ""));
	const char *symbol;
	uint64_t addr;
	uint64_t size;

	if (strcmp(source, "symbol") == 0) {
		symbol = dts_string_prop(fdt, node, "addr-symbol", "");
		if ((ops == NULL) || (ops->module_addr == NULL)) {
			panic("arm64 dts module addr symbol unsupported: %s", symbol);
		}
		return (uint64_t)ops->module_addr(symbol);
	}

	dts_reg_by_index(fdt, node, 0U, &addr, &size);
	return addr;
}

static uint64_t dts_module_size(const void *fdt, int32_t node,
	const struct arm64_platform_dts_ops *ops)
{
	const char *size_source = dts_string_prop(fdt, node, "size-source", "");
	const char *symbol;
	uint64_t addr;
	uint64_t size;

	if (strcmp(size_source, "symbol") == 0) {
		symbol = dts_string_prop(fdt, node, "size-symbol", "");
		if ((ops == NULL) || (ops->module_size == NULL)) {
			panic("arm64 dts module size symbol unsupported: %s", symbol);
		}
		return ops->module_size(symbol);
	}

	dts_reg_by_index(fdt, node, 0U, &addr, &size);
	return size;
}

static void dts_parse_sched(const void *fdt, int32_t vm_node,
	struct acrn_vm_config *vm_config)
{
	int32_t sched = dts_child_by_name(fdt, vm_node, "sched");

	if (sched >= 0) {
		vm_config->sched_params.bvt_weight =
			dts_u32_prop(fdt, sched, "bvt-weight", 0U);
		vm_config->sched_params.bvt_warp_value =
			(int32_t)dts_u32_prop(fdt, sched, "bvt-warp-value", 0U);
		vm_config->sched_params.bvt_warp_limit =
			dts_u32_prop(fdt, sched, "bvt-warp-limit", 0U);
		vm_config->sched_params.bvt_unwarp_period =
			dts_u32_prop(fdt, sched, "bvt-unwarp-period", 0U);
		vm_config->sched_params.cbs_period_us =
			dts_u32_prop(fdt, sched, "cbs-period-us", 0U);
		vm_config->sched_params.cbs_budget_us =
			dts_u32_prop(fdt, sched, "cbs-budget-us", 0U);
	}
}

static void dts_parse_pci_vbar_base(const void *fdt, int32_t node,
	struct acrn_vm_pci_dev_config *dev_config)
{
	const fdt32_t *prop;
	int32_t len;
	uint32_t entries;
	uint32_t idx;

	prop = fdt_getprop(fdt, node, "beau,vbar-base", &len);
	if (prop == NULL) {
		return;
	}
	if ((len <= 0) || ((len % (int32_t)(2U * sizeof(fdt32_t))) != 0)) {
		arm64_dts_panic("beau,vbar-base", -EINVAL);
	}

	entries = (uint32_t)len / (2U * (uint32_t)sizeof(fdt32_t));
	if (entries > PCI_BAR_COUNT) {
		arm64_dts_panic("too many beau,vbar-base entries", -EINVAL);
	}

	for (idx = 0U; idx < entries; idx++) {
		dev_config->vbar_base[idx] =
			((uint64_t)fdt32_to_cpu(prop[idx * 2U]) << 32U) |
			(uint64_t)fdt32_to_cpu(prop[(idx * 2U) + 1U]);
	}
}

static void dts_parse_pci_pbar_base(const void *fdt, int32_t node,
	struct acrn_vm_pci_dev_config *dev_config)
{
	const fdt32_t *prop;
	int32_t len;
	uint32_t entries;
	uint32_t idx;

	prop = fdt_getprop(fdt, node, "beau,pbar-base", &len);
	if (prop == NULL) {
		return;
	}
	if ((len <= 0) || ((len % (int32_t)(2U * sizeof(fdt32_t))) != 0)) {
		arm64_dts_panic("beau,pbar-base", -EINVAL);
	}

	entries = (uint32_t)len / (2U * (uint32_t)sizeof(fdt32_t));
	if (entries > PCI_BAR_COUNT) {
		arm64_dts_panic("too many beau,pbar-base entries", -EINVAL);
	}

	for (idx = 0U; idx < entries; idx++) {
		dev_config->pbar_base[idx] =
			((uint64_t)fdt32_to_cpu(prop[idx * 2U]) << 32U) |
			(uint64_t)fdt32_to_cpu(prop[(idx * 2U) + 1U]);
	}
}

static void dts_parse_pci_devices(const void *fdt, int32_t vm_node, uint16_t vm_id,
	struct acrn_vm_config *vm_config)
{
	int32_t pci_devices;
	int32_t node;

	vm_config->pci_devs = dts_storage->pci_devs[vm_id];
	vm_config->pci_dev_num = 0U;

	pci_devices = dts_child_by_unit_name(fdt, vm_node, "pci-devices");
	if (pci_devices < 0) {
		return;
	}

	fdt_for_each_subnode(node, fdt, pci_devices) {
		struct acrn_vm_pci_dev_config *dev_config;
		uint32_t pbdf;
		uint32_t vbdf;

		if (!dts_has_compatible(fdt, node, "beau,passthrough-pci-device")) {
			continue;
		}
		if (vm_config->pci_dev_num >= CONFIG_MAX_PCI_DEV_NUM) {
			arm64_dts_panic("too many pci-devices", -EINVAL);
		}

		pbdf = dts_u32_prop(fdt, node, "beau,pbdf", UINT32_MAX);
		if (pbdf > 0xffffU) {
			arm64_dts_panic("beau,pbdf", -EINVAL);
		}
		vbdf = dts_u32_prop(fdt, node, "beau,vbdf", pbdf);
		if (vbdf > 0xffffU) {
			arm64_dts_panic("beau,vbdf", -EINVAL);
		}

		dev_config = &vm_config->pci_devs[vm_config->pci_dev_num];
		(void)memset(dev_config, 0U, sizeof(*dev_config));
		dev_config->emu_type = PCI_DEV_TYPE_PTDEV;
		dev_config->pbdf.value = (uint16_t)pbdf;
		dev_config->vbdf.value = (uint16_t)vbdf;
		dev_config->optional = fdt_getprop(fdt, node, "beau,optional", NULL) != NULL;
		dts_parse_pci_pbar_base(fdt, node, dev_config);
		dts_parse_pci_vbar_base(fdt, node, dev_config);
		vm_config->pci_dev_num++;
	}
}

static void dts_parse_os(const void *fdt, int32_t vm_node,
	struct acrn_vm_config *vm_config, const struct arm64_platform_dts_ops *ops)
{
	int32_t os = dts_child_by_name(fdt, vm_node, "os");
	int32_t kernel = dts_find_module(fdt, vm_node, "beau,kernel");
	int32_t ramdisk = dts_find_module(fdt, vm_node, "beau,ramdisk");
	int32_t dtb = dts_find_module(fdt, vm_node, "beau,device-tree");
	uint64_t addr;
	uint64_t size;

	if (os < 0) {
		arm64_dts_panic("os", os);
	}

	dts_copy_string(vm_config->os_config.name, MAX_VM_OS_NAME_LEN,
		dts_string_prop(fdt, os, "os-name", ""));
	vm_config->os_config.os_family = dts_parse_os_family(
		dts_string_prop(fdt, os, "family", "baremetal"));
	vm_config->os_config.kernel_type = KERNEL_RAWIMAGE;

	if (kernel >= 0) {
		dts_copy_string(vm_config->os_config.kernel_mod_tag, MAX_MOD_TAG_LEN,
			dts_string_prop(fdt, kernel, "tag", ""));
		dts_copy_string(vm_config->os_config.bootargs, MAX_BOOTARGS_SIZE,
			dts_string_prop(fdt, kernel, "bootargs",
				dts_string_prop(fdt, os, "bootargs", "")));
		dts_reg_by_index(fdt, kernel, 0U, &addr, &size);
		vm_config->os_config.kernel_load_addr =
			dts_addr_prop(fdt, kernel, "load-addr", addr);
		vm_config->os_config.kernel_entry_addr =
			dts_addr_prop(fdt, kernel, "entry-addr",
				vm_config->os_config.kernel_load_addr);
	}

	if (ramdisk >= 0) {
		dts_copy_string(vm_config->os_config.ramdisk_mod_tag, MAX_MOD_TAG_LEN,
			dts_string_prop(fdt, ramdisk, "tag", ""));
		vm_config->os_config.kernel_ramdisk_addr =
			dts_addr_prop(fdt, ramdisk, "load-addr", 0UL);
		vm_config->os_config.kernel_ramdisk_size =
			dts_module_size(fdt, ramdisk, ops);
	}

	if (dtb >= 0) {
		dts_copy_string(vm_config->fdt_config.fdt_mod_tag, MAX_MOD_TAG_LEN,
			dts_string_prop(fdt, dtb, "tag", ""));
	}
}

static void dts_parse_vm_mpu(const void *fdt, int32_t vm_node,
	struct acrn_vm_config *vm_config)
{
	const char *sve = dts_string_prop(fdt, vm_node, "beau,sve",
		dts_string_prop(fdt, vm_node, "sve", "disabled"));
	uint32_t vl_bits;
	uint32_t host_vl_bits;

	vm_config->arch.guest_feature_mask = 0UL;
	vm_config->arch.guest_sve_vl_bits = ARM64_SVE_VL_BITS_DEFAULT;

	if (strcmp(sve, "disabled") == 0) {
		return;
	}
	if (strcmp(sve, "enabled") != 0) {
		panic("unknown arm64 dts beau,sve policy '%s'", sve);
	}
	if (vm_config->os_config.os_family != VM_OS_LINUX) {
		panic("arm64 dts SVE is only allowed for linux vm");
	}

	vl_bits = dts_u32_prop(fdt, vm_node, "beau,sve-vl-bits",
		dts_u32_prop(fdt, vm_node, "sve-vl-bits", ARM64_SVE_VL_BITS_DEFAULT));
	if ((vl_bits < ARM64_SVE_VL_BITS_MIN) ||
		(vl_bits > ARM64_SVE_VL_BITS_MAX) ||
		((vl_bits % ARM64_SVE_VL_BITS_MIN) != 0U)) {
		panic("invalid arm64 dts SVE vector length %u bits", vl_bits);
	}
	host_vl_bits = arm64_sve_host_vl_bits();
	if ((host_vl_bits != 0U) && (vl_bits > host_vl_bits)) {
		panic("arm64 dts SVE vector length %u exceeds host %u bits",
			vl_bits, host_vl_bits);
	}

	vm_config->arch.guest_feature_mask |= ARM64_VM_FEATURE_SVE;
	vm_config->arch.guest_sve_vl_bits = vl_bits;
}

static void dts_parse_arch(const void *fdt, int32_t generic, uint16_t vm_id,
	struct acrn_vm_config *vm_config)
{
	int32_t gic = dts_child_compatible(fdt, generic, "arm,vgic-v3");
	int32_t uart = dts_child_compatible(fdt, generic, "arm,vpl011");
	int32_t virtio_console = dts_child_compatible(fdt, generic, "beau,virtio-console");
	int32_t its;
	uint64_t base;
	uint64_t size;
	const fdt32_t *irq_prop;
	const char *access;
	uint16_t proxy_count = 0U;

	if (gic < 0) {
		arm64_dts_panic("gic-mmio", gic);
	}
	if (uart < 0) {
		arm64_dts_panic("uart-mmio", uart);
	}

	vm_config->arch.guest_ram_start = vm_config->memory.host_regions[0].start_hpa;
	vm_config->arch.guest_ram_size = vm_config->memory.host_regions[0].size_hpa;
	vm_config->arch.guest_ram_hpa = vm_config->memory.host_regions[0].start_hpa;

	dts_reg_by_index(fdt, gic, 0U, &vm_config->arch.guest_gicd_base,
		&vm_config->arch.guest_gicd_size);
	dts_reg_by_index(fdt, gic, 1U, &vm_config->arch.guest_gicr_base,
		&vm_config->arch.guest_gicr_size);
	vm_config->arch.guest_gicr_stride = vm_config->arch.guest_gicr_size / MAX_PCPU_NUM;

	its = dts_child_compatible(fdt, gic, "arm,gic-v3-its");
	if (its >= 0) {
		dts_reg_by_index(fdt, its, 0U, &base, &size);
		vm_config->arch.guest_its_base = base;
		vm_config->arch.guest_its_size = size;
	}

	dts_reg_by_index(fdt, uart, 0U, &vm_config->arch.guest_uart_base,
		&vm_config->arch.guest_uart_size);
	irq_prop = fdt_getprop(fdt, uart, "interrupts", NULL);
	if (irq_prop == NULL) {
		arm64_dts_panic("uart interrupts", -EINVAL);
	}
	vm_config->arch.guest_uart_irq = fdt32_to_cpu(irq_prop[1]) + 32U;

	if (virtio_console >= 0) {
		dts_reg_by_index(fdt, virtio_console, 0U,
			&vm_config->arch.guest_virtio_console_base,
			&vm_config->arch.guest_virtio_console_size);
		irq_prop = fdt_getprop(fdt, virtio_console, "interrupts", NULL);
		if (irq_prop == NULL) {
			arm64_dts_panic("virtio-console interrupts", -EINVAL);
		}
		vm_config->arch.guest_virtio_console_irq = fdt32_to_cpu(irq_prop[1]) + 32U;
	}

	for (int32_t virtio_proxy = fdt_first_subnode(fdt, generic);
		virtio_proxy >= 0; virtio_proxy = fdt_next_subnode(fdt, virtio_proxy)) {
		struct arm64_virtio_proxy_config *proxy_config;
		uint32_t frontend_vmid;

		if (!dts_is_virtio_proxy(fdt, virtio_proxy)) {
			continue;
		}
		frontend_vmid = dts_u32_prop(fdt, virtio_proxy, "beau,frontend-vmid",
			ACRN_INVALID_VMID);
		if ((frontend_vmid != ACRN_INVALID_VMID) &&
			(frontend_vmid >= CONFIG_MAX_VM_NUM)) {
			arm64_dts_panic("virtio-proxy frontend vm", -EINVAL);
		}
		if ((frontend_vmid != ACRN_INVALID_VMID) &&
			(frontend_vmid != vm_id)) {
			continue;
		}
		if (proxy_count >= ARM64_VIRTIO_PROXY_MAX) {
			arm64_dts_panic("too many virtio-proxy nodes", -EINVAL);
		}

		proxy_config = &vm_config->arch.guest_virtio_proxy[proxy_count];
		dts_reg_by_index(fdt, virtio_proxy, 0U,
			&proxy_config->base, &proxy_config->size);
		irq_prop = fdt_getprop(fdt, virtio_proxy, "interrupts", NULL);
		if (irq_prop == NULL) {
			arm64_dts_panic("virtio-proxy interrupts", -EINVAL);
		}
		proxy_config->irq = fdt32_to_cpu(irq_prop[1]) + 32U;
		proxy_config->device_id = dts_u32_prop(fdt, virtio_proxy,
			"beau,device-id", VIRTIO_DEVICE_ID_FS);
		proxy_config->frontend_vmid =
			(uint16_t)((frontend_vmid == ACRN_INVALID_VMID) ? vm_id :
			frontend_vmid);
		proxy_config->queue_num =
			(uint16_t)dts_u32_prop(fdt, virtio_proxy, "beau,queue-num",
				VIRTIO_PROXY_QUEUE_NUM_DEFAULT);
			proxy_config->queue_size =
				(uint16_t)dts_u32_prop(fdt, virtio_proxy, "beau,queue-size",
					VIRTIO_PROXY_QUEUE_SIZE_DEFAULT);
			proxy_config->pending_num =
				(uint16_t)dts_u32_prop(fdt, virtio_proxy, "beau,pending-num",
					0U);
			dts_copy_string(proxy_config->tag, sizeof(proxy_config->tag),
				dts_string_prop(fdt, virtio_proxy, "beau,tag", "beau"));
		proxy_config->throughput = dts_parse_virtio_proxy_throughput(fdt,
			virtio_proxy);
		/* [20260708] virtio-proxy access policy:
		 *
		 *   frontend VM request -> BEAU virtio_proxy -> backend VM protocol service
		 *
		 * The proxy records a coarse access hint so protocol backends can
		 * reject mutating requests before completing descriptors. This access
		 * bit describes the frontend request stream, not the backend VM's local shell
		 * permissions. Keep the export writable by default so the frontend VM
		 * can create or update files and the backend VM can serve that state.
		 */
		access = dts_string_prop(fdt, virtio_proxy, "beau,access", "rw");
		proxy_config->access =
			(strcmp(access, "ro") == 0) ? VIRTIO_PROXY_ACCESS_READONLY :
			VIRTIO_PROXY_ACCESS_READWRITE;
			if ((proxy_config->size == 0UL) || (proxy_config->queue_num == 0U) ||
				(proxy_config->queue_num > VIRTIO_MMIO_MAX_QUEUES) ||
				(proxy_config->queue_size == 0U) ||
				(proxy_config->pending_num > VIRTIO_PROXY_PENDING_MAX)) {
				arm64_dts_panic("virtio-proxy queue", -EINVAL);
			}
		proxy_count++;
	}
	vm_config->arch.guest_virtio_proxy_num = proxy_count;
}

static uint16_t dts_vm_id_from_node(const void *fdt, int32_t node)
{
	const fdt32_t *reg;
	int32_t len;
	uint32_t vm_id;

	reg = fdt_getprop(fdt, node, "reg", &len);
	if ((reg == NULL) || (len < (int32_t)sizeof(fdt32_t))) {
		arm64_dts_panic("vm reg", reg == NULL ? len : -EINVAL);
	}
	vm_id = fdt32_to_cpu(reg[0]);
	if (vm_id >= CONFIG_MAX_VM_NUM) {
		panic("invalid arm64 dts vm id %u", vm_id);
	}

	return (uint16_t)vm_id;
}

static void dts_parse_static_mem(const void *fdt, int32_t node,
	uint64_t *ram_start, uint64_t *ram_size)
{
	const fdt32_t *mem;
	int32_t len;

	mem = fdt_getprop(fdt, node, "beau,static-mem", &len);
	if ((mem == NULL) || (len < (int32_t)(4U * sizeof(fdt32_t)))) {
		arm64_dts_panic("beau,static-mem", mem == NULL ? len : -EINVAL);
	}

	*ram_start = ((uint64_t)fdt32_to_cpu(mem[0]) << 32U) |
		(uint64_t)fdt32_to_cpu(mem[1]);
	*ram_size = ((uint64_t)fdt32_to_cpu(mem[2]) << 32U) |
		(uint64_t)fdt32_to_cpu(mem[3]);
}

static void dts_parse_vm_node(const void *fdt, int32_t generic, int32_t vm_node,
	const struct arm64_platform_dts_ops *ops)
{
	uint16_t vm_id = dts_vm_id_from_node(fdt, vm_node);
	struct acrn_vm_config *vm_config = &dts_storage->vm_configs[vm_id];
	uint64_t ram_start;
	uint64_t ram_size;

	dts_parse_load_order(fdt, vm_node, vm_config);
	dts_copy_string(vm_config->name, MAX_VM_NAME_LEN,
		dts_string_prop(fdt, vm_node, "beau,name", ""));
	vm_config->cpu_affinity = dts_parse_cpu_affinity(fdt, vm_node,
		vm_config->cpu_affinity_order, &vm_config->cpu_affinity_num);
	vm_config->guest_flags = dts_parse_guest_flags(fdt, vm_node);

	dts_parse_static_mem(fdt, vm_node, &ram_start, &ram_size);

	dts_storage->memory_regions[vm_id].start_hpa = ram_start;
	dts_storage->memory_regions[vm_id].size_hpa = ram_size;
	vm_config->memory.size = ram_size;
	vm_config->memory.region_num = 1U;
	vm_config->memory.host_regions = &dts_storage->memory_regions[vm_id];

	dts_parse_pci_devices(fdt, vm_node, vm_id, vm_config);
	dts_parse_sched(fdt, vm_node, vm_config);
	dts_parse_os(fdt, vm_node, vm_config, ops);
	dts_parse_vm_mpu(fdt, vm_node, vm_config);
	dts_parse_arch(fdt, generic, vm_id, vm_config);
}

static void dts_add_boot_option(const void *fdt, int32_t module,
	const struct arm64_platform_dts_ops *ops)
{
	struct bare_boot_option *option;

	if (dts_bare_boot_option_count >= dts_storage->boot_option_capacity) {
		panic("too many arm64 dts bare boot options");
	}

	option = &dts_storage->boot_options[dts_bare_boot_option_count];
	option->addr = dts_module_addr(fdt, module, ops);
	option->size = dts_module_size(fdt, module, ops);
	option->tag = dts_string_prop(fdt, module, "tag", "");
	dts_bare_boot_option_count++;
}

static void dts_parse_boot_options(const void *fdt, int32_t vm_root,
	const struct arm64_platform_dts_ops *ops)
{
	int32_t vm_node;
	int32_t module;

	dts_bare_boot_option_count = 0U;

	fdt_for_each_subnode(vm_node, fdt, vm_root) {
		if (!dts_has_compatible(fdt, vm_node, "beau,vm")) {
			continue;
		}
		fdt_for_each_subnode(module, fdt, vm_node) {
			if (dts_is_vboot_node(fdt, module)) {
				dts_add_boot_option(fdt, module, ops);
			}
		}
	}

	*dts_storage->boot_option_count = dts_bare_boot_option_count;
}

static enum sched_policy_id dts_parse_sched_policy(const char *policy)
{
	enum sched_policy_id sched_policy = SCHED_POLICY_NONE;

	if (strcmp(policy, "noop") == 0) {
		sched_policy = SCHED_POLICY_NOOP;
	} else if (strcmp(policy, "iorr") == 0) {
		sched_policy = SCHED_POLICY_IORR;
	} else if (strcmp(policy, "bvt") == 0) {
		sched_policy = SCHED_POLICY_BVT;
	} else if (strcmp(policy, "rtds") == 0) {
		sched_policy = SCHED_POLICY_RTDS;
	} else if (strcmp(policy, "cbs") == 0) {
		sched_policy = SCHED_POLICY_CBS;
	} else if (strcmp(policy, "cbs+") == 0) {
		sched_policy = SCHED_POLICY_CBS_PLUS;
	} else if (strcmp(policy, "prio") == 0) {
		sched_policy = SCHED_POLICY_PRIO;
	} else {
		panic("unknown arm64 dts scheduler policy '%s'", policy);
	}

	return sched_policy;
}

static uint64_t dts_all_pcpus_mask(void)
{
	return (MAX_PCPU_NUM >= 64U) ? UINT64_MAX : ((1UL << MAX_PCPU_NUM) - 1UL);
}

static uint64_t dts_parse_sched_pcpus(const void *fdt, int32_t node)
{
	const fdt32_t *prop;
	int32_t len;
	int32_t cells;
	int32_t i;
	uint32_t pcpu_id;
	uint64_t mask = 0UL;

	prop = fdt_getprop(fdt, node, "pcpus", &len);
	if ((prop == NULL) || (len <= 0) || ((len % (int32_t)sizeof(fdt32_t)) != 0)) {
		arm64_dts_panic("scheduler pcpus", prop == NULL ? len : -EINVAL);
	}

	cells = len / (int32_t)sizeof(fdt32_t);
	if ((cells == 1) && (fdt32_to_cpu(prop[0]) == UINT32_MAX)) {
		return 0UL;
	}

	for (i = 0; i < cells; i++) {
		pcpu_id = fdt32_to_cpu(prop[i]);
		if (pcpu_id >= MAX_PCPU_NUM) {
			panic("invalid arm64 dts scheduler pcpu id %u", pcpu_id);
		}
		if ((mask & AFFINITY_CPU(pcpu_id)) != 0UL) {
			panic("duplicate arm64 dts scheduler pcpu id %u", pcpu_id);
		}
		mask |= AFFINITY_CPU(pcpu_id);
	}

	return mask;
}

static void dts_parse_sched_cpupool(const void *fdt, int32_t sched,
	const char *node_name, struct sched_cpupool_config *pool)
{
	int32_t node = dts_child_by_unit_name(fdt, sched, node_name);

	if (node < 0) {
		arm64_dts_panic(node_name, node);
	}

	(void)memset(pool, 0U, sizeof(*pool));
	pool->configured = true;
	pool->has_pcpu_mask = true;
	pool->pcpu_mask = dts_parse_sched_pcpus(fdt, node);
	pool->policy = dts_parse_sched_policy(dts_required_string_prop(fdt, node, "policy"));
	/*
	 * period/budget are pool-level aliases shared by budget schedulers. The old
	 * cbs-period-us, cbs-budget-us, rtds-period-us, and rtds-budget-us names
	 * remain accepted so DTS migration can be incremental while still converging
	 * on one scheduler cpupool schema.
	 */
	pool->period_us = dts_u32_prop(fdt, node, "period",
		dts_u32_prop(fdt, node, "cbs-period-us",
		dts_u32_prop(fdt, node, "rtds-period-us", 0U)));
	pool->budget_us = dts_u32_prop(fdt, node, "budget",
		dts_u32_prop(fdt, node, "cbs-budget-us",
		dts_u32_prop(fdt, node, "rtds-budget-us", 0U)));
	/*
	 * A boolean gang knob hides which scheduler semantics are active. Fail closed
	 * and require policy = "cbs+" so CBS and CBS+ remain distinguishable in DTS,
	 * shell output, and regression logs.
	 */
	if (fdt_getprop(fdt, node, "gang", NULL) != NULL) {
		panic("arm64 dts scheduler gang property is obsolete; use policy \"cbs+\"");
	}
	/*
	 * Parsed for every pool so the config snapshot is complete; sched_cbs consumes
	 * the value only when the selected pool policy is cbs+.
	 */
	pool->gang_skew_us = dts_u32_prop(fdt, node, "gang-skew-us",
		ARM64_DTS_CBS_GANG_SKEW_US_DEFAULT);
}

static void dts_validate_sched_config(const struct sched_platform_config *config)
{
	uint64_t exclusive_mask = config->exclusive.pcpu_mask;
	uint64_t shared_mask = config->shared.pcpu_mask;
	uint64_t all_mask = dts_all_pcpus_mask();

	if (((exclusive_mask & shared_mask) != 0UL) ||
		(((exclusive_mask | shared_mask) & ~all_mask) != 0UL)) {
		panic("invalid arm64 dts scheduler cpupool overlap");
	}
	if ((exclusive_mask == 0UL) && (shared_mask == 0UL)) {
		panic("arm64 dts scheduler cpupools cannot both be empty");
	}
	if ((exclusive_mask | shared_mask) != all_mask) {
		panic("arm64 dts scheduler cpupools do not cover all pCPUs");
	}
}

static void dts_parse_hypervisor_sched(const void *fdt)
{
	struct sched_platform_config config = { 0U };
	int32_t hypervisor;
	int32_t sched;

	/* [20260710] scheduler policy comes only from platform.dts:
	 *
	 *   /hypervisor/sched
	 *       +-- exclusive-cpupool: pcpus + policy
	 *       +-- shared-cpupool:    pcpus + policy
	 *
	 * `pcpus = <-1>` is an explicit empty pool. It is valid for either pool
	 * but not for both, and the non-empty side must still cover every pCPU not
	 * listed in the other pool.
	 */
	hypervisor = fdt_path_offset(fdt, "/hypervisor");
	if (hypervisor < 0) {
		arm64_dts_panic("/hypervisor", hypervisor);
	}
	sched = dts_child_by_unit_name(fdt, hypervisor, "sched");
	if (sched < 0) {
		arm64_dts_panic("/hypervisor/sched", sched);
	}

	config.configured = true;
	dts_parse_sched_cpupool(fdt, sched, "exclusive-cpupool", &config.exclusive);
	dts_parse_sched_cpupool(fdt, sched, "shared-cpupool", &config.shared);
	dts_validate_sched_config(&config);
	sched_set_platform_config(&config);
}

void arm64_platform_dts_parse_vms(const void *fdt,
	const struct arm64_platform_dts_ops *ops,
	const struct arm64_platform_dts_vm_storage *storage)
{
	int32_t vm_root;
	int32_t generic;
	int32_t vm_node;
	uint32_t service_vm_id;

	/* [20260707] ARM64 platform-DTS principle:
	 *
	 * Platform policy is parsed once into the long-standing BEAU tables. VM
	 * creation and boot loaders stay table-driven, so DTS ownership ends before
	 * common VM code starts consuming the ABI.
	 *
	 *   platform.dtb
	 *       |
	 *       v
	 *   sdk/bsp DTS parser
	 *       |
	 *       +--> vm_configs[] ------> create_vm()
	 *       |
	 *       +--> bare_boot_options[] -> init_acrn_boot_info()
	 */
	dts_set_storage(storage);

	(void)memset(dts_storage->vm_configs, 0U,
		dts_storage->vm_config_count * sizeof(dts_storage->vm_configs[0]));
	(void)memset(dts_storage->memory_regions, 0U,
		dts_storage->vm_config_count * sizeof(dts_storage->memory_regions[0]));
	(void)memset(dts_storage->pci_devs, 0U,
		dts_storage->vm_config_count * CONFIG_MAX_PCI_DEV_NUM *
		sizeof(dts_storage->pci_devs[0][0]));
	(void)memset(dts_storage->boot_options, 0U,
		dts_storage->boot_option_capacity * sizeof(dts_storage->boot_options[0]));
	*dts_storage->boot_option_count = 0U;

	dts_parse_hypervisor_sched(fdt);

	vm_root = fdt_path_offset(fdt, "/vm");
	if (vm_root < 0) {
		arm64_dts_panic("/vm", vm_root);
	}
	generic = dts_vm_generic_node(fdt, vm_root);

	service_vm_id = dts_u32_prop(fdt, generic, "service-vm-id", 0U);
	if (service_vm_id != dts_storage->service_vm_id) {
		panic("arm64 dts service-vm-id %u != storage service-vm-id %u",
			service_vm_id, dts_storage->service_vm_id);
	}

	fdt_for_each_subnode(vm_node, fdt, vm_root) {
		if (dts_has_compatible(fdt, vm_node, "beau,vm")) {
			dts_parse_vm_node(fdt, generic, vm_node, ops);
		}
	}

	dts_parse_boot_options(fdt, vm_root, ops);
}
