/*
 * hypercall definition
 *
 * Copyright (C) 2017-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file acrn_hv_defs.h
 *
 * @brief acrn data structure for hypercall
 */

#ifndef ACRN_HV_DEFS_H
#define ACRN_HV_DEFS_H

/*
 * Common structures for HV/HSM
 */

#define BASE_HC_ID(x, y) (((x)<<24U)|(y))
#define HC_IDX(id) ((id)&(0xFFUL))

#define HC_ID 0x80UL

/* general */
#define HC_ID_GEN_BASE               0x0UL
#define HC_GET_API_VERSION          BASE_HC_ID(HC_ID, HC_ID_GEN_BASE + 0x00UL)
#define HC_SERVICE_VM_OFFLINE_CPU   BASE_HC_ID(HC_ID, HC_ID_GEN_BASE + 0x01UL)
#define HC_SET_CALLBACK_VECTOR      BASE_HC_ID(HC_ID, HC_ID_GEN_BASE + 0x02UL)

/* VM management */
#define HC_ID_VM_BASE               0x10UL
#define HC_CREATE_VM                BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x00UL)
#define HC_DESTROY_VM               BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x01UL)
#define HC_START_VM                 BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x02UL)
#define HC_PAUSE_VM                 BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x03UL)
#define HC_CREATE_VCPU              BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x04UL)
#define HC_RESET_VM                 BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x05UL)
#define HC_SET_VCPU_REGS            BASE_HC_ID(HC_ID, HC_ID_VM_BASE + 0x06UL)

/* IRQ and Interrupts */
#define HC_ID_IRQ_BASE              0x20UL
#define HC_INJECT_MSI               BASE_HC_ID(HC_ID, HC_ID_IRQ_BASE + 0x03UL)
#define HC_VM_INTR_MONITOR          BASE_HC_ID(HC_ID, HC_ID_IRQ_BASE + 0x04UL)
#define HC_SET_IRQLINE              BASE_HC_ID(HC_ID, HC_ID_IRQ_BASE + 0x05UL)

/* DM ioreq management */
#define HC_ID_IOREQ_BASE            0x30UL
#define HC_SET_IOREQ_BUFFER         BASE_HC_ID(HC_ID, HC_ID_IOREQ_BASE + 0x00UL)
#define HC_NOTIFY_REQUEST_FINISH    BASE_HC_ID(HC_ID, HC_ID_IOREQ_BASE + 0x01UL)
#define HC_ASYNCIO_ASSIGN           BASE_HC_ID(HC_ID, HC_ID_IOREQ_BASE + 0x02UL)
#define HC_ASYNCIO_DEASSIGN         BASE_HC_ID(HC_ID, HC_ID_IOREQ_BASE + 0x03UL)


/* Guest memory management */
#define HC_ID_MEM_BASE              0x40UL
#define HC_VM_GPA2HPA               BASE_HC_ID(HC_ID, HC_ID_MEM_BASE + 0x01UL)
#define HC_VM_SET_MEMORY_REGIONS    BASE_HC_ID(HC_ID, HC_ID_MEM_BASE + 0x02UL)
#define HC_VM_WRITE_PROTECT_PAGE    BASE_HC_ID(HC_ID, HC_ID_MEM_BASE + 0x03UL)
#define HC_SETUP_SBUF               BASE_HC_ID(HC_ID, HC_ID_MEM_BASE + 0x04UL)

/* PCI assignment*/
#define HC_ID_PCI_BASE              0x50UL
#define HC_ASSIGN_PTDEV             BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x00UL)
#define HC_DEASSIGN_PTDEV           BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x01UL)
#define HC_VM_PCI_MSIX_REMAP        BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x02UL)
#define HC_SET_PTDEV_INTR_INFO      BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x03UL)
#define HC_RESET_PTDEV_INTR_INFO    BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x04UL)
#define HC_ASSIGN_PCIDEV            BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x05UL)
#define HC_DEASSIGN_PCIDEV          BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x06UL)
#define HC_ASSIGN_MMIODEV           BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x07UL)
#define HC_DEASSIGN_MMIODEV         BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x08UL)
#define HC_ADD_VDEV                 BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x09UL)
#define HC_REMOVE_VDEV              BASE_HC_ID(HC_ID, HC_ID_PCI_BASE + 0x0AUL)

/* DEBUG */
#define HC_ID_DBG_BASE              0x60UL
#define HC_SETUP_HV_NPK_LOG         BASE_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x01UL)
#define HC_PROFILING_OPS            BASE_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x02UL)
#define HC_GET_HW_INFO              BASE_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x03UL)
#define HC_VM_WDT_KICK              BASE_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x04UL)
#define HC_VIRTIO_PROXY_BACKEND     BASE_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x05UL)
#define HC_IPC                      BASE_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x06UL)

/* BEAU static VM IPC */
#define ACRN_IPC_ABI_VERSION        1U
#define ACRN_IPC_OP_QUERY           0U
#define ACRN_IPC_OP_NOTIFY          1U
#define ACRN_IPC_OP_ACK             2U
#define ACRN_IPC_STATUS_OK          0U
#define ACRN_IPC_STATUS_BAD_PARAM   1U
#define ACRN_IPC_STATUS_NO_CHANNEL  2U
#define ACRN_IPC_RING_MAGIC         0x42495043U
#define ACRN_IPC_CHANNEL_ANY        0xffffffffU
#define ACRN_IPC_RING_COUNT         2U
#define ACRN_IPC_DIR_EP0_TO_EP1     0U
#define ACRN_IPC_DIR_EP1_TO_EP0     1U
#define ACRN_IPC_FLAG_NOTIFY_IRQ    (1U << 0U)

struct acrn_ipc_ioc {
	uint32_t op;
	uint32_t status;
	uint32_t abi_version;
	uint32_t ioc_size;
	uint32_t channel_id;
	uint16_t peer_vmid;
	uint16_t flags;
	uint64_t gpa_base;
	uint32_t ring_size;
	uint32_t ring_count;
	uint32_t notify_count;
	uint32_t ack_count;
	uint32_t reserved;
} __aligned(8);

struct acrn_ipc_ring_header {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t ring_size;
	uint16_t owner_vmid;
	uint16_t peer_vmid;
	uint16_t direction;
	uint16_t flags;
	uint32_t elem_size;
	uint32_t elem_count;
	uint64_t prod __aligned(64);
	uint64_t cons __aligned(64);
	uint64_t notify_count;
	uint64_t drop_count;
	uint64_t bytes;
} __aligned(64);

/* Trusty */
#define HC_ID_TRUSTY_BASE           0x70UL
#define HC_INITIALIZE_TRUSTY        BASE_HC_ID(HC_ID, HC_ID_TRUSTY_BASE + 0x00UL)
#define HC_WORLD_SWITCH             BASE_HC_ID(HC_ID, HC_ID_TRUSTY_BASE + 0x01UL)
#define HC_SAVE_RESTORE_SWORLD_CTX  BASE_HC_ID(HC_ID, HC_ID_TRUSTY_BASE + 0x02UL)

/* Power management */
#define HC_ID_PM_BASE               0x80UL
#define HC_PM_GET_CPU_STATE         BASE_HC_ID(HC_ID, HC_ID_PM_BASE + 0x00UL)
#define HC_PM_CONTROL               BASE_HC_ID(HC_ID, HC_ID_PM_BASE + 0x01UL)

#define ACRN_PM_ABI_VERSION         1U

#define ACRN_PM_QUERY_CAPS          0U
#define ACRN_PM_REQUEST_SUSPEND     1U
#define ACRN_PM_GET_EVENT           2U
#define ACRN_PM_ABORT               3U
#define ACRN_PM_GET_STATUS          4U
#define ACRN_PM_GET_WAKE_REASON     5U
#define ACRN_PM_RESUME_COMPLETE     6U

#define ACRN_PM_FLAG_REQUIRED       (1U << 0U)
#define ACRN_PM_EVENT_PREPARE       (1U << 1U)
#define ACRN_PM_EVENT_RESUME        (1U << 2U)
#define ACRN_PM_CAP_SYSTEM_SUSPEND  (1U << 8U)

struct acrn_pm_ioc {
	uint32_t abi_version;
	uint32_t ioc_size;
	uint32_t op;
	int32_t status;
	uint64_t epoch;
	uint64_t wake_reason;
	uint64_t required_vm_mask;
	uint32_t pm_state;
	uint32_t vm_state;
	uint16_t vmid;
	uint16_t flags;
	uint32_t event_virq;
	uint64_t reserved;
} __aligned(64);

_Static_assert(sizeof(struct acrn_pm_ioc) == 64U,
	"acrn_pm_ioc ABI must remain 64 bytes");

/* X86 TEE */
#define HC_ID_TEE_BASE              0x90UL
#define HC_TEE_VCPU_BOOT_DONE	    BASE_HC_ID(HC_ID, HC_ID_TEE_BASE + 0x00UL)
#define HC_SWITCH_EE		    BASE_HC_ID(HC_ID, HC_ID_TEE_BASE + 0x01UL)

#define ACRN_INVALID_VMID (0xffffU)
#define ACRN_INVALID_HPA (~0UL)

/* Generic memory attributes */
#define	MEM_ACCESS_READ                 0x00000001U
#define	MEM_ACCESS_WRITE                0x00000002U
#define	MEM_ACCESS_EXEC	                0x00000004U
#define	MEM_ACCESS_RWX			(MEM_ACCESS_READ | MEM_ACCESS_WRITE | \
						MEM_ACCESS_EXEC)
#define MEM_ACCESS_RIGHT_MASK           0x00000007U
#define	MEM_TYPE_WB                     0x00000040U
#define	MEM_TYPE_WT                     0x00000080U
#define	MEM_TYPE_UC                     0x00000100U
#define	MEM_TYPE_WC                     0x00000200U
#define	MEM_TYPE_WP                     0x00000400U
#define MEM_TYPE_MASK                   0x000007C0U

/**
 * @brief Hypercall
 *
 * @defgroup acrn_hypercall ACRN Hypercall
 * @{
 */

/**
 * @brief Info to set guest memory region mapping
 *
 * the parameter for HC_VM_SET_MEMORY_REGION hypercall
 */
struct vm_memory_region {
#define MR_ADD		0U
#define MR_DEL		2U
#define MR_MODIFY	3U
	/** set memory region type: MR_ADD or MAP_DEL */
	uint32_t type;

	/** memory attributes: memory type + RWX access right */
	uint32_t prot;

	/** the beginning guest physical address of the memory reion*/
	uint64_t gpa;

	/** Service VM's guest physcial address which gpa will be mapped to */
	uint64_t service_vm_gpa;

	/** size of the memory region */
	uint64_t size;
} __aligned(8);

/**
 * set multi memory regions, used for HC_VM_SET_MEMORY_REGIONS
 */
struct set_regions {
	/** vmid for this hypercall */
	uint16_t vmid;

	/** Reserved */
	uint16_t reserved0;

	/** Reserved */
	uint32_t reserved1;

	/**  memory region numbers */
	uint32_t mr_num;

	/** the gpa of regions buffer, point to the regions array:
	 *  	struct vm_memory_region regions[mr_num]
	 * the max buffer size is one page.
	 */
	uint64_t regions_gpa;
} __aligned(8);

/**
 * @brief Info to change guest one page write protect permission
 *
 * the parameter for HC_VM_WRITE_PROTECT_PAGE hypercall
 */
struct wp_data {
	/** set page write protect permission.
	 *  ture: set the wp; flase: clear the wp
	 */
	uint8_t set;

	/** Reserved */
	uint64_t pad:56;

	/** the guest physical address of the page to change */
	uint64_t gpa;
} __aligned(8);

/**
 * Setup parameter for share buffer, used for HC_SETUP_SBUF hypercall
 */
struct acrn_sbuf_param {
	/** sbuf cpu id */
	uint16_t cpu_id;

	/** Reserved */
	uint16_t reserved;

	/** sbuf id */
	uint32_t sbuf_id;

	/** sbuf's guest physical address */
	uint64_t gpa;
} __aligned(8);

/**
 * @brief Info to setup the hypervisor NPK log
 *
 * the parameter for HC_SETUP_HV_NPK_LOG hypercall
 */
struct hv_npk_log_param {
	/** the setup command for the hypervisor NPK log */
	uint16_t cmd;

	/** the setup result for the hypervisor NPK log */
	uint16_t res;

	/** the loglevel for the hypervisor NPK log */
	uint16_t loglevel;

	/** Reserved */
	uint16_t reserved;

	/** the MMIO address for the hypervisor NPK log */
	uint64_t mmio_addr;
} __aligned(8);

/**
 * the parameter for HC_GET_HW_INFO hypercall
 */
struct acrn_hw_info {
	uint16_t cpu_num; /* Physical CPU number */
	uint16_t reserved[3];
} __aligned(8);

/*
 * BEAU virtio-proxy backend ABI.
 *
 * A backend VM uses this debug-range HVC during early bring-up to service a
 * frontend VM's virtqueue without giving BEAU protocol-specific filesystem,
 * block, network, I2C, or SPI logic.
 *
 *   backend VM HVC poll
 *        -> BEAU copies one frontend descriptor chain into backend buffers
 *        -> backend performs protocol work
 *        -> backend HVC reply copies response bytes back to frontend memory
 *        -> BEAU publishes used-ring completion and injects the frontend IRQ
 */
#define ACRN_VIRTIO_PROXY_ABI_VERSION	3U

#define ACRN_VIRTIO_PROXY_OP_REGISTER	0U
#define ACRN_VIRTIO_PROXY_OP_POLL	1U
#define ACRN_VIRTIO_PROXY_OP_REPLY	2U
#define ACRN_VIRTIO_PROXY_OP_HEARTBEAT	3U
#define ACRN_VIRTIO_PROXY_OP_BATCH_POLL	4U
#define ACRN_VIRTIO_PROXY_OP_BATCH_REPLY	5U

#define ACRN_VIRTIO_PROXY_DATA_MAX	8192U
#define ACRN_VIRTIO_PROXY_DESC_MAX	8U
#define ACRN_VIRTIO_PROXY_BATCH_MAX	4U
#define ACRN_VIRTIO_PROXY_BATCH_DATA_MAX	ACRN_VIRTIO_PROXY_DATA_MAX
#define ACRN_VIRTIO_PROXY_FLAG_RO	0x1U
#define ACRN_VIRTIO_PROXY_REG_F_FEATURES	0x1U
#define ACRN_VIRTIO_PROXY_REG_F_CONFIG	0x2U
#define ACRN_VIRTIO_PROXY_CAP_WAIT_HINT	0x1U
#define ACRN_VIRTIO_PROXY_CAP_HEARTBEAT	0x2U
#define ACRN_VIRTIO_PROXY_CAP_STATS	0x4U
#define ACRN_VIRTIO_PROXY_CAP_BATCH	0x8U
#define ACRN_VIRTIO_PROXY_CAP_SHARED_RING	0x10U

struct acrn_virtio_proxy_desc {
	uint32_t len;
	uint32_t flags;
} __aligned(8);

struct acrn_virtio_proxy_batch_entry {
	uint32_t status;
	uint16_t queue_id;
	uint16_t head;
	uint16_t desc_count;
	uint32_t in_len;
	uint32_t out_len;
	uint32_t reply_len;
	uint32_t flags;
	struct acrn_virtio_proxy_desc desc[ACRN_VIRTIO_PROXY_DESC_MAX];
	uint8_t in[ACRN_VIRTIO_PROXY_BATCH_DATA_MAX];
	uint8_t out[ACRN_VIRTIO_PROXY_BATCH_DATA_MAX];
} __aligned(8);

struct acrn_virtio_proxy_ioc {
	uint32_t op;
	/*
	 * Failure: negative errno cast to u32.
	 * POLL success: ACRN_VIRTIO_PROXY_FLAG_* bits for the frontend request.
	 * Other success: zero.
	 */
	uint32_t status;
	/* Virtio device id used to select one proxy when a frontend VM has many. */
	uint32_t device_id;
	uint16_t frontend_vmid;
	uint16_t queue_id;
	uint16_t head;
	uint16_t desc_count;
	uint32_t in_len;
	uint32_t out_len;
	uint64_t in_gpa;
	uint64_t out_gpa;
	uint64_t device_features;
	uint64_t config_gpa;
	uint32_t config_len;
	uint32_t register_flags;
	uint32_t abi_version;
	uint32_t ioc_size;
	uint32_t backend_caps;
	uint32_t wait_us;
	uint64_t heartbeat_seq;
	uint64_t batch_gpa;
	uint32_t batch_len;
	uint32_t batch_count;
	uint32_t batch_entry_size;
	uint32_t batch_flags;
	struct acrn_virtio_proxy_desc desc[ACRN_VIRTIO_PROXY_DESC_MAX];
} __aligned(8);

/**
 * Gpa to hpa translation parameter, used for HC_VM_GPA2HPA hypercall
 */
struct vm_gpa2hpa {
	/** gpa to do translation */
	uint64_t gpa;

	/** hpa to return after translation */
	uint64_t hpa;
} __aligned(8);

/**
 * Intr mapping info per ptdev, the parameter for HC_SET_PTDEV_INTR_INFO
 * hypercall
 */
struct hc_ptdev_irq {
#define IRQ_INTX 0U
#define IRQ_MSI 1U
#define IRQ_MSIX 2U
	/** irq mapping type: INTX or MSI */
	uint32_t type;

	/** virtual BDF of the ptdev */
	uint16_t virt_bdf;

	/** physical BDF of the ptdev */
	uint16_t phys_bdf;

	/** INTX remapping info */
	struct intx_info {
		/** virtual IOAPIC/PIC pin */
		uint32_t virt_pin;

		/** physical IOAPIC pin */
		uint32_t phys_pin;

		/** is virtual pin from PIC */
		bool pic_pin;

		/** Reserved */
		uint8_t reserved[3];
	} intx;

} __aligned(8);

/**
 * Hypervisor api version info, return it for HC_GET_API_VERSION hypercall
 */
struct hc_api_version {
	/** hypervisor api major version */
	uint32_t major_version;

	/** hypervisor api minor version */
	uint32_t minor_version;
} __aligned(8);

#define ACRN_PLATFORM_LAPIC_IDS_MAX	64U

/**
 * Trusty boot params, used for HC_INITIALIZE_TRUSTY
 */
struct trusty_boot_param {
	/** sizeof this structure */
	uint32_t size_of_this_struct;

	/** version of this structure */
	uint32_t version;

	/** trusty runtime memory base address */
	uint32_t base_addr;

	/** trusty entry point */
	uint32_t entry_point;

	/** trusty runtime memory size */
	uint32_t mem_size;

	/** padding */
	uint32_t padding;

	/** trusty runtime memory base address (high 32bit) */
	uint32_t base_addr_high;

	/** trusty entry point (high 32bit) */
	uint32_t entry_point_high;

	/** rpmb key */
	uint8_t rpmb_key[64];
} __aligned(8);

/**
 * @}
 */

enum profiling_cmd_type {
	PROFILING_MSR_OPS = 0U,
	PROFILING_GET_VMINFO,
	PROFILING_GET_VERSION,
	PROFILING_GET_CONTROL_SWITCH,
	PROFILING_SET_CONTROL_SWITCH,
	PROFILING_CONFIG_PMI,
	PROFILING_CONFIG_VMSWITCH,
	PROFILING_GET_PCPUID,
	PROFILING_GET_STATUS
};

#endif /* ACRN_HV_DEFS_H */
