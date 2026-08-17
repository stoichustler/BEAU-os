// Copyright 2026, BEAU OS contributors
// SPDX-License-Identifier: BSD-2-Clause

#![allow(unexpected_cfgs)]

const MESSAGE_LABEL_BITS: u32 = 52;
const MESSAGE_LABEL_MASK: u64 = (1_u64 << MESSAGE_LABEL_BITS) - 1;
const MESSAGE_LABEL_SHIFT: u32 = 12;
#[cfg(test)]
const MESSAGE_LENGTH_MASK: u64 = 0x7f;
#[cfg(any(test, ipc_benchmark_client, ipc_benchmark_responder))]
const BENCHMARK_REQUEST_LABEL: u64 = 0xb001;
#[cfg(any(test, ipc_benchmark_client, ipc_benchmark_responder))]
const BENCHMARK_REPLY_LABEL: u64 = 0xb002;
#[cfg(any(test, ipc_benchmark_responder))]
const BENCHMARK_PROTOCOL_ERROR_LABEL: u64 = 0xb0ff;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MessageInfo {
    words: [u64; 1],
}

impl MessageInfo {
    fn zero_length(label: u64) -> Self {
        assert!(label <= MESSAGE_LABEL_MASK);
        Self { words: [label << MESSAGE_LABEL_SHIFT] }
    }

    pub fn raw(self) -> u64 {
        self.words[0]
    }

    #[cfg(any(test, target_arch = "aarch64"))]
    pub fn from_raw(raw: u64) -> Self {
        Self { words: [raw] }
    }

    #[cfg(test)]
    fn label(self) -> u64 {
        self.raw() >> MESSAGE_LABEL_SHIFT
    }

    #[cfg(test)]
    fn length(self) -> u64 {
        self.raw() & MESSAGE_LENGTH_MASK
    }
}

#[cfg(any(test, ipc_benchmark_client))]
#[derive(Debug, Eq, PartialEq)]
pub enum ProtocolError {
    ReplyMismatch,
}

#[cfg(any(test, ipc_benchmark_client))]
pub fn benchmark_request() -> MessageInfo {
    MessageInfo::zero_length(BENCHMARK_REQUEST_LABEL)
}

#[cfg(any(test, ipc_benchmark_client))]
pub fn validate_benchmark_reply(reply: MessageInfo) -> Result<(), ProtocolError> {
    if reply.raw() != MessageInfo::zero_length(BENCHMARK_REPLY_LABEL).raw() {
        return Err(ProtocolError::ReplyMismatch);
    }
    Ok(())
}

#[cfg(any(test, ipc_benchmark_responder))]
pub fn benchmark_response(channel: u32, request: MessageInfo) -> MessageInfo {
    if channel == 0 && request.raw() == MessageInfo::zero_length(BENCHMARK_REQUEST_LABEL).raw() {
        MessageInfo::zero_length(BENCHMARK_REPLY_LABEL)
    } else {
        MessageInfo::zero_length(BENCHMARK_PROTOCOL_ERROR_LABEL)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zero_length_message_info_preserves_its_label() {
        let message = MessageInfo::zero_length(0xabc);

        assert_eq!(message.raw(), 0xabc << 12);
        assert_eq!(message.label(), 0xabc);
        assert_eq!(message.length(), 0);
    }

    #[test]
    fn benchmark_reply_requires_the_exact_zero_length_message() {
        assert_eq!(
            validate_benchmark_reply(MessageInfo::zero_length(BENCHMARK_REPLY_LABEL)),
            Ok(())
        );
        assert_eq!(
            validate_benchmark_reply(MessageInfo::zero_length(BENCHMARK_REQUEST_LABEL)),
            Err(ProtocolError::ReplyMismatch)
        );
        let reply_with_extra_cap =
            MessageInfo::from_raw(MessageInfo::zero_length(BENCHMARK_REPLY_LABEL).raw() | (1 << 7));
        assert_eq!(
            validate_benchmark_reply(reply_with_extra_cap),
            Err(ProtocolError::ReplyMismatch)
        );
    }

    #[test]
    fn responder_only_accepts_the_exact_benchmark_request() {
        let request = benchmark_request();

        assert_eq!(benchmark_response(0, request).label(), BENCHMARK_REPLY_LABEL);
        assert_eq!(benchmark_response(1, request).label(), BENCHMARK_PROTOCOL_ERROR_LABEL);
        assert_eq!(
            benchmark_response(0, MessageInfo::zero_length(BENCHMARK_REPLY_LABEL)).label(),
            BENCHMARK_PROTOCOL_ERROR_LABEL
        );
        let request_with_extra_cap = MessageInfo::from_raw(request.raw() | (1 << 7));
        assert_eq!(
            benchmark_response(0, request_with_extra_cap).label(),
            BENCHMARK_PROTOCOL_ERROR_LABEL
        );
    }
}
