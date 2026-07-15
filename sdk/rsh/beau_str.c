/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <cpu.h>
#include <smccc.h>
#include <drivers/pm.h>
#include <drivers/platform.h>

#define BEAU_HC_ID(x, y)                (((x) << 24) | (y))
#define BEAU_HC_CLASS                   0x80UL
#define BEAU_HC_PM_BASE                 0x80UL
#define HC_PM_CONTROL                   BEAU_HC_ID(BEAU_HC_CLASS, BEAU_HC_PM_BASE + 1UL)

#define ACRN_PM_ABI_VERSION             1U
#define ACRN_PM_QUERY_CAPS              0U
#define ACRN_PM_GET_EVENT               2U
#define ACRN_PM_GET_WAKE_REASON         5U
#define ACRN_PM_RESUME_COMPLETE         6U
#define ACRN_PM_FLAG_REQUIRED           (1U << 0)
#define ACRN_PM_EVENT_PREPARE           (1U << 1)
#define ACRN_PM_CAP_SYSTEM_SUSPEND      (1U << 8)
#define ACRN_INVALID_VMID               0xffffU

#define PSCI_0_2_FN_CPU_OFF             0x84000002UL
#define PSCI_0_2_FN64_CPU_ON            0xc4000003UL
#define PSCI_0_2_FN64_AFFINITY_INFO     0xc4000004UL
#define PSCI_1_0_FN64_SYSTEM_SUSPEND    0xc400000eUL
#define PSCI_AFFINITY_LEVEL_OFF         1UL

#define BEAU_PM_EVENT_IRQ               60
#define BEAU_PM_AP_CPU                  1U
#define BEAU_PM_STACK_SIZE              4096U
#define BEAU_PM_PRIORITY                5U
#define BEAU_PM_TIMEOUT_SECONDS         2U

struct acrn_pm_ioc
{
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
} __attribute__((aligned(64)));

struct beau_str_cpu_context
{
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
    uint64_t resumed;
} __attribute__((aligned(64)));

enum beau_str_ap_state
{
    BEAU_AP_ONLINE,
    BEAU_AP_OFFLINE_REQUESTED,
    BEAU_AP_OFFLINE,
    BEAU_AP_FAILED,
};

_Static_assert(sizeof(struct acrn_pm_ioc) == 64, "PM ABI size");
_Static_assert(offsetof(struct beau_str_cpu_context, epoch) == 112,
               "resume context epoch offset");
_Static_assert(offsetof(struct beau_str_cpu_context, resumed) == 120,
               "resume context flag offset");

static struct acrn_pm_ioc beau_pm_ioc __attribute__((aligned(64)));
static struct beau_str_cpu_context beau_bsp_context __attribute__((aligned(64)));
#if defined(RT_USING_SMP) && RT_CPUS_NR > 1
static struct beau_str_cpu_context beau_ap_context __attribute__((aligned(64)));
#endif
static struct rt_semaphore beau_event_sem;
static struct rt_semaphore beau_resume_sem;
static volatile uint64_t beau_pending_epoch;
static volatile uint64_t beau_last_epoch;
static volatile long beau_transaction_result;
static volatile enum beau_str_ap_state beau_ap_state = BEAU_AP_ONLINE;
static uint16_t beau_vmid = ACRN_INVALID_VMID;
static rt_bool_t beau_ready;

extern void beau_str_resume_entry(uint64_t context);

__asm__(
    ".pushsection .text.beau_str_resume_entry,\"ax\"\n"
    ".align 4\n"
    ".global beau_str_resume_entry\n"
    ".type beau_str_resume_entry, %function\n"
    "beau_str_resume_entry:\n"
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
    "mov x14, #1\n"
    "str x14, [x9, #120]\n"
    "ldr x11, [x9, #0]\n"
    "ldr x12, [x9, #88]\n"
    "ldr x13, [x9, #8]\n"
    "dsb sy\n"
    "isb\n"
    "ldr x10, [x9, #16]\n"
    "msr sctlr_el1, x10\n"
    "isb\n"
    "mov sp, x11\n"
    "msr daif, x12\n"
    "mov x0, xzr\n"
    "br x13\n"
    ".size beau_str_resume_entry, . - beau_str_resume_entry\n"
    ".popsection\n");

static unsigned long beau_hvc(unsigned long a0, unsigned long a1,
                              unsigned long a2, unsigned long a3)
{
    struct arm_smccc_res_t res;

    arm_smccc_hvc(a0, a1, a2, a3, 0, 0, 0, 0, &res, RT_NULL);
    return res.a0;
}

static uintptr_t beau_phys(const void *address)
{
    return (uintptr_t)rt_kmem_v2p((void *)address);
}

static long beau_pm_hcall(uint32_t op, uint64_t epoch)
{
    unsigned long ret;

    rt_memset(&beau_pm_ioc, 0, sizeof(beau_pm_ioc));
    beau_pm_ioc.abi_version = ACRN_PM_ABI_VERSION;
    beau_pm_ioc.ioc_size = sizeof(beau_pm_ioc);
    beau_pm_ioc.op = op;
    beau_pm_ioc.epoch = epoch;
    beau_pm_ioc.vmid = beau_vmid;
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &beau_pm_ioc, sizeof(beau_pm_ioc));
    ret = beau_hvc(HC_PM_CONTROL, beau_phys(&beau_pm_ioc), 0, 0);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, &beau_pm_ioc, sizeof(beau_pm_ioc));

    return ret != 0UL ? (long)ret : beau_pm_ioc.status;
}

static uint64_t beau_counter(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static uint64_t beau_counter_frequency(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

static void beau_context_flush(struct beau_str_cpu_context *context)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, context, sizeof(*context));
    __asm__ volatile("dsb sy" ::: "memory");
}

static long beau_power_call(unsigned long function_id,
                            struct beau_str_cpu_context *context,
                            uint64_t epoch)
{
    register unsigned long x0 __asm__("x0") = function_id;
    uintptr_t entry = beau_phys((void *)beau_str_resume_entry);
    uintptr_t context_pa = beau_phys(context);

    context->epoch = epoch;
    context->resumed = 0;
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
        "dc cvac, %[ctx]\n"
        "add x9, %[ctx], #64\n"
        "dc cvac, x9\n"
        "mov x1, %[entry]\n"
        "mov x2, %[context_pa]\n"
        "dsb sy\n"
        "hvc #0\n"
        "1:\n"
        : "+r"(x0)
        : [ctx] "r"(context), [entry] "r"(entry),
          [context_pa] "r"(context_pa)
        : "x1", "x2", "x9", "memory");

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, context, sizeof(*context));
    return (long)x0;
}

#if defined(RT_USING_SMP) && RT_CPUS_NR > 1
static rt_bool_t beau_ap_is_offline(void)
{
    uint64_t mpidr = rt_cpu_mpidr_table[BEAU_PM_AP_CPU] & MPIDR_AFFINITY_MASK;

    return beau_hvc(PSCI_0_2_FN64_AFFINITY_INFO, mpidr, 0, 0) ==
           PSCI_AFFINITY_LEVEL_OFF;
}

static long beau_offline_ap(uint64_t epoch)
{
    uint64_t deadline = beau_counter() +
                        beau_counter_frequency() * BEAU_PM_TIMEOUT_SECONDS;

    beau_ap_context.epoch = epoch;
    __atomic_store_n(&beau_ap_state, BEAU_AP_OFFLINE_REQUESTED, __ATOMIC_RELEASE);
    rt_hw_ipi_send(RT_SCHEDULE_IPI, 1U << BEAU_PM_AP_CPU);
    __asm__ volatile("sev" ::: "memory");

    do
    {
        if (beau_ap_is_offline())
        {
            __atomic_store_n(&beau_ap_state, BEAU_AP_OFFLINE, __ATOMIC_RELEASE);
            return 0;
        }
        if (__atomic_load_n(&beau_ap_state, __ATOMIC_ACQUIRE) == BEAU_AP_FAILED)
        {
            return -RT_ERROR;
        }
    } while (beau_counter() < deadline);

    return -RT_ETIMEOUT;
}

static long beau_online_ap(uint64_t epoch)
{
    uint64_t deadline = beau_counter() +
                        beau_counter_frequency() * BEAU_PM_TIMEOUT_SECONDS;
    uint64_t mpidr = rt_cpu_mpidr_table[BEAU_PM_AP_CPU] & MPIDR_AFFINITY_MASK;
    unsigned long ret;

    if (beau_ap_context.epoch != epoch)
    {
        return -RT_EINVAL;
    }
    beau_context_flush(&beau_ap_context);
    ret = beau_hvc(PSCI_0_2_FN64_CPU_ON, mpidr,
                   beau_phys((void *)beau_str_resume_entry),
                   beau_phys(&beau_ap_context));
    if ((long)ret != 0)
    {
        return (long)ret;
    }

    do
    {
        if (__atomic_load_n(&beau_ap_state, __ATOMIC_ACQUIRE) == BEAU_AP_ONLINE)
        {
            return 0;
        }
    } while (beau_counter() < deadline);

    return -RT_ETIMEOUT;
}

void rt_hw_secondary_cpu_idle_exec(void)
{
    uint64_t epoch;
    long ret;

    if (__atomic_load_n(&beau_ap_state, __ATOMIC_ACQUIRE) !=
        BEAU_AP_OFFLINE_REQUESTED)
    {
        __asm__ volatile("wfe" ::: "memory");
        return;
    }

    epoch = beau_ap_context.epoch;
    beau_context_flush(&beau_ap_context);
    ret = beau_power_call(PSCI_0_2_FN_CPU_OFF, &beau_ap_context, epoch);
    if (ret != 0 || beau_ap_context.epoch != epoch || !beau_ap_context.resumed)
    {
        __atomic_store_n(&beau_ap_state, BEAU_AP_FAILED, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&beau_ap_state, BEAU_AP_ONLINE, __ATOMIC_RELEASE);
}
#else
static long beau_offline_ap(uint64_t epoch)
{
    (void)epoch;
    return 0;
}

static long beau_online_ap(uint64_t epoch)
{
    (void)epoch;
    return 0;
}
#endif

static long beau_suspend_transaction(uint64_t epoch)
{
    long ret;

    ret = beau_offline_ap(epoch);
    if (ret != 0)
    {
        return ret;
    }

    beau_context_flush(&beau_bsp_context);
    ret = beau_power_call(PSCI_1_0_FN64_SYSTEM_SUSPEND, &beau_bsp_context,
                          epoch);
    if (ret != 0 || beau_bsp_context.epoch != epoch ||
        !beau_bsp_context.resumed)
    {
        long suspend_ret = ret != 0 ? ret : -RT_ERROR;

        (void)beau_online_ap(epoch);
        return suspend_ret;
    }

    return beau_online_ap(epoch);
}

static void beau_pm_sleep(struct rt_pm *pm, rt_uint8_t mode)
{
    uint64_t epoch = __atomic_load_n(&beau_pending_epoch, __ATOMIC_ACQUIRE);

    (void)pm;
    if (epoch == 0)
    {
        __asm__ volatile("wfi" ::: "memory");
        return;
    }
    if (mode != PM_SLEEP_MODE_DEEP)
    {
        beau_transaction_result = -RT_EIO;
    }
    else
    {
        beau_transaction_result = beau_suspend_transaction(epoch);
    }

    /* Keep the next idle iteration awake until the agent validates and ACKs. */
    (void)rt_pm_request(PM_SLEEP_MODE_NONE);
}

static const struct rt_pm_ops beau_pm_ops =
{
    .sleep = beau_pm_sleep,
};

static void beau_pm_notify(rt_uint8_t event, rt_uint8_t mode, void *data)
{
    (void)mode;
    (void)data;
    if (event == RT_PM_EXIT_SLEEP &&
        __atomic_load_n(&beau_pending_epoch, __ATOMIC_ACQUIRE) != 0)
    {
        rt_sem_release(&beau_resume_sem);
    }
}

static void beau_pm_irq(int vector, void *param)
{
    (void)vector;
    (void)param;
    rt_sem_release(&beau_event_sem);
}

static void beau_agent_thread(void *parameter)
{
    long ret;

    (void)parameter;
    ret = beau_pm_hcall(ACRN_PM_QUERY_CAPS, 0);
    if (ret != 0 || !(beau_pm_ioc.flags & ACRN_PM_CAP_SYSTEM_SUSPEND) ||
        beau_pm_ioc.event_virq != BEAU_PM_EVENT_IRQ)
    {
        rt_kprintf("[BEAU STR] ABI negotiation failed ret:%ld flags:0x%x irq:%u\n",
                   ret, beau_pm_ioc.flags, beau_pm_ioc.event_virq);
        return;
    }
    beau_vmid = beau_pm_ioc.vmid;
    beau_ready = RT_TRUE;
    rt_kprintf("[BEAU STR] ready vm:%u irq:%u\n", beau_vmid,
               beau_pm_ioc.event_virq);

    for (;;)
    {
        uint64_t epoch;
        uint64_t wake_reason;

        rt_sem_take(&beau_event_sem, RT_WAITING_FOREVER);
        ret = beau_pm_hcall(ACRN_PM_GET_EVENT, 0);
        if (ret != 0 || !(beau_pm_ioc.flags & ACRN_PM_EVENT_PREPARE) ||
            !(beau_pm_ioc.flags & ACRN_PM_FLAG_REQUIRED) ||
            beau_pm_ioc.epoch <= beau_last_epoch)
        {
            continue;
        }

        epoch = beau_pm_ioc.epoch;
        beau_transaction_result = -RT_EBUSY;
        __atomic_store_n(&beau_pending_epoch, epoch, __ATOMIC_RELEASE);
        rt_kprintf("[BEAU STR] prepare epoch:%lu\n", (unsigned long)epoch);
        (void)rt_pm_release(PM_SLEEP_MODE_NONE);
        rt_sem_take(&beau_resume_sem, RT_WAITING_FOREVER);

        ret = beau_transaction_result;
        if (ret == 0 && beau_bsp_context.epoch == epoch &&
            beau_bsp_context.resumed)
        {
            ret = beau_pm_hcall(ACRN_PM_GET_WAKE_REASON, epoch);
            wake_reason = beau_pm_ioc.wake_reason;
            if (ret == 0)
            {
                ret = beau_pm_hcall(ACRN_PM_RESUME_COMPLETE, epoch);
            }
        }
        else
        {
            wake_reason = 0;
        }

        __atomic_store_n(&beau_pending_epoch, 0, __ATOMIC_RELEASE);
        if (ret != 0)
        {
            rt_kprintf("[BEAU STR] transaction failed epoch:%lu ret:%ld\n",
                       (unsigned long)epoch, ret);
            continue;
        }
        beau_last_epoch = epoch;
        rt_kprintf("[BEAU STR] resume complete epoch:%lu wake:%lu\n",
                   (unsigned long)epoch, (unsigned long)wake_reason);
    }
}

static int beau_pm_platform_init(void)
{
    rt_sem_init(&beau_event_sem, "str_evt", 0, RT_IPC_FLAG_PRIO);
    rt_sem_init(&beau_resume_sem, "str_res", 0, RT_IPC_FLAG_PRIO);
    rt_system_pm_init(&beau_pm_ops, 0, RT_NULL);
    rt_pm_notify_set(beau_pm_notify, RT_NULL);
    return 0;
}
INIT_PREV_EXPORT(beau_pm_platform_init);

static rt_err_t beau_agent_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    rt_thread_t tid;
    int irq;

    irq = rt_dm_dev_get_irq(dev, 0);
    if (irq < 0)
    {
        return irq;
    }
    rt_hw_interrupt_install(irq, beau_pm_irq, dev, "beau-str");
    rt_hw_interrupt_umask(irq);
    tid = rt_thread_create("beau_str", beau_agent_thread, RT_NULL,
                           BEAU_PM_STACK_SIZE, BEAU_PM_PRIORITY, 10);
    if (tid == RT_NULL)
    {
        rt_hw_interrupt_mask(irq);
        rt_pic_detach_irq(irq, dev);
        return -RT_ENOMEM;
    }
#ifdef RT_USING_SMP
    rt_thread_control(tid, RT_THREAD_CTRL_BIND_CPU, (void *)0);
#endif
    rt_thread_startup(tid);
    return RT_EOK;
}

static const struct rt_ofw_node_id beau_agent_ofw_ids[] =
{
    { .compatible = "beau,pm" },
    { /* sentinel */ }
};

static struct rt_platform_driver beau_agent_driver =
{
    .name = "beau-str",
    .ids = beau_agent_ofw_ids,
    .probe = beau_agent_probe,
};
RT_PLATFORM_DRIVER_EXPORT(beau_agent_driver);

static int beau_str(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !rt_strcmp(argv[1], "status")))
    {
        rt_kprintf("beau_str:ready:%c vm:%u last-epoch:%lu pending:%lu ap:%u\n",
                   beau_ready ? 'Y' : 'N', beau_vmid,
                   (unsigned long)beau_last_epoch,
                   (unsigned long)beau_pending_epoch,
                   (unsigned int)beau_ap_state);
        return 0;
    }
    if (argc == 2 && !rt_strcmp(argv[1], "suspend"))
    {
        if (__atomic_load_n(&beau_pending_epoch, __ATOMIC_ACQUIRE) == 0)
        {
            rt_kprintf("beau_str:suspend denied:no-prepare\n");
            return -RT_EPERM;
        }
        rt_kprintf("beau_str:suspend managed:epoch:%lu\n",
                   (unsigned long)beau_pending_epoch);
        return 0;
    }

    rt_kprintf("Usage: beau_str [status|suspend]\n");
    return -RT_EINVAL;
}
MSH_CMD_EXPORT(beau_str, show BEAU coordinated STR state);
