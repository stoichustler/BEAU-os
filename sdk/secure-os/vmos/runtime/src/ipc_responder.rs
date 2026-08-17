// Copyright 2026, BEAU OS contributors
// SPDX-License-Identifier: BSD-2-Clause

#![no_main]
#![no_std]

use core::panic::PanicInfo;

mod ipc_protocol;

use ipc_protocol::{benchmark_response, MessageInfo};

#[no_mangle]
pub extern "C" fn init() {}

#[no_mangle]
pub extern "C" fn notified(_channel: u32) {}

#[no_mangle]
pub extern "C" fn protected(channel: u32, request: MessageInfo) -> MessageInfo {
    benchmark_response(channel, request)
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
