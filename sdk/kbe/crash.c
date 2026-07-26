// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU guest panic and oops reporter.
 *
 * The notifier preserves Linux's normal panic/oops path. It submits one
 * bounded diagnostic report to BEAU so the Host can retain and display the
 * reason even when the guest console is no longer available.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/smp.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/sched.h>

#include "hcall.h"

#define BEAU_CRASH_PANIC_REPORTED	0
#define BEAU_CRASH_OOPS_REPORTED	1
#define BEAU_CRASH_REPORT_SLOTS		2U

static atomic_t beau_crash_reported = ATOMIC_INIT(0);
static atomic64_t beau_crash_sequence = ATOMIC64_INIT(0);
static struct beau_vm_crash_report
	beau_crash_reports[BEAU_CRASH_REPORT_SLOTS] __aligned(8);

/*
 * Panic/Oops context
 *     |
 *     v
 * claim one report kind -> fixed static report -> HVC -> BEAU retained record
 *     |
 *     +--> duplicate: leave Linux's existing error path untouched
 *
 * Key rule:
 *   - the atomic mask owns reporting eligibility across all guest CPUs;
 *   - no allocation, lock acquisition, or guest printk occurs in notifier context;
 *   - the HVC receives only a fixed ABI object from direct-mapped static
 *     storage, preventing VMAP-stack address conversion and variable-length
 *     guest log parsing.
 */
static bool beau_crash_claim(unsigned int kind)
{
	int bit;

	if (kind == BEAU_VM_CRASH_PANIC)
		bit = BEAU_CRASH_PANIC_REPORTED;
	else if (kind == BEAU_VM_CRASH_OOPS)
		bit = BEAU_CRASH_OOPS_REPORTED;
	else
		return false;

	return (atomic_fetch_or(BIT(bit), &beau_crash_reported) & BIT(bit)) == 0;
}

static struct beau_vm_crash_report *beau_crash_report_slot(unsigned int kind)
{
	switch (kind) {
	case BEAU_VM_CRASH_PANIC:
		return &beau_crash_reports[BEAU_CRASH_PANIC_REPORTED];
	case BEAU_VM_CRASH_OOPS:
		return &beau_crash_reports[BEAU_CRASH_OOPS_REPORTED];
	default:
		return NULL;
	}
}

static void beau_crash_report(unsigned int kind, const char *message,
	struct pt_regs *regs, unsigned long error_code)
{
	struct beau_vm_crash_report *report = beau_crash_report_slot(kind);

	if (report == NULL)
		return;

	memset(report, 0, sizeof(*report));
	report->magic = BEAU_VM_CRASH_MAGIC;
	report->version = BEAU_VM_CRASH_ABI_VERSION;
	report->size = sizeof(*report);
	report->kind = kind;
	report->cpu_id = raw_smp_processor_id();
	report->sequence = atomic64_inc_return(&beau_crash_sequence);
	report->error_code = error_code;
	report->pid = task_pid_nr(current);
	report->tgid = task_tgid_nr(current);
	strscpy(report->comm, current->comm, sizeof(report->comm));

	if (regs != NULL) {
		report->pc = instruction_pointer(regs);
		report->sp = regs->sp;
		report->pstate = regs->pstate;
		report->flags |= BEAU_VM_CRASH_F_REGS_VALID;
		report->stack_count = stack_trace_save_regs(regs, report->stack,
			BEAU_VM_CRASH_STACK_MAX, 0U);
	} else {
		report->stack_count = stack_trace_save(report->stack,
			BEAU_VM_CRASH_STACK_MAX, 2U);
	}
	if (report->stack_count != 0U)
		report->flags |= BEAU_VM_CRASH_F_STACK_VALID;
	if (message != NULL)
		strscpy(report->message, message, sizeof(report->message));
	else
		strscpy(report->message, "Linux guest crash", sizeof(report->message));

	/* Static slots have direct-map GPAs; VMAP kernel stacks do not. */
	smp_wmb();
	/* The notifier cannot recover from an unavailable Host; preserve Linux flow. */
	(void)beau_hcall_vm_crash_report(report);
}

static int beau_panic_notify(struct notifier_block *notifier,
	unsigned long event, void *data)
{
	if (beau_crash_claim(BEAU_VM_CRASH_PANIC))
		beau_crash_report(BEAU_VM_CRASH_PANIC, data, NULL, event);

	return NOTIFY_DONE;
}

static int beau_die_notify(struct notifier_block *notifier,
	unsigned long event, void *data)
{
	const struct die_args *args = data;

	if ((event == DIE_OOPS) && (args != NULL) &&
		beau_crash_claim(BEAU_VM_CRASH_OOPS))
		beau_crash_report(BEAU_VM_CRASH_OOPS, args->str, args->regs, args->err);

	return NOTIFY_DONE;
}

static struct notifier_block beau_panic_notifier = {
	.notifier_call = beau_panic_notify,
};

static struct notifier_block beau_die_notifier = {
	.notifier_call = beau_die_notify,
};

static int __init beau_crash_init(void)
{
	int ret;

	ret = atomic_notifier_chain_register(&panic_notifier_list,
		&beau_panic_notifier);
	if (ret != 0)
		return ret;
	ret = register_die_notifier(&beau_die_notifier);
	if (ret != 0) {
		atomic_notifier_chain_unregister(&panic_notifier_list,
			&beau_panic_notifier);
		return ret;
	}

	return 0;
}

static void __exit beau_crash_exit(void)
{
	unregister_die_notifier(&beau_die_notifier);
	atomic_notifier_chain_unregister(&panic_notifier_list, &beau_panic_notifier);
}

core_initcall(beau_crash_init);
module_exit(beau_crash_exit);

MODULE_DESCRIPTION("BEAU guest panic and oops reporter");
MODULE_AUTHOR("BEAU");
MODULE_LICENSE("GPL");
