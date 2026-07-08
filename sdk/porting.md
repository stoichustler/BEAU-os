# Hardware Platform Porting Guide

This guide describes how to add or maintain an ARM64 hardware platform in
BEAU. It is written for non-Intel ARM64 platform work and assumes the target
platform follows the current static bring-up model used by QEMU and rk356x.

## Scope

- Add a platform under `arch/arm64/platform/<platform>`.
- Keep architecture-neutral ARM64 virtualization code under `arch/arm64`.
- Keep guest image inputs in `sdk/image`.
- Keep build outputs in `out/$(PLATFORM)_out`.
- Validate behavior first on QEMU where possible, then on hardware by manual
  flashing and serial-log inspection.

Do not document machine-local paths. Use repository-relative paths or
environment variables such as `BEAU_TOOLCHAINS`.

## Prerequisites

Set `BEAU_TOOLCHAINS` to the bare-metal toolchain bin directory before any
build:

```bash
test -n "${BEAU_TOOLCHAINS}" && test -d "${BEAU_TOOLCHAINS}"
```

Build commands should put the toolchain at the front of `PATH`:

```bash
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=<platform> CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

For QEMU comparison, use:

```bash
./scripts/kick.py --build
./scripts/regress.py --no-build
```

## Platform Directory

A hardware platform should be self-contained under:

```text
arch/arm64/platform/<platform>/
```

The current pattern is:

```text
platform.dts
platform.S
```

`arch/arm64/Makefile` selects `arch/arm64/platform/<platform>/platform.S` by
`PLATFORM`. Common ARM64 BSP code parses the embedded `platform.dtb`.

## Platform Data

The platform DTS supplies hardware and VM layout data through common ARM64 BSP
parsing. Add or update:

- Host RAM and MMIO ranges.
- GIC distributor base.
- GIC redistributor base and stride.
- Guest RAM start, size, and HPA.
- Guest vGICD/vGICR IPA windows.
- Guest PL011 IPA window and IRQ.
- Static vFDT device descriptions.

Keep board policy in `platform.dts`. Use static C header values only for
compile-time limits that cannot come from DTS, such as `MAX_PCPU_NUM`.

## Guest Images

LK and Zephyr stay as RTOS `.incbin` images:

```text
sdk/image/lk.bin
sdk/image/zephyr.bin
```

VM2 Linux uses repository-local images staged by the platform launcher or board
loader, then copied by BEAU:

```text
sdk/image/linux/vm1/Image
sdk/image/linux/vm2/Image
sdk/image/linux/Initramfs.cpio.gz
```

The Linux-on-BEAU DTB remains a small embedded module:

```text
sdk/image/linux/vm1/beau-linux.dtb
sdk/image/linux/vm2/beau-linux.dtb
```

`platform.S` should use `.incbin` only for LK, Zephyr, `platform.dtb`, and small DTB
modules. Do not add Linux `Image` or `Initramfs.cpio.gz` as `.incbin` inputs; use
loader/module delivery for those files.

## VM Configuration

Define static VM layout in:

```text
arch/arm64/platform/<platform>/platform.dts
```

For each VM, verify:

- `cpu_affinity` matches the intended pCPU placement.
- `kernel_mod_tag` matches the image tag in `platform.S`.
- `kernel_load_addr` and `kernel_entry_addr` fit the VM RAM window.
- `guest_ram_start`, `guest_ram_size`, and `guest_ram_hpa` match stage-2
  identity mapping expectations.
- vGICD, vGICR, and vPL011 IPAs do not overlap guest RAM.

For guest RAM, the current QEMU model is:

```text
guest IPA/GPA == host PA/HPA
```

If a platform cannot use this identity layout, update the loader and stage-2
design first. Do not hide non-identity mappings inside platform constants.

## Static vFDT

Each static ARM64 VM receives a synthetic vFDT. The platform should describe:

- `/cpus` nodes with PSCI `enable-method`.
- `/psci` using `method = "hvc"`.
- `/memory`.
- GICv3 interrupt controller.
- ARM generic timer.
- PL011 UART and UART clock.
- `/chosen/stdout-path`.

The vFDT should describe guest-visible virtual hardware, not host board internals
unless they are intentionally exposed to the guest.

## Build Outputs

The default output directory is platform-scoped:

```text
out/<platform>_out/
```

Expected debug artifacts include:

```text
out/<platform>_out/beau.out
out/<platform>_out/beau.debug.out
out/<platform>_out/beau.debug.bin
```

Do not design scripts or documentation around machine-local output locations.

## QEMU Cross-Check

Before hardware work, keep the QEMU baseline passing:

```bash
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)

./scripts/regress.py --no-build
```

QEMU validation should cover:

- BEAU shell prompt.
- `vcpus`, `schedstat`, `mmap`, `irqstat`, and `dumpstat`.
- `vsh 0` reaching the Zephyr shell.
- `vsh 1` reaching the LK shell.
- `vsh 2` reaching the Linux initramfs `uos` root shell and `id` reporting
  `uid=0`.
- No `[cut here]`, `unexpected arm64 trap`, or `unhandled arm64 vcpu exit`.

## Hardware Validation

Hardware validation is manual for now. Use the board flashing workflow for the
generated BEAU image and inspect serial logs.

Minimum hardware checks:

- EL2 entry reaches the BEAU banner.
- Host stage-1 mappings are printed and MMU enablement succeeds.
- GIC distributor and redistributor initialization succeeds on all pCPUs.
- Host secondary pCPUs enter the running state.
- The BEAU shell reaches `console:\>`.
- Static VMs are created and started.
- Guest stage-2 RAM maps match the platform VM configuration.
- vPL011 output is visible through `vsh`.
- Guest PSCI `CPU_ON` can bring up configured guest AP vCPUs.

If hardware fails before the BEAU shell, compare the serial log against the
QEMU boot sequence and isolate the first missing milestone.

## rk356x Notes

The rk356x platform currently provides a hardware-platform skeleton for manual
board bring-up:

```text
arch/arm64/platform/rk356x/
```

Build it with:

```bash
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

The generated artifacts are under:

```text
out/rk356x_out/
```

QEMU remains the automated interaction baseline. rk356x hardware interaction is
validated by manual flashing and serial-log inspection until an automated board
runner exists.

## Porting Checklist

- Add platform sources under `arch/arm64/platform/<platform>`.
- Add `PLATFORM=<platform>` source selection in `arch/arm64/Makefile`.
- Keep headers and new ARM64 platform files marked as
  `Copyright (C) 2026 Hustler Lo.` with `SPDX-License-Identifier: BSD-3-Clause`.
- Keep guest images under `sdk/image`.
- Keep platform output under `out/$(PLATFORM)_out`.
- Confirm `BEAU_TOOLCHAINS` before builds.
- Build QEMU and run regression before hardware validation.
- Build the hardware platform target.
- Flash manually and inspect serial logs.
- Record only repository-relative paths and command templates in docs.
- Keep board-specific constants out of common ARM64 virtualization code.

## Troubleshooting

Common first checks:

- Missing BEAU banner: inspect EL2 entry, stack setup, linker address, and image
  load address.
- MMU fault before shell: inspect host stage-1 RAM/MMIO ranges and page-table
  attributes.
- Secondary pCPU hang: inspect PSCI CPU_ON parameters, MPIDR mapping, and
  per-pCPU stack selection.
- Guest entry fault: inspect VM RAM window, raw-image load address, entry
  address, and stage-2 identity map.
- Missing guest timer interrupts: inspect CNTP/CNTV ownership, vGIC PPI state,
  and guest timer PPI selection.
- Missing guest console: inspect vPL011 IPA, IRQ, vFDT stdout path, and `vsh`
  selection state.

When a QEMU regression failure is intermittent, keep the first failing log and
compare it with the latest passing log. Do not paper over guest or hardware
faults by extending timeouts without first checking vCPU state and interrupt
delivery.
