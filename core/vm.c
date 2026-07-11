/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <per_cpu.h>
#include <cpu.h>
#include <vcpu.h>
#include <vm.h>
#include <errno.h>
#include <types.h>
#include <vboot.h>
#include <logmsg.h>
#include <sbuf.h>
#include <sprintf.h>
#include <ticks.h>
#include <vm_wdt.h>
#include <asm/notify.h>
#include <host_pm.h>

#ifndef CONFIG_AUTOSTART_VM
#define CONFIG_AUTOSTART_VM		1
#endif

#ifndef CONFIG_LAUNCH_VMS_FROM_BSP
#define CONFIG_LAUNCH_VMS_FROM_BSP	0
#endif

static struct acrn_vm vm_array[CONFIG_MAX_VM_NUM] __aligned(PAGE_SIZE);

static struct acrn_vm *service_vm_ptr = NULL;

/*
 * 2026-07-10, common VM lifecycle principle:
 *
 * core/vm.c owns the VM state machine and static launch policy. Architecture
 * code owns the EL2 virtualization payload attached to that VM: stage-2 tables,
 * vGIC state, timer offset, and guest entry context. Keeping the split explicit
 * prevents board/platform policy from leaking into common VM transitions.
 *
 *   platform vm_config
 *          |
 *          v
 *   common VM object + locks
 *          |
 *          v
 *   arch_init_vm()
 *     - stage-2 root
 *     - virtual devices
 *     - MMIO trap handlers
 *          |
 *          v
 *   create_vcpu() per configured pCPU
 *          |
 *          v
 *   VM_CREATED
 *          |
 *          v
 *   prepare_os_image() -> start_vm() -> VM_RUNNING
 *
 * The invariant is that VM_CREATED has all EL2 ownership structures allocated
 * but no guest instruction has executed. Guest state becomes live only after
 * start_vm() prepares the BSP and wakes its vCPU thread.
 */

uint16_t get_unused_vmid(void)
{
	uint16_t vm_id;
	struct acrn_vm_config *vm_config;

	for (vm_id = 0; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm_config = get_vm_config(vm_id);
		if ((vm_config->name[0] == '\0') && ((vm_config->guest_flags & GUEST_FLAG_STATIC_VM) == 0U)) {
			break;
		}
	}
	return (vm_id < CONFIG_MAX_VM_NUM) ? (vm_id) : (ACRN_INVALID_VMID);
}

uint16_t get_vmid_by_name(const char *name)
{
	uint16_t vm_id;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if ((*name != '\0') && vm_has_matched_name(vm_id, name)) {
			break;
		}
	}
	return (vm_id < CONFIG_MAX_VM_NUM) ? (vm_id) : (ACRN_INVALID_VMID);
}

/**
 * @pre vm != NULL
 */
bool is_poweroff_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_POWERED_OFF);
}

/**
 * @pre vm != NULL
 */
bool is_created_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_CREATED);
}

bool is_service_vm(const struct acrn_vm *vm)
{
	return (vm != NULL)  && (get_vm_config(vm->vm_id)->load_order == SERVICE_VM);
}

/**
 * @pre vm != NULL
 * @pre vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_postlaunched_vm(const struct acrn_vm *vm)
{
	return (get_vm_config(vm->vm_id)->load_order == POST_LAUNCHED_VM);
}

/**
 * @pre vm != NULL
 * @pre vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_prelaunched_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config;

	vm_config = get_vm_config(vm->vm_id);
	return (vm_config->load_order == PRE_LAUNCHED_VM);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_rt_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_RT) != 0U);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 *
 * Stateful VM refers to VM that has its own state (such as internal file cache),
 * and will experience state loss (file system corruption) if force powered down.
 */
bool is_stateful_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	/* TEE VM has GUEST_FLAG_STATELESS set implicitly */
	return ((vm_config->guest_flags & GUEST_FLAG_STATELESS) == 0U);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_static_configured_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_STATIC_VM) != 0U);
}

struct acrn_vm *get_highest_severity_vm(bool runtime)
{
	uint16_t vm_id, highest_vm_id = 0U;

	for (vm_id = 1U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (runtime && is_poweroff_vm(get_vm_from_vmid(vm_id))) {
			/* If vm is non-existed or shutdown, it's not highest severity VM */
			continue;
		}

		if (get_vm_severity(vm_id) > get_vm_severity(highest_vm_id)) {
			highest_vm_id = vm_id;
		}
	}

	return get_vm_from_vmid(highest_vm_id);
}

/**
 * @pre vm != NULL
 */
void poweroff_if_rt_vm(struct acrn_vm *vm)
{
	if (is_rt_vm(vm) && !is_paused_vm(vm) && !is_poweroff_vm(vm)) {
		vm->state = VM_READY_TO_POWEROFF;
	}
}

/**
 * if there is RT VM return true otherwise return false.
 */
bool has_rt_vm(void)
{
	uint16_t vm_id;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (is_rt_vm(get_vm_from_vmid(vm_id))) {
			break;
		}
	}

	return (vm_id != CONFIG_MAX_VM_NUM);
}

/*
 * @pre vm != NULL
 */
void get_vm_lock(struct acrn_vm *vm)
{
	spinlock_obtain(&vm->vm_state_lock);
}
/*
 * @pre vm != NULL
 */
void put_vm_lock(struct acrn_vm *vm)
{
	spinlock_release(&vm->vm_state_lock);
}
/**
 * @pre vm != NULL
 */
bool is_paused_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_PAUSED);
}

/**
 * @pre vm_config != NULL
 */
static inline uint16_t get_configured_bsp_pcpu_id(const struct acrn_vm_config *vm_config)
{
	return (vm_config->cpu_affinity_num != 0U) ?
		vm_config->cpu_affinity_order[0U] : ffs64(vm_config->cpu_affinity);
}

static inline uint16_t get_vm_launch_pcpu_id(const struct acrn_vm_config *vm_config)
{
#if CONFIG_LAUNCH_VMS_FROM_BSP
	(void)vm_config;
	return BSP_CPU_ID;
#else
	return get_configured_bsp_pcpu_id(vm_config);
#endif
}

static bool vm_boot_log_enabled(uint16_t vm_id)
{
	return vm_id < (SERVICE_VM_NUM + PRE_VM_NUM);
}

static const char *vm_boot_load_order_name(enum acrn_vm_load_order load_order)
{
	switch (load_order) {
	case SERVICE_VM:
		return "service";
	case PRE_LAUNCHED_VM:
		return "prelaunch";
	case POST_LAUNCHED_VM:
		return "postlaunch";
	default:
		return "unknown";
	}
}

static int32_t create_vm_vcpus(struct acrn_vm *vm, uint64_t pcpu_bitmap,
	const struct acrn_vm_config *vm_config)
{
	uint64_t tmp64 = pcpu_bitmap;
	uint16_t pcpu_id;
	uint16_t idx;
	int32_t status = 0;

	/*
	 * 2026-07-08, DTS-authored cpu-affinity is an ordered vCPU map:
	 *
	 *   cpu-affinity = <1 6>;
	 *          |
	 *          +--> vcpu0 -> pcpu1
	 *               vcpu1 -> pcpu6
	 *
	 * The bitmap remains for set membership, scheduler sharing tests, and
	 * hypercall validation, but VM creation must preserve the authored order.
	 */
	if (vm_config->cpu_affinity_num != 0U) {
		for (idx = 0U; (status == 0) && (idx < vm_config->cpu_affinity_num); idx++) {
			pcpu_id = vm_config->cpu_affinity_order[idx];
			if ((pcpu_bitmap & AFFINITY_CPU(pcpu_id)) != 0UL) {
				status = create_vcpu(vm, pcpu_id);
			}
		}
		return status;
	}

	while ((status == 0) && (tmp64 != 0UL)) {
		pcpu_id = ffs64(tmp64);
		bitmap_clear_non_atomic(pcpu_id, &tmp64);
		status = create_vcpu(vm, pcpu_id);
	}

	return status;
}

static void start_prepared_vm(struct acrn_vm *vm, __unused const struct acrn_vm_config *vm_config)
{
	start_vm(vm);
}

static void start_prepared_vms(struct acrn_vm *const start_vms[],
	const struct acrn_vm_config *const start_vm_configs[], uint16_t start_count,
	uint16_t pcpu_id, bool start_launcher_pcpu)
{
	uint16_t idx;

	for (idx = 0U; idx < start_count; idx++) {
		const struct acrn_vm_config *vm_config = start_vm_configs[idx];
		bool launcher_pcpu_vm = get_configured_bsp_pcpu_id(vm_config) == pcpu_id;

		if (launcher_pcpu_vm == start_launcher_pcpu) {
			start_prepared_vm(start_vms[idx], vm_config);
		}
	}
}

static void log_started_vms(struct acrn_vm *const start_vms[],
	const struct acrn_vm_config *const start_vm_configs[], uint16_t start_count)
{
	uint16_t idx;

	for (idx = 0U; idx < start_count; idx++) {
		const struct acrn_vm *vm = start_vms[idx];
		const struct acrn_vm_config *vm_config = start_vm_configs[idx];

		if (vm_boot_log_enabled(vm->vm_id)) {
			LOG_INF("VM%u: %-10s vm: %9s started",
				vm->vm_id, vm_boot_load_order_name(vm_config->load_order),
				vm_config->name);
		}
	}
}

/**
 * return a pointer to the virtual machine structure associated with
 * this VM ID
 *
 * @pre vm_id < CONFIG_MAX_VM_NUM
 */
struct acrn_vm *get_vm_from_vmid(uint16_t vm_id)
{
	return &vm_array[vm_id];
}

/* return a pointer to the virtual machine structure of Service VM */
struct acrn_vm *get_service_vm(void)
{
	ASSERT(service_vm_ptr != NULL, "service_vm_ptr is null");

	return service_vm_ptr;
}

bool is_ready_for_system_shutdown(void)
{
	bool ret = true;
	uint16_t vm_id;
	struct acrn_vm *vm;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm = get_vm_from_vmid(vm_id);
		/* TODO: Update code to cover hybrid mode */
		if (!is_poweroff_vm(vm) && is_stateful_vm(vm)) {
			ret = false;
			break;
		}
	}

	return ret;
}

/**
 * @pre vm_config != NULL
 * @Application constraint: The validity of vm_config->cpu_affinity should be guaranteed before run-time.
 */
/*
 * Static VM creation-to-run flow:
 *
 * The platform VM table is the policy source. Common VM code consumes only the
 * configured load order, CPU affinity, guest flags, and boot image metadata; it
 * does not invent board-specific placement or boot rules.
 *
 *   platform vm_config[]
 *          |
 *          v
 *   launch_vms(pcpu_id)
 *     - only the configured launch pCPU creates this VM
 *     - service/pre-launched VMs are autostart candidates
 *          |
 *          v
 *   create_vm()
 *     - initialize common VM object and locks
 *     - arch_init_vm() builds architecture state, including stage-2 root
 *     - create_vm_vcpus() creates one scheduler thread per affinity bit
 *          |
 *          v
 *   VM_CREATED
 *          |
 *          v
 *   init_vm_boot_info() / prepare_os_image()
 *     - discover configured boot modules
 *     - copy or place the guest image into the VM memory contract
 *          |
 *          v
 *   start_vm()
 *     - arch_vm_prepare_bsp() finalizes the BSP entry state
 *     - launch_vcpu(BSP) wakes the BSP vCPU thread
 *          |
 *          v
 *   VM_RUNNING
 *          |
 *          v
 *   scheduler picks vCPU thread -> arch_vcpu_thread() -> guest EL1 entry
 *
 * VM_CREATED means VM/vCPU objects and architecture state exist, but no guest
 * code has run yet. VM_RUNNING is set only after the BSP vCPU is made runnable;
 * AP vCPUs are later brought up by the guest-visible CPU_ON path.
 */
void launch_vms(uint16_t pcpu_id)
{
#if CONFIG_AUTOSTART_VM
	uint16_t vm_id;
	uint16_t start_count = 0U;
	struct acrn_vm *vm;
	struct acrn_vm_config *vm_config;
	struct acrn_vm *start_vms[CONFIG_MAX_VM_NUM];
	const struct acrn_vm_config *start_vm_configs[CONFIG_MAX_VM_NUM];

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		int32_t create_status;
		int32_t boot_status;
		int32_t image_status;
		uint64_t stage_tsc;
		uint64_t stage_us;

		vm_config = get_vm_config(vm_id);

		if (vm_config->cpu_affinity == 0UL) {
			continue;
		}

		if (((vm_config->guest_flags & GUEST_FLAG_REE) != 0U) &&
		    ((vm_config->guest_flags & GUEST_FLAG_TEE) != 0U)) {
			ASSERT(false, "%s: wrong vm (vm id: %u) configuration",
				__func__, vm_id);
		}

		if ((vm_config->load_order == SERVICE_VM) || (vm_config->load_order == PRE_LAUNCHED_VM)) {
			if (pcpu_id == get_vm_launch_pcpu_id(vm_config)) {
				vm = &vm_array[vm_id];
				if (!is_poweroff_vm(vm)) {
					continue;
				}

				if (vm_config->load_order == SERVICE_VM) {
					service_vm_ptr = vm;
				}

				/*
				 * We can only start a VM when there is no error in prepare_vm.
				 * Otherwise, print out the corresponding error.
				 *
				 * We can only start REE VM when get the notification from TEE VM.
				 * so skip "start_vm" here for REE, and start it in TEE hypercall
				 * HC_TEE_VCPU_BOOT_DONE.
				 */
				stage_tsc = cpu_ticks();
				create_status = create_vm(vm_id, vm_config->cpu_affinity,
					vm_config, &vm);
				stage_us = ticks_to_us(cpu_ticks() - stage_tsc);
				if (create_status == 0) {
					if ((vm_config->guest_flags & GUEST_FLAG_REE) != 0U) {
						/* Nothing need to do here, REE will start in TEE hypercall */
					} else {
						stage_tsc = cpu_ticks();
						boot_status = init_vm_boot_info(vm);
						image_status = (boot_status == 0) ?
							prepare_os_image(vm) : boot_status;
						stage_us = ticks_to_us(cpu_ticks() - stage_tsc);
						if ((boot_status == 0) && (image_status == 0)) {
							start_vms[start_count] = vm;
							start_vm_configs[start_count] = vm_config;
							start_count++;
						} else {
							LOG_ERR("VM%u: prepare failed +%6luus boot=%d image=%d k=%s",
								vm_id, stage_us, boot_status, image_status,
								vm_config->os_config.kernel_mod_tag);
							(void)destroy_vm(vm);
						}
					}
				} else {
					LOG_ERR("VM%u: create failed +%6luus ret=%d name=%s",
						vm_id, stage_us, create_status, vm_config->name);
				}
			}
		}
	}

	/*
	 * 2026-07-08, two-phase static VM start:
	 *
	 *   create/prepare all VM images
	 *        -> start VM BSPs on remote pCPUs
	 *        -> start VM BSPs pinned to this launcher pCPU
	 *
	 * Keep the launcher pCPU available until the static table is prepared.
	 * Otherwise a VM whose BSP vCPU is pinned here can run guest code before
	 * later VMs in the same table are even created.
	 */
	start_prepared_vms(start_vms, start_vm_configs, start_count, pcpu_id, false);
	start_prepared_vms(start_vms, start_vm_configs, start_count, pcpu_id, true);
	log_started_vms(start_vms, start_vm_configs, start_count);
#else
	(void)pcpu_id;
#endif
}

/**
 * @pre vm != NULL
 * @pre vm->state == VM_CREATED
 */
void start_vm(struct acrn_vm *vm)
{
	struct acrn_vcpu *vcpu = vcpu_from_vid(vm, BSP_CPU_ID);

	vm_wdt_reset(vm);
	arch_vm_prepare_bsp(vcpu);
	launch_vcpu(vcpu);
	vm->state = VM_RUNNING;
}

/**
 * @pre vm != NULL
 */
void pause_vm(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu = NULL;

	if (((is_severity_pass(vm->vm_id)) && (vm->state == VM_RUNNING)) ||
			(vm->state == VM_READY_TO_POWEROFF) ||
			(vm->state == VM_CREATED)) {
		foreach_vcpu(i, vm, vcpu) {
			zombie_vcpu(vcpu);
		}
		vm->state = VM_PAUSED;
	}
}

int32_t destroy_vm(struct acrn_vm *vm)
{
	int32_t ret = 0;
	uint16_t i;
	struct acrn_vm_config *vm_config = NULL;
	struct acrn_vcpu *vcpu = NULL;

	/* Only allow shutdown paused vm */
	vm->state = VM_POWERED_OFF;
	vm_wdt_reset(vm);

	if (is_service_vm(vm)) {
		sbuf_reset();
	}

	/* TODO: Same as create_vm, we have several common module initialization
	 * logic inside arch_deinit_vm. */
	ret = arch_deinit_vm(vm);

	foreach_vcpu(i, vm, vcpu) {
		destroy_vcpu(vcpu);
	}

	/* after guest_flags not used, then clear it */
	vm_config = get_vm_config(vm->vm_id);
	vm_config->guest_flags &= ~DM_OWNED_GUEST_FLAG_MASK;
	if (!is_static_configured_vm(vm)) {
		memset(vm_config->name, 0U, MAX_VM_NAME_LEN);
	}

	return ret;
}

/**
 * @pre vm_id < CONFIG_MAX_VM_NUM && vm_config != NULL && rtn_vm != NULL
 * @pre vm->state == VM_POWERED_OFF
 */
int32_t create_vm(uint16_t vm_id, uint64_t pcpu_bitmap, struct acrn_vm_config *vm_config, struct acrn_vm **rtn_vm)
{
	int32_t status = 0;
	struct acrn_vm *vm = NULL;

	/* Allocate memory for virtual machine */
	vm = &vm_array[vm_id];
	vm->vm_id = vm_id;
	vm->hw.created_vcpus = 0U;

	if (vm_config->name[0] == '\0') {
		/* if VM name is not configured, specify with VM ID */
		snprintf(vm_config->name, 16, "BEAU vm-%d", vm_id);
	}

	(void)memcpy_s(&vm->name[0], MAX_VM_NAME_LEN, &vm_config->name[0], MAX_VM_NAME_LEN);

	vm->sw.vm_event_sbuf = NULL;
	vm->sw.io_shared_page = NULL;
	vm->sw.asyncio_sbuf = NULL;

	if ((vm_config->load_order == POST_LAUNCHED_VM)
			&& ((vm_config->guest_flags & GUEST_FLAG_IO_COMPLETION_POLLING) != 0U)) {
		/* enable IO completion polling mode per its guest flags in vm_config. */
		vm->sw.is_polling_ioreq = true;
	}

	spinlock_init(&vm->stg2pt_lock);
	spinlock_init(&vm->emul_mmio_lock);
	vm->nr_emul_mmio_regions = 0U;

	/* TODO: Some logic inside arch_init_vm can also be moved to common but
	 * we didn't come up with abstraction good enough to capture dependencies. Leave those
	 * inside arch for now.
	 */
	status = arch_init_vm(vm, vm_config);

	if (status == 0) {
		/* We have assumptions:
		 *   1) vcpus used by Service VM has been offlined by DM before User VM re-use it.
		 *   2) pcpu_bitmap passed sanitization is OK for vcpu creating.
		 */
		vm->hw.cpu_affinity = pcpu_bitmap;
		status = create_vm_vcpus(vm, pcpu_bitmap, vm_config);
	}

	if (status == 0) {
		vm->state = VM_CREATED;

		/* Populate return VM handle */
		*rtn_vm = vm;
	}

	return status;
}

/**
 * "Warm" reset a VM.
 * To "Cold" reset a VM, simply destroy and re-create.
 *
 * @pre vm->state == VM_PAUSED
 */
int32_t reset_vm(struct acrn_vm *vm)
{
	int32_t ret = -1;

	ret = arch_reset_vm(vm);
	if (ret == 0) {
		vm->state = VM_CREATED;
	}

	return ret;
}

static int32_t restart_vm_locked(struct acrn_vm *vm)
{
	int32_t ret = -EINVAL;

	if ((vm == NULL) || is_poweroff_vm(vm) || is_service_vm(vm)) {
		return ret;
	}

	pause_vm(vm);
	if (is_paused_vm(vm)) {
		ret = reset_vm(vm);
		if (ret == 0) {
			/*
			 * Warm-resetting vCPU/device state is not enough for Linux. The
			 * previous kernel instance may have dirtied its loaded image, FDT,
			 * ramdisk, and data sections. Re-run the boot image copy before
			 * waking the BSP so reset behaves like a VM reboot, not just a
			 * register restart.
			 */
			ret = prepare_os_image(vm);
		}
		if (ret == 0) {
			start_vm(vm);
			LOG_INF("vm%u reset complete", vm->vm_id);
		}
	}

	if (ret != 0) {
		LOG_ERR("vm%u reset failed: state=%d", vm->vm_id, vm->state);
	}

	return ret;
}

int32_t restart_vm(struct acrn_vm *vm)
{
	int32_t ret;

	/*
	 * Synchronous restart is for EL2 management contexts such as the shell or
	 * idle thread. A target VM's own vCPU exit path must use
	 * make_reset_vm_request(), because resetting the currently executing vCPU
	 * would overwrite the state frame that the exit handler is still using.
	 */
	if (vm == NULL) {
		return -EINVAL;
	}

	get_vm_lock(vm);
	ret = restart_vm_locked(vm);
	put_vm_lock(vm);

	return ret;
}

int32_t make_reset_vm_request(uint16_t pcpu_id, uint16_t vm_id)
{
	struct acrn_vm *vm;
	int32_t ret = -EINVAL;

	if ((pcpu_id >= MAX_PCPU_NUM) || (vm_id >= CONFIG_MAX_VM_NUM)) {
		return ret;
	}

	vm = get_vm_from_vmid(vm_id);
	if ((vm != NULL) && !is_poweroff_vm(vm) && !is_service_vm(vm)) {
		/*
		 * VM reset is intentionally asynchronous:
		 *
		 *   PSCI/shell producer -> per-pCPU reset bitmap -> idle thread
		 *                         pause -> arch reset -> start BSP
		 *
		 * The producer might run in a vCPU exit path where sleeping or tearing
		 * down device state would be fragile. The idle thread is a stable EL2
		 * owner for the VM state machine.
		 */
		bitmap_set_non_atomic(vm_id, &per_cpu(reset_vm_bitmap, pcpu_id));
		bitmap_set(NEED_RESET_VM, &per_cpu(pcpu_flag, pcpu_id));
		if (get_pcpu_id() != pcpu_id) {
			arch_smp_call_kick_pcpu(pcpu_id);
		}
		ret = 0;
	}

	return ret;
}

bool need_reset_vm(uint16_t pcpu_id)
{
	return bitmap_test_and_clear(NEED_RESET_VM, &per_cpu(pcpu_flag, pcpu_id));
}

void reset_vm_from_idle(uint16_t pcpu_id)
{
	uint16_t vm_id;
	uint64_t *vms = &per_cpu(reset_vm_bitmap, pcpu_id);
	struct acrn_vm *vm;

	for (vm_id = fls64(*vms); vm_id < CONFIG_MAX_VM_NUM; vm_id = fls64(*vms)) {
		vm = get_vm_from_vmid(vm_id);
		get_vm_lock(vm);
		(void)restart_vm_locked(vm);
		put_vm_lock(vm);
		bitmap_clear_non_atomic(vm_id, vms);
	}
}

void make_shutdown_vm_request(uint16_t pcpu_id)
{
	bitmap_set(NEED_SHUTDOWN_VM, &per_cpu(pcpu_flag, pcpu_id));
	if (get_pcpu_id() != pcpu_id) {
		arch_smp_call_kick_pcpu(pcpu_id);
	}
}

bool need_shutdown_vm(uint16_t pcpu_id)
{
	return bitmap_test_and_clear(NEED_SHUTDOWN_VM, &per_cpu(pcpu_flag, pcpu_id));
}

void shutdown_vm_from_idle(uint16_t pcpu_id)
{
	uint16_t vm_id;
	uint64_t *vms = &per_cpu(shutdown_vm_bitmap, pcpu_id);
	struct acrn_vm *vm;

	for (vm_id = fls64(*vms); vm_id < CONFIG_MAX_VM_NUM; vm_id = fls64(*vms)) {
		vm = get_vm_from_vmid(vm_id);
		get_vm_lock(vm);
		if (is_paused_vm(vm)) {
			(void)destroy_vm(vm);
			if (is_ready_for_system_shutdown()) {
				shutdown_host();
			}
		}
		put_vm_lock(vm);
		bitmap_clear_non_atomic(vm_id, vms);
	}
}
