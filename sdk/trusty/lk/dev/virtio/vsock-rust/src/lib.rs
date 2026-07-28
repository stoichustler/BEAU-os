#![no_std]
#![allow(non_camel_case_types)]
#![feature(cfg_version)]
// C string literals were stabilized in Rust 1.77
#![cfg_attr(not(version("1.77")), feature(c_str_literals))]

mod err;
mod hal;
mod pci;
