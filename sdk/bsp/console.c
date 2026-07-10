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
#include <dbg_cmd.h>
#include <rtl.h>
#include <sprintf.h>
#include <spinlock.h>
#include <asm/guest/vm.h>
#include "shell_priv.h"

/*
 * 2026-06-30, console ownership principle:
 *
 * The host has one physical serial stream, but BEAU must multiplex three
 * roles on top of it: the hypervisor shell, the selected VM console, and
 * background VM output. console_vmid is the ownership switch:
 *
 *   ACRN_INVALID_VMID  -> BEAU shell owns host input/output
 *   VM id             -> that VM owns host input; its TX ring drains to host
 *
 * Guest TX does not write directly to the serial driver. vPL011/vUART pushes
 * bytes into a per-VM receive FIFO. vsh binds the host vuart to one VM console
 * at a time; unbound VMs keep buffering output until that vuart is bound again,
 * which prevents one noisy guest from stealing the BEAU shell.
 *
 * 2026-07-10, console service data flow:
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
 *   per-VM console ring
 *      |
 *      +-- bound by vsh   -> drain to host UART with VM prefix
 *      +-- not selected   -> keep recent output for later replay
 *
 * The service invariant is single host-input owner, many guest-output
 * producers. Backpressure and drop-oldest buffering protect the shell from
 * being blocked by a noisy or stalled guest console.
 */
struct hv_timer console_timer;

#ifndef CONFIG_CONSOLE_KICK_TIMER_TIMEOUT
#define CONFIG_CONSOLE_KICK_TIMER_TIMEOUT 2UL
#endif
#define CONSOLE_KICK_TIMER_TIMEOUT CONFIG_CONSOLE_KICK_TIMER_TIMEOUT
#ifndef CONFIG_VM_CONSOLE_RINGBUF_SIZE
#define CONFIG_VM_CONSOLE_RINGBUF_SIZE  4096U
#endif
#ifndef CONFIG_VM_CONSOLE_RINGBUF_VM_NUM
#define CONFIG_VM_CONSOLE_RINGBUF_VM_NUM CONFIG_MAX_VM_NUM
#endif
#if (CONFIG_VM_CONSOLE_RINGBUF_SIZE < 2U)
#error "CONFIG_VM_CONSOLE_RINGBUF_SIZE must be at least 2 bytes"
#endif
#if ((CONFIG_VM_CONSOLE_RINGBUF_SIZE & (CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1U)) != 0U)
#error "CONFIG_VM_CONSOLE_RINGBUF_SIZE must be a power of two"
#endif
/*
 * Power-of-two rings use monotonic producer/consumer indexes and mask only
 * when indexing the backing array. One byte is intentionally left unused so
 * empty and full states are distinguishable from prod == cons style checks.
 */
#define VM_CONSOLE_RINGBUF_MASK     (CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1U)
#define VM_CONSOLE_RINGBUF_CAPACITY (CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1U)
#ifndef CONFIG_VM_CONSOLE_DRAIN_BUDGET
#define CONFIG_VM_CONSOLE_DRAIN_BUDGET 512U
#endif
#define VM_CONSOLE_DRAIN_BUDGET CONFIG_VM_CONSOLE_DRAIN_BUDGET
#ifndef CONFIG_VM_CONSOLE_DRAIN_BURST_BUDGET
#define CONFIG_VM_CONSOLE_DRAIN_BURST_BUDGET 2048U
#endif
#define VM_CONSOLE_DRAIN_BURST_BUDGET CONFIG_VM_CONSOLE_DRAIN_BURST_BUDGET
#ifndef CONFIG_VM_CONSOLE_DRAIN_BURST_THRESHOLD
#define CONFIG_VM_CONSOLE_DRAIN_BURST_THRESHOLD (CONFIG_VM_CONSOLE_RINGBUF_SIZE / 2U)
#endif
#define VM_CONSOLE_DRAIN_BURST_THRESHOLD CONFIG_VM_CONSOLE_DRAIN_BURST_THRESHOLD
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
#define VM_CONSOLE_PREFIX_MAX_SIZE 16U
#define VM_CONSOLE_CPR_QUERY_LEN 4U
#define VM_CONSOLE_EXCEPTION_RINGBUF_SIZE 4096U
#define VM_CONSOLE_EXCEPTION_RINGBUF_MASK (VM_CONSOLE_EXCEPTION_RINGBUF_SIZE - 1U)
#define VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY (VM_CONSOLE_EXCEPTION_RINGBUF_SIZE - 1U)
#ifndef CONFIG_VM_CONSOLE_TX_BACKPRESSURE_HIGH_WATER
#define CONFIG_VM_CONSOLE_TX_BACKPRESSURE_HIGH_WATER \
	((VM_CONSOLE_RINGBUF_CAPACITY * 7U) / 8U)
#endif
#ifndef CONFIG_VM_CONSOLE_TX_BACKPRESSURE_LOW_WATER
#define CONFIG_VM_CONSOLE_TX_BACKPRESSURE_LOW_WATER \
	((VM_CONSOLE_RINGBUF_CAPACITY * 3U) / 4U)
#endif
#define VM_CONSOLE_TX_BACKPRESSURE_HIGH_WATER CONFIG_VM_CONSOLE_TX_BACKPRESSURE_HIGH_WATER
#define VM_CONSOLE_TX_BACKPRESSURE_LOW_WATER CONFIG_VM_CONSOLE_TX_BACKPRESSURE_LOW_WATER
/* Switching key combinations for shell and uart console */
#define GUEST_CONSOLE_TO_HV_SWITCH_KEY  0x4U /* Ctrl-D */
uint16_t console_vmid = CONFIG_CONSOLE_DEFAULT_VM;

uint16_t console_loglevel = CONFIG_CONSOLE_LOGLEVEL_DEFAULT;
static spinlock_t console_log_lock;

/*
 * VM console output ring:
 *
 * prod/cons are monotonic counters, not bounded array indexes. Writers keep
 * the newest CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1 bytes by advancing cons on
 * overflow. That drop-oldest policy is intentional for boot logs: recent guest
 * failures are more useful than preserving stale early output at the cost of
 * hiding the current prompt.
 *
 * pending marks queued bytes for shell diagnostics. draining prevents nested
 * timer/vsh paths from replaying the same VM stream concurrently. line_start,
 * last_cr, and terminal_query_* are host-serial presentation state used while
 * adding VM prefixes and hiding terminal cursor-position probes. vuart_bound
 * records whether vsh has bound the host vuart to this VM console; guest
 * output is kept in the FIFO while unbound, then drained to the host when vsh
 * binds.
 */
struct vm_console_ringbuf {
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
	bool pending;
	bool draining;
	bool line_start;
	bool last_cr;
	bool vuart_bound;
	uint8_t terminal_query_len;
	char terminal_query_buf[VM_CONSOLE_CPR_QUERY_LEN];
	char buf[CONFIG_VM_CONSOLE_RINGBUF_SIZE];
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

static struct vm_console_ringbuf vm_console_ringbufs[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
static struct vm_exception_ringbuf vm_exception_ringbufs[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
static bool vm_console_tx_blocked[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
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
static void console_vm_vuart_receive(uint16_t vmid, char ch);
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
		spinlock_init(&vm_console_ringbufs[i].lock);
		vm_console_ringbufs[i].line_start = true;
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
	bool handled = false;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		console_vm_vuart_receive(vmid, ch);
		handled = true;
	}

	return handled;
}

static bool console_vm_tx_update_blocked(uint16_t vmid, uint32_t queued, bool bound)
{
	bool blocked = false;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		if (bound) {
			blocked = vm_console_tx_blocked[vmid];
			if (queued >= VM_CONSOLE_TX_BACKPRESSURE_HIGH_WATER) {
				blocked = true;
			} else if (queued <= VM_CONSOLE_TX_BACKPRESSURE_LOW_WATER) {
				blocked = false;
			}
		}
		vm_console_tx_blocked[vmid] = blocked;
	}

	return blocked;
}

static bool console_vm_tx_update_blocked_locked(uint16_t vmid,
	const struct vm_console_ringbuf *rb)
{
	uint32_t queued;

	queued = console_ring_queued(rb->prod, rb->cons, VM_CONSOLE_RINGBUF_CAPACITY);
	return console_vm_tx_update_blocked(vmid, queued, rb->vuart_bound);
}

bool console_vm_tx_can_accept(uint16_t vmid)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	bool can_accept = false;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		can_accept = !console_vm_tx_update_blocked_locked(vmid, rb);
		spinlock_irqrestore_release(&rb->lock, rflags);
	}

	return can_accept;
}

size_t console_write(const char *s, size_t len)
{
	return  serial_puts(s, len);
}

char console_getc(void)
{
	return serial_getc();
}

static void console_vm_vuart_receive(uint16_t vmid, char ch)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	uint32_t prod;
	uint32_t queued;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		/*
		 * TX can be called from guest MMIO exits on different pCPUs.
		 * Serialize ring state here, but do not write host serial under
		 * this producer lock; slow serial output is handled by the drain
		 * side so guest TX exits remain bounded.
		 */
		rb->input_bytes++;
		prod = rb->prod;
		rb->buf[prod & VM_CONSOLE_RINGBUF_MASK] = ch;
		rb->prod = prod + 1U;
		if ((rb->prod - rb->cons) > VM_CONSOLE_RINGBUF_CAPACITY) {
			rb->dropped_bytes += (rb->prod - rb->cons) - VM_CONSOLE_RINGBUF_CAPACITY;
			rb->overflow_events++;
			rb->last_overflow_tsc = cpu_ticks();
			rb->cons = rb->prod - VM_CONSOLE_RINGBUF_CAPACITY;
		}
		queued = rb->prod - rb->cons;
		if (queued > rb->high_water) {
			rb->high_water = queued;
		}
		(void)console_vm_tx_update_blocked(vmid, queued, rb->vuart_bound);
		rb->stored_bytes++;
		rb->pending = rb->vuart_bound;
		spinlock_irqrestore_release(&rb->lock, rflags);
	}
}

void console_vm_ring_drain(uint16_t vmid)
{
	console_vm_ring_drain_internal(vmid);
}

bool console_vm_vuart_bind(uint16_t vmid)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	bool valid = false;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		/*
		 * 2026-07-01, console-vuart binding principle:
		 *
		 *   vPL011 TX -> per-VM receive FIFO -> optional host vuart binding
		 *
		 * vsh is the single host vuart binding. Binding it marks existing
		 * FIFO bytes pending so the timer/backend can drain backlog after
		 * ownership is visible on the serial stream.
		 */
		rb->vuart_bound = true;
		rb->pending = (rb->prod != rb->cons);
		(void)console_vm_tx_update_blocked_locked(vmid, rb);
		spinlock_irqrestore_release(&rb->lock, rflags);
		valid = true;
	}

	return valid;
}

void console_vm_vuart_unbind(uint16_t vmid)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	bool was_blocked = false;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		was_blocked = vm_console_tx_blocked[vmid];
		rb->vuart_bound = false;
		rb->pending = false;
		vm_console_tx_blocked[vmid] = false;
		spinlock_irqrestore_release(&rb->lock, rflags);
		if (was_blocked) {
			console_vm_tx_space_changed(vmid);
		}
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
				"\r\n%s──────── [switch to BEAU console] ────────%s\r\n",
				SHELL_COLOR_YELLOW, SHELL_COLOR_RESET);
			shell_puts(temp_str);
			break;
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

static uint32_t console_vm_ring_budget_from_queued(uint32_t queued, bool input_pending)
{
	uint32_t budget = (queued >= VM_CONSOLE_DRAIN_BURST_THRESHOLD) ?
		VM_CONSOLE_DRAIN_BURST_BUDGET : VM_CONSOLE_DRAIN_BUDGET;

	if (input_pending && (budget > VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET)) {
		budget = VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET;
	} else if ((queued < VM_CONSOLE_DRAIN_BURST_THRESHOLD) &&
		(budget > VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET)) {
		budget = VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET;
	}

	return budget;
}

static uint32_t console_vm_ring_drain_budget(struct vm_console_ringbuf *rb)
{
	uint64_t rflags;
	uint32_t queued;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	queued = console_ring_queued(rb->prod, rb->cons, VM_CONSOLE_RINGBUF_CAPACITY);
	spinlock_irqrestore_release(&rb->lock, rflags);

	return console_vm_ring_budget_from_queued(queued, console_vm_input_pending_for_drain());
}

bool console_vm_ring_get_stats(uint16_t vmid, struct console_vm_ring_stats *stats)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	uint32_t queued;
	bool bound;
	bool valid = false;

	if ((stats != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		queued = console_ring_queued(rb->prod, rb->cons, VM_CONSOLE_RINGBUF_CAPACITY);
		bound = rb->vuart_bound;
		stats->vmid = vmid;
		stats->size = CONFIG_VM_CONSOLE_RINGBUF_SIZE;
		stats->capacity = VM_CONSOLE_RINGBUF_CAPACITY;
		stats->drain_budget = bound ? console_vm_ring_budget_from_queued(queued,
			(console_vmid == vmid) && console_vm_input_pending_for_drain()) : 0U;
		stats->queued = queued;
		stats->high_water = rb->high_water;
		stats->input_bytes = rb->input_bytes;
		stats->stored_bytes = rb->stored_bytes;
		stats->drained_bytes = rb->drained_bytes;
		stats->dropped_bytes = rb->dropped_bytes;
		stats->overflow_events = rb->overflow_events;
		stats->last_overflow_tsc = rb->last_overflow_tsc;
		stats->pending = rb->pending;
		stats->draining = rb->draining;
		stats->vuart_bound = bound;
		spinlock_irqrestore_release(&rb->lock, rflags);
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

static bool console_vm_ring_drain_begin(struct vm_console_ringbuf *rb)
{
	uint64_t rflags;
	bool draining;
	bool bound;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	/*
	 * Only the bound host vuart drains guest output to the physical UART.
	 * One active drain is enough because it claims bytes by advancing cons;
	 * nested drains would only interleave prefixes and serial writes.
	 */
	draining = rb->draining;
	bound = rb->vuart_bound;
	if (!draining && bound) {
		rb->draining = true;
	}
	spinlock_irqrestore_release(&rb->lock, rflags);

	return !draining && bound;
}

static void console_vm_ring_drain_end(struct vm_console_ringbuf *rb)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	rb->draining = false;
	spinlock_irqrestore_release(&rb->lock, rflags);
}

static uint32_t console_vm_ring_claim(struct vm_console_ringbuf *rb, char *buf, uint32_t len)
{
	uint64_t rflags;
	uint32_t count;
	uint32_t cons;
	uint32_t first;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	count = rb->prod - rb->cons;
	if (count > VM_CONSOLE_RINGBUF_CAPACITY) {
		/*
		 * A producer may have advanced beyond capacity before this drain
		 * snapshot. Clamp to the newest valid window before copying so
		 * the reader never replays bytes that have already been dropped.
		 */
		rb->cons = rb->prod - VM_CONSOLE_RINGBUF_CAPACITY;
		count = VM_CONSOLE_RINGBUF_CAPACITY;
	}
	if (count > len) {
		count = len;
	}
	if (count > 0U) {
		cons = rb->cons;
		first = CONFIG_VM_CONSOLE_RINGBUF_SIZE - (cons & VM_CONSOLE_RINGBUF_MASK);
		if (first > count) {
			first = count;
		}
		(void)memcpy(buf, &rb->buf[cons & VM_CONSOLE_RINGBUF_MASK], first);
		if (first < count) {
			(void)memcpy(&buf[first], rb->buf, count - first);
		}
		rb->cons = cons + count;
		rb->drained_bytes += count;
	}
	rb->pending = (rb->prod != rb->cons);
	spinlock_irqrestore_release(&rb->lock, rflags);

	return count;
}

static bool console_vm_ring_release_tx_backpressure(uint16_t vmid, struct vm_console_ringbuf *rb)
{
	uint64_t rflags;
	bool was_blocked = false;
	bool is_blocked = false;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		was_blocked = vm_console_tx_blocked[vmid];
		is_blocked = console_vm_tx_update_blocked_locked(vmid, rb);
	}
	spinlock_irqrestore_release(&rb->lock, rflags);

	return was_blocked && !is_blocked;
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
	struct vm_console_ringbuf *rb, char *out, uint32_t *out_len, char ch)
{
	if (rb->line_start) {
		if (!rb->last_cr || (ch != '\n')) {
			console_vm_prefixed_write_bytes(out, out_len, prefix, prefix_len);
			rb->line_start = false;
		}
	}

	console_vm_prefixed_write_byte(out, out_len, ch);
	if (ch == '\r') {
		rb->line_start = true;
		rb->last_cr = true;
	} else if (ch == '\n') {
		rb->line_start = true;
		rb->last_cr = false;
	} else {
		rb->last_cr = false;
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
	struct vm_console_ringbuf *rb, char *out, uint32_t *out_len)
{
	for (uint32_t idx = 0U; idx < rb->terminal_query_len; idx++) {
		console_vm_ring_write_visible_byte(prefix, prefix_len, rb, out, out_len,
			rb->terminal_query_buf[idx]);
	}
	rb->terminal_query_len = 0U;
}

static bool console_vm_filter_terminal_query(uint16_t vmid, struct vm_console_ringbuf *rb,
	const char *prefix, uint32_t prefix_len, char *out, uint32_t *out_len, char ch)
{
	bool consumed = false;

	if ((rb->terminal_query_len != 0U) || (ch == vm_console_cpr_query[0U])) {
		if (rb->terminal_query_len < VM_CONSOLE_CPR_QUERY_LEN) {
			rb->terminal_query_buf[rb->terminal_query_len] = ch;
			rb->terminal_query_len++;
		}
		consumed = true;

		if (memcmp(rb->terminal_query_buf, vm_console_cpr_query,
			rb->terminal_query_len) == 0) {
			if (rb->terminal_query_len == VM_CONSOLE_CPR_QUERY_LEN) {
				rb->terminal_query_len = 0U;
				console_vm_inject_cpr_reply(vmid);
			}
		} else {
			console_vm_flush_terminal_query(prefix, prefix_len, rb, out, out_len);
		}
	}

	return consumed;
}

static void console_vm_ring_write_prefixed(uint16_t vmid, struct vm_console_ringbuf *rb,
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
	(void)snprintf(prefix, sizeof(prefix), "[vmid %u] ", vmid);
	prefix_len = strnlen_s(prefix, sizeof(prefix));

	for (uint32_t idx = 0U; idx < len; idx++) {
		char ch = buf[idx];

		/*
		 * BusyBox ash queries terminal cursor position with ESC[6n after
		 * prompt redraws. The BEAU vsh path is a console multiplexer, not a
		 * transparent terminal, so answer the query internally and hide it
		 * from the host serial stream.
		 */
		if (console_vm_filter_terminal_query(vmid, rb, prefix,
			(uint32_t)prefix_len, vm_console_output_buf, &out_len, ch)) {
			continue;
		}

		console_vm_ring_write_visible_byte(prefix, (uint32_t)prefix_len,
			rb, vm_console_output_buf, &out_len, ch);
	}

	console_vm_prefixed_write_flush(vm_console_output_buf, &out_len);
}

static void console_vm_ring_drain_internal(uint16_t vmid)
{
	struct vm_console_ringbuf *rb;
	uint32_t count;
	uint32_t budget;
	uint32_t chunk;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		if (console_vm_ring_drain_begin(rb)) {
			bool tx_space_released;

			/*
			 * 2026-07-01, vuart live-drain pacing:
			 *
			 * Guest TX can be much faster than the physical serial backend.
			 * Keep each bound-vuart pass small, and shrink it again when
			 * host input is waiting, so long guest output cannot hide Ctrl-D
			 * or the next typed command behind a large synchronous write.
			 */
			budget = console_vm_ring_drain_budget(rb);
			do {
				chunk = (budget < VM_CONSOLE_DRAIN_BUF_SIZE) ? budget : VM_CONSOLE_DRAIN_BUF_SIZE;
				if (chunk == 0U) {
					break;
				}
				count = console_vm_ring_claim(rb, vm_console_drain_buf, chunk);
				if (count > 0U) {
					console_vm_ring_write_prefixed(vmid, rb, vm_console_drain_buf, count);
					budget -= count;
				}
			} while ((count == chunk) && (budget > 0U));
			console_vm_ring_drain_end(rb);
			tx_space_released = console_vm_ring_release_tx_backpressure(vmid, rb);
			if (tx_space_released) {
				console_vm_tx_space_changed(vmid);
			}
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

static void console_timer_callback(__unused void *data)
{
	if (!console_vm_kick()) {
		shell_kick();
	}
}

void console_setup_timer(void)
{
	uint64_t period_in_cycle, fire_tsc;

	/*
	 * The console timer is the pacing boundary for shell and VM console work.
	 * Keeping it periodic avoids doing slow serial output directly in guest
	 * MMIO exits, while the configurable low-ms cadence keeps vsh input/output
	 * responsive enough for interactive debugging.
	 */
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
	if (VUART_TIMER_CPU == BSP_CPU_ID) {
		del_timer(&console_timer);
	}
}

void resume_console(void)
{
	if (VUART_TIMER_CPU == BSP_CPU_ID) {
		console_setup_timer();
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
