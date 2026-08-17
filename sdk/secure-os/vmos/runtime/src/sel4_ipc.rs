// Copyright 2026, BEAU OS contributors
// SPDX-License-Identifier: BSD-2-Clause

#[cfg(target_arch = "aarch64")]
use crate::ipc_protocol::{
    benchmark_request, validate_benchmark_reply, MessageInfo, ProtocolError,
};

const BASE_ENDPOINT_CAP: u64 = 74;
const MAX_CHANNEL_ID: u32 = 61;
#[cfg(target_arch = "aarch64")]
const BENCHMARK_CHANNEL: u32 = 1;
#[cfg(target_arch = "aarch64")]
const SEL4_SYS_CALL: u64 = u64::MAX;

#[cfg(target_arch = "aarch64")]
unsafe extern "C" {
    static microkit_pps: u64;
}

#[derive(Debug, Eq, PartialEq)]
pub enum IpcError {
    InvalidChannel,
    #[cfg(target_arch = "aarch64")]
    ReplyMismatch,
}

pub fn endpoint_for_channel(channel: u32, protected_channels: u64) -> Result<u64, IpcError> {
    if channel > MAX_CHANNEL_ID || protected_channels & (1_u64 << channel) == 0 {
        return Err(IpcError::InvalidChannel);
    }
    Ok(BASE_ENDPOINT_CAP + u64::from(channel))
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
fn read_physical_counter() -> u64 {
    let value: u64;
    unsafe {
        core::arch::asm!(
            "isb",
            "mrs {value}, cntpct_el0",
            value = out(reg) value,
            options(nostack, preserves_flags),
        );
    }
    value
}

#[cfg(target_arch = "aarch64")]
pub fn counter_frequency_hz() -> u64 {
    let value: u64;
    unsafe {
        core::arch::asm!(
            "mrs {value}, cntfrq_el0",
            value = out(reg) value,
            options(nostack, preserves_flags),
        );
    }
    value
}

#[cfg(target_arch = "aarch64")]
unsafe fn sel4_call_zero_words(endpoint: u64, request: MessageInfo) -> MessageInfo {
    let reply_raw: u64;
    unsafe {
        core::arch::asm!(
            "svc #0",
            inlateout("x0") endpoint => _,
            inlateout("x1") request.raw() => reply_raw,
            inlateout("x2") 0_u64 => _,
            inlateout("x3") 0_u64 => _,
            inlateout("x4") 0_u64 => _,
            inlateout("x5") 0_u64 => _,
            in("x6") 0_u64,
            in("x7") SEL4_SYS_CALL,
            options(nostack),
        );
    }
    MessageInfo::from_raw(reply_raw)
}

#[cfg(target_arch = "aarch64")]
pub fn benchmark_round_trip_ticks() -> Result<u64, IpcError> {
    let protected_channels = unsafe { microkit_pps };
    let endpoint = endpoint_for_channel(BENCHMARK_CHANNEL, protected_channels)?;
    let request = benchmark_request();

    let start = read_physical_counter();
    let reply = unsafe { sel4_call_zero_words(endpoint, request) };
    let end = read_physical_counter();

    validate_benchmark_reply(reply).map_err(|error| match error {
        ProtocolError::ReplyMismatch => IpcError::ReplyMismatch,
    })?;
    Ok(end.wrapping_sub(start))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn protected_channel_maps_to_its_microkit_endpoint_capability() {
        assert_eq!(endpoint_for_channel(1, 1 << 1), Ok(75));
    }

    #[test]
    fn protected_channel_rejects_out_of_range_or_unprovisioned_ids() {
        assert_eq!(endpoint_for_channel(62, u64::MAX), Err(IpcError::InvalidChannel));
        assert_eq!(endpoint_for_channel(1, 0), Err(IpcError::InvalidChannel));
    }
}
