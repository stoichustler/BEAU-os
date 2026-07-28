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

use virtio_drivers::transport::pci::VirtioPciError;

use rust_support::Error as LkError;
use virtio_drivers::Error as VirtioError;

pub enum Error {
    Pci(VirtioPciError),
    Virtio(VirtioError),
    Lk(LkError),
}

impl From<VirtioPciError> for Error {
    fn from(e: VirtioPciError) -> Self {
        Self::Pci(e)
    }
}

impl From<VirtioError> for Error {
    fn from(e: VirtioError) -> Self {
        Self::Virtio(e)
    }
}

impl From<LkError> for Error {
    fn from(e: LkError) -> Self {
        Self::Lk(e)
    }
}

impl Error {
    pub fn into_c(self) -> i32 {
        match self {
            Self::Pci(_) => rust_support::Error::ERR_GENERIC,
            Self::Virtio(_) => rust_support::Error::ERR_GENERIC,
            Self::Lk(e) => e,
        }
        .into()
    }
}
