# BEAU OS

## Code Hierarchy

```
.
├── arch
│   ├── arm64            // Arm64
│   └── lds
├── core                 // BEAU OS core services
├── include
├── lib
├── scripts
└── sdk
    ├── beau             // Documentation
    ├── bsp              // BEAU dev/vdev drivers
    ├── image
    ├── kbe              // BEAU general-purpose Linux drivers
    ├── ube              // Userspace backend (not used yet)
    └── zsh              // BEAU general-purpose Zephyr applications
```

## Coding Spec

1. Comments should explain design intent, ownership, ordering, isolation, and
   failure handling. Do not restate simple assignments, branch conditions, or
   function names.

2. Add a short framework diagram or flowchart when code crosses subsystem
   boundaries, such as VM/vCPU state, stage-2 memory, SMMUv3 DMA ownership,
   vGIC/ITS interrupt routing, vPCI passthrough, console routing, watchdog, or
   virtio proxy paths.

3. C source comments should prefer plain ASCII diagrams so toolchains, serial
   logs, and review tools render them consistently. Markdown documents may use
   box-drawing characters when they improve readability.

4. BEAU shell and console presentation may keep the existing box-drawing style
   for human-facing tables, section separators, and interactive command output.
   Keep fatal logs and low-level boot diagnostics readable on plain serial
   terminals; use ASCII there unless the surrounding shell output already uses
   the box-drawing style.

```
Example box-drawing elements:

▴ ▾

◄────►

▼

▲

 ╰─▶  ◀─╮

┌───────────────┐
│               │
│               │
└───────────────┘

 ╰─   ─╮  ╭─  ─╯

──┤  ├──  ─┬─  ─┴─


← →  ↓ ↑ 
```

5. Use this shape for non-trivial C design comments. The compact ASCII diagram
   may be a framework/architecture diagram, flowchart, or sequence diagram:

```
/* [YYYYMMDD] Topic
 *
 * compact ASCII framework/architecture diagram, flowchart, or sequence diagram
 *
 * Key rule:
 *   - explanatory Contents (for principles and background knowledge);
 *   - what owns the state;
 *   - what must happen before/after;
 *   - what failure is prevented.
 */
```

As for `FIXME` comment, use the followig structure:

```text
/* [YYYYMMDD] CASE-xx: [FIXED/UNSOLVED]
 *
 * FIXME(File, Type): <Issue Description>
 *
 * METHOD(File, Type): <Solution for this Issue>
 */
```

- `METHOD` must comes with related code implementation.

- `UNSOLVED`: Only with `FIXME`, no `METHOD` offered.

- `FIXED`: issue has been solved, with `METHOD` comment and corresponding code
  implementation.

6. Diagram style examples:

```
source state
    |
    v
validate policy
    |
    +--> fail closed
    |
    v
publish ownership
```

7. C changes should follow ISO 26262-oriented safety discipline: validate
   external inputs, fail closed on isolation errors, keep cleanup deterministic,
   avoid hidden ownership transfer, and keep diagnostics actionable.
   (More details refer to [ISO26262.md](beau/ISO26262.md))

8. After changes to BEAU general-purpose Linux drivers or BEAU general-purpose
   Zephyr applications are validated in their corresponding guest OS source
   trees, copy the approved reusable files into `sdk/kbe` and `sdk/zsh`
   respectively so later porting and validation can reuse them quickly.

9. Move validation images that need to be reused, including Linux `Image` and
   `zephyr.bin`, into `sdk/imgs` so VM boot and regression
   validation can find stable guest image inputs.

10. Do not commit temporary test scripts matching `scripts/test_*.py`. Move any
    regression coverage that must be retained into `scripts/regress.py`, and
    include the corresponding `scripts/regress.py` changes in the submitted
    change.

## Commit Spec

Every commit message must use the following structure:

```text
[beau 0007] (xx)

description
```

- `0007` is the BEAU OS version number. Replace it with the version number of
  the release being developed.
- `xx` is the two-digit commit count within that version and must increase for
  each subsequent commit.
- Keep one blank line between the header and the description. The description
  must state the purpose of the change.


## BEAU OS Architecture

```text
                             BEAU OS / EL2

  +------------------------- host platform --------------------------+
  |                                                                  |
  |  platform.dts / boot modules                                     |
  |      |                                                           |
  |      +--> host HW policy: CPU, memory, GIC/ITS, UART, PCIe, SMMU |
  |      +--> VM policy: RAM, images, vCPU affinity, virtual devices |
  |                                                                  |
  +------------------------------+-----------------------------------+
                                 |
                                 v
  +--------------------------- arch/arm64 ---------------------------+
  | EL2 entry, host stage-1, VM stage-2, vCPU entry/exit, traps      |
  | vGICv3/vITS, vtimer, vPL011, SMMUv3, PSCI/HVC handling           |
  +---------------+--------------------------+-----------------------+
                  |                          |
                  v                          v
  +--------------------------- core ---------------------------+   +----------------------+
  | VM/vCPU lifecycle, scheduler, timers, IRQ dispatch,        |   | lib/include/scripts  |
  | hypercall dispatch, MMIO request routing                   |   | common helpers/build |
  +---------------+--------------------------+-----------------+   +----------------------+
                  |                          |
                  v                          v
  +--------------------------- sdk/bsp ----------------------------+
  | shell/console/debug commands, platform DTS parser, vFDT,       |
  | vPCI, passthrough policy, virtio proxy, watchdog, image loader |
  +---------------+--------------------------+---------------------+
                  |                          |
                  v                          v
  +---------------------- guest VM topology ------------------------+
  | VM0 Zephyr      : RTOS/service path, vPL011 console             |
  | VM1 Linux-1     : secure/KBE, Trusty client, PCI passthrough    |
  | VM2 Linux-2     : virtio-proxy frontend                         |
  | VM3 Linux-3     : virtio-proxy frontend                         |
  +-----------------------------------------------------------------+
```

## Runtime Data Paths

```text
guest CPU execution
    -> vCPU thread scheduled by core scheduler
    -> arch/arm64 saves/restores EL1 state
    -> CPU memory access translated by VM stage-2
    -> MMIO/data abort routed to vGIC/vPL011/vPCI/virtio handlers

RTOS console
    -> guest PL011 MMIO trap
    -> arch/arm64/guest/vpl011.c
    -> sdk/bsp console ring
    -> vcon/host shell

Linux console and backend IO
    -> virtio-console or virtio-mmio frontend
    -> BEAU virtio proxy HVC/MMIO path
    -> VM1 Linux backend and VM2/VM3 frontend drivers
    -> sdk/kbe backend services when Linux owns the backend role

PCIe passthrough
    -> platform.dts passthrough policy
    -> sdk/bsp/passthrough.c ownership checks
    -> sdk/bsp/vpci config/BAR/MSI virtualization
    -> arch/arm64/iommu/iommu.c StreamID -> immutable VM S2 domain request
    -> arch/arm64/iommu/smmu.c physical STE/CMDQ transaction
    -> arch/arm64/gic ITS/LPI remap for MSI/MSI-X

watchdog and observability
    -> guest WDT driver HVC
    -> BEAU watchdog state
    -> timeout capture: durable state + bounded live pCPU sample
    -> async cold reset on the VM BSP pCPU, then heartbeat verification
    -> shell commands: swtdbg, vmstat, vcpus, schedstat, irqstat, pcistat
```

## Core Invariants

1. VM CPU memory and device DMA must agree on the same ownership model: CPU
   access uses VM stage-2 translation, and passthrough DMA uses the VM SMMUv3
   domain before the device is exposed.
2. RTOS console traffic stays on vPL011; Linux console and high-volume channels
   should prefer virtio-console or virtio proxy paths.
3. PCI config space is virtual first. Only explicitly allowed fields, BAR
   windows, and remapped MSI/MSI-X messages are programmed to physical devices.
4. `platform.dts` is the static source of truth for QEMU/rk356x host hardware,
   VM layout, image modules, CPU affinity, and passthrough policy.
