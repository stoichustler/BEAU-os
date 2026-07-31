// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU hypervisor watchdog heartbeat.
 *
 * This driver does not expose a Linux /dev/watchdog policy device. It only
 * proves per-CPU guest progress to the BEAU hypervisor by periodically issuing
 * HC_VM_WDT_KICK from pinned kernel threads. A pinned timer only wakes each
 * thread; the hypercall is issued only after process context is scheduled.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/smpboot.h>

#include "hcall.h"

#define BEAU_WDT_DEFAULT_PERIOD_MS	5000U

static unsigned int period_ms = BEAU_WDT_DEFAULT_PERIOD_MS;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "BEAU watchdog heartbeat period in milliseconds");

struct beau_wdt_cpu {
	struct hrtimer timer;
	atomic_t pending;
	unsigned long kicks;
};

static DEFINE_PER_CPU(struct beau_wdt_cpu, beau_wdt_cpus);
static DEFINE_PER_CPU(struct task_struct *, beau_wdt_threads);

static unsigned int beau_wdt_period_ms(void)
{
	unsigned int value = READ_ONCE(period_ms);

	return value != 0U ? value : BEAU_WDT_DEFAULT_PERIOD_MS;
}

static enum hrtimer_restart beau_wdt_timerfn(struct hrtimer *timer)
{
	struct beau_wdt_cpu *wdt = container_of(timer, struct beau_wdt_cpu, timer);
	struct task_struct *task = this_cpu_read(beau_wdt_threads);

	atomic_set(&wdt->pending, 1);
	if (task != NULL)
		wake_up_process(task);

	hrtimer_forward_now(timer, ms_to_ktime(beau_wdt_period_ms()));
	return HRTIMER_RESTART;
}

static int beau_wdt_should_run(unsigned int cpu)
{
	return atomic_read(&per_cpu(beau_wdt_cpus, cpu).pending);
}

static void beau_wdt_threadfn(unsigned int cpu)
{
	struct beau_wdt_cpu *wdt = per_cpu_ptr(&beau_wdt_cpus, cpu);
	long ret;

	if (atomic_xchg(&wdt->pending, 0) == 0)
		return;

	ret = beau_hcall_vm_wdt_kick_vcpu(++wdt->kicks);
	if (ret != 0)
		pr_warn_ratelimited("[k] BEAU WDT CPU%u kick failed: %ld\n",
			cpu, ret);
}

static void beau_wdt_start_cpu(unsigned int cpu)
{
	struct beau_wdt_cpu *wdt = per_cpu_ptr(&beau_wdt_cpus, cpu);

	atomic_set(&wdt->pending, 1);
	hrtimer_start(&wdt->timer, ms_to_ktime(beau_wdt_period_ms()),
		HRTIMER_MODE_REL_PINNED);
}

static void beau_wdt_setup(unsigned int cpu)
{
	beau_wdt_start_cpu(cpu);
}

static void beau_wdt_unpark(unsigned int cpu)
{
	beau_wdt_start_cpu(cpu);
}

static void beau_wdt_stop_cpu(unsigned int cpu)
{
	struct beau_wdt_cpu *wdt = per_cpu_ptr(&beau_wdt_cpus, cpu);

	hrtimer_cancel(&wdt->timer);
	atomic_set(&wdt->pending, 0);
}

static void beau_wdt_park(unsigned int cpu)
{
	beau_wdt_stop_cpu(cpu);
}

static void beau_wdt_cleanup(unsigned int cpu, bool online)
{
	(void)online;
	beau_wdt_stop_cpu(cpu);
}

static struct smp_hotplug_thread beau_wdt_thread = {
	.store = &beau_wdt_threads,
	.thread_should_run = beau_wdt_should_run,
	.thread_fn = beau_wdt_threadfn,
	.setup = beau_wdt_setup,
	.cleanup = beau_wdt_cleanup,
	.park = beau_wdt_park,
	.unpark = beau_wdt_unpark,
	.thread_comm = "beau_wdt/%u",
};

static int __init beau_wdt_init(void)
{
	unsigned int cpu;
	int ret;

	if (period_ms == 0)
		period_ms = BEAU_WDT_DEFAULT_PERIOD_MS;

	for_each_possible_cpu(cpu) {
		struct beau_wdt_cpu *wdt = per_cpu_ptr(&beau_wdt_cpus, cpu);

		hrtimer_setup(&wdt->timer, beau_wdt_timerfn, CLOCK_MONOTONIC,
			HRTIMER_MODE_REL_PINNED);
		atomic_set(&wdt->pending, 0);
		wdt->kicks = 0UL;
	}

	ret = smpboot_register_percpu_thread(&beau_wdt_thread);
	if (ret != 0) {
		pr_err("[k] BEAU WDT thread registration failed: %d\n", ret);
		return ret;
	}

	pr_info("[k] BEAU WDT enabled: per-vCPU period:%u ms\n", period_ms);

	return 0;
}

static void __exit beau_wdt_exit(void)
{
	smpboot_unregister_percpu_thread(&beau_wdt_thread);
}

/*
 * This heartbeat is a hypervisor liveness contract, not a normal device probe.
 * device_initcall/module_init is too late when multiple Linux guests boot
 * together under BEAU. core_initcall registers the hotplug threads before the
 * heavy filesystem and device initcall levels.
 */
core_initcall(beau_wdt_init);
module_exit(beau_wdt_exit);

MODULE_DESCRIPTION("BEAU hypervisor watchdog");
MODULE_AUTHOR("BEAU");
MODULE_LICENSE("GPL");
