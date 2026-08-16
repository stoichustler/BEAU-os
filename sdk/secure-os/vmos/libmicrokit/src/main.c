/*
 * Copyright 2021, Breakaway Consulting Pty. Ltd.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sel4/sel4.h>

#include <microkit.h>

#define INPUT_CAP 1
#define REPLY_CAP 4

#define PD_MASK 0xff
#define CHANNEL_MASK 0x3f

#define BADGE_FAULT_BIT 62
#define BADGE_ENDPOINT_BIT 63

/* All globals are prefixed with microkit_* to avoid clashes with user defined globals. */

bool microkit_passive;
char microkit_name[MICROKIT_PD_NAME_LENGTH];
/* We use seL4 typedefs as this variable is exposed to the libmicrokit header
 * and we do not want to rely on compiler built-in defines. */
seL4_Bool microkit_have_signal = seL4_False;
seL4_CPtr microkit_signal_cap;
seL4_MessageInfo_t microkit_signal_msg;

seL4_Word microkit_irqs;
seL4_Word microkit_notifications;
seL4_Word microkit_pps;
seL4_Word microkit_ioports;

#define BIT(n) (1ULL << (n))
#define MASK(n) (BIT(n) - 1ULL)

/* The tool assumes the IPC buffer in the top page of user memory */
seL4_IPCBuffer *__sel4_ipc_buffer = (seL4_IPCBuffer *)(seL4_UserVSpaceTop & ~MASK(seL4_PageBits));
_Static_assert(sizeof(seL4_IPCBuffer) <= BIT(seL4_PageBits),
               "IPC Buffer is expected to need less than one page in size");

extern const void (*const __init_array_start [])(void);
extern const void (*const __init_array_end [])(void);

__attribute__((weak)) microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    microkit_dbg_puts(microkit_name);
    microkit_dbg_puts(" is missing the 'protected' entry point\n");
    microkit_internal_crash(0);
    return seL4_MessageInfo_new(0, 0, 0, 0);
}

__attribute__((weak)) seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo)
{
    microkit_dbg_puts(microkit_name);
    microkit_dbg_puts(" is missing the 'fault' entry point\n");
    microkit_internal_crash(0);
    return seL4_False;
}

static void run_init_funcs(void)
{
    size_t count = __init_array_end - __init_array_start;
    for (size_t i = 0; i < count; i++) {
        __init_array_start[i]();
    }
}

static void deferred_flush(void)
{
    if (microkit_have_signal) {
        seL4_Send(microkit_signal_cap, microkit_signal_msg);
        microkit_have_signal = seL4_False;
    }
}

static void handler_loop(void)
{
    bool have_reply = false;
    seL4_MessageInfo_t reply_tag = seL4_MessageInfo_new(0, 0, 0, 0);

    /**
     * Because of https://github.com/seL4/seL4/issues/1536
     * let's acknowledge all the IRQs after we've started.
     */
    {
        seL4_Word irqs_to_ack = microkit_irqs;
        unsigned int idx = 0;
        do {
            if (irqs_to_ack & 1) {
                microkit_irq_ack(idx);
            }

            irqs_to_ack >>= 1;
            idx++;
        } while (irqs_to_ack != 0);
    }

    for (;;) {
        seL4_Word badge;
        seL4_MessageInfo_t tag;

        if (have_reply) {
            deferred_flush();
            tag = seL4_ReplyRecv(INPUT_CAP, reply_tag, &badge, REPLY_CAP);
        } else if (microkit_have_signal) {
            tag = seL4_NBSendRecv(microkit_signal_cap, microkit_signal_msg, INPUT_CAP, &badge, REPLY_CAP);
            microkit_have_signal = seL4_False;
        } else {
            tag = seL4_Recv(INPUT_CAP, &badge, REPLY_CAP);
        }

        uint64_t is_endpoint = badge >> BADGE_ENDPOINT_BIT;
        uint64_t is_fault = (badge >> BADGE_FAULT_BIT) & 1;

        have_reply = false;

        if (is_fault) {
            seL4_Bool reply_to_fault = fault(badge & PD_MASK, tag, &reply_tag);
            if (reply_to_fault) {
                have_reply = true;
            }
        } else if (is_endpoint) {
            have_reply = true;
            reply_tag = protected(badge & CHANNEL_MASK, tag);
        } else {
            unsigned int idx = 0;
            do  {
                if (badge & 1) {
                    notified(idx);
                }
                badge >>= 1;
                idx++;
            } while (badge != 0);
        }
    }
}

void main(void)
{
    run_init_funcs();
    init();

    /*
     * If we are passive, now our initialisation is complete we can
     * signal the monitor to unbind our scheduling context and bind
     * it to our notification object.
     * We delay this signal so we are ready waiting on a recv() syscall
     */
    if (microkit_passive) {
        microkit_have_signal = seL4_True;
        microkit_signal_msg = seL4_MessageInfo_new(0, 0, 0, 0);
        microkit_signal_cap = MONITOR_EP;
    }

    handler_loop();
}
