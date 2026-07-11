/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <rtl.h>
#include <logmsg.h>
#include <vm.h>
#include <vm_config.h>
#include <bsp/vfdt.h>
#include <fdt_api.h>
#include <libfdt.h>
#include <pgtable.h>
#include <sprintf.h>
#include <asm/platform.h>
#include <asm/vtd.h>
#include <arm64_platform_dts.h>

#define ARM64_FDT_PHANDLE_GIC		1U
#define ARM64_FDT_PHANDLE_RAM		2U
#define ARM64_FDT_PHANDLE_UART		3U
#define ARM64_FDT_PHANDLE_UARTCLK	4U
#define ARM64_FDT_PHANDLE_ITS		5U
#define ARM64_FDT_PHANDLE_VIRTIO_CONSOLE 6U
#define ARM64_FDT_PHANDLE_VIRTIO_PROXY_BASE 7U

#define ARM64_FDT_GIC_SPI		0U
#define ARM64_FDT_GIC_PPI		1U
#define ARM64_FDT_IRQ_TYPE_LEVEL	4U

extern const uint8_t arm64_platform_dtb_start[];
extern void arm64_parse_vm_config_from_dts(const void *fdt);

struct beau_config beau_config;

static struct arm64_platform_dts_info platform_info;

void arm64_platform_init(uint64_t fdt_paddr)
{
	(void)fdt_paddr;

	/*
	 * Static ARM64 builds use the per-platform platform.dts as the source of
	 * truth for host geometry and static VM policy. The DTB is embedded by
	 * platform.S, then copied into the common host FDT buffer before consumers
	 * read it.
	 */
	init_devtree(hva2hpa_early((void *)arm64_platform_dtb_start));
	arm64_platform_dts_parse_info(get_host_fdt(), &platform_info);
	arm64_platform_dts_parse_board(get_host_fdt(), &platform_info);
}

void arm64_platform_init_post_console(void)
{
	arm64_parse_vm_config_from_dts(get_host_fdt());
}

void arm64_platform_init_smmu(void)
{
	if (beau_config.smmu_size != 0UL) {
		arm_smmu_probe(beau_config.smmu_base, beau_config.smmu_size);
	}
}

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count)
{
	return arm64_platform_dts_mmio_regions(count);
}

static void fdt_check_ret(int32_t ret, const char *op)
{
	if (ret < 0) {
		panic("failed to build arm64 service vm fdt: %s ret:%d", op, ret);
	}
}

static void fdt_property_reg64(void *fdt, const char *name, uint64_t addr,
	uint64_t size)
{
	fdt32_t reg[4];

	reg[0] = cpu_to_fdt32((uint32_t)(addr >> 32U));
	reg[1] = cpu_to_fdt32((uint32_t)addr);
	reg[2] = cpu_to_fdt32((uint32_t)(size >> 32U));
	reg[3] = cpu_to_fdt32((uint32_t)size);
	fdt_check_ret(fdt_property(fdt, name, reg, sizeof(reg)), name);
}

static void fdt_property_gic_reg(void *fdt, struct acrn_vm *vm)
{
	fdt32_t reg[8];
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t gicd_base = arch_config->guest_gicd_base;
	uint64_t gicd_size = arch_config->guest_gicd_size;
	uint64_t gicr_base = arch_config->guest_gicr_base;
	uint64_t gicr_size = arch_config->guest_gicr_size;

	reg[0] = cpu_to_fdt32((uint32_t)(gicd_base >> 32U));
	reg[1] = cpu_to_fdt32((uint32_t)gicd_base);
	reg[2] = cpu_to_fdt32((uint32_t)(gicd_size >> 32U));
	reg[3] = cpu_to_fdt32((uint32_t)gicd_size);
	reg[4] = cpu_to_fdt32((uint32_t)(gicr_base >> 32U));
	reg[5] = cpu_to_fdt32((uint32_t)gicr_base);
	reg[6] = cpu_to_fdt32((uint32_t)(gicr_size >> 32U));
	reg[7] = cpu_to_fdt32((uint32_t)gicr_size);
	fdt_check_ret(fdt_property(fdt, "reg", reg, sizeof(reg)), "gic reg");
}

static void fdt_property_irq(void *fdt, const char *name, const uint32_t *cells,
	uint32_t nr_cells)
{
	fdt32_t values[12];
	uint32_t i;

	if (nr_cells > ARRAY_SIZE(values)) {
		panic("too many arm64 vfdt irq cells");
	}

	for (i = 0U; i < nr_cells; i++) {
		values[i] = cpu_to_fdt32(cells[i]);
	}

	fdt_check_ret(fdt_property(fdt, name, values,
		(int32_t)(nr_cells * sizeof(fdt32_t))), name);
}

static bool fdt_vm_uses_virtio_console(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_console_size != 0UL);
}

static bool fdt_vm_uses_virtio_proxy(const struct acrn_vm_config *vm_config)
{
	return (vm_config->os_config.os_family == VM_OS_LINUX) &&
		(vm_config->arch.guest_virtio_proxy_num != 0U);
}

static void fdt_begin_cpu_node(void *fdt, uint32_t vcpu_id)
{
	char name[16];
	fdt32_t reg = cpu_to_fdt32(vcpu_id);

	snprintf(name, sizeof(name), "cpu@%u", vcpu_id);
	fdt_check_ret(fdt_begin_node(fdt, name), name);
	fdt_check_ret(fdt_property_string(fdt, "device_type", "cpu"), "cpu device_type");
	fdt_check_ret(fdt_property_string(fdt, "compatible",
		platform_info.guest_cpu_compatible), "cpu compatible");
	fdt_check_ret(fdt_property_string(fdt, "enable-method", "psci"),
		"cpu enable-method");
	fdt_check_ret(fdt_property(fdt, "reg", &reg, sizeof(reg)), "cpu reg");
	fdt_check_ret(fdt_end_node(fdt), "cpu end");
}

static void fdt_add_chosen(void *fdt, struct acrn_vm *vm)
{
	const struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	const struct arch_vm_config *arch_config = &vm_config->arch;
	uint64_t initrd_start = vm_config->os_config.kernel_ramdisk_addr;
	uint64_t initrd_size = vm_config->os_config.kernel_ramdisk_size;
	char stdout_path[40];

	if (fdt_vm_uses_virtio_console(vm_config)) {
		snprintf(stdout_path, sizeof(stdout_path), "/virtio_mmio@%lx",
			arch_config->guest_virtio_console_base);
	} else {
		snprintf(stdout_path, sizeof(stdout_path), "/serial@%lx",
			arch_config->guest_uart_base);
	}
	fdt_check_ret(fdt_begin_node(fdt, "chosen"), "chosen");
	fdt_check_ret(fdt_property_string(fdt, "stdout-path", stdout_path), "stdout-path");
	if (vm_config->os_config.bootargs[0] != '\0') {
		fdt_check_ret(fdt_property_string(fdt, "bootargs",
			vm_config->os_config.bootargs), "bootargs");
	}
	if (platform_info.service_vm_initrd && (initrd_start != 0UL) &&
		(initrd_size != 0UL)) {
		fdt_check_ret(fdt_property_u64(fdt, "linux,initrd-start", initrd_start),
			"initrd-start");
		fdt_check_ret(fdt_property_u64(fdt, "linux,initrd-end",
			initrd_start + initrd_size), "initrd-end");
	}
	fdt_check_ret(fdt_end_node(fdt), "chosen end");
}

static void fdt_add_cpus(void *fdt, const struct acrn_vm *vm)
{
	uint64_t cpu_bitmap = get_vm_config(vm->vm_id)->cpu_affinity;
	uint32_t cpu_count = 0U;
	uint32_t i;

	fdt_check_ret(fdt_begin_node(fdt, "cpus"), "cpus");
	fdt_check_ret(fdt_property_u32(fdt, "#address-cells", 1U), "cpus address-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#size-cells", 0U), "cpus size-cells");

	while (cpu_bitmap != 0UL) {
		cpu_bitmap &= cpu_bitmap - 1UL;
		cpu_count++;
	}

	for (i = 0U; i < cpu_count; i++) {
		fdt_begin_cpu_node(fdt, i);
	}

	fdt_check_ret(fdt_end_node(fdt), "cpus end");
}

static void fdt_add_psci(void *fdt)
{
	static const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2\0arm,psci";

	fdt_check_ret(fdt_begin_node(fdt, "psci"), "psci");
	fdt_check_ret(fdt_property(fdt, "compatible", psci_compat, sizeof(psci_compat)),
		"psci compatible");
	fdt_check_ret(fdt_property_string(fdt, "method", "hvc"), "psci method");
	fdt_check_ret(fdt_end_node(fdt), "psci end");
}

static void fdt_add_memory(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char name[32];
	uint64_t ram_start = arch_config->guest_ram_start;
	uint64_t ram_size = arch_config->guest_ram_size;

	snprintf(name, sizeof(name), "memory@%lx", ram_start);
	fdt_check_ret(fdt_begin_node(fdt, name), "memory");
	fdt_check_ret(fdt_property_string(fdt, "device_type", "memory"), "memory device_type");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", ARM64_FDT_PHANDLE_RAM),
		"memory phandle");
	fdt_property_reg64(fdt, "reg", ram_start, ram_size);
	fdt_check_ret(fdt_end_node(fdt), "memory end");
}

static void fdt_add_gic(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char name[48];
	uint64_t gicd_base = arch_config->guest_gicd_base;

	snprintf(name, sizeof(name), "interrupt-controller@%lx", gicd_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "gic");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,gic-v3"),
		"gic compatible");
	fdt_check_ret(fdt_property_u32(fdt, "#interrupt-cells", 3U),
		"gic interrupt-cells");
	fdt_check_ret(fdt_property(fdt, "interrupt-controller", NULL, 0),
		"gic controller");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", ARM64_FDT_PHANDLE_GIC),
		"gic phandle");
	fdt_property_gic_reg(fdt, vm);
	fdt_check_ret(fdt_end_node(fdt), "gic end");
}

static void fdt_add_its(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char name[48];
	uint64_t its_base = arch_config->guest_its_base;
	uint64_t its_size = arch_config->guest_its_size;

	if (its_size == 0UL) {
		return;
	}

	snprintf(name, sizeof(name), "msi-controller@%lx", its_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "its");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,gic-v3-its"),
		"its compatible");
	fdt_check_ret(fdt_property(fdt, "msi-controller", NULL, 0), "its msi-controller");
	fdt_check_ret(fdt_property_u32(fdt, "#msi-cells", 1U), "its msi-cells");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", ARM64_FDT_PHANDLE_ITS),
		"its phandle");
	fdt_property_reg64(fdt, "reg", its_base, its_size);
	fdt_check_ret(fdt_end_node(fdt), "its end");
}

static void fdt_add_timer(void *fdt)
{
	static const char timer_compat[] = "arm,armv8-timer\0arm,armv7-timer";
	static const uint32_t interrupts[] = {
		ARM64_FDT_GIC_PPI, 13U, ARM64_FDT_IRQ_TYPE_LEVEL,
		ARM64_FDT_GIC_PPI, 14U, ARM64_FDT_IRQ_TYPE_LEVEL,
		ARM64_FDT_GIC_PPI, 11U, ARM64_FDT_IRQ_TYPE_LEVEL,
		ARM64_FDT_GIC_PPI, 10U, ARM64_FDT_IRQ_TYPE_LEVEL,
	};

	fdt_check_ret(fdt_begin_node(fdt, "timer"), "timer");
	fdt_check_ret(fdt_property(fdt, "compatible", timer_compat, sizeof(timer_compat)),
		"timer compatible");
	fdt_check_ret(fdt_property_u32(fdt, "interrupt-parent", ARM64_FDT_PHANDLE_GIC),
		"timer interrupt-parent");
	fdt_property_irq(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_end_node(fdt), "timer end");
}

static void fdt_add_uart_clock(void *fdt)
{
	fdt_check_ret(fdt_begin_node(fdt, "apb-pclk"), "uartclk");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "fixed-clock"),
		"uartclk compatible");
	fdt_check_ret(fdt_property_u32(fdt, "clock-frequency",
		platform_info.uart_clock_hz), "uartclk frequency");
	fdt_check_ret(fdt_property_u32(fdt, "#clock-cells", 0U), "uartclk clock-cells");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", ARM64_FDT_PHANDLE_UARTCLK),
		"uartclk phandle");
	fdt_check_ret(fdt_end_node(fdt), "uartclk end");
}

static void fdt_add_uart(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t uart_base = arch_config->guest_uart_base;
	uint32_t irq = arch_config->guest_uart_irq;
	uint32_t interrupts[] = {
		ARM64_FDT_GIC_SPI, 0U, ARM64_FDT_IRQ_TYPE_LEVEL,
	};
	static const char uart_compat[] = "arm,pl011\0arm,primecell";
	char name[32];

	if (irq >= 32U) {
		interrupts[1] = irq - 32U;
	}

	snprintf(name, sizeof(name), "serial@%lx", uart_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "uart");
	fdt_check_ret(fdt_property(fdt, "compatible", uart_compat, sizeof(uart_compat)),
		"uart compatible");
	fdt_property_reg64(fdt, "reg", uart_base, arch_config->guest_uart_size);
	fdt_property_irq(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_property_u32(fdt, "current-speed", platform_info.uart_baud),
		"uart baud");
	fdt_check_ret(fdt_property_u32(fdt, "clocks", ARM64_FDT_PHANDLE_UARTCLK),
		"uart clocks");
	fdt_check_ret(fdt_property_string(fdt, "clock-names", "uartclk"), "uart clock name");
	fdt_check_ret(fdt_property_string(fdt, "status", "okay"), "uart status");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", ARM64_FDT_PHANDLE_UART),
		"uart phandle");
	fdt_check_ret(fdt_end_node(fdt), "uart end");
}

static void fdt_add_virtio_console(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t base = arch_config->guest_virtio_console_base;
	uint32_t irq = arch_config->guest_virtio_console_irq;
	uint32_t interrupts[] = {
		ARM64_FDT_GIC_SPI, 0U, ARM64_FDT_IRQ_TYPE_LEVEL,
	};
	char name[40];

	if (arch_config->guest_virtio_console_size == 0UL) {
		return;
	}
	if (irq >= 32U) {
		interrupts[1] = irq - 32U;
	}

	snprintf(name, sizeof(name), "virtio_mmio@%lx", base);
	fdt_check_ret(fdt_begin_node(fdt, name), "virtio console");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "virtio,mmio"),
		"virtio compatible");
	fdt_property_reg64(fdt, "reg", base, arch_config->guest_virtio_console_size);
	fdt_property_irq(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_property_u32(fdt, "phandle", ARM64_FDT_PHANDLE_VIRTIO_CONSOLE),
		"virtio phandle");
	fdt_check_ret(fdt_end_node(fdt), "virtio console end");
}

static void fdt_add_virtio_proxy(void *fdt, struct acrn_vm *vm, uint16_t index)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	const struct arm64_virtio_proxy_config *proxy_config;
	uint64_t base;
	uint32_t irq;
	uint32_t interrupts[] = {
		ARM64_FDT_GIC_SPI, 0U, ARM64_FDT_IRQ_TYPE_LEVEL,
	};
	char name[40];

	if (index >= arch_config->guest_virtio_proxy_num) {
		return;
	}
	proxy_config = &arch_config->guest_virtio_proxy[index];
	base = proxy_config->base;
	irq = proxy_config->irq;
	if (irq >= 32U) {
		interrupts[1] = irq - 32U;
	}

	snprintf(name, sizeof(name), "virtio_mmio@%lx", base);
	fdt_check_ret(fdt_begin_node(fdt, name), "virtio proxy");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "virtio,mmio"),
		"virtio compatible");
	fdt_property_reg64(fdt, "reg", base, proxy_config->size);
	fdt_property_irq(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_property_string(fdt, "status", "okay"), "virtio proxy status");
	fdt_check_ret(fdt_property_u32(fdt, "phandle",
		ARM64_FDT_PHANDLE_VIRTIO_PROXY_BASE + index),
		"virtio proxy phandle");
	fdt_check_ret(fdt_end_node(fdt), "virtio proxy end");
}

void arch_init_service_vm_vfdt(struct acrn_vm *vm)
{
	void *fdt = vm_get_vfdt(vm);

	fdt_check_ret(fdt_create(fdt, MAX_FDT_SIZE), "create");
	fdt_check_ret(fdt_finish_reservemap(fdt), "reservemap");
	fdt_check_ret(fdt_begin_node(fdt, ""), "root");
	fdt_check_ret(fdt_property_string(fdt, "model", platform_info.vfdt_model), "model");
	fdt_check_ret(fdt_property_string(fdt, "compatible", platform_info.vfdt_compatible),
		"compatible");
	fdt_check_ret(fdt_property_u32(fdt, "interrupt-parent", ARM64_FDT_PHANDLE_GIC),
		"interrupt-parent");
	fdt_check_ret(fdt_property_u32(fdt, "#address-cells", 2U), "address-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#size-cells", 2U), "size-cells");

	fdt_add_chosen(fdt, vm);
	fdt_add_cpus(fdt, vm);
	fdt_add_psci(fdt);
	fdt_add_memory(fdt, vm);
	fdt_add_gic(fdt, vm);
	fdt_add_its(fdt, vm);
	fdt_add_timer(fdt);
	if (fdt_vm_uses_virtio_console(get_vm_config(vm->vm_id))) {
		fdt_add_virtio_console(fdt, vm);
	} else {
		fdt_add_uart_clock(fdt);
		fdt_add_uart(fdt, vm);
	}
	if (fdt_vm_uses_virtio_proxy(get_vm_config(vm->vm_id))) {
		for (uint16_t i = 0U;
			i < get_vm_config(vm->vm_id)->arch.guest_virtio_proxy_num; i++) {
			fdt_add_virtio_proxy(fdt, vm, i);
		}
	}

	fdt_check_ret(fdt_end_node(fdt), "root end");
	fdt_check_ret(fdt_finish(fdt), "finish");
}
