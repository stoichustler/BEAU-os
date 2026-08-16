# VMOS ARM64 Hypervisor

VMOS is the standalone ARM64 hypervisor component for this seL4 tree. It
contains the required Microkit runtime and system-generation sources, and does
not require a sibling Microkit checkout to build or test.

Vendored implementation files are treated as read-only; explanatory changes
to them are limited to comments. New VMOS-specific functionality is written in
Rust under `support/`. The existing Python builder remains a legacy
compatibility entry point and does not receive new feature logic.

## Components

```text
system description
    |
    v
microkit tool --> CapDL initialiser --> seL4 ARM64 VCPU objects
                                        |
loader --> seL4 --> monitor + libmicrokit protection domains / VM vCPUs
```

- `loader` boots the generated image and enters seL4 at EL2.
- `initialiser` creates the statically described kernel objects and capabilities.
- `monitor` supervises protection-domain and vCPU faults.
- `libmicrokit` provides the protection-domain runtime and ARM vCPU APIs.
- `tool/microkit` parses system descriptions, creates stage-2 mappings and vCPU
  objects, and packages boot images.

VMOS does not add guest device emulation or guest-specific VMM policy.

## Prerequisites

- Python 3, CMake, Ninja, and GNU Make
- `qemu-system-aarch64` for the `qemu` smoke-boot target
- `xmllint` (`libxml2-utils` on Ubuntu)
- seL4 Python dependencies (`python3 -m pip install ./tools/python-deps`)
- `aarch64-none-elf-` GCC/binutils
- Rust/Cargo compatible with the versions declared in `Cargo.toml`
- the Rust `aarch64-unknown-none` target

## Build

Run from the seL4 repository root:

```shell
make -f Makefile.vmos test
make -f Makefile.vmos build
```

The Makefile automatically uses `~/.venvs/secure-os-vmos/bin/python3` when
that virtual environment exists. Override `VMOS_VENV` or `PYTHON` for a
different environment.

The default is `qemu_virt_aarch64`, four Cortex-A53 CPUs, and the debug
configuration. Override build variables when selecting another ARM64 seL4
platform:

```shell
make -f Makefile.vmos build \
    BOARD=my_arm64_board \
    KERNEL_PLATFORM=my-sel4-platform \
    GCC_CPU=cortex-a55 \
    LOADER_LINK_ADDRESS=0x70000000 \
    CPUS=4 CONFIG=release
```

Platform-specific CMake definitions may be passed through `CMAKE_DEFS`, for
example `CMAKE_DEFS='--cmake-def KernelARMPlatform=my-platform'`.

### QEMU smoke boot

Build, package, and boot the Rust smoke protection domain on the ARM64 QEMU
`virt` machine with:

```shell
make -f Makefile.vmos qemu
```

The target uses four Cortex-A53 CPUs, 2 GiB RAM, EL2 virtualization, and the
serial console. A successful complete boot ends with:

```text
MON|INFO: Microkit Monitor started!
VMOS|INFO: Rust smoke protection domain started
VMOS runtime console ready
vmos>
```

The console is driven by QEMU's PL011 receive interrupt and supports:

```text
help                 list commands
version              identify the VMOS runtime
ping                 return pong for a liveness check
echo <text>          print text
selftest             run deterministic runtime checks
stats                show recognized, unknown, and overflowed command counts
```

Input is printable ASCII with CR/LF and backspace/delete editing. Lines longer
than 127 bytes are rejected and the next prompt starts with an empty buffer.
The console is diagnostic only: it deliberately has no reboot command or
arbitrary-memory access.

Exit QEMU with `Ctrl-a x`. Use `make -f Makefile.vmos qemu-image` to package
`build/vmos/qemu_virt_aarch64/debug/qemu/loader.img` without starting QEMU.
Override the executable or memory size with `QEMU=...` or `QEMU_MEMORY=...`.

The primary image outputs are:

```text
build/vmos/<board>/<config>/elf/sel4.elf
build/vmos/<board>/<config>/elf/loader.elf
build/vmos/<board>/<config>/elf/monitor.elf
build/vmos/<board>/<config>/elf/initialiser.elf
```

The complete Microkit-compatible SDK remains at
`build/vmos/<board>/<config>/sdk/` as internal build state. It contains
`libmicrokit.a`, headers, linker script, metadata, the host `microkit` tool,
and the board-specific ELF staging directory required by that tool.

## Upstream integrity

`UPSTREAM_SHA256` is the in-repository checksum baseline for the vendored
implementation. Verify it with:

```shell
make -f Makefile.vmos verify-source
```

Verification is entirely local and does not read another repository. Do not
change listed implementation logic. If an allowed explanatory comment is
added, review it deliberately, update the corresponding checksum, and rerun
the verifier and full test suite. Put new functional code in Rust outside the
listed vendored files.
