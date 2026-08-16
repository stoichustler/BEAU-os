# VMOS ARM64 Hypervisor Design

## Scope

`vmos/` is a standalone ARM64 hypervisor framework containing a vendored
Microkit source baseline. Existing seL4 source files remain unchanged. The
vendored implementation is treated as read-only apart from explanatory
comments; new project functionality is implemented in the separate Rust
support crate.

The component supplies Microkit's VM/vCPU infrastructure. It does not add a
device model, guest-specific VMM policy, or example guest system.

## Source layout

The copied dependency closure is:

- `vmos/loader`: common loader sources and the AArch64 implementation;
- `vmos/initialiser`: the CapDL initialiser;
- `vmos/libmicrokit`: the runtime library, vCPU API, and linker script;
- `vmos/monitor`: common monitor sources and the AArch64 entry code;
- `vmos/tool/microkit`: the system-description parser, VM/vCPU and stage-2
  object builder, and image packaging tool.

Examples, documentation, SDK release packaging, RISC-V assembly/runtime files,
x86 assembly/runtime files, and upstream test fixtures are not copied. The
host-side Rust tool remains internally multi-architecture because its source is
intertwined; the VMOS build interface accepts only AArch64 targets.

## Build architecture

The overall build entry points live at the current seL4 repository root rather
than inside `vmos/` or by modifying seL4's existing `CMakeLists.txt`.

```text
Makefile.vmos
    |
    v
validate host tools and ARM64 board configuration
    |
    +--> configure/build/install current seL4
    |      AArch64 + MCS + ARM hypervisor support
    |
    +--> build Microkit host tool and CapDL initialiser with Cargo
    |
    +--> build loader, monitor, and libmicrokit with their upstream Makefiles
    |
    v
internal SDK: build/vmos/<board>/<config>/sdk/
    |
    v
Rust publisher --> build/vmos/<board>/<config>/elf/
                   sel4.elf, loader.elf, monitor.elf, initialiser.elf
```

`qemu_virt_aarch64` is the default board. Other ARM64 seL4 platforms may be
selected through build variables when their CPU and loader address are
provided. Non-AArch64 selections fail before configuration starts.

## Runtime and generation flow

```text
system description
    |
    v
Microkit host tool --> CapDL objects, vCPUs, stage-2 mappings, boot image
                            |
                            v
loader --> seL4 --> initialiser --> monitor + protection domains / VM vCPUs
```

The initialiser creates and distributes capabilities described by the host
tool. The monitor owns fault supervision. `libmicrokit` provides the protection
domain entry loop and ARM vCPU control API. seL4 remains the owner of VCPU
objects, scheduling contexts, stage-2 mappings, and hardware guest execution.

## Failure handling

The build fails closed when a required host tool or cross target is absent, a
board is not ARM64, seL4 hypervisor configuration is missing, a component build
fails, or a required output is absent. Partial products stay under `build/` and
are never copied over source files.

At runtime, fault handling is the unchanged Microkit behavior: the monitor
diagnoses unhandled faults, and the protection-domain runtime resumes a vCPU
only when its fault handler explicitly permits it.

## Verification

Verification covers four boundaries:

1. A SHA-256 manifest checks the vendored Microkit implementation entirely
   within this repository; no external checkout is required.
2. Git checks confirm no pre-existing tracked seL4 file changed.
3. Unit tests exercise build argument validation and required-output checks.
4. A build smoke test produces the ARM64 hypervisor kernel and all VMOS
   components, then verifies ELF architecture and
   `CONFIG_ARM_HYPERVISOR_SUPPORT`.
