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

use alloc::boxed::Box;
use core::ffi::c_int;
use core::ffi::c_void;
use core::ffi::CStr;
use core::fmt;
use core::fmt::Display;
use core::fmt::Formatter;
use core::ops::Add;
use core::ops::Sub;
use core::ptr::NonNull;

use crate::Error;

use crate::sys::thread_create;
use crate::sys::thread_resume;
use crate::sys::thread_t;
use crate::sys::DEFAULT_PRIORITY;
use crate::sys::DPC_PRIORITY;
use crate::sys::HIGHEST_PRIORITY;
use crate::sys::HIGH_PRIORITY;
use crate::sys::IDLE_PRIORITY;
use crate::sys::LOWEST_PRIORITY;
use crate::sys::LOW_PRIORITY;
use crate::sys::NUM_PRIORITIES;

use crate::sys::DEFAULT_STACK_SIZE;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct Priority(c_int);

impl Priority {
    pub const NUM: usize = NUM_PRIORITIES as _;

    pub const LOWEST: Self = Self(LOWEST_PRIORITY as _);
    pub const HIGHEST: Self = Self(HIGHEST_PRIORITY as _);
    pub const DPC: Self = Self(DPC_PRIORITY as _);
    pub const IDLE: Self = Self(IDLE_PRIORITY as _);
    pub const LOW: Self = Self(LOW_PRIORITY as _);
    pub const DEFAULT: Self = Self(DEFAULT_PRIORITY as _);
    pub const HIGH: Self = Self(HIGH_PRIORITY as _);
}

impl Default for Priority {
    fn default() -> Self {
        Self::DEFAULT
    }
}

#[derive(Debug)]
pub enum PriorityError {
    TooLow(c_int),
    TooHigh(c_int),
}

impl Display for PriorityError {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        match self {
            PriorityError::TooLow(val) => write!(f, "{val} < {}", Priority::LOWEST.0),
            PriorityError::TooHigh(val) => write!(f, "{val} > {}", Priority::HIGHEST.0),
        }
    }
}

impl TryFrom<c_int> for Priority {
    type Error = PriorityError;

    fn try_from(value: c_int) -> Result<Self, Self::Error> {
        if value < Priority::LOWEST.0 {
            Err(PriorityError::TooLow(value))
        } else if value > Priority::HIGHEST.0 {
            Err(PriorityError::TooHigh(value))
        } else {
            Ok(Priority(value))
        }
    }
}

impl Add<c_int> for Priority {
    type Output = Self;

    fn add(self, other: c_int) -> Self {
        match self.0.checked_add(other).map(Self::try_from) {
            None => panic!("priority overflow"),
            Some(Err(reason)) => panic!("priority out of range: {reason}"),
            Some(Ok(priority)) => priority,
        }
    }
}

impl Sub<c_int> for Priority {
    type Output = Self;

    fn sub(self, other: c_int) -> Self {
        match self.0.checked_sub(other).map(Self::try_from) {
            None => panic!("priority overflow"),
            Some(Err(reason)) => panic!("priority out of range: {reason}"),
            Some(Ok(priority)) => priority,
        }
    }
}

pub struct JoinHandle {
    _thread: NonNull<thread_t>,
}

#[derive(Debug)]
pub struct Builder<'a> {
    pub name: Option<&'a CStr>,
    pub priority: Priority,
    pub stack_size: usize,
}

impl<'a> Default for Builder<'a> {
    fn default() -> Self {
        Self::new()
    }
}

impl<'a> Builder<'a> {
    pub const fn new() -> Self {
        Self { name: None, priority: Priority::DEFAULT, stack_size: DEFAULT_STACK_SIZE as _ }
    }

    pub fn name(mut self, name: &'a CStr) -> Self {
        self.name = Some(name);
        self
    }

    pub fn priority(mut self, priority: Priority) -> Self {
        self.priority = priority;
        self
    }

    pub fn stack_size(mut self, stack_size: usize) -> Self {
        self.stack_size = stack_size;
        self
    }

    pub fn spawn<F>(self, f: F) -> Result<JoinHandle, i32>
    where
        F: FnOnce() -> c_int + Send + 'static,
    {
        let name = self.name.unwrap_or(c"thread");
        // We need a pointer to f that lasts until `thread_entry_wrapper`
        // gets called. `thread_resume` does not wait for the new
        // thread to run. Thus, passing the address of a local
        // wouldn't live long enough so we heap allocate instead.
        let f = Box::new(f);

        extern "C" fn thread_entry_wrapper<F>(arg: *mut c_void) -> c_int
        where
            F: FnOnce() -> c_int + Send + 'static,
        {
            // SAFETY:
            // We passed in a `Box<F>`.
            // `thread_entry_wrapper` is called exactly once per thread.
            let f = unsafe { Box::<F>::from_raw(arg as _) };
            f()
        }

        // SAFETY:
        // `name` outlives the call to `thread_create` during which the
        // string is copied into the newly created thread structure.
        //
        // `arg`: The lifetime of `Box<F>` lasts until the end of the
        // call to `thread_entry_wrapper`. The trusty kernel will pass `arg`
        // to `thread_entry_wrapper` exactly once per thread.
        let thread = unsafe {
            thread_create(
                name.as_ptr(),
                Some(thread_entry_wrapper::<F>),
                Box::<F>::into_raw(f) as *mut c_void,
                self.priority.0,
                self.stack_size,
            )
        };
        let thread = NonNull::new(thread).ok_or::<i32>(Error::ERR_GENERIC.into())?;
        // SAFETY: `thread` is non-null, so `thread_create` initialized it properly.
        let status = unsafe { thread_resume(thread.as_ptr()) };
        if status == Error::NO_ERROR.into() {
            Ok(JoinHandle { _thread: thread })
        } else {
            Err(status)
        }
    }
}

pub fn spawn<F>(f: F) -> Result<JoinHandle, i32>
where
    F: FnOnce() -> c_int + Send + 'static,
{
    Builder::new().spawn(f)
}
