/*
 * Copyright (c) 2024 Google Inc. All rights reserved
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

use core::{fmt, num::NonZeroI32};
use log::error;
use num_traits::FromPrimitive;

use crate::sys::Error;

impl Error {
    pub fn from_lk(e: i32) -> Result<(), Self> {
        if e == 0 {
            return Ok(());
        }
        match Error::try_from(NonZeroI32::new(e).unwrap()) {
            Ok(expected_err) => Err(expected_err),
            Err(_conversion) => {
                error!("don't know how to map {e} to Error");
                Err(Error::ERR_INVALID_ARGS)
            }
        }
    }
}

impl TryFrom<NonZeroI32> for Error {
    type Error = &'static str;

    fn try_from(e: NonZeroI32) -> Result<Self, Self::Error> {
        match e.get() {
            e if -45 < e && e < 0 => {
                let res = <Error as FromPrimitive>::from_i32(e);
                // unwrap must succeed as e was in the valid range
                Ok(res.unwrap())
            }
            _ => Err("don't know how to map integer to Error"),
        }
    }
}

impl From<Error> for i32 {
    fn from(e: Error) -> i32 {
        e.0
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let code = self.0;
        let msg = match self {
            &Self::NO_ERROR => "no error",
            &Self::ERR_GENERIC => "generic error",
            &Self::ERR_NOT_FOUND => "not ready",
            &Self::ERR_NO_MSG => "no message",
            &Self::ERR_NO_MEMORY => "no memory",
            &Self::ERR_ALREADY_STARTED => "already started",
            &Self::ERR_NOT_VALID => "not valid",
            &Self::ERR_INVALID_ARGS => "invalid arguments",
            &Self::ERR_NOT_ENOUGH_BUFFER => "not enough buffer",
            &Self::ERR_NOT_SUSPENDED => "not suspended",
            &Self::ERR_OBJECT_DESTROYED => "object destroyed",
            &Self::ERR_NOT_BLOCKED => "not blocked",
            &Self::ERR_TIMED_OUT => "timed out",
            &Self::ERR_ALREADY_EXISTS => "already exists",
            &Self::ERR_CHANNEL_CLOSED => "channel closed",
            &Self::ERR_OFFLINE => "offline",
            &Self::ERR_NOT_ALLOWED => "not allowed",
            &Self::ERR_BAD_PATH => "bad path",
            &Self::ERR_ALREADY_MOUNTED => "already mounted",
            &Self::ERR_IO => "input/output error",
            &Self::ERR_NOT_DIR => "not a directory",
            &Self::ERR_NOT_FILE => "not a file",
            &Self::ERR_RECURSE_TOO_DEEP => "recursion too deep",
            &Self::ERR_NOT_SUPPORTED => "not supported",
            &Self::ERR_TOO_BIG => "too big",
            &Self::ERR_CANCELLED => "cancelled",
            &Self::ERR_NOT_IMPLEMENTED => "not implemented",
            &Self::ERR_CHECKSUM_FAIL => "checksum failure",
            &Self::ERR_CRC_FAIL => "CRC failure",
            &Self::ERR_CMD_UNKNOWN => "command unknown",
            &Self::ERR_BAD_STATE => "bad state",
            &Self::ERR_BAD_LEN => "bad length",
            &Self::ERR_BUSY => "busy",
            &Self::ERR_THREAD_DETACHED => "thread detached",
            &Self::ERR_I2C_NACK => "I2C negative acknowledgement",
            &Self::ERR_ALREADY_EXPIRED => "already expired",
            &Self::ERR_OUT_OF_RANGE => "out of range",
            &Self::ERR_NOT_CONFIGURED => "not configured",
            &Self::ERR_NOT_MOUNTED => "not mounted",
            &Self::ERR_FAULT => "fault",
            &Self::ERR_NO_RESOURCES => "no resources",
            &Self::ERR_BAD_HANDLE => "bad handle",
            &Self::ERR_ACCESS_DENIED => "access denied",
            &Self::ERR_PARTIAL_WRITE => "partial write",
            &Self::ERR_USER_BASE => panic!("attempt to display invalid error code"),
            _ => unimplemented!("don't know how to display {self:?}"),
        };
        write!(f, "{msg} ({code})")
    }
}
