/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <hv_pm.h>
#include <sprintf.h>
#include <ticks.h>
#include <util.h>
#include <vm.h>
#include <vm_wdt.h>
#include <bsp/pm.h>

#include "shell_cmds.h"

static const char *shell_pm_mode_to_str(uint8_t mode)
{
	const char *name;

	switch (mode) {
	case HV_PM_PLATFORM_SIMULATED:
		name = "simulated";
		break;
	case HV_PM_PLATFORM_STRICT:
		name = "strict";
		break;
	default:
		name = "disabled";
		break;
	}

	return name;
}

static uint64_t shell_pm_phase_ticks(const struct beau_pm_snapshot *snapshot,
	uint32_t phase)
{
	uint64_t duration = 0UL;

	if (phase < HV_PM_PHASE_COUNT) {
		duration = snapshot->phase_duration_ticks[phase];
		if ((duration == 0UL) && (snapshot->state == phase) &&
			(snapshot->phase_start_ticks[phase] != 0UL)) {
			duration = cpu_ticks() - snapshot->phase_start_ticks[phase];
		}
	}

	return duration;
}

static bool shell_pm_vcpu_masks_present(const struct beau_vm_pm_record *record)
{
	return (record->gated_vcpu_mask | record->active_vcpu_mask |
		record->frozen_vcpu_mask | record->wake_owned_vcpu_mask) != 0UL;
}

static void shell_pm_print_snapshot(const struct beau_pm_snapshot *snapshot,
	bool verbose)
{
	uint64_t visible_vm_mask;
	uint16_t vmid;
	uint32_t phase;
	bool vcpu_masks_present = false;

	shell_item_begin("pm epoch:%lu", snapshot->epoch);
	/* epoch identifies one transaction; phase/scope/owner/target/controller show
	 * coordination roles. Masks are VM bitmaps; VM rows retain request/result;
	 * vCPU masks expose gate, active, freeze, and wake ownership.
	 */
	if (snapshot->target_vmid < CONFIG_MAX_VM_NUM) {
		shell_item_line("phase:%s scope:%s owner:vm%hu target:vm%hu controller:vm%hu enabled:%s mode:%s",
			hv_pm_state_to_str((enum beau_pm_system_state)snapshot->state),
			hv_pm_scope_to_str((enum beau_pm_scope)snapshot->scope),
			snapshot->initiator_vmid, snapshot->target_vmid,
			snapshot->controller_vmid,
			shell_yes_no(snapshot->enabled != 0U),
			shell_pm_mode_to_str(snapshot->platform_mode));
	} else {
		shell_item_line("phase:%s scope:%s owner:vm%hu target:N/A controller:vm%hu enabled:%s mode:%s",
			hv_pm_state_to_str((enum beau_pm_system_state)snapshot->state),
			hv_pm_scope_to_str((enum beau_pm_scope)snapshot->scope),
			snapshot->initiator_vmid, snapshot->controller_vmid,
			shell_yes_no(snapshot->enabled != 0U),
			shell_pm_mode_to_str(snapshot->platform_mode));
	}
	shell_item_line("masks:policy:0x%016lx required:0x%016lx io-gated:0x%016lx topology:0x%016lx hooks:0x%016lx",
		snapshot->policy_required_vm_mask, snapshot->required_vm_mask,
		snapshot->io_gated_vm_mask, snapshot->topology_change_vm_mask,
		snapshot->completed_hook_mask);
	shell_item_line("timeouts:prepare:%ums resume:%ums io-gated:%s",
		snapshot->prepare_timeout_ms, snapshot->resume_timeout_ms,
		shell_yes_no(snapshot->io_gated != 0U));
	shell_item_line("platform-caps:0x%08x", platform_pm_capabilities());
	shell_item_line("wake:reason:%lu bitmap:0x%016lx",
		snapshot->wake_reason, snapshot->wake_bitmap);
	shell_item_line("last:epoch:%lu phase:%s status:%d error:epoch:%lu phase:%s vm:%hu status:%d",
		snapshot->last_epoch,
		hv_pm_state_to_str((enum beau_pm_system_state)snapshot->last_state),
		snapshot->last_status, snapshot->last_error.epoch,
		hv_pm_state_to_str((enum beau_pm_system_state)snapshot->last_error.phase),
		snapshot->last_error.vmid, snapshot->last_error.status);
	shell_item_line("current-duration.us:%lu",
		ticks_to_us(shell_pm_phase_ticks(snapshot, snapshot->state)));

	if (verbose) {
		visible_vm_mask = snapshot->policy_required_vm_mask |
			snapshot->required_vm_mask | snapshot->io_gated_vm_mask;
		for (phase = 0U; phase < HV_PM_PHASE_COUNT; phase++) {
			uint64_t duration = shell_pm_phase_ticks(snapshot, phase);

			if ((duration != 0UL) || (snapshot->state == phase)) {
				shell_item_line("phase:%-16s duration.us:%lu",
					hv_pm_state_to_str((enum beau_pm_system_state)phase),
					ticks_to_us(duration));
				shell_output_checkpoint();
			}
		}
		shell_item_section("VM PM state");
		shell_item_line("vm   epoch                 state  prior  req  status");
		shell_item_line("──── ────────────────────  ─────  ─────  ───  ──────");
		for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
			const struct beau_vm_pm_record *record = &snapshot->vm[vmid];

			if ((visible_vm_mask & (1UL << vmid)) != 0UL) {
				shell_item_line("vm%-2hu %-20lu  %-5u  %-5u  %-3u  %d",
					vmid, record->epoch, record->state,
					record->prior_vm_state, record->required, record->status);
				vcpu_masks_present |= shell_pm_vcpu_masks_present(record);
				shell_output_checkpoint();
			}
		}
		if (!vcpu_masks_present) {
			shell_item_line("vCPU masks: none");
		} else {
			shell_item_section("vCPU gate/activity");
			shell_item_line("vm   gated               active");
			shell_item_line("──── ──────────────────  ──────────────────");
			for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
				const struct beau_vm_pm_record *record = &snapshot->vm[vmid];

				if (((visible_vm_mask & (1UL << vmid)) != 0UL) &&
					shell_pm_vcpu_masks_present(record)) {
					shell_item_line("vm%-2hu 0x%016lx  0x%016lx", vmid,
						record->gated_vcpu_mask, record->active_vcpu_mask);
					shell_output_checkpoint();
				}
			}
			shell_item_section("vCPU freeze/wake");
			shell_item_line("vm   frozen              wake-owned");
			shell_item_line("──── ──────────────────  ──────────────────");
			for (vmid = 0U; vmid < CONFIG_MAX_VM_NUM; vmid++) {
				const struct beau_vm_pm_record *record = &snapshot->vm[vmid];

				if (((visible_vm_mask & (1UL << vmid)) != 0UL) &&
					shell_pm_vcpu_masks_present(record)) {
					shell_item_line("vm%-2hu 0x%016lx  0x%016lx", vmid,
						record->frozen_vcpu_mask,
						record->wake_owned_vcpu_mask);
					shell_output_checkpoint();
				}
			}
		}
	}
	shell_item_end();
}

static bool shell_pm_parse_vmid(const char *arg, uint16_t *vmid)
{
	uint32_t value = 0U;
	uint32_t idx;

	if ((arg == NULL) || (arg[0] == '\0') || (vmid == NULL)) {
		return false;
	}
	for (idx = 0U; arg[idx] != '\0'; idx++) {
		if ((arg[idx] < '0') || (arg[idx] > '9')) {
			return false;
		}
		value = (value * 10U) + (uint32_t)(arg[idx] - '0');
		if (value >= CONFIG_MAX_VM_NUM) {
			return false;
		}
	}
	*vmid = (uint16_t)value;

	return true;
}

static int32_t shell_pm_reboot_vm(uint16_t vmid)
{
	struct acrn_vm *vm = get_vm_from_vmid(vmid);

	if (is_service_vm(vm)) {
		shell_puts("refuse to reboot service VM from shell\r\n");
		return -EPERM;
	}
	if (is_poweroff_vm(vm)) {
		shell_puts("vm is powered off\r\n");
		return -EINVAL;
	}

	return request_vm_cold_restart(vm);
}

int32_t shell_pm(int32_t argc, char **argv)
{
	struct beau_pm_snapshot snapshot;
	uint16_t vmid;

	if ((argc < 2) || (argc > 3)) {
		shell_puts("usage: pm <suspend|resume|reboot> <vmid> | status\r\n");
		return -EINVAL;
	}

	if (strcmp(argv[1], "status") == 0) {
		if (argc != 2) {
			return -EINVAL;
		}
		hv_pm_get_snapshot(&snapshot);
		shell_pm_print_snapshot(&snapshot, false);
		return 0;
	}
	if (strcmp(argv[1], "suspend") == 0) {
		if ((argc != 3) || !shell_pm_parse_vmid(argv[2], &vmid)) {
			return -EINVAL;
		}
		return bsp_pm_suspend_vm(vmid);
	}
	if (strcmp(argv[1], "resume") == 0) {
		if ((argc != 3) || !shell_pm_parse_vmid(argv[2], &vmid)) {
			return -EINVAL;
		}
		return bsp_pm_resume_vm(vmid);
	}
	if (strcmp(argv[1], "reboot") == 0) {
		if ((argc != 3) || !shell_pm_parse_vmid(argv[2], &vmid)) {
			return -EINVAL;
		}
		return shell_pm_reboot_vm(vmid);
	}

	return -EINVAL;
}

int32_t shell_pmstat(int32_t argc, __unused char **argv)
{
	struct beau_pm_snapshot snapshot;

	if (argc != 1) {
		return -EINVAL;
	}
	hv_pm_get_snapshot(&snapshot);
	shell_pm_print_snapshot(&snapshot, true);

	return 0;
}

int32_t shell_reboot(__unused int32_t argc, __unused char **argv)
{
	reset_host(false);
	return 0;
}
