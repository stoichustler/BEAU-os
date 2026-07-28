# BEAU QEMU Trusty Bring-up Record

## Target

The QEMU secure boot chain is intended to run as:

```text
TF-A BL1/BL2/BL31 -> Trusty LK BL32 -> BEAU EL2 BL33 -> four BEAU VMs
```

The Trusty LK image is loaded at `0x0e100000` and owns the QEMU secure DRAM
range of `0x00f00000` bytes. BEAU is preloaded as BL33 at `0x50000000`.

## Root Cause And Evidence

- TF-A loads BL32 and reports a 64-bit Trusty image.
- BL32 CPU0 starts and releases the first LK trampoline lock at
  `0x0e1200c0` with value zero.
- BEAU CPU0 starts successfully and its PSCI `CPU_ON` reaches BL32 CPU1.
- BL32 CPU1 reads the same first-lock address with value zero, enables its
  early MMU state, and reaches the second LK lock at `0x0e1200c4`.
- CPU1 reads the second lock as one because CPU0 never releases it.
- CPU0 faults in `memset()` while `arm64_early_mmu_init()` builds the final
  kernel mapping. The reported syndrome is `ESR_EL1=0x96000044`, a same-EL
  level-0 translation fault, at `FAR_EL1=0xffff000000035000`.
- `0xffff000000035000` is LK's linked `_end`, which is the initial
  `boot_alloc_end` value. The page-table allocator requires its physical
  address before the final high virtual mapping exists.

The QEMU profile keeps `PIE_KERNEL=false` because the requested
`aarch64-none-elf-ld` linker cannot emit Trusty's RELR records. The profile
therefore enables `BOOT_ALLOC_RELOCATE_EARLY`. Around
`vm_map_initial_mappings()`, this switch converts the boot allocator bounds
from the linked virtual base to the physical base, then converts them back to
the deterministic final virtual base. This is the subset of relocation needed
before the final kernel mapping exists. `KERNEL_BASE_ASLR=false` remains
intentional.

## Ruled Out

- QEMU CPU topology: fixed eight-core topology does not change the failure.
- BEAU PSCI CPU_ON: BL32 CPU1 reset code executes.
- First-lock publication and address agreement: both CPUs observe zero at the
  same physical address.
- BL32 memory size mismatch: TF-A `SEC_DRAM_SIZE` and LK `MEMSIZE` are both
  `0x00f00000`.

## Diagnostic Cleanup

The temporary SMC character, lock, and halt traces were removed after the
fault was identified. They invoked an unimplemented TF-A service and are not
part of the BL32 runtime contract. The QEMU debug stub now emits no traffic
until a separately designed secure console transport is added.

## Manual Validation

Run the image manually with:

```sh
python3 scripts/kick.py --tee --smp 8
```

The passing boot must show every BEAU pCPU online, no `trusty_smc_handler`
unknown-SMC diagnostics, and no BEAU DDB panic. The subsequent VM messages
must show all four configured VMs being created and started.
