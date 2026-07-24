/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <bsp/pci.h>
#include <serial.h>
#include <shell.h>
#include <timer.h>
#include <ticks.h>
#include <bsp/vuart.h>
#include <logmsg.h>
#include <acrn_hv_defs.h>
#include <vm.h>
#include <console.h>
#include <boot.h>
#include <debug/ramlog.h>
#include <dbg_cmd.h>
#include <rtl.h>
#include <sprintf.h>
#include <spinlock.h>
#include <asm/guest/vm.h>
#include "shell_priv.h"

/* [20260712] console ownership and replay model
 *
 * The host has one physical serial stream, but BEAU must multiplex three
 * roles on top of it: the hypervisor shell, the selected VM console, and
 * background VM output. console_vmid is the ownership switch:
 *
 *   ACRN_INVALID_VMID  -> BEAU shell owns host input/output
 *   VM id             -> that VM owns host input; its ramlog cursor drains to host
 *
 *   physical UART
 *      |
 *      +-- console_vmid == ACRN_INVALID_VMID
 *      |      -> shell input/output
 *      |
 *      +-- console_vmid == VMID
 *             -> host input backlog
 *             -> selected VM vUART RX FIFO
 *
 *   guest output
 *      |
 *      v
 *   vPL011 / virtio-console backend
 *      |
 *      v
 *   per-VM ramlog
 *      |
 *      +-- bound by vsh  -> drain new output to host UART with VM prefix
 *      +-- not selected   -> retained history remains available to ramlog
 *
 * RTOS VMs use vPL011/vUART, Linux VMs use virtio-console, and both converge
 * at ramlog before vsh prints new data to the host UART.
 *
 * Key rule:
 *   - only one endpoint owns host input at a time;
 *   - ramlog, rather than vcon, owns concurrent output retention and overflow;
 *   - vsh is a bounded consumer and never controls guest TX progress.
 */
struct hv_timer console_timer;
static bool console_pm_suspended;

#ifndef CONFIG_CONSOLE_KICK_TIMER_TIMEOUT
#define CONFIG_CONSOLE_KICK_TIMER_TIMEOUT 2UL
#endif
#define CONSOLE_KICK_TIMER_TIMEOUT CONFIG_CONSOLE_KICK_TIMER_TIMEOUT
#ifndef CONFIG_VM_CONSOLE_RINGBUF_VM_NUM
#define CONFIG_VM_CONSOLE_RINGBUF_VM_NUM CONFIG_MAX_VM_NUM
#endif
#ifndef CONFIG_VM_CONSOLE_DRAIN_BUDGET
#define CONFIG_VM_CONSOLE_DRAIN_BUDGET 512U
#endif
#define VM_CONSOLE_DRAIN_BUDGET CONFIG_VM_CONSOLE_DRAIN_BUDGET
#ifndef CONFIG_VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET
#define CONFIG_VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET 512U
#endif
#define VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET CONFIG_VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET
#ifndef CONFIG_VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET
#define CONFIG_VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET 256U
#endif
#define VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET CONFIG_VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET
#ifndef CONFIG_VM_CONSOLE_RX_BUDGET
#define CONFIG_VM_CONSOLE_RX_BUDGET 32U
#endif
#define VM_CONSOLE_RX_BUDGET CONFIG_VM_CONSOLE_RX_BUDGET
#ifndef CONFIG_VM_CONSOLE_RX_LOW_WATERMARK
#define CONFIG_VM_CONSOLE_RX_LOW_WATERMARK 1U
#endif
#define VM_CONSOLE_RX_LOW_WATERMARK CONFIG_VM_CONSOLE_RX_LOW_WATERMARK
#ifndef CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE
#define CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE 64U
#endif
#if (CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE < 2U)
#error "CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE must be at least 2 bytes"
#endif
#if ((CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE & (CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE - 1U)) != 0U)
#error "CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE must be a power of two"
#endif
#define VM_CONSOLE_INPUT_BACKLOG_MASK (CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE - 1U)
#define VM_CONSOLE_INPUT_BACKLOG_CAPACITY (CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE - 1U)
#ifndef CONFIG_VM_CONSOLE_INPUT_COLLECT_BUDGET
#define CONFIG_VM_CONSOLE_INPUT_COLLECT_BUDGET 64U
#endif
#define VM_CONSOLE_INPUT_COLLECT_BUDGET CONFIG_VM_CONSOLE_INPUT_COLLECT_BUDGET
#define VM_CONSOLE_DRAIN_BUF_SIZE 1024U
#define VM_CONSOLE_PREFIX_MAX_SIZE 32U
#define VM_CONSOLE_CPR_QUERY_LEN 4U
#define VM_CONSOLE_EXCEPTION_RINGBUF_SIZE 4096U
#define VM_CONSOLE_EXCEPTION_RINGBUF_MASK (VM_CONSOLE_EXCEPTION_RINGBUF_SIZE - 1U)
#define VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY (VM_CONSOLE_EXCEPTION_RINGBUF_SIZE - 1U)
/* Switching key combinations for shell and uart console */
#define GUEST_CONSOLE_TO_HV_SWITCH_KEY  0x4U /* Ctrl-D */
#define VM_CONSOLE_ASCII_BS             '\b'
#define VM_CONSOLE_ASCII_DEL            0x7fU
uint16_t console_vmid = CONFIG_CONSOLE_DEFAULT_VM;

uint16_t console_loglevel = CONFIG_CONSOLE_LOGLEVEL_DEFAULT;
static spinlock_t console_log_lock;

/* [20260721] RAMLOG-backed vsh presentation
 *
 * guest TX -> ramlog durable ring -> vsh cursor -> physical UART
 *
 * Key rule:
 *   - ramlog is the only VM output store;
 *   - vsh owns only a consumer cursor and terminal presentation state;
 *   - a slow UART advances only the presentation cursor and never blocks a
 *     guest TX producer.
 */
struct vm_console_presentation {
	spinlock_t lock;
	uint64_t cursor;
	uint64_t drained_bytes;
	uint64_t skipped_bytes;
	uint64_t bind_epoch;
	bool draining;
	bool line_start;
	bool last_cr;
	bool vuart_bound;
	uint8_t terminal_query_len;
	char terminal_query_buf[VM_CONSOLE_CPR_QUERY_LEN];
};

struct vm_exception_ringbuf {
	spinlock_t lock;
	uint32_t cons;
	uint32_t prod;
	uint32_t high_water;
	uint64_t input_bytes;
	uint64_t stored_bytes;
	uint64_t drained_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t last_overflow_tsc;
	char buf[VM_CONSOLE_EXCEPTION_RINGBUF_SIZE];
};

static struct vm_console_presentation vm_console_presentations[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
static struct vm_exception_ringbuf vm_exception_ringbufs[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
/*
 * Host-to-VM input is staged separately from the guest RX FIFO. The console
 * timer first collects host serial bytes into this small backlog, then feeds
 * at most VM_CONSOLE_RX_BUDGET bytes into the selected VM while the backend RX
 * FIFO is below VM_CONSOLE_RX_LOW_WATERMARK. This keeps held keys from turning
 * the timer callback into an unbounded UART-injection loop.
 */
static char vm_console_input_backlog[CONFIG_VM_CONSOLE_INPUT_BACKLOG_SIZE];
static uint32_t vm_console_input_cons;
static uint32_t vm_console_input_prod;
static uint32_t vm_console_input_guest_budget;
static uint16_t vm_console_input_vmid = ACRN_INVALID_VMID;
static bool vm_console_input_last_enter;
static bool vm_console_input_priority;
static char vm_console_drain_buf[VM_CONSOLE_DRAIN_BUF_SIZE];
static char vm_console_output_buf[VM_CONSOLE_DRAIN_BUF_SIZE + 128U];
static const char vm_console_cpr_query[] = "\033[6n";
static const char vm_console_cpr_reply[] = "\033[1;1R";

static uint32_t console_ring_queued(uint32_t prod, uint32_t cons, uint32_t capacity);
static void console_vm_ring_drain_internal(uint16_t vmid);

__attribute__((weak)) void console_vm_tx_space_changed(__unused uint16_t vmid)
{
}

void console_init(void)
{
	/*
	 * Enable UART as early as possible.
	 * Then we could use printf for debugging on early boot stage.
	 */
	serial_init(true);

	spinlock_init(&console_log_lock);
	for (uint16_t i = 0U; i < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM; i++) {
		spinlock_init(&vm_console_presentations[i].lock);
		vm_console_presentations[i].line_start = true;
		spinlock_init(&vm_exception_ringbufs[i].lock);
	}
}

void console_putc(const char *ch)
{
	(void)serial_puts(ch, 1U);
}

bool console_is_hv(void)
{
	return (console_vmid == ACRN_INVALID_VMID);
}

bool console_is_vm_active(uint16_t vmid)
{
	return console_vmid == vmid;
}

bool console_vm_tx_put(uint16_t vmid, char ch)
{
	return console_vm_tx_write(vmid, &ch, 1U);
}

bool console_vm_tx_write(uint16_t vmid, const char *buffer, uint32_t length)
{
	if ((buffer == NULL) || (length == 0U) ||
		(vmid >= CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		return false;
	}

	/* [20260721] VM console persistent ownership
	 *
	 * guest transport bytes
	 *     |
	 *     v
	 * ramlog append and publish
	 *
	 * Key rule:
	 *   - ramlog is the only guest TX store before vsh presentation observes it;
	 *   - the guest transport never waits for serial output;
	 *   - a missing ramlog slot rejects capture instead of silently creating a
	 *     second volatile log store.
	 */
	return ramlog_append_vm_console(vmid, buffer, length);
}

bool console_vm_tx_can_accept(uint16_t vmid)
{
	return vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM;
}

size_t console_write(const char *s, size_t len)
{
	return  serial_puts(s, len);
}

char console_getc(void)
{
	return serial_getc();
}

void console_vm_ring_drain(uint16_t vmid)
{
	console_vm_ring_drain_internal(vmid);
}

bool console_vm_vuart_bind(uint16_t vmid)
{
	struct vm_console_presentation *presentation;
	struct ramlog_window window;
	uint64_t rflags;
	bool valid = false;

	if ((vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) && ramlog_get_window(vmid, &window)) {
		presentation = &vm_console_presentations[vmid];
		spinlock_irqsave_obtain(&presentation->lock, &rflags);
		presentation->cursor = window.next;
		presentation->bind_epoch++;
		presentation->vuart_bound = true;
		spinlock_irqrestore_release(&presentation->lock, rflags);
		valid = true;
	}

	return valid;
}

void console_vm_vuart_unbind(uint16_t vmid)
{
	struct vm_console_presentation *presentation;
	uint64_t rflags;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		presentation = &vm_console_presentations[vmid];
		spinlock_irqsave_obtain(&presentation->lock, &rflags);
		presentation->vuart_bound = false;
		presentation->bind_epoch++;
		spinlock_irqrestore_release(&presentation->lock, rflags);
	}
}

static uint32_t console_ring_queued(uint32_t prod, uint32_t cons, uint32_t capacity)
{
	uint32_t queued = prod - cons;

	return (queued > capacity) ? capacity : queued;
}

static void console_vm_input_reset(uint16_t vmid)
{
	vm_console_input_vmid = vmid;
	vm_console_input_cons = 0U;
	vm_console_input_prod = 0U;
	vm_console_input_guest_budget = 0U;
	vm_console_input_last_enter = false;
	vm_console_input_priority = false;
}

static void console_vm_input_sync(uint16_t vmid)
{
	if (vm_console_input_vmid != vmid) {
		console_vm_input_reset(vmid);
	}
}

static bool console_vm_input_empty(void)
{
	return vm_console_input_prod == vm_console_input_cons;
}

static bool console_vm_input_has_non_enter(void)
{
	uint32_t queued = vm_console_input_prod - vm_console_input_cons;
	bool has_non_enter = false;

	if (queued > VM_CONSOLE_INPUT_BACKLOG_CAPACITY) {
		queued = VM_CONSOLE_INPUT_BACKLOG_CAPACITY;
	}
	for (uint32_t idx = 0U; idx < queued; idx++) {
		if (vm_console_input_backlog[(vm_console_input_cons + idx) &
			VM_CONSOLE_INPUT_BACKLOG_MASK] != '\r') {
			has_non_enter = true;
			break;
		}
	}

	return has_non_enter;
}

static bool console_vm_input_pending_for_drain(void)
{
	return vm_console_input_priority || !console_vm_input_empty();
}

bool console_vm_input_get_stats(struct console_vm_input_stats *stats)
{
	bool valid = stats != NULL;

	if (valid) {
		/*
		 * The console input backlog is producer/consumer state owned by the
		 * host console timer. A best-effort snapshot is enough for live shell
		 * diagnostics and avoids perturbing the timing-sensitive input path.
		 */
		stats->selected_vmid = console_vmid;
		stats->input_vmid = vm_console_input_vmid;
		stats->queued = console_ring_queued(vm_console_input_prod,
			vm_console_input_cons, VM_CONSOLE_INPUT_BACKLOG_CAPACITY);
		stats->capacity = VM_CONSOLE_INPUT_BACKLOG_CAPACITY;
		stats->guest_budget = vm_console_input_guest_budget;
		stats->last_enter = vm_console_input_last_enter;
		stats->has_non_enter = console_vm_input_has_non_enter();
	}

	return valid;
}

static bool console_vm_input_put(char ch)
{
	bool stored = false;
	uint32_t queued = vm_console_input_prod - vm_console_input_cons;

	/*
	 * Held Enter keys should behave like "there is a blank line pending", not
	 * like an unbounded FIFO that can hide the next real command behind dozens
	 * of empty submissions.
	 */
	if ((ch == '\r') && vm_console_input_last_enter) {
		return true;
	}

	if (queued >= VM_CONSOLE_INPUT_BACKLOG_CAPACITY) {
		/*
		 * Prefer the newest host input over stale backlog. This keeps a command
		 * typed after a key-repeat burst from being dropped just because old
		 * blank lines filled the staging queue.
		 */
		vm_console_input_cons++;
		queued--;
	}
	if (queued < VM_CONSOLE_INPUT_BACKLOG_CAPACITY) {
		vm_console_input_backlog[vm_console_input_prod & VM_CONSOLE_INPUT_BACKLOG_MASK] = ch;
		vm_console_input_prod++;
		vm_console_input_last_enter = (ch == '\r');
		stored = true;
	}

	return stored;
}

static char console_vm_input_peek(void)
{
	return vm_console_input_backlog[vm_console_input_cons & VM_CONSOLE_INPUT_BACKLOG_MASK];
}

static void console_vm_input_drop_one(void)
{
	char ch = console_vm_input_peek();

	vm_console_input_cons++;
	if (ch != '\r') {
		vm_console_input_last_enter = false;
	}
}

static bool console_vm_input_collect(uint16_t target_vmid)
{
	uint32_t budget = VM_CONSOLE_INPUT_COLLECT_BUDGET;
	char ch = -1;
	bool collected = false;
	char temp_str[TEMP_STR_SIZE];

	console_vm_input_sync(target_vmid);
	/*
	 * Host serial input is polled in bounded chunks. Ctrl-D is consumed by
	 * the multiplexer itself, not forwarded to the guest, because it changes
	 * ownership back to the BEAU shell.
	 */
	while (budget > 0U) {
		ch = serial_getc();
		if (ch == -1) {
			if (console_vm_input_empty()) {
				vm_console_input_last_enter = false;
			}
			break;
		}
		budget--;
		if (ch == GUEST_CONSOLE_TO_HV_SWITCH_KEY) {
			console_vm_vuart_unbind(target_vmid);
			console_vmid = ACRN_INVALID_VMID;
			console_vm_input_reset(ACRN_INVALID_VMID);
			snprintf(temp_str, TEMP_STR_SIZE,
				"\r\n%s───────────── [switch to BEAU console] ─────────────%s\r\n",
				SHELL_COLOR_YELLOW, SHELL_COLOR_RESET);
			shell_puts(temp_str);
			break;
		}
		/* Match guest terminal line disciplines that use DEL for erase. */
		if (ch == VM_CONSOLE_ASCII_BS) {
			ch = VM_CONSOLE_ASCII_DEL;
		}
		(void)console_vm_input_put(ch);
		collected = true;
	}

	return collected;
}

static bool console_vm_input_push_to_guest(struct acrn_vuart *vu)
{
	uint16_t target_vmid;
	char ch;
	bool pushed = false;

	if ((vu != NULL) && (vu->vm != NULL)) {
		target_vmid = vu->vm->vm_id;
		if ((console_vmid == target_vmid) && (vm_console_input_guest_budget > 0U)) {
			console_vm_input_sync(target_vmid);
			/*
			 * Keep the guest RX FIFO shallow. vPL011/vUART will ask
			 * for another refill when the guest consumes DR, so input
			 * stays ordered without flooding RX IRQ handling.
			 */
			if (!console_vm_input_empty() &&
				(vuart_rx_numchars(vu) < VM_CONSOLE_RX_LOW_WATERMARK)) {
				ch = console_vm_input_peek();
				if (vuart_try_putchar(vu, ch)) {
					console_vm_input_drop_one();
					vm_console_input_guest_budget--;
					pushed = true;
				}
			}
		}
	}

	return pushed;
}

bool console_vm_rx_refill(struct acrn_vuart *vu)
{
	return console_vm_input_push_to_guest(vu);
}

static uint32_t console_vm_presentation_drain_budget(bool input_pending)
{
	uint32_t budget = VM_CONSOLE_DRAIN_BUDGET;

	if (input_pending && (budget > VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET)) {
		budget = VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET;
	} else if (budget > VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET) {
		budget = VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET;
	}

	return budget;
}

bool console_vm_ring_get_stats(uint16_t vmid, struct console_vm_ring_stats *stats)
{
	struct vm_console_presentation *presentation;
	struct ramlog_window window;
	struct ramlog_stats ramlog_stats;
	uint64_t rflags;
	uint64_t queued = 0UL;
	uint64_t cursor;
	bool valid = false;

	if ((stats != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) &&
		ramlog_get_window(vmid, &window) && ramlog_get_stats(vmid, &ramlog_stats)) {
		presentation = &vm_console_presentations[vmid];
		spinlock_irqsave_obtain(&presentation->lock, &rflags);
		cursor = presentation->cursor;
		stats->vmid = vmid;
		stats->size = 0U;
		stats->capacity = ramlog_stats.capacity;
		stats->drain_budget = presentation->vuart_bound ?
			console_vm_presentation_drain_budget((console_vmid == vmid) &&
			console_vm_input_pending_for_drain()) : 0U;
		stats->high_water = 0U;
		stats->input_bytes = 0UL;
		stats->stored_bytes = 0UL;
		stats->drained_bytes = presentation->drained_bytes;
		stats->dropped_bytes = presentation->skipped_bytes;
		stats->overflow_events = 0UL;
		stats->last_overflow_tsc = 0UL;
		stats->draining = presentation->draining;
		stats->vuart_bound = presentation->vuart_bound;
		spinlock_irqrestore_release(&presentation->lock, rflags);
		if (cursor < window.oldest) {
			queued = window.next - window.oldest;
		} else if (cursor < window.next) {
			queued = window.next - cursor;
		}
		stats->queued = (queued > UINT32_MAX) ? UINT32_MAX : (uint32_t)queued;
		stats->pending = stats->vuart_bound && (stats->queued != 0U);
		stats->tx_high_water = 0U;
		stats->tx_low_water = 0U;
		stats->tx_blocked = false;
		valid = true;
	}

	return valid;
}

void console_vm_exception_log(uint16_t vmid, const char *buf, size_t len)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;
	uint32_t prod;
	uint32_t queued;

	if ((buf != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		/*
		 * Exception logs use a separate ring from normal console TX so
		 * trap/oops evidence survives even when a guest fills its normal
		 * PL011 output ring with noisy boot or stall messages.
		 */
		for (size_t i = 0U; i < len; i++) {
			rb->input_bytes++;
			prod = rb->prod;
			rb->buf[prod & VM_CONSOLE_EXCEPTION_RINGBUF_MASK] = buf[i];
			rb->prod = prod + 1U;
			if ((rb->prod - rb->cons) > VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY) {
				rb->dropped_bytes += (rb->prod - rb->cons) -
					VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY;
				rb->overflow_events++;
				rb->last_overflow_tsc = cpu_ticks();
				rb->cons = rb->prod - VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY;
			}
			queued = rb->prod - rb->cons;
			if (queued > rb->high_water) {
				rb->high_water = queued;
			}
			rb->stored_bytes++;
		}
		spinlock_irqrestore_release(&rb->lock, rflags);
	}
}

uint32_t console_vm_exception_count(void)
{
	return CONFIG_VM_CONSOLE_RINGBUF_VM_NUM;
}

bool console_vm_exception_get_stats(uint16_t vmid, struct console_vm_exception_stats *stats)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;
	bool valid = false;

	if ((stats != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		stats->vmid = vmid;
		stats->size = VM_CONSOLE_EXCEPTION_RINGBUF_SIZE;
		stats->capacity = VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY;
		stats->queued = console_ring_queued(rb->prod, rb->cons,
			VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY);
		stats->high_water = rb->high_water;
		stats->input_bytes = rb->input_bytes;
		stats->stored_bytes = rb->stored_bytes;
		stats->dropped_bytes = rb->dropped_bytes;
		stats->overflow_events = rb->overflow_events;
		stats->last_overflow_tsc = rb->last_overflow_tsc;
		stats->pending = (rb->prod != rb->cons);
		spinlock_irqrestore_release(&rb->lock, rflags);
		valid = true;
	}

	return valid;
}

uint32_t console_vm_exception_copy(uint16_t vmid, uint32_t offset, char *buf, uint32_t len)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;
	uint32_t queued;
	uint32_t count = 0U;
	uint32_t cons;
	uint32_t first;

	if ((buf != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		queued = console_ring_queued(rb->prod, rb->cons, VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY);
		if (offset < queued) {
			count = queued - offset;
			if (count > len) {
				count = len;
			}
			cons = rb->cons + offset;
			first = VM_CONSOLE_EXCEPTION_RINGBUF_SIZE -
				(cons & VM_CONSOLE_EXCEPTION_RINGBUF_MASK);
			if (first > count) {
				first = count;
			}
			(void)memcpy(buf, &rb->buf[cons & VM_CONSOLE_EXCEPTION_RINGBUF_MASK], first);
			if (first < count) {
				(void)memcpy(&buf[first], rb->buf, count - first);
			}
		}
		spinlock_irqrestore_release(&rb->lock, rflags);
	}

	return count;
}

void console_vm_exception_clear(uint16_t vmid)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		rb->cons = 0U;
		rb->prod = 0U;
		rb->high_water = 0U;
		rb->input_bytes = 0UL;
		rb->stored_bytes = 0UL;
		rb->drained_bytes = 0UL;
		rb->dropped_bytes = 0UL;
		rb->overflow_events = 0UL;
		rb->last_overflow_tsc = 0UL;
		spinlock_irqrestore_release(&rb->lock, rflags);
	}
}

void console_vm_exception_clear_all(void)
{
	for (uint16_t vmid = 0U; vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM; vmid++) {
		console_vm_exception_clear(vmid);
	}
}

/*
 * @post return != NULL
 */
struct acrn_vuart *vm_console_vuart(struct acrn_vm *vm)
{
	return &vm->vuart[0];
}

static void vuart_console_rx_chars(struct acrn_vuart *vu)
{
	uint16_t target_vmid = console_vmid;
	bool recv = false;
	bool collected;

	collected = console_vm_input_collect(target_vmid);
	vm_console_input_guest_budget = VM_CONSOLE_RX_BUDGET;
	while (console_vm_input_push_to_guest(vu)) {
		recv = true;
	}
	vm_console_input_priority = collected || !console_vm_input_empty();
	if (!recv && (vu != NULL) && (console_vmid == target_vmid) &&
		vuart_rx_pending(vu) && console_vm_input_has_non_enter()) {
		/*
		 * A stale blank Enter can occupy the guest RX FIFO while the real
		 * command waits in the host backlog. Re-kick the already asserted
		 * PL011 line so Linux drains that byte and refill can expose the
		 * pending command, without replaying pure Enter key-repeat bursts.
		 */
		recv = true;
		vm_console_input_priority = true;
	}

	if (recv && (vu != NULL) && (console_vmid == target_vmid)) {
		vuart_notify_rx(vu);
	}
}

static struct acrn_vuart *vuart_console_active(void)
{
	struct acrn_vm *vm = NULL;
	struct acrn_vuart *vu = NULL;

	if (console_vmid < CONFIG_MAX_VM_NUM) {
		vm = get_vm_from_vmid(console_vmid);
		if (!is_paused_vm(vm) && !is_poweroff_vm(vm)) {
			vu = vm_console_vuart(vm);
		} else {
			/* Console vm is invalid, switch back to HV-Shell */
			console_vm_vuart_unbind(console_vmid);
			console_vmid = ACRN_INVALID_VMID;
		}
	}

	return ((vu != NULL) && vu->active) ? vu : NULL;
}

static bool console_vm_presentation_drain_begin(uint16_t vmid, uint64_t *cursor,
	uint64_t *epoch)
{
	struct vm_console_presentation *presentation;
	uint64_t rflags;
	bool draining;
	bool bound;

	presentation = &vm_console_presentations[vmid];
	spinlock_irqsave_obtain(&presentation->lock, &rflags);
	draining = presentation->draining;
	bound = presentation->vuart_bound;
	if (!draining && bound) {
		presentation->draining = true;
		*cursor = presentation->cursor;
		*epoch = presentation->bind_epoch;
	}
	spinlock_irqrestore_release(&presentation->lock, rflags);

	return !draining && bound;
}

static void console_vm_presentation_drain_end(uint16_t vmid, uint64_t cursor,
	uint64_t epoch, uint64_t drained, uint64_t skipped)
{
	struct vm_console_presentation *presentation;
	uint64_t rflags;

	presentation = &vm_console_presentations[vmid];
	spinlock_irqsave_obtain(&presentation->lock, &rflags);
	if ((presentation->bind_epoch == epoch) && presentation->vuart_bound) {
		presentation->cursor = cursor;
		presentation->drained_bytes += drained;
		presentation->skipped_bytes += skipped;
	}
	presentation->draining = false;
	spinlock_irqrestore_release(&presentation->lock, rflags);
}

static void console_vm_prefixed_write_flush(char *out, uint32_t *out_len)
{
	if (*out_len > 0U) {
		(void)console_write(out, *out_len);
		*out_len = 0U;
	}
}

static void console_vm_prefixed_write_byte(char *out, uint32_t *out_len, char ch)
{
	if (*out_len == ARRAY_SIZE(vm_console_output_buf)) {
		console_vm_prefixed_write_flush(out, out_len);
	}
	out[*out_len] = ch;
	(*out_len)++;
}

static void console_vm_prefixed_write_bytes(char *out, uint32_t *out_len,
	const char *buf, uint32_t len)
{
	for (uint32_t idx = 0U; idx < len; idx++) {
		console_vm_prefixed_write_byte(out, out_len, buf[idx]);
	}
}

static void console_vm_ring_write_visible_byte(const char *prefix, uint32_t prefix_len,
	struct vm_console_presentation *presentation, char *out, uint32_t *out_len, char ch)
{
	if (presentation->line_start) {
		if (!presentation->last_cr || (ch != '\n')) {
			console_vm_prefixed_write_bytes(out, out_len, prefix, prefix_len);
			presentation->line_start = false;
		}
	}

	console_vm_prefixed_write_byte(out, out_len, ch);
	if (ch == '\r') {
		presentation->line_start = true;
		presentation->last_cr = true;
	} else if (ch == '\n') {
		presentation->line_start = true;
		presentation->last_cr = false;
	} else {
		presentation->last_cr = false;
	}
}

static void console_vm_inject_cpr_reply(uint16_t vmid)
{
	struct acrn_vm *vm;
	struct acrn_vuart *vu;
	bool pushed = false;

	if (vmid < CONFIG_MAX_VM_NUM) {
		vm = get_vm_from_vmid(vmid);
		if ((vm != NULL) && !is_paused_vm(vm) && !is_poweroff_vm(vm)) {
			vu = vm_console_vuart(vm);
			for (uint32_t idx = 0U; idx < (ARRAY_SIZE(vm_console_cpr_reply) - 1U); idx++) {
				if (!vuart_try_putchar(vu, vm_console_cpr_reply[idx])) {
					break;
				}
				pushed = true;
			}
			if (pushed) {
				vuart_notify_rx(vu);
			}
		}
	}
}

static void console_vm_flush_terminal_query(const char *prefix, uint32_t prefix_len,
	struct vm_console_presentation *presentation, char *out, uint32_t *out_len)
{
	for (uint32_t idx = 0U; idx < presentation->terminal_query_len; idx++) {
		console_vm_ring_write_visible_byte(prefix, prefix_len, presentation, out, out_len,
			presentation->terminal_query_buf[idx]);
	}
	presentation->terminal_query_len = 0U;
}

static bool console_vm_filter_terminal_query(uint16_t vmid, struct vm_console_presentation *presentation,
	const char *prefix, uint32_t prefix_len, char *out, uint32_t *out_len, char ch)
{
	bool consumed = false;

	if ((presentation->terminal_query_len != 0U) || (ch == vm_console_cpr_query[0U])) {
		if (presentation->terminal_query_len < VM_CONSOLE_CPR_QUERY_LEN) {
			presentation->terminal_query_buf[presentation->terminal_query_len] = ch;
			presentation->terminal_query_len++;
		}
		consumed = true;

		if (memcmp(presentation->terminal_query_buf, vm_console_cpr_query,
			presentation->terminal_query_len) == 0) {
			if (presentation->terminal_query_len == VM_CONSOLE_CPR_QUERY_LEN) {
				presentation->terminal_query_len = 0U;
				console_vm_inject_cpr_reply(vmid);
			}
		} else {
			console_vm_flush_terminal_query(prefix, prefix_len, presentation, out, out_len);
		}
	}

	return consumed;
}

static void console_vm_ring_write_prefixed(uint16_t vmid, struct vm_console_presentation *presentation,
	const char *buf, uint32_t len)
{
	char prefix[VM_CONSOLE_PREFIX_MAX_SIZE];
	uint32_t out_len = 0U;
	size_t prefix_len;

	/*
	 * vsh is a multiplexed debug console rather than a transparent terminal.
	 * Prefix each visible guest line so interleaved host/guest diagnostics can
	 * still be attributed after copying logs from a single serial stream.
	 */
	(void)snprintf(prefix, sizeof(prefix), "%s[vmid %u] %s",
		SHELL_COLOR_MAGENTA, vmid, SHELL_COLOR_RESET);
	prefix_len = strnlen_s(prefix, sizeof(prefix));

	for (uint32_t idx = 0U; idx < len; idx++) {
		char ch = buf[idx];

		/*
		 * BusyBox ash queries terminal cursor position with ESC[6n after
		 * prompt redraws. The BEAU vsh path is a console multiplexer, not a
		 * transparent terminal, so answer the query internally and hide it
		 * from the host serial stream.
		 */
		if (console_vm_filter_terminal_query(vmid, presentation, prefix,
			(uint32_t)prefix_len, vm_console_output_buf, &out_len, ch)) {
			continue;
		}

		console_vm_ring_write_visible_byte(prefix, (uint32_t)prefix_len,
			presentation, vm_console_output_buf, &out_len, ch);
	}

	console_vm_prefixed_write_flush(vm_console_output_buf, &out_len);
}

static void console_vm_ring_drain_internal(uint16_t vmid)
{
	struct vm_console_presentation *presentation;
	uint64_t cursor;
	uint64_t epoch;
	uint64_t skipped = 0UL;
	uint64_t drained = 0UL;
	uint32_t count;
	uint32_t budget;
	uint32_t chunk;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		presentation = &vm_console_presentations[vmid];
		if (console_vm_presentation_drain_begin(vmid, &cursor, &epoch)) {
			budget = console_vm_presentation_drain_budget(
				console_vm_input_pending_for_drain());
			do {
				chunk = (budget < VM_CONSOLE_DRAIN_BUF_SIZE) ? budget : VM_CONSOLE_DRAIN_BUF_SIZE;
				if (chunk == 0U) {
					break;
				}
				count = ramlog_read_vm_console(vmid, &cursor, vm_console_drain_buf,
					chunk, &skipped);
				if (count > 0U) {
					console_vm_ring_write_prefixed(vmid, presentation,
						vm_console_drain_buf, count);
					budget -= count;
					drained += count;
				}
			} while ((count == chunk) && (budget > 0U));
			console_vm_presentation_drain_end(vmid, cursor, epoch, drained, skipped);
		}
	}
}

bool console_vm_kick(void)
{
	struct acrn_vuart *vu;
	bool handled = !console_is_hv();

	if (handled) {
		/*
		 * One periodic kick performs both directions for the selected VM:
		 * host serial input is collected and injected into vUART first,
		 * then guest TX backlog is drained to host serial. If the selected
		 * vUART is no longer active, input state is reset so stale host
		 * bytes cannot leak into a later VM selection.
		 */
		console_vm_input_sync(console_vmid);
		vu = vuart_console_active();
		vuart_console_rx_chars(vu);

		if (vu != NULL) {
			console_vm_ring_drain_internal(console_vmid);
		} else {
			console_vm_input_reset(console_vmid);
		}
	}

	return handled;
}

/* [20260717] Console polling ownership
 *
 *   timer softirq
 *        |
 *        +-- selected VM -> bounded collect / RX push / TX drain
 *        |
 *        +-- BEAU + RX ready -> latch event -> wake shell thread
 *        |
 *        +-- VM -> BEAU ownership change -> latch event -> restore prompt
 *
 * Key rule:
 *   - selected-VM I/O stays on the fixed-period path so low-priority shell
 *     scheduling cannot add interactive latency;
 *   - collect and drain budgets bound the work performed in timer softirq;
 *   - RX readiness is observed without consuming DR; only the shell thread
 *     owns BEAU input editing and command dispatch.
 */
static void console_timer_callback(__unused void *data)
{
	if (console_vm_kick()) {
		if (console_is_hv()) {
			shell_kick();
		}
	} else if (serial_rx_ready()) {
		shell_kick();
	}
}

void console_setup_timer(void)
{
	uint64_t period_in_cycle, fire_tsc;

	/* Fixed-period bounded polling keeps vsh input/output latency predictable. */
	period_in_cycle = TICKS_PER_MS * CONSOLE_KICK_TIMER_TIMEOUT;
	fire_tsc = cpu_ticks() + period_in_cycle;
	initialize_timer(&console_timer,
			console_timer_callback, NULL,
			fire_tsc, period_in_cycle);

	/* Start an periodic timer */
	if (add_timer(&console_timer) != 0) {
		LOG_ERR("failed to add console kick timer");
	}
}

/* When lapic-pt is enabled for a vcpu working on the pcpu hosting
 * console timer, we utilize vm-exits to drive the console.
 *
 * Note that currently this approach will result in a laggy shell when
 * the number of VM-exits/second is low (which is mostly true when lapic-pt is
 * enabled).
 */
void console_vmexit_callback(struct acrn_vcpu *vcpu)
{
	static uint64_t prev_tsc = 0;
	uint64_t tsc;

	if (pcpuid_from_vcpu(vcpu) == VUART_TIMER_CPU) {
		tsc = cpu_ticks();
		if (tsc - prev_tsc > (TICKS_PER_MS * CONSOLE_KICK_TIMER_TIMEOUT)) {
			console_timer_callback(NULL);
			prev_tsc = tsc;
		}
	}
}

void suspend_console(void)
{
	if (!console_pm_suspended && (VUART_TIMER_CPU == BSP_CPU_ID)) {
		del_timer(&console_timer);
		console_pm_suspended = true;
	}
}

void resume_console(void)
{
	if (console_pm_suspended && (VUART_TIMER_CPU == BSP_CPU_ID)) {
		console_setup_timer();
		console_pm_suspended = false;
	}
}

bool console_need_log(uint32_t severity)
{
	return (severity <= console_loglevel);
}

void console_log(uint32_t severity, char *buffer)
{
	uint64_t rflags;
	size_t len;
	const char *color = logmsg_severity_color(severity);
	size_t color_len;
	size_t reset_len;
	size_t pos = 0U;
	char line[LOG_MESSAGE_MAX_SIZE + 32U];

	spinlock_irqsave_obtain(&console_log_lock, &rflags);

	len = strnlen_s(buffer, LOG_MESSAGE_MAX_SIZE);
	while ((len > 0U) && ((buffer[len - 1U] == '\n') || (buffer[len - 1U] == '\r'))) {
		len--;
	}

	/*
	 * Color only the live console stream. Memory/NPK logs keep the plain prefix
	 * produced by do_logmsg(). When the BEAU shell owns the terminal, route the
	 * line through shell_async_puts_raw() so the prompt/input row is cleared,
	 * the log owns one full line, and the prompt is restored afterwards:
	 *
	 *   console:\> _     -> clear row
	 *   [κ][..] log      -> async log line
	 *   console:\> _     -> restored shell row
	 */
	(void)memset(line, 0U, sizeof(line));
	color_len = strnlen_s(color, 16U);
	reset_len = strnlen_s(LOG_VT100_RESET, 8U);
	(void)memcpy(&line[pos], color, color_len);
	pos += color_len;
	(void)memcpy(&line[pos], buffer, len);
	pos += len;
	(void)memcpy(&line[pos], LOG_VT100_RESET, reset_len);
	pos += reset_len;
	line[pos++] = '\r';
	line[pos++] = '\n';
	if (!shell_async_puts_raw(line)) {
		(void)console_write(line, pos);
	}

	spinlock_irqrestore_release(&console_log_lock, rflags);
}
