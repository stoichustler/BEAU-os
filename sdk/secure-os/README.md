# secure-os

> **Learning and research only.** This project is not a production-ready
> secure operating system. Do not use it for sensitive data, critical systems,
> or safety-related workloads.

`secure-os` is an ARM64 hypervisor learning project built on the seL4
microkernel. It shows how a small static system boots at EL2, creates seL4
objects, starts a monitor, and runs a Rust runtime.

The project supports AArch64 only. The default target is QEMU `virt` with four
Cortex-A53 CPUs.

## Architecture

```text
┌─────────────────────────────────────────────┐
│ QEMU virt · ARM64 · EL2                     │
├─────────────────────────────────────────────┤
│ VMOS loader                                 │
│  └─ loads the kernel and system image       │
├─────────────────────────────────────────────┤
│ seL4 microkernel                            │
│  ├─ capabilities and kernel objects         │
│  ├─ scheduling contexts                     │
│  └─ VCPU and stage-2 translation            │
├───────────────────┬─────────────────────────┤
│ CapDL initialiser │ Microkit monitor        │
│  └─ builds system │  └─ handles PD/VCPU     │
├───────────────────┴─────────────────────────┤
│ Rust runtime                                │
│  └─ PL011 IRQ console and test commands     │
└─────────────────────────────────────────────┘
```

This repository includes the seL4 source needed by the build and a standalone
VMOS component under `vmos/`. It does not require a sibling Microkit checkout.

## Scope

Included:

- seL4 ARM hypervisor support;
- AArch64 loader, initialiser, monitor, and protection-domain runtime;
- static system description and image packaging;
- Rust build support and source-integrity checks;
- an interrupt-driven Rust runtime console for QEMU.

Not included:

- a production security policy;
- full guest device emulation;
- secure update or key-management services;
- production hardening, certification, or long-term support.

The formal verification claims of upstream seL4 do not automatically apply to
the VMOS integration, build configuration, or Rust runtime in this repository.

## Layout

```text
include/ src/ libsel4/   seL4 kernel and ABI sources
tools/                   seL4 build tools
vmos/                    ARM64 hypervisor components
vmos/loader/             EL2 loader
vmos/initialiser/        CapDL initialiser
vmos/monitor/            PD and VCPU monitor
vmos/libmicrokit/        protection-domain runtime
vmos/tool/microkit/      system builder and image packager
vmos/support/            Rust build support
vmos/runtime/            Rust runtime console
Makefile.vmos            top-level build entry point
build_vmos.py            legacy build compatibility layer
```

Vendored seL4 and Microkit implementation files are treated as read-only.
Explanatory comments may be added, but new project features must be written in
Rust.

## Requirements

- Python 3 with the seL4 Python dependencies;
- CMake, Ninja, GNU Make, and `xmllint`;
- `aarch64-none-elf-` GCC/binutils;
- Rust/Cargo with the `aarch64-unknown-none` target;
- `qemu-system-aarch64` for runtime testing.

The build uses `~/.venvs/secure-os-vmos/bin/python3` when available. Override
it with `VMOS_VENV` or `PYTHON`.

## Build

Run from this directory:

```shell
make -f Makefile.vmos test
make -f Makefile.vmos verify-source
make -f Makefile.vmos build
```

The public ELF files are written to:

```text
build/vmos/qemu_virt_aarch64/debug/elf/
├── sel4.elf
├── loader.elf
├── monitor.elf
└── initialiser.elf
```

The deeper `sdk/` tree is internal compatibility state for the Microkit build
tools.

## Run on QEMU

Build and start the runtime:

```shell
make -f Makefile.vmos qemu
```

A successful boot reaches:

```text
LDR|INFO|CPU0: CurrentEL=EL2
Booting all finished, dropped to user space
MON|INFO: Microkit Monitor started!
VMOS|INFO: Rust runtime started
VMOS runtime console ready
vmos>
```

Console commands:

```text
help                 list commands
version              show runtime information
ping                 return pong
echo <text>          print text
selftest             run deterministic checks
stats                show command counters
```

The console uses QEMU PL011 IRQ 33 and a fixed 128-byte buffer. Press
`Ctrl-a x` to exit QEMU.

Build the image without starting QEMU:

```shell
make -f Makefile.vmos qemu-image
```

The image is written to:

```text
build/vmos/qemu_virt_aarch64/debug/qemu/loader.img
```

## Source Integrity

`vmos/UPSTREAM_SHA256` records the local vendored-source baseline:

```shell
make -f Makefile.vmos verify-source
```

The check is local and does not access another repository.

## License

seL4 and third-party source files keep their original copyright and license
notices. See [LICENSE.md](LICENSE.md).
