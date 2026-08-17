// Copyright 2026, BEAU OS contributors
// SPDX-License-Identifier: BSD-2-Clause

#![cfg_attr(target_arch = "aarch64", no_main)]
#![cfg_attr(target_arch = "aarch64", no_std)]

#[cfg_attr(not(target_arch = "aarch64"), allow(dead_code))]
mod benchmark;
#[cfg_attr(not(target_arch = "aarch64"), allow(dead_code))]
mod console;
#[cfg(any(test, target_arch = "aarch64"))]
mod ipc_protocol;
#[cfg(any(test, target_arch = "aarch64"))]
mod sel4_ipc;

#[cfg(target_arch = "aarch64")]
use benchmark::{run_ipc, IpcBackend, RunError, Stats};
#[cfg(target_arch = "aarch64")]
use console::{Console, Output};
#[cfg(target_arch = "aarch64")]
use core::arch::asm;
#[cfg(target_arch = "aarch64")]
use core::cell::UnsafeCell;
#[cfg(target_arch = "aarch64")]
use core::panic::PanicInfo;
#[cfg(target_arch = "aarch64")]
use core::ptr::{read_volatile, write_volatile};

#[cfg(target_arch = "aarch64")]
unsafe extern "C" {
    fn microkit_dbg_putc(character: i32);
}

#[cfg(target_arch = "aarch64")]
const PL011_BASE: usize = 0x1000_0000;
#[cfg(target_arch = "aarch64")]
const PL011_DATA: usize = 0x00;
#[cfg(target_arch = "aarch64")]
const PL011_FLAGS: usize = 0x18;
#[cfg(target_arch = "aarch64")]
const PL011_INTERRUPT_MASK: usize = 0x38;
#[cfg(target_arch = "aarch64")]
const PL011_MASKED_INTERRUPT_STATUS: usize = 0x40;
#[cfg(target_arch = "aarch64")]
const PL011_INTERRUPT_CLEAR: usize = 0x44;
#[cfg(target_arch = "aarch64")]
const PL011_RX_FIFO_EMPTY: u32 = 1 << 4;
#[cfg(target_arch = "aarch64")]
const PL011_RX_INTERRUPT: u32 = 1 << 4;
#[cfg(target_arch = "aarch64")]
const PL011_RECEIVE_TIMEOUT_INTERRUPT: u32 = 1 << 6;
#[cfg(target_arch = "aarch64")]
const PL011_INPUT_INTERRUPTS: u32 = PL011_RX_INTERRUPT | PL011_RECEIVE_TIMEOUT_INTERRUPT;
#[cfg(target_arch = "aarch64")]
const PL011_DATA_ERROR: u32 = 0x0f00;
#[cfg(target_arch = "aarch64")]
const CONSOLE_IRQ_CHANNEL: u32 = 0;
#[cfg(target_arch = "aarch64")]
const BASE_IRQ_CAP: u64 = 138;
#[cfg(target_arch = "aarch64")]
const SEL4_IRQ_ACK_LABEL: u64 = 31;

#[cfg(target_arch = "aarch64")]
struct DebugOutput;

#[cfg(target_arch = "aarch64")]
struct Sel4IpcBackend;

#[cfg(target_arch = "aarch64")]
impl IpcBackend for Sel4IpcBackend {
    fn counter_frequency_hz(&self) -> u64 {
        sel4_ipc::counter_frequency_hz()
    }

    fn round_trip_ticks(&mut self) -> Result<u64, RunError> {
        sel4_ipc::benchmark_round_trip_ticks().map_err(|error| match error {
            sel4_ipc::IpcError::InvalidChannel => RunError::Unavailable,
            sel4_ipc::IpcError::ReplyMismatch => RunError::ReplyMismatch,
        })
    }
}

#[cfg(target_arch = "aarch64")]
fn run_sel4_ipc_benchmark(iterations: u32) -> Result<Stats, RunError> {
    run_ipc(&mut Sel4IpcBackend, iterations)
}

#[cfg(target_arch = "aarch64")]
impl Output for DebugOutput {
    fn write_str(&mut self, text: &str) {
        for byte in text.bytes() {
            unsafe {
                microkit_dbg_putc(i32::from(byte));
            }
        }
    }
}

#[cfg(target_arch = "aarch64")]
struct ConsoleCell(UnsafeCell<Console<DebugOutput>>);

// SAFETY: a Microkit protection domain dispatches init and notifications on
// one thread, so the contained console is never accessed concurrently.
#[cfg(target_arch = "aarch64")]
unsafe impl Sync for ConsoleCell {}

#[cfg(target_arch = "aarch64")]
impl ConsoleCell {
    fn with<R>(&self, action: impl FnOnce(&mut Console<DebugOutput>) -> R) -> R {
        unsafe { action(&mut *self.0.get()) }
    }
}

#[cfg(target_arch = "aarch64")]
static CONSOLE: ConsoleCell =
    ConsoleCell(UnsafeCell::new(Console::with_benchmark(DebugOutput, run_sel4_ipc_benchmark)));

#[cfg(target_arch = "aarch64")]
fn pl011_read(offset: usize) -> u32 {
    unsafe { read_volatile((PL011_BASE + offset) as *const u32) }
}

#[cfg(target_arch = "aarch64")]
fn pl011_write(offset: usize, value: u32) {
    unsafe {
        write_volatile((PL011_BASE + offset) as *mut u32, value);
    }
}

#[cfg(target_arch = "aarch64")]
fn debug_write(text: &str) {
    DebugOutput.write_str(text);
}

#[cfg(target_arch = "aarch64")]
fn microkit_irq_ack(channel: u32) {
    let mut destination = BASE_IRQ_CAP + u64::from(channel);
    let mut message_info = SEL4_IRQ_ACK_LABEL << 12;
    let mut mr0 = 0_u64;
    let mut mr1 = 0_u64;
    let mut mr2 = 0_u64;
    let mut mr3 = 0_u64;
    let reply = 0_u64;
    let syscall = u64::MAX;

    unsafe {
        asm!(
            "svc #0",
            inout("x0") destination,
            inout("x1") message_info,
            inout("x2") mr0,
            inout("x3") mr1,
            inout("x4") mr2,
            inout("x5") mr3,
            in("x6") reply,
            in("x7") syscall,
            options(nostack),
        );
    }

    let _ = (destination, message_info, mr0, mr1, mr2, mr3);
}

#[cfg(target_arch = "aarch64")]
#[no_mangle]
pub extern "C" fn init() {
    pl011_write(PL011_INTERRUPT_MASK, 0);
    pl011_write(PL011_INTERRUPT_CLEAR, u32::MAX);
    debug_write("VMOS|INFO: Rust runtime started\n");
    CONSOLE.with(Console::start);
    pl011_write(PL011_INTERRUPT_MASK, PL011_INPUT_INTERRUPTS);
}

#[cfg(target_arch = "aarch64")]
#[no_mangle]
pub extern "C" fn notified(channel: u32) {
    if channel != CONSOLE_IRQ_CHANNEL {
        debug_write("VMOS|ERROR: unexpected console notification channel\n");
        return;
    }

    let pending = pl011_read(PL011_MASKED_INTERRUPT_STATUS) & PL011_INPUT_INTERRUPTS;
    while pl011_read(PL011_FLAGS) & PL011_RX_FIFO_EMPTY == 0 {
        let data = pl011_read(PL011_DATA);
        let byte = if data & PL011_DATA_ERROR == 0 { data as u8 } else { 0x80 };
        CONSOLE.with(|console| console.input(byte));
    }
    pl011_write(PL011_INTERRUPT_CLEAR, if pending == 0 { PL011_INPUT_INTERRUPTS } else { pending });
    microkit_irq_ack(CONSOLE_IRQ_CHANNEL);
}

#[cfg(target_arch = "aarch64")]
#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

#[cfg(all(not(test), not(target_arch = "aarch64")))]
fn main() {}
