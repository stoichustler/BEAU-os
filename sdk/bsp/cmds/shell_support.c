/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <schedule.h>
#include <vm.h>

#include "shell_cmds.h"

/* These conversions have no mutable state and need no shell or VM lock. */
const char *shell_yes_no(bool value)
{
	return value ? "Y" : "N";
}

const char *shell_thread_state_to_str(uint32_t state)
{
	switch (state) {
	case THREAD_STS_RUNNING:
		return "running";
	case THREAD_STS_RUNNABLE:
		return "runnable";
	case THREAD_STS_BLOCKED:
		return "blocked";
	default:
		return "unknown";
	}
}

const char *shell_vm_state_to_str(uint32_t state)
{
	switch (state) {
	case VM_POWERED_OFF:
		return "poweroff";
	case VM_CREATED:
		return "created";
	case VM_RUNNING:
		return "running";
	case VM_READY_TO_POWEROFF:
		return "ready-off";
	case VM_PAUSED:
		return "paused";
	default:
		return "N/A";
	}
}
