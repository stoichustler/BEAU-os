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

use core::cell::UnsafeCell;
use core::mem;
use core::ops::Deref;
use core::ops::DerefMut;

use alloc::boxed::Box;

use crate::Error;

use crate::sys::mutex_acquire_timeout;
use crate::sys::mutex_destroy;
use crate::sys::mutex_init;
use crate::sys::mutex_release;
use crate::sys::mutex_t;
use crate::sys::status_t;

// TODO: `INFINITE_TIME` is defined in `lk/types.h` as `UINT32_MAX`,
// which in turn is defined as `UINT_MAX`, which is not recognized
// by bindgen according to the bug below so we use `u32::MAX`.
// See <https://github.com/rust-lang/rust-bindgen/issues/1636>.
const INFINITE_TIME: u32 = u32::MAX;

/// Try to acquire the mutex with a timeout value.
///
/// # Safety
///
/// Same requirements as [`mutex_acquire_timeout`].
#[inline]
unsafe fn mutex_acquire(mutex: *mut mutex_t) -> status_t {
    // SAFETY: Delegated.
    unsafe { mutex_acquire_timeout(mutex, INFINITE_TIME) }
}

/// A mutex that does not encapsulate the data it protects.
///
/// This is done so that there is no manual `impl<T> Drop for Mutex<T>`,
/// as that prevents moving out of it.
struct LoneMutex {
    /// We need to [`Box`] the C [`mutex_t`] because
    /// it contains a [`list_node`], which cannot be moved.
    /// `std` does a similar thing for immovable mutexes,
    /// though they now use a `LazyBox` so that `fn new` can be a `const fn`.
    /// We don't need that (yet), and `LazyBox` is tricky and not exposed by `std`,
    /// so we just always allocate upfront.
    ///
    /// We need to put the [`mutex_t`] in an [`UnsafeCell`], too,
    /// because it has interior mutability.
    inner: Box<UnsafeCell<mutex_t>>,
}

impl LoneMutex {
    fn new() -> Self {
        // SAFETY: C types like `mutex_t` are zeroizable.
        let mutex = unsafe { mem::zeroed() };
        let mut inner = Box::new(UnsafeCell::new(mutex));
        // SAFETY: `mutex_init` only writes to each field of a `mutex_t`
        unsafe { mutex_init(inner.get_mut()) };
        Self { inner }
    }

    fn get_raw(&self) -> *mut mutex_t {
        self.inner.get()
    }
}

impl Drop for LoneMutex {
    fn drop(&mut self) {
        // SAFETY: `mutex_destroy` is thread safe and it was `mutex_init`ialized.
        unsafe {
            mutex_destroy(self.get_raw());
        }
    }
}

impl Default for LoneMutex {
    fn default() -> Self {
        Self::new()
    }
}

/// A mutex wrapping the C [`mutex_t`].
/// Its API is modeled after [`std::sync::Mutex`].
///
/// The main differences are that [`Mutex`]:
///
/// * doesn't support poisoning (it eagerly panics).
///
/// * doesn't lazily [`Box`] in [`Self::new`],
///   so [`Self::new`] isn't a `const fn` either.
#[derive(Default)]
pub struct Mutex<T: ?Sized> {
    mutex: LoneMutex,
    value: UnsafeCell<T>,
}

impl<T> Mutex<T> {
    pub fn new(value: T) -> Self {
        Self { mutex: LoneMutex::new(), value: UnsafeCell::new(value) }
    }

    pub fn into_inner(self) -> T {
        self.value.into_inner()
    }
}

impl<T: ?Sized> Mutex<T> {
    pub fn get_mut(&mut self) -> &mut T {
        self.value.get_mut()
    }
}

pub struct MutexGuard<'a, T: ?Sized> {
    lock: &'a Mutex<T>,
}

impl<T: ?Sized> Mutex<T> {
    pub fn lock(&self) -> MutexGuard<'_, T> {
        // SAFETY: `mutex_acquire` is thread safe and it was `mutex_init`ialized.
        let status = unsafe { mutex_acquire(self.mutex.get_raw()) };
        assert_eq!(Error::from_lk(status), Ok(()));
        MutexGuard { lock: &self }
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: `mutex_release` is thread safe and it was `mutex_init`ialized.
        let status = unsafe { mutex_release(self.lock.mutex.get_raw()) };
        assert_eq!(Error::from_lk(status), Ok(()));
    }
}

impl<T: ?Sized> Deref for MutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Interior mutability checked by `mutex_t`.
        unsafe { &*self.lock.value.get() }
    }
}

impl<T: ?Sized> DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: Interior mutability checked by `mutex_t`.
        unsafe { &mut *self.lock.value.get() }
    }
}

/// SAFETY: This is a mutex wrapping [`mutex_t`].
unsafe impl<T: ?Sized + Send> Send for Mutex<T> {}

/// SAFETY: This is a mutex wrapping [`mutex_t`].
unsafe impl<T: ?Sized + Send> Sync for Mutex<T> {}

// Note: We don't impl `UnwindSafe` and `RefUnwindSafe`
// because we don't poison our `Mutex` upon panicking
// like `std::sync::Mutex` does.

impl<T> From<T> for Mutex<T> {
    fn from(value: T) -> Self {
        Self::new(value)
    }
}
