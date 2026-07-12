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
#define ARM64_VIRTIO_PROXY_MAX		32U

#define ARM64_VIRTIO_PROXY_THROUGHPUT_LOW	0U
#define ARM64_VIRTIO_PROXY_THROUGHPUT_HIGH	1U

#define ARM64_VM_FEATURE_SVE			(1UL << 0U)
#define ARM64_SVE_VL_BITS_MIN			128U
#define ARM64_SVE_VL_BITS_DEFAULT		128U
#define ARM64_SVE_VL_BITS_MAX			2048U

struct arm64_virtio_proxy_config {
	uint64_t base;
	uint64_t size;
	uint32_t irq;
	uint32_t device_id;
	uint16_t frontend_vmid;
	uint16_t queue_num;
	uint16_t queue_size;
	uint16_t pending_num;
	uint32_t access;
	uint32_t throughput;
	char tag[ARM64_VIRTIO_PROXY_TAG_MAX];
};

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

	uint64_t guest_feature_mask;
	uint32_t guest_sve_vl_bits;

	uint16_t guest_virtio_proxy_num;
	struct arm64_virtio_proxy_config guest_virtio_proxy[ARM64_VIRTIO_PROXY_MAX];
};

#endif /* ARM64_VM_CONFIG_H */
