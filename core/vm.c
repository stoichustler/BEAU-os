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
#include <barrier.h>
#include <vboot.h>
#include <logmsg.h>
#include <sbuf.h>
#include <sprintf.h>
#include <ticks.h>
#include <vm_wdt.h>
#include <asm/notify.h>
#include <hv_pm.h>
#include <ai_sched.h>

#ifndef CONFIG_AUTOSTART_VM
#define CONFIG_AUTOSTART_VM		1
#endif

#ifndef CONFIG_LAUNCH_VMS_FROM_BSP
#define CONFIG_LAUNCH_VMS_FROM_BSP	0
#endif

static struct acrn_vm vm_array[CONFIG_MAX_VM_NUM] __aligned(PAGE_SIZE);

static struct acrn_vm *service_vm_ptr = NULL;

/* [20260710] common VM lifecycle principle:
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

bool is_poweroff_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_POWERED_OFF);
}

bool is_created_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_CREATED);
}

bool is_service_vm(const struct acrn_vm *vm)
{
	return (vm != NULL)  && (get_vm_config(vm->vm_id)->load_order == SERVICE_VM);
}

bool is_postlaunched_vm(const struct acrn_vm *vm)
{
	return (get_vm_config(vm->vm_id)->load_order == POST_LAUNCHED_VM);
}

bool is_prelaunched_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config;

	vm_config = get_vm_config(vm->vm_id);
	return (vm_config->load_order == PRE_LAUNCHED_VM);
}

bool is_rt_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_RT) != 0U);
}

bool is_stateful_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	/* TEE VM has GUEST_FLAG_STATELESS set implicitly */
	return ((vm_config->guest_flags & GUEST_FLAG_STATELESS) == 0U);
}

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

void get_vm_lock(struct acrn_vm *vm)
{
	spinlock_obtain(&vm->vm_state_lock);
}
void put_vm_lock(struct acrn_vm *vm)
{
	spinlock_release(&vm->vm_state_lock);
}

const char *vm_lifecycle_phase_name(uint16_t phase)
{
	static const char *const names[] = {
		"off", "creating", "created", "preparing", "starting", "running",
		"quiescing", "resetting", "failed", "destroying",
	};

	return (phase < ARRAY_SIZE(names)) ? names[phase] : "invalid";
}

static uint64_t vm_lifecycle_begin_locked(struct acrn_vm *vm, uint16_t phase)
{
	vm->lifecycle.generation++;
	if (vm->lifecycle.generation == 0UL) {
		vm->lifecycle.generation = 1UL;
	}
	vm->lifecycle.phase = phase;
	vm->lifecycle.last_error = 0;
	return vm->lifecycle.generation;
}

static void vm_lifecycle_commit_locked(struct acrn_vm *vm, uint16_t phase,
	enum vm_state state)
{
	vm->lifecycle.phase = phase;
	vm->state = state;
}

static void vm_lifecycle_fail_locked(struct acrn_vm *vm, int32_t error)
{
	vm->lifecycle.failed_phase = vm->lifecycle.phase;
	vm->lifecycle.last_error = error;
	vm->lifecycle.phase = VM_LIFECYCLE_FAILED;
}
bool is_paused_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_PAUSED);
}

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
		return "Service";
	case PRE_LAUNCHED_VM:
		return "Prelaunch";
	case POST_LAUNCHED_VM:
		return "Postlaunch";
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

	/* [20260708] DTS-authored cpu-affinity is an ordered vCPU map:
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

struct acrn_vm *get_vm_from_vmid(uint16_t vm_id)
{
	return &vm_array[vm_id];
}

/* return a pointer to the virtual machine structure of Service VM */
struct acrn_vm *get_service_vm(void)
{
	struct acrn_vm *vm = __atomic_load_n(&service_vm_ptr, __ATOMIC_ACQUIRE);

	return ((vm != NULL) && (vm->lifecycle.phase >= VM_LIFECYCLE_CREATED) &&
		(vm->lifecycle.phase != VM_LIFECYCLE_DESTROYING) &&
		(vm->lifecycle.phase != VM_LIFECYCLE_FAILED)) ? vm : NULL;
}

bool is_ready_for_system_shutdown(void)
{
	bool ret = true;
	uint16_t vm_id;
	struct acrn_vm *vm;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm = get_vm_from_vmid(vm_id);
		if (!is_poweroff_vm(vm) && is_stateful_vm(vm)) {
			ret = false;
			break;
		}
	}

	return ret;
}

/* [20260726] Static Service VM reset gate
 *
 * BSP boot setup -> clear service_vm_ptr -> release VM launch barrier
 *                                             |
 *                                             v
 * service VM creator -> VM_CREATED -> release-publish service_vm_ptr
 *
 * Key rule:
 *   - core/vm.c owns the static VM object array and this global reference;
 *   - a pointer is published only after the Service VM locks and vPCI state exist;
 *   - a consumer seeing NULL must retry instead of using a BSS-backed VM object.
 */
void vm_publish_static_boot_state(void)
{
	__atomic_store_n(&service_vm_ptr, NULL, __ATOMIC_RELEASE);
}

/* [20260725] ACRN-style static VM ready-first launch
 *
 * The platform VM table is the policy source. Common VM code consumes only the
 * configured load order, CPU affinity, guest flags, and boot image metadata; it
 * does not invent board-specific placement or boot rules.
 *
 *   platform vm_config[]
 *          |
 *          v
 *   launch_vms(pcpu_id)
 *     - only the configured VM BSP pCPU creates this VM
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
 *   start_vm() immediately after this VM is ready
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
 * code has run yet. There is no cross-VM start barrier: each pCPU processes its
 * own static VM BSPs in table order, while independent pCPUs make progress in
 * parallel. VM_RUNNING is set only after the BSP vCPU is made runnable; AP
 * vCPUs are later brought up by the guest-visible CPU_ON path.
 */
void launch_vms(uint16_t pcpu_id)
{
#if CONFIG_AUTOSTART_VM
	uint16_t vm_id;
	struct acrn_vm *vm;
	struct acrn_vm_config *vm_config;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		int32_t create_status;
		int32_t boot_status;
		int32_t image_status;
		int32_t cleanup_status;
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
						(void)vm_lifecycle_begin_locked(vm, VM_LIFECYCLE_PREPARING);
						stage_tsc = cpu_ticks();
						boot_status = init_vm_boot_info(vm);
						image_status = (boot_status == 0) ?
							prepare_os_image(vm) : boot_status;
						stage_us = ticks_to_us(cpu_ticks() - stage_tsc);
						if ((boot_status == 0) && (image_status == 0)) {
							vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_CREATED, VM_CREATED);
							if (start_vm(vm) == 0) {
								if (vm_boot_log_enabled(vm_id)) {
									LOG_INF("VM%u:    %-10s VM: %9s started",
										vm_id, vm_boot_load_order_name(vm_config->load_order),
										vm_config->name);
								}
							} else {
								LOG_ERR("VM%u: start deferred state=%s", vm_id,
									vm_lifecycle_phase_name(vm->lifecycle.phase));
							}
						} else {
							LOG_ERR("VM%u: prepare failed +%6luus boot=%d image=%d k=%s",
								vm_id, stage_us, boot_status, image_status,
								vm_config->os_config.kernel_mod_tag);
							cleanup_status = destroy_vm(vm);
							if (cleanup_status != 0) {
								LOG_ERR("VM%u: prepare rollback failed ret=%d", vm_id,
									cleanup_status);
							}
						}
					}
				} else {
					LOG_ERR("VM%u: create failed +%6luus ret=%d name=%s",
						vm_id, stage_us, create_status, vm_config->name);
				}
			}
		}
	}
#else
	(void)pcpu_id;
#endif
}

int32_t start_vm(struct acrn_vm *vm)
{
	struct acrn_vcpu *vcpu;

	if (vm == NULL) {
		return -EINVAL;
	}
	vcpu = vcpu_from_vid(vm, BSP_CPU_ID);
	if ((vm->lifecycle.phase != VM_LIFECYCLE_CREATED) ||
		(vcpu_get_state(vcpu) != VCPU_INIT)) {
		return -EBUSY;
	}
	vm->lifecycle.phase = VM_LIFECYCLE_STARTING;
	vm_wdt_reset(vm);
	arch_vm_prepare_bsp(vcpu);
	if (launch_vcpu(vcpu)) {
		vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_RUNNING, VM_RUNNING);
		return 0;
	} else {
		vm->lifecycle.failed_phase = VM_LIFECYCLE_STARTING;
		vm->lifecycle.last_error = -EBUSY;
		vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_CREATED, VM_CREATED);
		LOG_ERR("VM%u: BSP launch from %s denied", vm->vm_id,
			vcpu_state_to_str(vcpu_get_state(vcpu)));
		return -EBUSY;
	}
}

void pause_vm(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu = NULL;

	if (((is_severity_pass(vm->vm_id)) && (vm->state == VM_RUNNING)) ||
			(vm->state == VM_READY_TO_POWEROFF) ||
			(vm->state == VM_CREATED)) {
		foreach_vcpu(i, vm, vcpu) {
			pause_vcpu_sync(vcpu);
		}
		vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_QUIESCING, VM_PAUSED);
	}
}

uint64_t pause_vm_async(struct acrn_vm *vm, uint64_t generation)
{
	uint16_t i;
	uint64_t pending_vcpus = 0UL;
	struct acrn_vcpu *vcpu = NULL;

	if ((vm == NULL) || (generation == 0UL)) {
		return ~0UL;
	}
	if (is_paused_vm(vm)) {
		return 0UL;
	}
	if (!(((is_severity_pass(vm->vm_id)) && (vm->state == VM_RUNNING)) ||
		(vm->state == VM_READY_TO_POWEROFF) || (vm->state == VM_CREATED))) {
		return ~0UL;
	}
	if (vm->lifecycle.phase == VM_LIFECYCLE_RUNNING) {
		(void)vm_lifecycle_begin_locked(vm, VM_LIFECYCLE_QUIESCING);
	}

	foreach_vcpu(i, vm, vcpu) {
		(void)request_vcpu_quiesce(vcpu, generation);
	}
	foreach_vcpu(i, vm, vcpu) {
		if (!is_vcpu_quiesced(vcpu, generation)) {
			bitmap_set_non_atomic(vcpu->vcpu_id, &pending_vcpus);
		}
	}
	if (pending_vcpus == 0UL) {
		vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_QUIESCING, VM_PAUSED);
	}

	return pending_vcpus;
}

int32_t destroy_vm(struct acrn_vm *vm)
{
	int32_t ret = 0;
	uint16_t i;
	struct acrn_vm_config *vm_config = NULL;
	struct acrn_vcpu *vcpu = NULL;

	vm->lifecycle.phase = VM_LIFECYCLE_DESTROYING;
	vm_wdt_reset(vm);
	if (is_service_vm(vm)) {
		__atomic_store_n(&service_vm_ptr, NULL, __ATOMIC_RELEASE);
	}

	if (is_service_vm(vm)) {
		sbuf_reset();
	}

	ret = arch_deinit_vm(vm);

	foreach_vcpu(i, vm, vcpu) {
		destroy_vcpu(vcpu);
	}
	vm->hw.created_vcpus = 0U;

	vm_config = get_vm_config(vm->vm_id);
	vm_config->guest_flags &= ~DM_OWNED_GUEST_FLAG_MASK;
	if (!is_static_configured_vm(vm)) {
		memset(vm_config->name, 0U, MAX_VM_NAME_LEN);
	}
	vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_OFF, VM_POWERED_OFF);

	return ret;
}

int32_t create_vm(uint16_t vm_id, uint64_t pcpu_bitmap, struct acrn_vm_config *vm_config, struct acrn_vm **rtn_vm)
{
	int32_t status = 0;
	struct acrn_vm *vm = NULL;

	vm = &vm_array[vm_id];
	vm->vm_id = vm_id;
	vm->hw.created_vcpus = 0U;

	if (vm_config->name[0] == '\0') {
		snprintf(vm_config->name, 16, "BEAU vm-%d", vm_id);
	}

	(void)memcpy_s(&vm->name[0], MAX_VM_NAME_LEN, &vm_config->name[0], MAX_VM_NAME_LEN);

	vm->sw.vm_event_sbuf = NULL;
	vm->sw.io_shared_page = NULL;
	vm->sw.asyncio_sbuf = NULL;

	if ((vm_config->load_order == POST_LAUNCHED_VM)
			&& ((vm_config->guest_flags & GUEST_FLAG_IO_COMPLETION_POLLING) != 0U)) {
		vm->sw.is_polling_ioreq = true;
	}

	spinlock_init(&vm->stg2pt_lock);
	spinlock_init(&vm->emul_mmio_lock);
	(void)memset(&vm->lifecycle, 0U, sizeof(vm->lifecycle));
	(void)vm_lifecycle_begin_locked(vm, VM_LIFECYCLE_CREATING);
	vm->nr_emul_mmio_regions = 0U;
	vm->hw.cpu_affinity = pcpu_bitmap;
	status = arch_init_vm(vm, vm_config);

	if (status == 0) {
		status = create_vm_vcpus(vm, pcpu_bitmap, vm_config);
	}

	if (status == 0) {
		vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_CREATED, VM_CREATED);

		/* Populate return VM handle */
		*rtn_vm = vm;
		if (is_service_vm(vm)) {
			__atomic_store_n(&service_vm_ptr, vm, __ATOMIC_RELEASE);
		}
	} else {
		uint16_t i;
		struct acrn_vcpu *vcpu = NULL;

		foreach_vcpu(i, vm, vcpu) {
			destroy_vcpu(vcpu);
		}
		vm->hw.created_vcpus = 0U;
		(void)arch_deinit_vm(vm);
		vm_lifecycle_fail_locked(vm, status);
	}

	return status;
}

static int32_t reset_vm_scheduler(struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	struct acrn_vcpu *vcpu = NULL;
	uint16_t i;
	int32_t ret = 0;

	foreach_vcpu(i, vm, vcpu) {
		ret = reset_thread_data(&vcpu->thread_obj, &vm_config->sched_params);
		if (ret != 0) {
			LOG_ERR("VM%u: vCPU%hu scheduler reset failed ret=%d",
				vm->vm_id, vcpu->vcpu_id, ret);
			break;
		}
	}

	return ret;
}

int32_t reset_vm(struct acrn_vm *vm)
{
	int32_t ret = -1;

	if (vm == NULL) {
		return ret;
	}
	ai_sched_invalidate_vm(vm->vm_id);
	(void)vm_lifecycle_begin_locked(vm, VM_LIFECYCLE_RESETTING);
	ret = reset_vm_scheduler(vm);
	if (ret == 0) {
		ret = arch_reset_vm(vm);
	}
	if (ret == 0) {
		vm_lifecycle_commit_locked(vm, VM_LIFECYCLE_CREATED, VM_CREATED);
	} else {
		vm_lifecycle_fail_locked(vm, ret);
	}

	return ret;
}

static int32_t restart_vm_locked(struct acrn_vm *vm, bool reload_image)
{
	int32_t ret = -EINVAL;

	if ((vm == NULL) || is_poweroff_vm(vm) || is_service_vm(vm)) {
		return ret;
	}

	pause_vm(vm);
	if (is_paused_vm(vm)) {
		ret = reset_vm(vm);
		if ((ret == 0) && reload_image) {
			/*
			 * A cold management or watchdog reset reloads the boot payload before
			 * waking the BSP. Warm restart deliberately skips this copy and reuses
			 * the guest RAM image after vCPU and device state has been reset.
			 */
			ret = prepare_os_image(vm);
		}
		if (ret == 0) {
			ret = start_vm(vm);
			if (ret == 0) {
				LOG_INF("VM%u:    %4s reset complete", vm->vm_id,
					reload_image ? "Cold" : "Warm");
			}
		}
	}

	if (ret != 0) {
		LOG_ERR("VM%u: reset failed: state=%d", vm->vm_id, vm->state);
	}

	return ret;
}

static int32_t restart_vm_with_mode(struct acrn_vm *vm, bool reload_image)
{
	int32_t ret;

	/*
	 * Synchronous restart is reserved for stable lifecycle owners. Interactive
	 * shell, watchdog, and target-vCPU exit paths use asynchronous requests so
	 * they do not block control work or overwrite a register frame that is still
	 * being consumed.
	 */
	if (vm == NULL) {
		return -EINVAL;
	}
	ret = hv_pm_begin_vm_topology_change(vm->vm_id);
	if (ret != 0) {
		return ret;
	}

	get_vm_lock(vm);
	ret = restart_vm_locked(vm, reload_image);
	put_vm_lock(vm);
	hv_pm_end_vm_topology_change(vm->vm_id);

	return ret;
}

int32_t restart_vm(struct acrn_vm *vm)
{
	return restart_vm_with_mode(vm, true);
}

int32_t restart_vm_warm(struct acrn_vm *vm)
{
	return restart_vm_with_mode(vm, false);
}

/* [20260718] Asynchronous cold-restart ownership
 *
 *   shell / guest PSCI / WDT producer
 *            |
 *            v
 *   optional WDT owner bit -> reset bitmap -> reschedule
 *            |
 *            v
 *   target pCPU idle -> pause -> reset -> reload -> start
 *            |
 *            +--> WDT-owned request -> completion callback
 *
 * Key rule:
 *   - PM topology state owns exclusion from suspend and duplicate restart;
 *   - the owner bit is published before the reset request becomes visible;
 *   - reset work runs outside control threads and vCPU exit frames;
 *   - only WDT-owned requests report completion into the recovery state machine.
 */
static int32_t make_reset_vm_request_internal(uint16_t pcpu_id,
	uint16_t vm_id, bool wdt_owned)
{
	struct acrn_vm *vm;
	int32_t ret = -EINVAL;

	if ((pcpu_id >= get_pcpu_nums()) || (vm_id >= CONFIG_MAX_VM_NUM)) {
		return ret;
	}
	ret = hv_pm_begin_vm_topology_change(vm_id);
	if (ret != 0) {
		return ret;
	}

	vm = get_vm_from_vmid(vm_id);
	if ((vm != NULL) && !is_poweroff_vm(vm) && !is_service_vm(vm)) {
		get_vm_lock(vm);
		if ((vm->lifecycle.phase == VM_LIFECYCLE_RUNNING) ||
			(vm->lifecycle.phase == VM_LIFECYCLE_CREATED)) {
			(void)vm_lifecycle_begin_locked(vm, VM_LIFECYCLE_QUIESCING);
		}
		if (vm->lifecycle.phase == VM_LIFECYCLE_QUIESCING) {
			vm->lifecycle.reset_generation = vm->lifecycle.generation;
		} else {
			put_vm_lock(vm);
			hv_pm_end_vm_topology_change(vm_id);
			return -EBUSY;
		}
		put_vm_lock(vm);
		if (wdt_owned) {
			bitmap_set(vm_id, &per_cpu(wdt_reset_vm_bitmap, pcpu_id));
			cpu_write_memory_barrier();
		}
		bitmap_set(vm_id, &per_cpu(reset_vm_bitmap, pcpu_id));
		bitmap_set(NEED_RESET_VM, &per_cpu(pcpu_flag, pcpu_id));
		make_reschedule_request(pcpu_id);
		ret = 0;
	}
	if (ret != 0) {
		hv_pm_end_vm_topology_change(vm_id);
	}

	return ret;
}

static int32_t request_vm_restart(struct acrn_vm *vm, bool wdt_owned)
{
	struct acrn_vcpu *bsp;

	if ((vm == NULL) || (vm->vm_id >= CONFIG_MAX_VM_NUM) ||
		(vm->hw.created_vcpus == 0U) || is_poweroff_vm(vm) ||
		is_service_vm(vm)) {
		return -EINVAL;
	}

	bsp = vcpu_from_vid(vm, BSP_CPU_ID);
	return make_reset_vm_request_internal(pcpuid_from_vcpu(bsp),
		vm->vm_id, wdt_owned);
}

int32_t request_vm_cold_restart(struct acrn_vm *vm)
{
	return request_vm_restart(vm, false);
}

int32_t request_vm_wdt_restart(struct acrn_vm *vm)
{
	return request_vm_restart(vm, true);
}

int32_t make_reset_vm_request(uint16_t pcpu_id, uint16_t vm_id)
{
	return make_reset_vm_request_internal(pcpu_id, vm_id, false);
}

bool has_reset_vm_request(uint16_t pcpu_id)
{
	return (pcpu_id < get_pcpu_nums()) &&
		bitmap_test(NEED_RESET_VM, &per_cpu(pcpu_flag, pcpu_id));
}

bool need_reset_vm(uint16_t pcpu_id)
{
	return (pcpu_id < get_pcpu_nums()) &&
		bitmap_test_and_clear(NEED_RESET_VM,
			&per_cpu(pcpu_flag, pcpu_id));
}

void reset_vm_from_idle(uint16_t pcpu_id)
{
	uint16_t vm_id;
	volatile uint64_t *vms = &per_cpu(reset_vm_bitmap, pcpu_id);
	volatile uint64_t *wdt_vms = &per_cpu(wdt_reset_vm_bitmap, pcpu_id);
	struct acrn_vm *vm;

	for (vm_id = fls64(*vms); vm_id < CONFIG_MAX_VM_NUM; vm_id = fls64(*vms)) {
		bool wdt_owned;
		int32_t reset_ret;

		cpu_read_memory_barrier();
		wdt_owned = bitmap_test_and_clear(vm_id, wdt_vms);
		vm = get_vm_from_vmid(vm_id);
		get_vm_lock(vm);
		if ((vm->lifecycle.phase != VM_LIFECYCLE_QUIESCING) ||
			(vm->lifecycle.reset_generation != vm->lifecycle.generation)) {
			reset_ret = -EBUSY;
		} else {
			reset_ret = restart_vm_locked(vm, true);
		}
		put_vm_lock(vm);
		bitmap_clear(vm_id, vms);
		hv_pm_end_vm_topology_change(vm_id);
		if (wdt_owned) {
			vm_wdt_restart_complete(vm_id, reset_ret);
		}
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
