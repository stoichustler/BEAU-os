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

use core::ffi::c_char;
use core::ffi::c_uint;
use core::ffi::c_void;
use core::ptr::{addr_of, addr_of_mut};

use crate::paddr_t;
use crate::status_t;

pub use crate::sys::vaddr_to_paddr;
pub use crate::sys::vmm_alloc_contiguous;
pub use crate::sys::vmm_alloc_physical_etc;
pub use crate::sys::vmm_aspace_t;
pub use crate::sys::vmm_free_region;

#[inline]
pub fn vmm_get_kernel_aspace() -> *mut vmm_aspace_t {
    // SAFETY: The returned raw pointer holds the same safety invariants as accessing a `static mut`,
    // so this `unsafe` is unconditionally sound, and may become safe in edition 2024:
    // <https://github.com/rust-lang/rust/issues/114447>.
    unsafe { addr_of_mut!(crate::sys::_kernel_aspace) }
}

/// # Safety
///
/// Same as [`vmm_alloc_physical_etc`].
#[inline]
pub unsafe fn vmm_alloc_physical(
    aspace: *mut vmm_aspace_t,
    name: *const c_char,
    size: usize,
    ptr: *const *mut c_void,
    align_log2: u8,
    paddr: paddr_t,
    vmm_flags: c_uint,
    arch_mmu_flags: c_uint,
) -> status_t {
    // SAFETY: Delegated.
    unsafe {
        vmm_alloc_physical_etc(
            aspace,
            name,
            size,
            ptr.cast_mut(),
            align_log2,
            addr_of!(paddr),
            1,
            vmm_flags,
            arch_mmu_flags,
        )
    }
}
