/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SCHEDULE_H
#define SCHEDULE_H
#include <spinlock.h>
#include <lib/list.h>
#include <timer.h>

#define	NEED_RESCHEDULE		(1U)

#define THREAD_DATA_SIZE	(256U)
#define SCHED_LATENCY_HIST_BUCKETS	8U

enum thread_object_state {
	THREAD_STS_RUNNING = 1,
	THREAD_STS_RUNNABLE,
	THREAD_STS_BLOCKED
};

/* Tools can configure a VM to use PRIO_LOW or PRIO_HIGH */
enum thread_priority {
	PRIO_IDLE = 0,
	PRIO_LOW,
	PRIO_HIGH,
	PRIO_MAX
};

struct sched_params {
	uint32_t prio;		/* The priority of a thread */

	/* per-thread parameters for the BVT scheduler */
	uint8_t bvt_weight;	/* CPU share weight, clamped by sched_bvt */
	int32_t bvt_warp_value;	/* EVT credit in MCU units while warp is active */
	uint32_t bvt_warp_limit;	/* max charged MCU units for one warp window */
	uint32_t bvt_unwarp_period;	/* cooldown in MCU units after a warp ends */

	/* per-thread parameters for the CBS scheduler */
	uint32_t cbs_period_us;	/* server period in microseconds */
	uint32_t cbs_budget_us;	/* server execution budget in microseconds */
};

enum sched_policy_id {
	SCHED_POLICY_NONE = 0,
	SCHED_POLICY_NOOP,
	SCHED_POLICY_IORR,
	SCHED_POLICY_BVT,
	SCHED_POLICY_RTDS,
	SCHED_POLICY_CBS,	/* partitioned CBS */
	SCHED_POLICY_PRIO,
};

struct sched_cpupool_config {
	bool configured;
	bool has_pcpu_mask;
	uint64_t pcpu_mask;
	enum sched_policy_id policy;
	uint32_t period_us;	/* pool default period for budget schedulers */
	uint32_t budget_us;	/* pool default execution budget */
};

struct sched_platform_config {
	bool configured;
	bool strict_placement;
	struct sched_cpupool_config exclusive;
	struct sched_cpupool_config shared;
};

struct sched_latency_stats {
	uint64_t switches;
	uint64_t last_wait_ticks;
	uint64_t max_wait_ticks;
	uint64_t state_since;
	uint64_t runnable_since;
	uint64_t runtime_ticks;
	uint64_t wait_hist[SCHED_LATENCY_HIST_BUCKETS];
};

struct sched_bvt_stats {
	uint8_t weight;
	uint64_t vt_ratio;
	int64_t avt;
	int64_t evt;
};

struct sched_rtds_stats {
	uint64_t period_ticks;
	uint64_t budget_ticks;
	uint64_t remaining_ticks;
	uint64_t deadline_ticks;
	uint64_t last_start_ticks;
};

struct sched_cbs_stats {
	uint64_t period_ticks;
	uint64_t budget_ticks;
	uint64_t remaining_ticks;
	uint64_t deadline_ticks;
	uint64_t last_start_ticks;
	uint64_t depleted_count;
	uint64_t replenish_count;
	uint64_t wake_replenish_count;
	uint64_t late_account_count;
};

struct sched_cbs_pcpu_stats {
	uint64_t admission_utilization;
	uint32_t runqueue_count;
};

struct thread_object;
typedef void (*thread_entry_t)(struct thread_object *obj);
typedef void (*switch_t)(struct thread_object *obj);
struct thread_object {
	struct list_head node;
	char name[16];
	uint16_t pcpu_id;
	struct sched_control *sched_ctl;
	thread_entry_t thread_entry;
	volatile enum thread_object_state status;
	bool be_blocking;
	bool is_vcpu;
	/*
	 * One-shot request consumed by schedule() under scheduler_lock. Event
	 * injection paths can ask for priority treatment without taking scheduler_lock
	 * inside IRQ/vCPU locks; schedulers that do not implement .prioritize ignore it.
	 */
	volatile bool priority_pending;
	uint64_t freeze_epoch;
	bool deferred_wake;
	/*
	 * VM identity is scheduler metadata, not ownership. It lets CBS make a
	 * bounded co-scheduling preference without depending on VM/vCPU internals.
	 */
	uint16_t vm_id;
	uint16_t vcpu_id;

	uint64_t host_sp;
	uint64_t host_stack_base;
	uint64_t host_stack_size;
	switch_t switch_out;
	switch_t switch_in;

	uint8_t data[THREAD_DATA_SIZE];
	struct sched_latency_stats latency;
};

struct sched_control {
	uint16_t pcpu_id;
	uint64_t flags;
	struct thread_object *curr_obj;
	spinlock_t scheduler_lock;	/* to protect sched_control and thread_object */
	struct acrn_scheduler *scheduler;
	void *priv;
	/*
	 * scheduler_ticks counts scheduler timer callbacks on this pCPU.
	 * context_switches counts real thread changes in schedule(). A tick may
	 * request a reschedule, but it does not imply that a different thread was
	 * selected.
	 * reschedule_requests counts requests raised through make_reschedule_request().
	 */
	uint64_t scheduler_ticks;
	uint64_t context_switches;
	uint64_t reschedule_requests;
	/*
	 * Fast path for deferred scheduler-specific priority requests. Producers set
	 * this per-pCPU flag without taking scheduler_lock; schedule() consumes it
	 * under the lock and only scans thread_list when the flag was observed.
	 */
	volatile bool priority_pending;
};

#define SCHEDULER_MAX_NUMBER 6U
struct acrn_scheduler {
	char name[16];
	char stat_desc[64];

	/* init scheduler */
	int32_t	(*init)(struct sched_control *ctl);
	/* init private data of scheduler */
	void	(*init_data)(struct thread_object *obj, struct sched_params *params);
	/* pick the next thread object */
	struct thread_object* (*pick_next)(struct sched_control *ctl);
	/* put thread object into sleep */
	void	(*sleep)(struct thread_object *obj);
	/* wake up thread object from sleep status */
	void	(*wake)(struct thread_object *obj);
	/* yield current thread object */
	void	(*yield)(struct sched_control *ctl);
	/* Optional event boost with the common scheduler lock already held. */
	void	(*prioritize)(struct thread_object *obj);
	/* deinit private data of scheduler */
	void	(*deinit_data)(struct thread_object *obj);
	/* deinit scheduler */
	void	(*deinit)(struct sched_control *ctl);
	/* suspend scheduler */
	void	(*suspend)(struct sched_control *ctl);
	/* resume scheduler */
	void	(*resume)(struct sched_control *ctl);
};
extern struct acrn_scheduler sched_noop;
extern struct acrn_scheduler sched_iorr;

struct sched_noop_control {
	struct thread_object *noop_thread_obj;
};

struct sched_iorr_control {
	struct list_head runqueue;
	struct hv_timer tick_timer;
};

extern struct acrn_scheduler sched_bvt;
struct sched_bvt_control {
	struct list_head runqueue;
	struct hv_timer tick_timer;
	/* The minimum AVT of any runnable threads */
	int64_t svt;
};

extern struct acrn_scheduler sched_rtds;
struct sched_rtds_control {
	struct list_head ready_queue;
	struct list_head depleted_queue;
	struct hv_timer tick_timer;
};

extern struct acrn_scheduler sched_cbs;
struct sched_cbs_control {
	struct list_head runqueue;
	struct hv_timer tick_timer;
	/* Cached one-shot deadline; 0 means no CBS timer is armed. */
	uint64_t timer_deadline_ticks;
	uint64_t admission_utilization;
};

extern struct acrn_scheduler sched_prio;
struct sched_prio_control {
	struct list_head prio_queue;
};

const struct list_head *sched_get_thread_list(void);
uint32_t sched_get_thread_count(void);
bool is_idle_thread(const struct thread_object *obj);
uint16_t sched_get_pcpuid(const struct thread_object *obj);
struct thread_object *sched_get_current(uint16_t pcpu_id);
const char *sched_get_scheduler_name(uint16_t pcpu_id);
const char *sched_get_scheduler_stat_desc(uint16_t pcpu_id);
uint64_t sched_get_ticks(uint16_t pcpu_id);
uint64_t sched_get_context_switches(uint16_t pcpu_id);
uint64_t sched_get_reschedule_requests(uint16_t pcpu_id);
void sched_account_tick(struct sched_control *ctl);
void sched_get_latency(const struct thread_object *obj, struct sched_latency_stats *stats);
const char *sched_latency_hist_bucket_name(uint32_t bucket);
bool sched_get_bvt_stats(const struct thread_object *obj, struct sched_bvt_stats *stats);
bool sched_get_rtds_stats(const struct thread_object *obj, struct sched_rtds_stats *stats);
bool sched_get_cbs_stats(const struct thread_object *obj, struct sched_cbs_stats *stats);
bool sched_get_cbs_pcpu_stats(uint16_t pcpu_id, struct sched_cbs_pcpu_stats *stats);
void sched_set_platform_config(const struct sched_platform_config *config);
const struct sched_platform_config *sched_get_platform_config(void);
const struct sched_cpupool_config *sched_get_pcpu_pool_config(uint16_t pcpu_id);

void init_sched(uint16_t pcpu_id);
void deinit_sched(uint16_t pcpu_id);
void suspend_sched(void);
void resume_sched(void);
void obtain_schedule_lock(uint16_t pcpu_id, uint64_t *rflag);
void release_schedule_lock(uint16_t pcpu_id, uint64_t rflag);

void init_thread_data(struct thread_object *obj, struct sched_params *params);
void deinit_thread_data(struct thread_object *obj);

void make_reschedule_request(uint16_t pcpu_id);
bool need_reschedule(uint16_t pcpu_id);
bool sched_clear_reschedule_if_current_only(uint16_t pcpu_id);

void run_thread(struct thread_object *obj);
void sleep_thread(struct thread_object *obj);
void sleep_thread_sync(struct thread_object *obj);
void wake_thread(struct thread_object *obj);
bool freeze_thread(struct thread_object *obj, uint64_t epoch,
	bool *wake_owned);
bool is_thread_frozen(struct thread_object *obj, uint64_t epoch);
bool thaw_thread(struct thread_object *obj, uint64_t epoch,
	bool wake_owned);
/*
 * Request best-effort scheduler-specific priority treatment. This always raises
 * NEED_RESCHEDULE; only schedulers with .prioritize attach extra ordering state.
 */
void request_thread_priority(struct thread_object *obj);
void request_thread_priority_no_resched(struct thread_object *obj);
void yield_current(void);
void schedule(void);

void arch_send_reschedule_request(uint16_t pcpu_id);
void arch_default_idle(__unused struct thread_object *obj);
void arch_switch_to(void *prev_sp, void *next_sp);
uint64_t arch_setup_thread_stack(struct thread_object *obj, uint8_t *stack, uint64_t stack_size);
void run_idle_thread(void);
#endif /* SCHEDULE_H */
