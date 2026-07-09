---
name: beau-hypervisor-dev
description: Develop, debug, validate, and document the BEAU hypervisor in this repository, especially the ARM64 QEMU virt and rk356x bring-up, static VM configuration, stage-2 identity mappings, vCPU scheduling/shared-pCPU behavior, vGIC/vPL011/vUART console paths, hypercalls, shell commands, and SDK documentation. Use when Codex is asked to modify BEAU hypervisor code, investigate VM boot/runtime issues, add observability commands, update ARM64 virtualization logic, or run QEMU/build validation for this tree.
---

# BEAU Hypervisor Development

## Core Workflow

1. Read local context first:
   - Start with `sdk/sdk.md` for current architecture, verified behavior, and limitations.
   - Read only the relevant source files after that; use `rg` before broad file reads.
   - Treat the repository as possibly dirty. Do not revert unrelated changes.
   - Before making any file modification, explicitly notify the user in the
     conversation which files or areas will be changed and why. Do not edit
     silently.
   - The project skill can be combined with other available skills when they
     are explicitly requested or their trigger rules apply; `superpowers` is
     available and not excluded from this repository workflow.

2. Keep ARM64 platform bring-up simple:
   - Preserve static VM configuration unless the user explicitly asks for dynamic policy.
   - Preserve QEMU RTOS stage-2 RAM identity mapping: guest IPA/GPA equals HPA/PA.
   - Keep Zephyr/LK RTOS boot paths independent of external ACPI/FDT modules.
   - Keep LK and Zephyr image inputs under `sdk/image`, and keep Linux
     `Image`, `Initramfs.cpio.gz`, `beau-linux.dts`, and `beau-linux.dtb` under
     `sdk/image/linux`.
   - Do not move Linux `Image` or `Initramfs.cpio.gz` into `.incbin`; use loader/module
     delivery. The Linux-on-BEAU DTB may remain an embedded module.

3. Make focused changes:
   - Follow existing module boundaries: `arch/arm64` for architecture logic, `core` for common scheduler/vCPU/VM logic, and `sdk/bsp` for shell, console, boot loaders, and BSP-owned device emulation.
   - Newly added code must include succinct English comments that explain the
     underlying principle and the purpose of the code, especially around boot,
     interrupt, timer, scheduler, vCPU, and console behavior.
   - Add English comments for design intent, architecture invariants, ownership, and failure modes.
   - For touched ARM64 virtualization modules, write compact comments that include:
     date, involved-module principle, and code-flow analysis.
   - Pair non-obvious comments with `sdk/box-drawing.md`-style ASCII flow/framework
     diagrams so the reader can follow ownership and state transitions quickly.
   - Keep these diagrams small and close to the relevant code; prefer one
     focused flow over broad architecture narration.
   - Avoid comments that restate assignments or obvious branches.
   - Do not write machine-local directory or file paths into documentation,
     code design notes, or this skill. Use variables such as `BEAU_TOOLCHAINS`
     or repository-relative paths instead.

4. Validate before final response:
   - Before any build or regression, confirm `BEAU_TOOLCHAINS` is set and
     points to a valid bare-metal toolchain bin directory.
   - After any hypervisor code update, run the matching build validation before
     the final response and report the result.
   - Build ARM64 QEMU:
     ```bash
     ./scripts/kick.py --build --dry-run
     ./scripts/kick.py --build
     ```
   - Build rk356x when changing platform selection, common ARM64 platform
     guards, guest image embedding, or rk356x files:
     ```bash
     PATH=${BEAU_TOOLCHAINS}:$PATH \
     make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
     ```
   - Expect platform-scoped default output directories:
     `out/qemu_out` for QEMU and `out/rk356x_out` for rk356x.
   - For debug/shell/console changes, force rebuild stale debug artifacts first:
     ```bash
     rm -f out/qemu_out/modules/libdebug.a out/qemu_out/beau.out out/qemu_out/beau.debug.out out/qemu_out/beau.debug.bin
     ```
   - Check QEMU command:
     ```bash
     ./scripts/kick.py --dry-run
     ```
   - Run the boot regression when touching boot, scheduler, IRQ, memory-map, or
     console behavior:
     ```bash
     ./scripts/regress.py
     ```
   - Run `./scripts/kick.py` for boot/runtime validation when behavior changes.

## Common Tasks

### VM Boot Or Bring-Up Bugs

- Inspect `arch/arm64/platform/<platform>/platform.dts`, `arch/arm64/guest/vm.c`, `sdk/bsp/boot/guest/vboot_info.c`, and `sdk/bsp/boot/guest/rawimage_loader.c`.
- Verify VM RAM windows, load/entry addresses, `GUEST_FLAG_NO_FW`, static vFDT placement, and stage-2 identity logs.
- In QEMU, check `vcpus`, `schedstat`, `vsh 0`, `vsh 1`, and VM2 Linux
  reaching the initramfs `uos` root shell through `vsh 2`.

### rk356x Platform Work

- Keep rk356x-specific RAM, GIC, UART, and VM layout data under
  `arch/arm64/platform/rk356x`.
- Keep guest image embedding pointed at `sdk/image/lk.bin`,
  `sdk/image/zephyr.bin`, and small DTB modules. Keep Linux `Image` and
  `Initramfs.cpio.gz` under `sdk/image` but load them through module/loader staging.
- Treat rk356x hardware validation as manual flashing and serial-log inspection
  until an automated board workflow exists.
- Do not add board flashing commands or local artifact paths to documentation
  or skills.

### Shared pCPU Scheduling

- Inspect `core/vm.c`, `core/schedule.c`, `core/sched_iorr.c`, and ARM64 vCPU context switch files.
- Keep VM0 on ordinary pCPUs and allow VM1 to mix ordinary/performance pCPUs.
- Ensure shared pCPUs are time-sliced by scheduler behavior, not by adding ad hoc VM placement hacks.
- Use `schedstat` to compare scheduler `timer`, `resched`, and `switches`.

### Console Or Shell Work

- Inspect `sdk/bsp/shell.c`, `sdk/bsp/shell_priv.h`, `sdk/bsp/console.c`, and `arch/arm64/guest/vpl011.c`.
- For `vsh`, make ownership markers appear before replaying buffered VM output.
- Remember selected and non-selected VM console output use per-VM async rings.
- Keep shell observability fields actionable for live diagnosis. Do not add
  cumulative time totals or averaged-from-total latency fields to shell command
  output by default; prefer current, recent, and high-water values such as
  `since.us`, `lastwait.us`, and `maxwait.us`. Interrupt counts and scheduler
  or vCPU `switches` are allowed as event-count exceptions.

### Hypercall Work

- Inspect `arch/arm64/guest/hypercall.c`, `arch/arm64/guest/vcpu_exit.c`, and common hypercall headers.
- Keep ARM64 hypercall dispatch explicit and small.
- Return `-ENOTTY` for unimplemented calls until their ARM64 dependencies exist.

## References

Read these only when needed:

- `references/architecture.md`: current ARM64 QEMU/rk356x architecture,
  invariants, commands, and validation checklist.
