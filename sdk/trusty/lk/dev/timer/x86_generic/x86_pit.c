/*
 * Copyright (c) 2009 Corey Tabaka
 * Copyright (c) 2012-2019 LK Trusty Authors. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <arch/x86.h>
#include <bits.h>
#include <debug.h>
#include <dev/interrupt/x86_interrupts.h>
#include <dev/timer/x86_pit.h>
#include <err.h>
#include <lib/fixed_point.h>
#include <lk/trace.h>
#include <platform/interrupts.h>
#include <platform/timer.h>

#define LOCAL_TRACE 0

static platform_timer_callback t_callback;

static struct fp_32_64 ms_per_tsc;
static struct fp_32_64 ns_per_tsc;
static struct fp_32_64 pit_ticks_per_ns;

/* The oscillator used by the PIT chip runs at 1.193182MHz */
#define INTERNAL_FREQ   1193182UL

/* Mode/Command register */
#define I8253_CONTROL_REG   0x43
/* Channel 0 data port */
#define I8253_DATA_REG      0x40

static uint64_t lk_time_ns_to_pit_ticks(lk_time_ns_t lk_time_ns)
{
    return u64_mul_u64_fp32_64(lk_time_ns, pit_ticks_per_ns);
}

static lk_time_t tsc_cnt_to_lk_time(uint64_t tsc_cnt)
{
    return u32_mul_u64_fp32_64(tsc_cnt, ms_per_tsc);
}

static lk_time_ns_t tsc_cnt_to_lk_time_ns(uint64_t tsc_cnt)
{
    return u64_mul_u64_fp32_64(tsc_cnt, ns_per_tsc);
}

static inline void serializing_instruction(void)
{
    uint32_t unused = 0;

    cpuid(unused, &unused, &unused, &unused, &unused);
}

static bool is_constant_tsc_avail(void)
{
    uint32_t version_info;
    uint32_t unused;
    uint8_t  family;
    uint8_t  model;

    cpuid(X86_CPUID_VERSION_INFO, &version_info, &unused, &unused, &unused);
    model  = (version_info >> 4) & 0x0F;
    family = (version_info >> 8) & 0x0F;

    /*
     * According to description in IDSM vol3 chapter 17.17, the time stamp
     * counter of following processors increments at a constant rate.
     * TSC would be stopped at deep ACPI C-states.
     */
    return !!(((family == 0x0F) && (model > 0x03)) ||
            ((family == 0x06) && (model == 0x0E)) ||
            ((family == 0x06) && (model == 0x0F)) ||
            ((family == 0x06) && (model == 0x17)) ||
            ((family == 0x06) && (model == 0x1C)));
}

static bool is_invariant_tsc_avail(void)
{
    uint32_t invariant_tsc;
    uint32_t unused;

    cpuid(0x80000007, &unused, &unused, &unused, &invariant_tsc);

    /*
     * According to description in IDSM vol3 chapter 17.17, the invariant
     * TSC will run at a constant rate in all ACPI P-, C- and T-states.
     */
    return !!BIT(invariant_tsc, 8);
}

lk_time_ns_t current_time_ns(void)
{
    return tsc_cnt_to_lk_time_ns(__rdtsc());
}

lk_time_t current_time(void)
{
    return tsc_cnt_to_lk_time(__rdtsc());
}

static enum handler_return x86_pit_timer_interrupt_handler(void* arg)
{
    if (t_callback) {
        return t_callback(arg, current_time_ns());
    } else {
        return INT_NO_RESCHEDULE;
    }
}

static void x86_pit_init_conversion_factors(void)
{
    uint64_t begin, end;
    uint8_t status = 0;
    uint16_t ticks_per_ms = INTERNAL_FREQ / 1000;

    fp_32_64_div_32_32(&pit_ticks_per_ns, INTERNAL_FREQ, 1000000000);

    /* Set PIT mode to count down and set OUT pin high when count reaches 0 */
    outp(I8253_CONTROL_REG, 0x30);

    /*
     * According to ISDM vol3 chapter 17.17:
     *  The RDTSC instruction is not serializing or ordered with other
     *  instructions. Subsequent instructions may begin execution before
     *  the RDTSC instruction operation is performed.
     *
     * Insert serializing instruction before and after RDTSC to make
     * calibration more accurate.
     */
    serializing_instruction();
    begin = __rdtsc();
    serializing_instruction();

    /* Write LSB in counter 0 */
    outp(I8253_DATA_REG, ticks_per_ms & 0xff);
    /* Write MSB in counter 0 */
    outp(I8253_DATA_REG, ticks_per_ms >> 8);

    do {
        /* Read-back command, count MSB, counter 0 */
        outp(I8253_CONTROL_REG, 0xE2);
        /* Wait till OUT pin goes high and null count goes low */
        status = inp(I8253_DATA_REG);
    } while ((status & 0xC0) != 0x80);

    /* Make sure all instructions above executed and submitted */
    serializing_instruction();
    end = __rdtsc();
    serializing_instruction();

    /* Enable interrupt mode that will stop the decreasing counter of the PIT */
    outp(I8253_CONTROL_REG, 0x30);

    fp_32_64_div_32_32(&ms_per_tsc, 1, (end - begin));
    fp_32_64_div_32_32(&ns_per_tsc, 1000 * 1000, (end - begin));
}

void x86_init_pit(void)
{
    if(!is_invariant_tsc_avail() && !is_constant_tsc_avail()) {
        dprintf(INFO, "CAUTION: Current time is inaccurate!\n");
    }

    x86_pit_init_conversion_factors();

    register_int_handler(INT_PIT, &x86_pit_timer_interrupt_handler, NULL);
    unmask_interrupt(INT_PIT);
}

status_t platform_set_oneshot_timer(platform_timer_callback callback,
                                    lk_time_ns_t time_ns_abs)
{
    uint64_t pit_ticks;
    uint32_t pit_ticks_clamped;
    uint16_t divisor;
    lk_time_ns_t time_ns_rel = time_ns_abs - current_time_ns();

    t_callback = callback;

    pit_ticks = lk_time_ns_to_pit_ticks(time_ns_rel);
    /* Clamp ticks to 1 - 0x10000. 0 in the 16 bit counter means 0x10000 */
    pit_ticks_clamped = MAX(1, MIN(pit_ticks, UINT16_MAX + 1));
    divisor = pit_ticks_clamped & UINT16_MAX;

    LTRACEF("time_ns_abs %" PRIu64 " -> time_ns_rel %" PRIu64
            " -> pit_ticks %" PRIu64 " -> pit_ticks_clamped %" PRIu32
            " -> pit_ticks %" PRIu16 "\n",
            time_ns_abs, time_ns_rel, pit_ticks, pit_ticks_clamped, divisor);

    /*
     * Program PIT in the software strobe configuration, to send one pulse
     * after the count reach 0
     */
    outp(I8253_CONTROL_REG, 0x38);
    outp(I8253_DATA_REG, divisor & 0xff);
    outp(I8253_DATA_REG, divisor >> 8);

    return NO_ERROR;
}

void platform_stop_timer(void)
{
    LTRACE;
    /* Enable interrupt mode that will stop the decreasing counter of the PIT */
    outp(I8253_CONTROL_REG, 0x30);
}
