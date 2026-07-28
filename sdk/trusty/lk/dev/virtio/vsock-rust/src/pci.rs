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

#![deny(unsafe_op_in_unsafe_fn)]

use core::ffi::c_int;
use core::ptr;

use log::debug;

use virtio_drivers::device::socket::VirtIOSocket;
use virtio_drivers::device::socket::VsockConnectionManager;
use virtio_drivers::transport::pci::bus::Cam;
use virtio_drivers::transport::pci::bus::Command;
use virtio_drivers::transport::pci::bus::DeviceFunction;
use virtio_drivers::transport::pci::bus::PciRoot;
use virtio_drivers::transport::pci::virtio_device_type;
use virtio_drivers::transport::pci::PciTransport;
use virtio_drivers::transport::DeviceType;

use rust_support::mmu::ARCH_MMU_FLAG_PERM_NO_EXECUTE;
use rust_support::mmu::ARCH_MMU_FLAG_UNCACHED_DEVICE;
use rust_support::paddr_t;
use rust_support::thread::Builder;
use rust_support::thread::Priority;
use rust_support::vmm::vmm_alloc_physical;
use rust_support::vmm::vmm_get_kernel_aspace;
use rust_support::Error as LkError;

use crate::err::Error;
use crate::hal::TrustyHal;

impl TrustyHal {
    fn init_vsock(pci_root: &mut PciRoot, device_function: DeviceFunction) -> Result<(), Error> {
        let transport = PciTransport::new::<Self>(pci_root, device_function)?;
        let driver: VirtIOSocket<TrustyHal, PciTransport> = VirtIOSocket::new(transport)?;
        let _manager = VsockConnectionManager::new(driver); // TODO move

        Builder::new()
            .name(c"virtio_vsock_rx")
            .priority(Priority::HIGH)
            .spawn(move || {
                todo!("Call routine for virtio rx worker");
            })
            .map_err(|e| LkError::from_lk(e).unwrap_err())?;

        Builder::new()
            .name(c"virtio_vsock_tx")
            .priority(Priority::HIGH)
            .spawn(move || {
                todo!("Call routine for virtio tx worker");
            })
            .map_err(|e| LkError::from_lk(e).unwrap_err())?;

        Ok(())
    }

    fn init_all_vsocks(mut pci_root: PciRoot, pci_size: usize) -> Result<(), Error> {
        for bus in u8::MIN..=u8::MAX {
            // each bus can use up to one megabyte of address space, make sure we stay in range
            if bus as usize * 0x100000 >= pci_size {
                break;
            }
            for (device_function, info) in pci_root.enumerate_bus(bus) {
                if virtio_device_type(&info) != Some(DeviceType::Socket) {
                    continue;
                };

                // Map the BARs of the device into virtual memory. Since the mappings must
                // outlive the `PciTransport` constructed in `init_vsock` we no make no
                // attempt to deallocate them.
                Self::mmio_alloc(&mut pci_root, device_function)?;

                // Enable the device to use its BARs.
                pci_root.set_command(
                    device_function,
                    Command::IO_SPACE | Command::MEMORY_SPACE | Command::BUS_MASTER,
                );

                Self::init_vsock(&mut pci_root, device_function)?;
            }
        }
        Ok(())
    }
}

/// # Safety
///
/// `pci_paddr` must be a valid physical address with `'static` lifetime to the base of the MMIO region,
/// which must have a size of `pci_size`.
unsafe fn map_pci_root(
    pci_paddr: paddr_t,
    pci_size: usize,
    _cfg_size: usize,
) -> Result<PciRoot, Error> {
    // The ECAM is defined in Section 7.2.2 of the PCI Express Base Specification, Revision 2.0.
    // The ECAM size must be a power of two with the exponent between 1 and 8.
    let cam = Cam::Ecam;
    if !pci_size.is_power_of_two() || pci_size > cam.size() as usize {
        return Err(LkError::ERR_BAD_LEN.into());
    }
    // The ECAM base must be 2^(n + 20)-bit aligned.
    if pci_paddr & (pci_size - 1) != 0 {
        return Err(LkError::ERR_INVALID_ARGS.into());
    }

    // Map the PCI configuration space.
    let pci_vaddr = ptr::null_mut();
    // # Safety
    // `aspace` is `vmm_get_kernel_aspace()`.
    // `name` is a `&'static CStr`.
    // `pci_paddr` and `pci_size` are safe by this function's safety requirements.
    let e = unsafe {
        vmm_alloc_physical(
            vmm_get_kernel_aspace(),
            c"pci_config_space".as_ptr(),
            pci_size,
            &pci_vaddr,
            0,
            pci_paddr,
            0,
            ARCH_MMU_FLAG_PERM_NO_EXECUTE | ARCH_MMU_FLAG_UNCACHED_DEVICE,
        )
    };
    LkError::from_lk(e)?;

    // # Safety:
    // `pci_paddr` is a valid physical address to the base of the MMIO region.
    // `pci_vaddr` is the mapped virtual address of that.
    // `pci_paddr` has `'static` lifetime, and `pci_vaddr` is never unmapped,
    // so it, too, has `'static` lifetime.
    // We also check that the `cam` size is valid.
    let pci_root = unsafe { PciRoot::new(pci_vaddr.cast(), cam) };

    Ok(pci_root)
}

/// # Safety
///
/// See [`map_pci_root`].
#[no_mangle]
pub unsafe extern "C" fn pci_init_mmio(
    pci_paddr: paddr_t,
    pci_size: usize,
    cfg_size: usize,
) -> c_int {
    debug!("initializing vsock: pci_paddr 0x{pci_paddr:x}, pci_size 0x{pci_size:x}");
    || -> Result<(), Error> {
        // Safety: Delegated to `map_pci_root`.
        let pci_root = unsafe { map_pci_root(pci_paddr, pci_size, cfg_size) }?;
        TrustyHal::init_all_vsocks(pci_root, pci_size)?;
        Ok(())
    }()
    .err()
    .unwrap_or(LkError::NO_ERROR.into())
    .into_c()
}
