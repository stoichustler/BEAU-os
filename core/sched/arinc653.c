/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cpu.h>
#include <errno.h>
#include <per_cpu.h>
#include <schedule.h>
#include <ticks.h>
#include <vconfig.h>

struct sched_arinc653_thread_data {
	struct list_head list;
	struct thread_object *obj;
};

struct sched_arinc653_entry {
	uint16_t vm_id;
	uint16_t vcpu_id;
	uint32_t runtime_us;
	uint64_t runtime_ticks;
	struct thread_object *obj;
};

struct sched_arinc653_control {
	struct sched_arinc653_entry entries[SCHED_ARINC653_MAX_ENTRIES];
	struct list_head thread_list;
	struct hv_timer window_timer;
	uint64_t major_frame_ticks;
	uint64_t frame_start_ticks;
	uint64_t next_major_frame_ticks;
	uint64_t next_switch_ticks;
	uint64_t timer_deadline_ticks;
	uint32_t major_frame_us;
	uint16_t entry_count;
	uint16_t sched_index;
	bool suspended;
};

static struct sched_arinc653_control sched_arinc653_controls[MAX_PCPU_NUM];

/* [20260731] Fixed-window ownership
 *
 * major frame
 *   +-----------+-----------+-----------+----------------+
 *   | VM0/vCPU0 | VM1/vCPU0 | VM0/vCPU1 | idle tail      |
 *   +-----------+-----------+-----------+----------------+
 *                 ^ window timer requests rescheduling
 *
 * Key rules:
 *   - a window is never donated to another runnable vCPU;
 *   - missing or blocked owners make only their own window idle;
 *   - catch-up work is bounded by SCHED_ARINC653_MAX_ENTRIES.
 */
static struct thread_object *arinc653_find_thread(
	const struct sched_arinc653_control *arinc_ctl, uint16_t vm_id,
	uint16_t vcpu_id)
{
	struct sched_arinc653_thread_data *data;
	struct list_head *pos;
	struct thread_object *obj = NULL;

	list_for_each(pos, &arinc_ctl->thread_list) {
		data = container_of(pos, struct sched_arinc653_thread_data, list);
		if ((data->obj->vm_id == vm_id) && (data->obj->vcpu_id == vcpu_id)) {
			obj = data->obj;
			break;
		}
	}

	return obj;
}

static void arinc653_resolve_entries(struct sched_arinc653_control *arinc_ctl)
{
	uint16_t i;

	for (i = 0U; i < arinc_ctl->entry_count; i++) {
		arinc_ctl->entries[i].obj = arinc653_find_thread(arinc_ctl,
			arinc_ctl->entries[i].vm_id, arinc_ctl->entries[i].vcpu_id);
	}
}

static bool arinc653_start_frame(struct sched_arinc653_control *arinc_ctl,
	uint64_t now)
{
	bool started = false;

	if ((arinc_ctl->entry_count != 0U) &&
		(arinc_ctl->major_frame_ticks != 0UL) &&
		(arinc_ctl->major_frame_ticks <= (UINT64_MAX - now))) {
		arinc_ctl->frame_start_ticks = now;
		arinc_ctl->next_major_frame_ticks = now + arinc_ctl->major_frame_ticks;
		arinc_ctl->sched_index = 0U;
		arinc_ctl->next_switch_ticks = now + arinc_ctl->entries[0].runtime_ticks;
		started = true;
	}

	return started;
}

static bool arinc653_advance(struct sched_arinc653_control *arinc_ctl,
	uint64_t now, uint64_t *deadline)
{
	uint64_t remainder;
	uint16_t steps = 0U;
	bool valid = false;

	if ((arinc_ctl->entry_count == 0U) ||
		(arinc_ctl->major_frame_ticks == 0UL)) {
		return false;
	}

	if ((arinc_ctl->next_major_frame_ticks == 0UL) ||
		(now < arinc_ctl->frame_start_ticks)) {
		valid = arinc653_start_frame(arinc_ctl, now);
	} else if (now >= arinc_ctl->next_major_frame_ticks) {
		remainder = (now - arinc_ctl->next_major_frame_ticks) %
			arinc_ctl->major_frame_ticks;
		arinc_ctl->frame_start_ticks = now - remainder;
		if (arinc_ctl->major_frame_ticks <=
			(UINT64_MAX - arinc_ctl->frame_start_ticks)) {
			arinc_ctl->next_major_frame_ticks = arinc_ctl->frame_start_ticks +
				arinc_ctl->major_frame_ticks;
			arinc_ctl->sched_index = 0U;
			arinc_ctl->next_switch_ticks = arinc_ctl->frame_start_ticks +
				arinc_ctl->entries[0].runtime_ticks;
			valid = true;
		}
	} else {
		valid = true;
	}

	while (valid && (arinc_ctl->sched_index < arinc_ctl->entry_count) &&
		(now >= arinc_ctl->next_switch_ticks) &&
		(steps < SCHED_ARINC653_MAX_ENTRIES)) {
		arinc_ctl->sched_index++;
		steps++;
		if (arinc_ctl->sched_index < arinc_ctl->entry_count) {
			arinc_ctl->next_switch_ticks +=
				arinc_ctl->entries[arinc_ctl->sched_index].runtime_ticks;
		}
	}

	if (valid) {
		if (arinc_ctl->sched_index < arinc_ctl->entry_count) {
			*deadline = arinc_ctl->next_switch_ticks;
		} else {
			*deadline = arinc_ctl->next_major_frame_ticks;
		}
		valid = *deadline > now;
	}

	return valid;
}

static void arinc653_timer_handler(void *param)
{
	struct sched_control *ctl = (struct sched_control *)param;
	struct sched_arinc653_control *arinc_ctl;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t rflags;

	ASSERT(ctl->pcpu_id == pcpu_id, "ARINC653 timer on wrong cpu!");

	obtain_schedule_lock(pcpu_id, &rflags);
	arinc_ctl = (struct sched_arinc653_control *)ctl->priv;
	arinc_ctl->timer_deadline_ticks = 0UL;
	sched_account_tick(ctl);
	make_reschedule_request(pcpu_id);
	release_schedule_lock(pcpu_id, rflags);
}

static bool arinc653_program_local_timer(struct sched_control *ctl,
	uint64_t deadline)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)ctl->priv;
	int32_t ret = 0;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "program ARINC653 timer on wrong cpu!");

	if ((deadline == arinc_ctl->timer_deadline_ticks) &&
		timer_is_started(&arinc_ctl->window_timer)) {
		return true;
	}

	if (timer_is_started(&arinc_ctl->window_timer)) {
		del_timer(&arinc_ctl->window_timer);
	}
	arinc_ctl->timer_deadline_ticks = 0UL;

	if (deadline != 0UL) {
		update_timer(&arinc_ctl->window_timer, deadline, 0UL);
		ret = add_timer(&arinc_ctl->window_timer);
		ASSERT(ret == 0, "failed to arm ARINC653 window timer!");
		if (ret == 0) {
			arinc_ctl->timer_deadline_ticks = deadline;
		}
	}

	return ret == 0;
}

static bool arinc653_validate_config(const struct sched_arinc653_config *config,
	uint64_t *major_frame_ticks)
{
	uint64_t total_runtime_us = 0UL;
	uint16_t i;
	bool valid = false;

	if ((config == NULL) || (major_frame_ticks == NULL) ||
		(config->major_frame_us == 0U) || (config->entry_count == 0U) ||
		(config->entry_count > SCHED_ARINC653_MAX_ENTRIES)) {
		return false;
	}

	*major_frame_ticks = us_to_ticks(config->major_frame_us);
	if (*major_frame_ticks == 0UL) {
		return false;
	}

	valid = true;
	for (i = 0U; (i < config->entry_count) && valid; i++) {
		const struct sched_arinc653_entry_config *entry = &config->entries[i];

		if ((entry->runtime_us == 0U) ||
			(us_to_ticks(entry->runtime_us) == 0UL) ||
			(entry->vm_id >= CONFIG_MAX_VM_NUM) ||
			(entry->vcpu_id >= MAX_VCPUS_PER_VM)) {
			valid = false;
		} else {
			total_runtime_us += entry->runtime_us;
			valid = total_runtime_us <= config->major_frame_us;
		}
	}

	return valid;
}

int32_t sched_arinc653_set_schedule(uint16_t pcpu_id,
	const struct sched_arinc653_config *config)
{
	struct sched_control *ctl;
	struct sched_arinc653_control *arinc_ctl;
	uint64_t major_frame_ticks;
	uint64_t now;
	uint64_t rflags;
	uint16_t i;
	int32_t ret = -EINVAL;

	if ((pcpu_id >= get_pcpu_nums()) ||
		!arinc653_validate_config(config, &major_frame_ticks)) {
		return ret;
	}

	now = cpu_ticks();
	if (major_frame_ticks > (UINT64_MAX - now)) {
		return -EIO;
	}

	ctl = &per_cpu(sched_ctl, pcpu_id);
	if ((ctl->scheduler != &sched_arinc653) || (ctl->priv == NULL)) {
		return -ENODEV;
	}
	obtain_schedule_lock(pcpu_id, &rflags);
	if ((ctl->scheduler == &sched_arinc653) && (ctl->priv != NULL)) {
		arinc_ctl = (struct sched_arinc653_control *)ctl->priv;
		arinc_ctl->major_frame_us = config->major_frame_us;
		arinc_ctl->major_frame_ticks = major_frame_ticks;
		arinc_ctl->entry_count = config->entry_count;
		for (i = 0U; i < SCHED_ARINC653_MAX_ENTRIES; i++) {
			if (i < config->entry_count) {
				arinc_ctl->entries[i].vm_id = config->entries[i].vm_id;
				arinc_ctl->entries[i].vcpu_id = config->entries[i].vcpu_id;
				arinc_ctl->entries[i].runtime_us = config->entries[i].runtime_us;
				arinc_ctl->entries[i].runtime_ticks =
					us_to_ticks(config->entries[i].runtime_us);
			} else {
				arinc_ctl->entries[i].vm_id = 0U;
				arinc_ctl->entries[i].vcpu_id = 0U;
				arinc_ctl->entries[i].runtime_us = 0U;
				arinc_ctl->entries[i].runtime_ticks = 0UL;
			}
			arinc_ctl->entries[i].obj = NULL;
		}
		arinc653_resolve_entries(arinc_ctl);
		if (arinc653_start_frame(arinc_ctl, now)) {
			make_reschedule_request(pcpu_id);
			ret = 0;
		} else {
			ret = -EIO;
		}
	} else {
		ret = -ENODEV;
	}
	release_schedule_lock(pcpu_id, rflags);

	return ret;
}

int32_t sched_arinc653_get_schedule(uint16_t pcpu_id,
	struct sched_arinc653_config *config)
{
	struct sched_control *ctl;
	struct sched_arinc653_control *arinc_ctl;
	uint64_t rflags;
	uint16_t i;
	int32_t ret = -EINVAL;

	if ((pcpu_id >= get_pcpu_nums()) || (config == NULL)) {
		return ret;
	}

	ctl = &per_cpu(sched_ctl, pcpu_id);
	if ((ctl->scheduler != &sched_arinc653) || (ctl->priv == NULL)) {
		return -ENODEV;
	}
	obtain_schedule_lock(pcpu_id, &rflags);
	if ((ctl->scheduler == &sched_arinc653) && (ctl->priv != NULL)) {
		arinc_ctl = (struct sched_arinc653_control *)ctl->priv;
		config->major_frame_us = arinc_ctl->major_frame_us;
		config->entry_count = arinc_ctl->entry_count;
		config->reserved = 0U;
		for (i = 0U; i < SCHED_ARINC653_MAX_ENTRIES; i++) {
			if (i < arinc_ctl->entry_count) {
				config->entries[i].vm_id = arinc_ctl->entries[i].vm_id;
				config->entries[i].vcpu_id = arinc_ctl->entries[i].vcpu_id;
				config->entries[i].runtime_us = arinc_ctl->entries[i].runtime_us;
			} else {
				config->entries[i].vm_id = 0U;
				config->entries[i].vcpu_id = 0U;
				config->entries[i].runtime_us = 0U;
			}
		}
		ret = 0;
	} else {
		ret = -ENODEV;
	}
	release_schedule_lock(pcpu_id, rflags);

	return ret;
}

static int32_t sched_arinc653_init(struct sched_control *ctl)
{
	struct sched_arinc653_control *arinc_ctl =
		&sched_arinc653_controls[ctl->pcpu_id];
	uint16_t i;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "init ARINC653 on wrong cpu!");

	ctl->priv = arinc_ctl;
	INIT_LIST_HEAD(&arinc_ctl->thread_list);
	initialize_timer(&arinc_ctl->window_timer, arinc653_timer_handler, ctl,
		0UL, 0UL);
	for (i = 0U; i < SCHED_ARINC653_MAX_ENTRIES; i++) {
		arinc_ctl->entries[i].vm_id = 0U;
		arinc_ctl->entries[i].vcpu_id = 0U;
		arinc_ctl->entries[i].runtime_us = 0U;
		arinc_ctl->entries[i].runtime_ticks = 0UL;
		arinc_ctl->entries[i].obj = NULL;
	}
	arinc_ctl->major_frame_ticks = 0UL;
	arinc_ctl->frame_start_ticks = 0UL;
	arinc_ctl->next_major_frame_ticks = 0UL;
	arinc_ctl->next_switch_ticks = 0UL;
	arinc_ctl->timer_deadline_ticks = 0UL;
	arinc_ctl->major_frame_us = 0U;
	arinc_ctl->entry_count = 0U;
	arinc_ctl->sched_index = 0U;
	arinc_ctl->suspended = false;

	return 0;
}

static void sched_arinc653_deinit(struct sched_control *ctl)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)ctl->priv;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "deinit ARINC653 on wrong cpu!");
	del_timer(&arinc_ctl->window_timer);
	arinc_ctl->timer_deadline_ticks = 0UL;
	arinc_ctl->entry_count = 0U;
	ctl->priv = NULL;
}

static void sched_arinc653_init_data(struct thread_object *obj,
	__unused struct sched_params *params)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)per_cpu(sched_ctl, obj->pcpu_id).priv;
	struct sched_arinc653_thread_data *data =
		(struct sched_arinc653_thread_data *)obj->data;

	ASSERT(sizeof(*data) <= THREAD_DATA_SIZE, "ARINC653 thread data is too large!");
	INIT_LIST_HEAD(&data->list);
	data->obj = obj;
	if (obj->is_vcpu) {
		list_add_tail(&data->list, &arinc_ctl->thread_list);
		arinc653_resolve_entries(arinc_ctl);
	}
}

static void sched_arinc653_deinit_data(struct thread_object *obj)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)per_cpu(sched_ctl, obj->pcpu_id).priv;
	struct sched_arinc653_thread_data *data =
		(struct sched_arinc653_thread_data *)obj->data;
	uint16_t i;

	if (!list_empty(&data->list)) {
		list_del_init(&data->list);
	}
	for (i = 0U; i < arinc_ctl->entry_count; i++) {
		if (arinc_ctl->entries[i].obj == obj) {
			arinc_ctl->entries[i].obj = NULL;
		}
	}
}

static struct thread_object *sched_arinc653_pick_next(struct sched_control *ctl)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)ctl->priv;
	struct thread_object *next = &get_cpu_var(idle);
	struct thread_object *candidate;
	uint64_t deadline = 0UL;
	uint64_t now = cpu_ticks();

	if (!arinc_ctl->suspended && arinc653_advance(arinc_ctl, now, &deadline)) {
		if (arinc_ctl->sched_index < arinc_ctl->entry_count) {
			candidate = arinc_ctl->entries[arinc_ctl->sched_index].obj;
			if ((candidate != NULL) && candidate->is_vcpu &&
				(candidate->pcpu_id == ctl->pcpu_id) && !candidate->be_blocking &&
				((candidate->status == THREAD_STS_RUNNABLE) ||
				(candidate->status == THREAD_STS_RUNNING))) {
				next = candidate;
			}
		}
	}

	if (!arinc653_program_local_timer(ctl, deadline)) {
		next = &get_cpu_var(idle);
	}

	return next;
}

static void sched_arinc653_suspend(struct sched_control *ctl)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)ctl->priv;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "suspend ARINC653 on wrong cpu!");
	del_timer(&arinc_ctl->window_timer);
	arinc_ctl->timer_deadline_ticks = 0UL;
	arinc_ctl->suspended = true;
}

static void sched_arinc653_resume(struct sched_control *ctl)
{
	struct sched_arinc653_control *arinc_ctl =
		(struct sched_arinc653_control *)ctl->priv;

	ASSERT(ctl->pcpu_id == get_pcpu_id(), "resume ARINC653 on wrong cpu!");
	arinc_ctl->suspended = false;
	if (arinc_ctl->entry_count != 0U) {
		(void)arinc653_start_frame(arinc_ctl, cpu_ticks());
		make_reschedule_request(ctl->pcpu_id);
	}
}

struct acrn_scheduler sched_arinc653 = {
	.name = "arinc653",
	.stat_desc = "fixed major/minor frame",
	.init = sched_arinc653_init,
	.init_data = sched_arinc653_init_data,
	.pick_next = sched_arinc653_pick_next,
	.sleep = NULL,
	.wake = NULL,
	.yield = NULL,
	.prioritize = NULL,
	.deinit_data = sched_arinc653_deinit_data,
	.deinit = sched_arinc653_deinit,
	.suspend = sched_arinc653_suspend,
	.resume = sched_arinc653_resume,
};
