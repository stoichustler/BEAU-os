/*
 * Copyright (C) 2020-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <util.h>
#include <acrn_hv_defs.h>
#include <vm.h>
#include <vm_event.h>
#include <sbuf.h>

/*
 * 2026-07-10, VM event service principle:
 *
 * VM events are one-way notifications from BEAU core to the service-side event
 * consumer through a shared sbuf. This file owns only the transport binding and
 * send-side serialization; event meaning remains owned by the producer and the
 * consumer.
 *
 *   service VM registers sbuf
 *          |
 *          v
 *   init_vm_event()
 *     - verify SBUF_MAGIC
 *     - attach vm->sw.vm_event_sbuf
 *          |
 *          v
 *   core producer builds vm_event
 *          |
 *          v
 *   send_vm_event()
 *     - append one fixed-size record
 *     - fire HSM notification
 *
 * The shared page is not trusted just because it is mapped by a service VM.
 * sbuf_put() validates the element size against the caller-provided record
 * size before updating producer indexes.
 */

int32_t init_vm_event(struct acrn_vm *vm, uint64_t *hva)
{
	struct shared_buf *sbuf = (struct shared_buf *)hva;
	int ret = -1;

	pre_user_access();
	if (sbuf != NULL) {
		if (sbuf->magic == SBUF_MAGIC) {
			vm->sw.vm_event_sbuf = sbuf;
			spinlock_init(&vm->vm_event_lock);
			ret = 0;
		}
	}
	post_user_access();

	return ret;
}

int32_t send_vm_event(struct acrn_vm *vm, struct vm_event *event)
{
	struct shared_buf *sbuf = (struct shared_buf *)vm->sw.vm_event_sbuf;
	int32_t ret = -ENODEV;
	uint32_t size_sent;

	if (sbuf != NULL) {
		spinlock_obtain(&vm->vm_event_lock);
		size_sent = sbuf_put(sbuf, (uint8_t *)event, sizeof(*event));
		spinlock_release(&vm->vm_event_lock);
		if (size_sent == sizeof(struct vm_event)) {
			arch_fire_hsm_interrupt();
			ret = 0;
		}
	}
	return ret;
}
