/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_VM_CONFIG_H
#define ARM64_VM_CONFIG_H

#include <types.h>
#ifndef CONFIG_STATIC_ARM64_PLATFORM
#include <board_info.h>
#endif

#define MAX_VCPUS_PER_VM	MAX_PCPU_NUM
#define CONFIG_MAX_VM_NUM	16U

#define DM_OWNED_GUEST_FLAG_MASK	0UL

/*
 * ARM64 VM layout data is kept in the scenario configuration instead of being
 * hard-coded in generic virtualization code. guest_ram_* defines the stage-2
 * RAM IPA window. guest_ram_hpa is kept for platforms that need a non-identity
 * backing window, but the QEMU static RTOS layout intentionally keeps
 * IPA == PA. guest_gic*, guest_its*, guest_uart*, and guest_virtio* define IPA
 * ranges that trap to vGIC/vITS/virtio MMIO handlers. virtio_proxy is a
 * transport-owned window; DTS selects the advertised virtio device id and
 * leaves protocol semantics to a backend module.
 */
#define ARM64_VIRTIO_PROXY_TAG_MAX	36U

struct arch_vm_config {
	uint64_t guest_ram_start;
	uint64_t guest_ram_size;
	uint64_t guest_ram_hpa;

	uint64_t guest_gicd_base;
	uint64_t guest_gicd_size;
	uint64_t guest_gicr_base;
	uint64_t guest_gicr_size;
	uint64_t guest_gicr_stride;
	uint64_t guest_its_base;
	uint64_t guest_its_size;

	uint64_t guest_uart_base;
	uint64_t guest_uart_size;
	uint32_t guest_uart_irq;

	uint64_t guest_virtio_console_base;
	uint64_t guest_virtio_console_size;
	uint32_t guest_virtio_console_irq;

	uint64_t guest_virtio_proxy_base;
	uint64_t guest_virtio_proxy_size;
	uint32_t guest_virtio_proxy_irq;
	uint32_t guest_virtio_proxy_device_id;
	uint16_t guest_virtio_proxy_queue_num;
	uint16_t guest_virtio_proxy_queue_size;
	uint32_t guest_virtio_proxy_access;
	char guest_virtio_proxy_tag[ARM64_VIRTIO_PROXY_TAG_MAX];
};

#endif /* ARM64_VM_CONFIG_H */
