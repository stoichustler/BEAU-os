// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU hypervisor watchdog heartbeat.
 *
 * This driver does not expose a Linux /dev/watchdog policy device. It only
 * proves guest liveness to the BEAU hypervisor by periodically issuing the
 * HC_VM_WDT_KICK hypercall. If Linux stops scheduling this worker, the
 * hypervisor-side vm_wdt service can mark this VM as stuck.
 */

#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>

#include "hcall.h"

#define BEAU_WDT_DEFAULT_PERIOD_MS	5000U

static unsigned int period_ms = BEAU_WDT_DEFAULT_PERIOD_MS;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "BEAU watchdog heartbeat period in milliseconds");

static struct timer_list beau_wdt_timer;
static unsigned long beau_wdt_kicks;

static long beau_wdt_kick(void)
{
	unsigned long token = ++beau_wdt_kicks;

	return beau_hcall_vm_wdt_kick(token);
}

static void beau_wdt_timerfn(struct timer_list *timer)
{
	long ret = beau_wdt_kick();

	if (ret)
		pr_warn("[κ] BEAU WDT kick failed: %ld\n", ret);

	mod_timer(timer, jiffies + msecs_to_jiffies(period_ms));
}

static int __init beau_wdt_init(void)
{
	long ret;

	if (period_ms == 0)
		period_ms = BEAU_WDT_DEFAULT_PERIOD_MS;

	timer_setup(&beau_wdt_timer, beau_wdt_timerfn, 0);
	ret = beau_wdt_kick();
	mod_timer(&beau_wdt_timer, jiffies + msecs_to_jiffies(period_ms));

	if (ret != 0)
		pr_warn("[κ] BEAU WDT first kick failed:%ld\n", ret);
	else
		pr_info("[κ] BEAU WDT enabled: period:%u ms\n", period_ms);

	return 0;
}

static void __exit beau_wdt_exit(void)
{
	timer_delete_sync(&beau_wdt_timer);
}

/*
 * This heartbeat is a hypervisor liveness contract, not a normal device probe.
 * device_initcall/module_init is too late when multiple Linux guests boot
 * together under BEAU; the hypervisor can still see kick:0 at its 15s timeout
 * while Linux is working through filesystem/device initcalls. core_initcall
 * keeps builtin parameter parsing but sends the first kick before the heavy
 * initcall levels.
 */
core_initcall(beau_wdt_init);
module_exit(beau_wdt_exit);

MODULE_DESCRIPTION("BEAU hypervisor watchdog");
MODULE_AUTHOR("BEAU");
MODULE_LICENSE("GPL");
