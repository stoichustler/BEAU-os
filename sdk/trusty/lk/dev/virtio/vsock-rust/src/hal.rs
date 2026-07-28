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

use core::ops::Deref;
use core::ops::DerefMut;
use core::ptr::NonNull;

use lazy_static::lazy_static;

use rust_support::mmu::ARCH_MMU_FLAG_PERM_NO_EXECUTE;
use rust_support::mmu::ARCH_MMU_FLAG_UNCACHED_DEVICE;
use rust_support::mmu::PAGE_SIZE_SHIFT;
use rust_support::paddr_t;
use rust_support::sync::Mutex;
use rust_support::vaddr_t;
use rust_support::vmm::vaddr_to_paddr;
use rust_support::vmm::vmm_alloc_contiguous;
use rust_support::vmm::vmm_alloc_physical;
use rust_support::vmm::vmm_free_region;
use rust_support::vmm::vmm_get_kernel_aspace;

use static_assertions::const_assert_eq;

use virtio_drivers::transport::pci::bus::DeviceFunction;
use virtio_drivers::transport::pci::bus::PciRoot;
use virtio_drivers::{BufferDirection, Hal, PhysAddr, PAGE_SIZE};

use crate::err::Error;

#[derive(Copy, Clone)]
struct BarInfo {
    paddr: paddr_t,
    size: usize,
    vaddr: vaddr_t,
}

const NUM_BARS: usize = 6;
lazy_static! {
    static ref BARS: Mutex<[Option<BarInfo>; NUM_BARS]> = Mutex::new([None; NUM_BARS]);
}

// virtio-drivers requires 4k pages, check that we meet requirement
const_assert_eq!(PAGE_SIZE, rust_support::mmu::PAGE_SIZE as usize);

pub struct TrustyHal;

impl TrustyHal {
    pub fn mmio_alloc(
        pci_root: &mut PciRoot,
        device_function: DeviceFunction,
    ) -> Result<(), Error> {
        for bar in 0..NUM_BARS {
            let bar_info = pci_root.bar_info(device_function, bar as u8).unwrap();
            if let Some((bar_paddr, bar_size)) = bar_info.memory_address_size() {
                let bar_vaddr = core::ptr::null_mut();
                let bar_size_aligned = (bar_size as usize + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

                // # Safety
                // `aspace` is `vmm_get_kernel_aspace()`.
                // `name` is a `&'static CStr`.
                // `bar_paddr` and `bar_size_aligned` are safe by this function's safety requirements.
                let ret = unsafe {
                    vmm_alloc_physical(
                        vmm_get_kernel_aspace(),
                        c"pci_config_space".as_ptr(),
                        bar_size_aligned,
                        &bar_vaddr,
                        0,
                        bar_paddr as usize,
                        0,
                        ARCH_MMU_FLAG_PERM_NO_EXECUTE | ARCH_MMU_FLAG_UNCACHED_DEVICE,
                    )
                };
                rust_support::Error::from_lk(ret)?;

                BARS.lock().deref_mut()[bar] = Some(BarInfo {
                    paddr: bar_paddr as usize,
                    size: bar_size_aligned,
                    vaddr: bar_vaddr as usize,
                });
            }
        }
        Ok(())
    }
}

unsafe impl Hal for TrustyHal {
    // Safety:
    // Function either returns a non-null, properly aligned pointer or panics the kernel.
    // The call to `vmm_alloc_contiguous` ensures that the pointed to memory is zeroed.
    fn dma_alloc(pages: usize, _direction: BufferDirection) -> (PhysAddr, NonNull<u8>) {
        let name = c"vsock-rust";
        // dma_alloc requests num pages but vmm_alloc_contiguous expects bytes.
        let size = pages * PAGE_SIZE as usize;
        let mut vaddr = core::ptr::null_mut(); // stores pointer to virtual memory
        let align_pow2 = PAGE_SIZE_SHIFT as u8;
        let vmm_flags = 0;
        let arch_mmu_flags = 0;
        let aspace = vmm_get_kernel_aspace();

        // NOTE: the allocated memory will be zeroed since vmm_alloc_contiguous
        // calls vmm_alloc_pmm which does not set the PMM_ALLOC_FLAG_NO_CLEAR
        // flag.
        //
        // # Safety
        // `aspace` is `vmm_get_kernel_aspace()`.
        // `name` is a `&'static CStr`.
        // `size` is validated by the callee
        let rc = unsafe {
            vmm_alloc_contiguous(
                aspace,
                name.as_ptr(),
                size,
                &mut vaddr,
                align_pow2,
                vmm_flags,
                arch_mmu_flags,
            )
        };
        if rc != 0 {
            panic!("error {} allocating physical memory", rc);
        }
        if vaddr as usize & (PAGE_SIZE - 1usize) != 0 {
            panic!("error page-aligning allocation {:#x}", vaddr as usize);
        }

        // Safety: `vaddr` is valid because the call to `vmm_alloc_continuous` succeeded
        let paddr = unsafe { vaddr_to_paddr(vaddr) };

        (paddr, NonNull::<u8>::new(vaddr as *mut u8).unwrap())
    }

    unsafe fn dma_dealloc(_paddr: PhysAddr, vaddr: NonNull<u8>, _pages: usize) -> i32 {
        // TODO: store pointers allocated with dma_alloc to validate the args
        let aspace = vmm_get_kernel_aspace();
        vmm_free_region(aspace, vaddr.as_ptr() as _)
    }

    // Only used for MMIO addresses within BARs read from the device,
    // for the PCI transport.
    //
    // Safety: `paddr` and `size` are validated against allocations made in
    // `Self::mmio_alloc`; panics on validation failure.
    unsafe fn mmio_phys_to_virt(paddr: PhysAddr, size: usize) -> NonNull<u8> {
        for bar in BARS.lock().deref().iter().flatten() {
            let bar_paddr_end = bar.paddr + bar.size;
            if (bar.paddr..bar_paddr_end).contains(&paddr) {
                // check that the address range up to the given size is within
                // the region expected for MMIO.
                if paddr + size > bar_paddr_end {
                    panic!("invalid arguments passed to mmio_phys_to_virt");
                }
                let offset: isize = (paddr - bar.paddr).try_into().unwrap();

                let bar_vaddr_ptr: *mut u8 = bar.vaddr as _;
                return NonNull::<u8>::new(bar_vaddr_ptr.offset(offset)).unwrap();
            }
        }

        panic!("error mapping physical memory to virtual for mmio");
    }

    unsafe fn share(buffer: NonNull<[u8]>, _direction: BufferDirection) -> PhysAddr {
        // no-op on x86_64, not implemented on other architectures
        #[cfg(not(target_arch = "x86_64"))]
        unimplemented!();

        vaddr_to_paddr(buffer.as_ptr().cast())
    }

    // Safety: no-op on x86-64, panic elsewhere.
    unsafe fn unshare(_paddr: PhysAddr, _buffer: NonNull<[u8]>, _direction: BufferDirection) {
        // no-op on x86_64, not implemented on other architectures
        #[cfg(not(target_arch = "x86_64"))]
        unimplemented!();
    }
}
