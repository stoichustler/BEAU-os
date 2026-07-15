/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <zephyr/arch/arm64/arm-smccc.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/internal/mm.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(beau_pm);

#define BEAU_HC_ID(x, y)		(((x) << 24) | (y))
#define BEAU_HC_CLASS			0x80UL
#define BEAU_HC_PM_BASE			0x80UL
#define HC_PM_CONTROL			BEAU_HC_ID(BEAU_HC_CLASS, BEAU_HC_PM_BASE + 0x01UL)

#define ACRN_PM_ABI_VERSION		1U
#define ACRN_PM_QUERY_CAPS		0U
#define ACRN_PM_GET_EVENT		2U
#define ACRN_PM_GET_WAKE_REASON		5U
#define ACRN_PM_RESUME_COMPLETE		6U
#define ACRN_PM_FLAG_REQUIRED		(1U << 0)
#define ACRN_PM_EVENT_PREPARE		(1U << 1)
#define ACRN_PM_CAP_SYSTEM_SUSPEND	(1U << 8)
#define ACRN_INVALID_VMID		0xffffU

#define PSCI_1_0_FN64_SYSTEM_SUSPEND	0xc400000eUL
#define BEAU_PM_EVENT_IRQ		60U
#define BEAU_PM_STACK_SIZE		2048
#define BEAU_PM_PRIORITY			5

struct acrn_pm_ioc {
	uint32_t abi_version;
	uint32_t ioc_size;
	uint32_t op;
	int32_t status;
	uint64_t epoch;
	uint64_t wake_reason;
	uint64_t required_vm_mask;
	uint32_t pm_state;
	uint32_t vm_state;
	uint16_t vmid;
	uint16_t flags;
	uint32_t event_virq;
	uint64_t reserved;
} __aligned(64);

struct beau_pm_cpu_context {
	uint64_t sp;
	uint64_t resume_pc;
	uint64_t sctlr_el1;
	uint64_t tcr_el1;
	uint64_t ttbr0_el1;
	uint64_t ttbr1_el1;
	uint64_t mair_el1;
	uint64_t vbar_el1;
	uint64_t tpidr_el1;
	uint64_t tpidrro_el0;
	uint64_t contextidr_el1;
	uint64_t daif;
	uint64_t cpacr_el1;
	uint64_t cntkctl_el1;
	uint64_t epoch;
} __aligned(64);

_Static_assert(sizeof(struct acrn_pm_ioc) == 64, "PM ABI size");
_Static_assert(offsetof(struct beau_pm_cpu_context, epoch) == 112,
	"resume trampoline layout");

static struct acrn_pm_ioc beau_pm_ioc __aligned(64);
static struct beau_pm_cpu_context beau_pm_cpu_context __aligned(64);
static struct k_sem beau_pm_event;
static uint16_t beau_pm_vmid = ACRN_INVALID_VMID;
static uint64_t beau_pm_last_epoch;
static bool beau_pm_ready;

extern void beau_pm_resume_entry(uint64_t context);

__asm__(
	".pushsection .text.beau_pm_resume_entry,\"ax\"\n"
	".align 4\n"
	".global beau_pm_resume_entry\n"
	".type beau_pm_resume_entry, %function\n"
	"beau_pm_resume_entry:\n"
	"mov x9, x0\n"
	"ldr x10, [x9, #24]\n"
	"msr tcr_el1, x10\n"
	"ldr x10, [x9, #32]\n"
	"msr ttbr0_el1, x10\n"
	"ldr x10, [x9, #40]\n"
	"msr ttbr1_el1, x10\n"
	"ldr x10, [x9, #48]\n"
	"msr mair_el1, x10\n"
	"ldr x10, [x9, #56]\n"
	"msr vbar_el1, x10\n"
	"ldr x10, [x9, #64]\n"
	"msr tpidr_el1, x10\n"
	"ldr x10, [x9, #72]\n"
	"msr tpidrro_el0, x10\n"
	"ldr x10, [x9, #80]\n"
	"msr contextidr_el1, x10\n"
	"ldr x10, [x9, #96]\n"
	"msr cpacr_el1, x10\n"
	"ldr x10, [x9, #104]\n"
	"msr cntkctl_el1, x10\n"
	"dsb sy\n"
	"isb\n"
	"ldr x10, [x9, #16]\n"
	"msr sctlr_el1, x10\n"
	"isb\n"
	"ldr x10, [x9, #0]\n"
	"mov sp, x10\n"
	"ldr x10, [x9, #88]\n"
	"msr daif, x10\n"
	"ldr x10, [x9, #8]\n"
	"mov x0, xzr\n"
	"br x10\n"
	".size beau_pm_resume_entry, . - beau_pm_resume_entry\n"
	".popsection\n");

static long beau_pm_hcall(uint32_t op, uint64_t epoch)
{
	struct arm_smccc_res res;

	memset(&beau_pm_ioc, 0, sizeof(beau_pm_ioc));
	beau_pm_ioc.abi_version = ACRN_PM_ABI_VERSION;
	beau_pm_ioc.ioc_size = sizeof(beau_pm_ioc);
	beau_pm_ioc.op = op;
	beau_pm_ioc.epoch = epoch;
	beau_pm_ioc.vmid = beau_pm_vmid;
	arm_smccc_hvc(HC_PM_CONTROL, k_mem_phys_addr(&beau_pm_ioc),
		0, 0, 0, 0, 0, 0, &res);
	return (res.a0 != 0UL) ? (long)res.a0 : beau_pm_ioc.status;
}

static long beau_pm_system_suspend(uint64_t epoch)
{
	register unsigned long x0 __asm__("x0") = PSCI_1_0_FN64_SYSTEM_SUSPEND;
	uintptr_t entry = k_mem_phys_addr((void *)beau_pm_resume_entry);
	uintptr_t context = k_mem_phys_addr(&beau_pm_cpu_context);
	struct beau_pm_cpu_context *ctx = &beau_pm_cpu_context;

	ctx->epoch = epoch;
	__asm__ volatile(
		"mov x9, sp\n"
		"str x9, [%[ctx], #0]\n"
		"adr x9, 1f\n"
		"str x9, [%[ctx], #8]\n"
		"mrs x9, sctlr_el1\n"
		"str x9, [%[ctx], #16]\n"
		"mrs x9, tcr_el1\n"
		"str x9, [%[ctx], #24]\n"
		"mrs x9, ttbr0_el1\n"
		"str x9, [%[ctx], #32]\n"
		"mrs x9, ttbr1_el1\n"
		"str x9, [%[ctx], #40]\n"
		"mrs x9, mair_el1\n"
		"str x9, [%[ctx], #48]\n"
		"mrs x9, vbar_el1\n"
		"str x9, [%[ctx], #56]\n"
		"mrs x9, tpidr_el1\n"
		"str x9, [%[ctx], #64]\n"
		"mrs x9, tpidrro_el0\n"
		"str x9, [%[ctx], #72]\n"
		"mrs x9, contextidr_el1\n"
		"str x9, [%[ctx], #80]\n"
		"mrs x9, daif\n"
		"str x9, [%[ctx], #88]\n"
		"mrs x9, cpacr_el1\n"
		"str x9, [%[ctx], #96]\n"
		"mrs x9, cntkctl_el1\n"
		"str x9, [%[ctx], #104]\n"
		"mov x1, %[entry]\n"
		"mov x2, %[context]\n"
		"hvc #0\n"
		"1:\n"
		: "+r"(x0)
		: [ctx] "r"(ctx), [entry] "r"(entry), [context] "r"(context)
		: "x1", "x2", "x9", "memory");

	return (long)x0;
}

static void beau_pm_irq(const void *unused)
{
	ARG_UNUSED(unused);
	k_sem_give(&beau_pm_event);
}

static int beau_pm_init(void)
{
	k_sem_init(&beau_pm_event, 0, 1);
	IRQ_CONNECT(BEAU_PM_EVENT_IRQ, 0, beau_pm_irq, NULL, 0);
	irq_enable(BEAU_PM_EVENT_IRQ);
	return 0;
}

SYS_INIT(beau_pm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int beau_pm_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "beau_pm:ready:%c vm:%u last-epoch:%llu",
		beau_pm_ready ? 'Y' : 'N', beau_pm_vmid, beau_pm_last_epoch);
	return 0;
}

SHELL_CMD_REGISTER(beau_pm, NULL, "BEAU coordinated PM status",
	beau_pm_status);

static void beau_pm_thread(void *arg1, void *arg2, void *arg3)
{
	long ret;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	ret = beau_pm_hcall(ACRN_PM_QUERY_CAPS, 0);
	if (ret != 0 || !(beau_pm_ioc.flags & ACRN_PM_CAP_SYSTEM_SUSPEND) ||
	    beau_pm_ioc.event_virq != BEAU_PM_EVENT_IRQ) {
		LOG_ERR("PM ABI negotiation failed ret:%ld flags:0x%x irq:%u",
			ret, beau_pm_ioc.flags, beau_pm_ioc.event_virq);
		return;
	}
	beau_pm_vmid = beau_pm_ioc.vmid;
	beau_pm_ready = true;
	LOG_INF("coordinated PM agent ready vm:%u irq:%u",
		beau_pm_vmid, BEAU_PM_EVENT_IRQ);

	for (;;) {
		uint64_t epoch;
		uint64_t wake_reason;

		k_sem_take(&beau_pm_event, K_FOREVER);
		ret = beau_pm_hcall(ACRN_PM_GET_EVENT, 0);
		if (ret != 0 || !(beau_pm_ioc.flags & ACRN_PM_EVENT_PREPARE) ||
		    !(beau_pm_ioc.flags & ACRN_PM_FLAG_REQUIRED) ||
		    beau_pm_ioc.epoch <= beau_pm_last_epoch) {
			continue;
		}
		epoch = beau_pm_ioc.epoch;
		LOG_INF("prepare epoch:%llu", epoch);
		ret = beau_pm_system_suspend(epoch);
		if (ret != 0 || beau_pm_cpu_context.epoch != epoch) {
			LOG_ERR("SYSTEM_SUSPEND failed epoch:%llu ret:%ld", epoch, ret);
			continue;
		}

		ret = beau_pm_hcall(ACRN_PM_GET_WAKE_REASON, epoch);
		wake_reason = beau_pm_ioc.wake_reason;
		if (ret == 0)
			ret = beau_pm_hcall(ACRN_PM_RESUME_COMPLETE, epoch);
		if (ret != 0) {
			LOG_ERR("resume acknowledgement failed epoch:%llu ret:%ld",
				epoch, ret);
			continue;
		}
		beau_pm_last_epoch = epoch;
		LOG_INF("resume complete epoch:%llu wake:%llu", epoch, wake_reason);
	}
}

K_THREAD_DEFINE(beau_pm_tid, BEAU_PM_STACK_SIZE, beau_pm_thread,
	NULL, NULL, NULL, BEAU_PM_PRIORITY, 0, 0);
