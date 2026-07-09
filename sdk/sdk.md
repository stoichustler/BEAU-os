# BEAU OS


## Hierarchy

```
.
├── arch
│   ├── arm64            // Arm64
│   └── lds
├── core                 // BEAU core services
├── include
├── lib
├── scripts
└── sdk
    ├── beau             // Documentation
    ├── bsp              // BEAU dev/vdev drivers
    ├── codex
    ├── image
    ├── kbe              // Linux kernel virtio backend
    └── udev             // Userspace implementation (ask if allow to modify)
```

## Codex Working Requirements

When Codex works on this repository, it must read this file first and also
apply the repository-local skill under `sdk/codex`. Use `sdk/codex/SKILL.md` as
the active BEAU hypervisor development workflow, and read
`sdk/codex/references/architecture.md` when architecture details, invariants,
commands, or validation checklists are needed.

Codex must keep changes focused on the existing BEAU ARM64/QEMU/rk356x design,
preserve the documented static VM and image-loading assumptions unless an
explicit task changes them, announce intended file or area edits before making
them, and follow the English design-comment rules for ARM64 virtualization
code.

ARM64 comments must stay concise and easy to understand. For non-obvious
vGICv3, vtimer, vCPU entry/exit, trap, or scheduler handoff flows, combine short
text with `sdk/box-drawing.md`-style ASCII diagrams so the ownership transition can be
followed quickly. In short: 注释风格: 简洁易懂，图(`sdk/box-drawing.md`)文结合.

BEAU development now requires Codex-run build validation after code updates.
When Codex changes hypervisor code, Codex must run the matching build command
before the final response and report the result. QEMU boots, hardware flashing,
and `scripts/regress.py` still require an explicit task request or a clear
validation need for the current change. Treat human-provided runtime logs as the
source of truth for boot, regression, and hardware behavior that Codex did not
run.

Codex may also run lightweight local checks such as text searches,
`git diff --check`, script syntax checks, DT source compilation after an
approved DTS change, and command dry-runs that only print commands. The final
response must clearly separate checks Codex actually ran from QEMU, regression,
or hardware validation that still requires human confirmation.

Optimizations or behavior changes under `core/` require explicit human
confirmation before implementation. Document and discuss common-code timer,
scheduler, vCPU, VM, IRQ, or memory-management optimizations first, then wait for
approval before editing shared core code.

Changes to `sdk/image/linux/vm1/beau-linux.dts` or
`sdk/image/linux/vm2/beau-linux.dts` require explicit human confirmation before
implementation. Treat Linux bootargs, CPU topology, device nodes, memory ranges,
interrupt/timer properties, and debug-only boot parameters in these DTS files as
user-controlled. Discuss the intended DTS change first; after approval, update
the matching generated DTB only as part of that approved DTS change.

## ARM64 Development Status

For a first-pass implementation reading path, start with
`sdk/arm64-walkthrough.md`. It follows the ARM64 flow from EL2 entry through
VM creation, vCPU entry/exit, stage-2 memory, vGIC/vtimer virtualization, and
console debugging.

The ARM64 bring-up currently targets QEMU `virt` for the standard manual
validation path and rk356x for hardware-platform bring-up. QEMU uses
`qemu-system-aarch64` with the `virt` machine, GICv3, EL2 virtualization
enabled, and 8 physical CPUs. Use the wrapper script for the default QEMU
launch:

```bash
./scripts/kick.py
```

The wrapper expands to the ARM64 QEMU `virt` command with:

```bash
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3,its=on \
  -cpu cortex-a57 \
  -smp 8 \
  -m 1024M \
  -nographic \
  -serial mon:stdio \
  -kernel out/qemu_out/beau.debug.out \
  -device loader,file=sdk/image/linux/vm1/Image,addr=0x70000000,force-raw=on \
  -device loader,file=sdk/image/linux/vm2/Image,addr=0x76000000,force-raw=on \
  -device loader,file=sdk/image/linux/Initramfs.cpio.gz,addr=0x74000000,force-raw=on
```

For manual validation, set `BEAU_TOOLCHAINS` to the bare-metal toolchain bin
directory, then build:

```bash
./scripts/kick.py --build --dry-run
./scripts/kick.py --build
```

The rk356x hardware-platform skeleton builds with the same toolchain:

```bash
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

The default build output directories are platform-scoped:
`out/qemu_out` for `PLATFORM=qemu` and `out/rk356x_out` for
`PLATFORM=rk356x`.

ARM64 static platform policy is authored in each platform's `platform.dts`.
The build embeds `out/<platform>_out/platform.dtb` through
`arch/arm64/platform/<platform>/platform.S`; common BSP code under
`sdk/bsp/arm64` parses that DTB at boot into `beau_config`, `vm_configs[]`,
`bare_boot_options[]`, and the service-VM vFDT inputs. Keep host hardware,
VM memory windows, image module tags, bootargs, CPU affinity, scheduler
weights, and guest virtual device defaults in `platform.dts` whenever possible.
Only use generated/static C header values for compile-time limits that cannot
come from DTS.

Hardware validation for rk356x is manual for now: flash the generated BEAU
image with the board workflow and inspect serial logs for EL2 boot, MMU setup,
GIC init, the BEAU shell prompt, and VM launch logs.

Zephyr stays as an `.incbin` RTOS image under `sdk/image/zephyr.bin`. LK remains
available as the optional VM1 RTOS image under `sdk/image/lk.bin` when
`CONFIG_ENABLE_VM1_LK=1`, but the default QEMU 3OS scenario sets
`CONFIG_ENABLE_VM1_LK=0` and boots RTOS + Linux-1 + Linux-2. Both Linux guests
use the shared `sdk/image/linux/Initramfs.cpio.gz`, while their kernels are now
staged separately from `sdk/image/linux/vm1/Image` and
`sdk/image/linux/vm2/Image`. QEMU stages VM1 at `0x70000000`, the shared
initramfs at `0x74000000`, and VM2 at `0x76000000`; BEAU copies each module
into its Linux guest RAM window. The Linux DTBs remain embedded as small
`.incbin` modules because they describe Linux running on BEAU.

Current QEMU VM layout for the default `CONFIG_ENABLE_VM1_LK=0` build:

- VM0 is the Zephyr service VM.
  - Image: `sdk/image/zephyr.bin`
  - Raw image tag: `zephyr`
  - Load address and entry: `0x42000000`
  - Identity RAM window: `0x42000000-0x48000000`
  - vCPUs: 4, ordered affinity `<1 2 3 5>`
- VM1 is the Linux-1 pre-launched VM by default.
  - Kernel image: `sdk/image/linux/vm1/Image`
  - Kernel module tag: `Linux-VM1`
  - QEMU kernel stage address: `0x70000000`
  - Kernel load address and entry: `0x58080000`
  - Initramfs image: `sdk/image/linux/Initramfs.cpio.gz`
  - Initramfs module tag: `Initramfs`
  - QEMU initramfs stage address: `0x74000000`
  - Initramfs load address: `0x5c000000`
  - DTB module tag: `dtb-vm1`
  - Boot console: `console=hvc0 rdinit=/init loglevel=7`
  - Identity RAM window: `0x58000000-0x60000000`
  - vCPUs: 4, ordered affinity `<4 2 5 6>`
  - Optional LK path when `CONFIG_ENABLE_VM1_LK=1`: image `sdk/image/lk.bin`,
    RAM `0x40000000-0x42000000`, entry `0x40100000`, affinity `<3 5 6 7>`
- VM2 is the Linux-2 pre-launched VM.
  - Kernel image: `sdk/image/linux/vm2/Image`
  - Kernel module tag: `Linux-VM2`
  - QEMU kernel stage address: `0x76000000`
  - Kernel load address and entry: `0x48080000`
  - Initramfs image: `sdk/image/linux/Initramfs.cpio.gz`
  - Initramfs module tag: `Initramfs.cpio.gz`
  - QEMU initramfs stage address: `0x74000000`
  - Initramfs load address: `0x4c000000`
  - DTB module tag: `dtb-vm2`
  - Boot console: `console=hvc0 rdinit=/init loglevel=4`
  - Identity RAM window: `0x48000000-0x50000000`
  - vCPUs: 4, ordered affinity `<7 0 3 6>`
  - QEMU vITS window: `0x08080000-0x0809ffff`
  - Initramfs shell: `uos` prompt as root
- QEMU `cpu-affinity` is ordered: entry N maps vCPU N to that pCPU. Example:
  `<1 6>` means vCPU0 runs on pCPU1 and vCPU1 runs on pCPU6.
- The default QEMU placement keeps vCPU0 pCPUs unique: VM0 vCPU0 on pCPU1,
  VM1 vCPU0 on pCPU4, and VM2 vCPU0 on pCPU7. Secondary vCPUs may share pCPUs;
  pCPU0 is currently used only by VM2 vCPU1 to avoid three-way RTDS contention
  on the secondary pool.

The generated `out/qemu_out/beau.debug.out` has been boot-tested on QEMU. The
build also emits `out/qemu_out/beau.out` as the base link image and
`out/qemu_out/beau.debug.bin` as the raw debug image. Zephyr autostarts from
its embedded RTOS image. Linux-1 and Linux-2 autostart when QEMU stages their
per-VM `Image` files and the shared `Initramfs.cpio.gz`; BEAU supplies the
embedded per-VM Linux DTB modules.
LK autostarts only in the optional `CONFIG_ENABLE_VM1_LK=1` VM1 build.
The BEAU shell stays quiet during late guest AP bring-up; press Enter after the
boot logs settle to show the `console:\>` prompt.

## Implemented

- ARM64 build path for the QEMU `virt` platform.
- `scripts/kick.py` QEMU launcher with `--kernel`, `--qemu`, `--smp`,
  `--memory`, `--toolchains`, `--cross-prefix`, `--build`, `--dry-run`, and
  extra QEMU argument support.
- `scripts/regress.py` boot regression harness for build, QEMU launch, BEAU
  shell commands, VM console switching, and fatal boot-log checks.
- QEMU platform code and static board/scenario configuration under
  `arch/arm64/platform/qemu`.
- Bare-boot image embedding for LK and Zephyr raw images from `sdk/image`.
- Linux loader tags for externally staged `Image` and `Initramfs.cpio.gz`.
  The default QEMU scenario uses two per-VM embedded Linux DTB modules for
  Linux-1 and Linux-2.
- Static ARM64 VM configuration is DTS-authored for QEMU and rk356x in each
  `arch/arm64/platform/<platform>/platform.dts`. The DTS uses standard hardware
  nodes for host CPUs, PSCI, timer, GIC, UART, and `soc`, plus a `/beau,platform`
  node for BEAU-specific host/vFDT policy and a `/vm` node for generic guest
  devices and per-VM image/memory policy.
- QEMU guest RAM, GIC, and PL011 layout is centralized in each VM's
  `vm_configs[].arch` entry, following the ACRN-style `vm_configurations.c`
  source-of-truth pattern. ARM64 code reads per-VM guest layout directly from
  `get_vm_config(vm_id)->arch`; `asm/platform.h` is reserved for host/platform
  properties rather than thin guest-layout getter wrappers.
- Static QEMU RTOS VMs use a KISS stage-2 layout: each configured guest RAM
  window is mapped GPA/IPA == PA/HPA.
- Raw-image copies are checked against the owning VM's configured guest RAM
  range before writing into the 1:1 RAM window.
- Stage-2 initialization validates the 1:1 RAM invariant. The shared guest vGIC
  and vPL011 IPA windows are registered as vio MMIO instead of RAM mappings.
- Zephyr is marked with `GUEST_FLAG_NO_FW`, so the boot-info path does not
  require external ACPI/FDT modules for the service RTOS image. The optional LK
  VM1 path uses the same RTOS rule when `CONFIG_ENABLE_VM1_LK=1`. The default
  Linux-1/Linux-2 guests clear `GUEST_FLAG_NO_FW`, receive per-VM Linux DTBs,
  and use the loader/module path instead of platform `.incbin` image embedding
  for `Image` and `Initramfs.cpio.gz`.
- EL2 entry, exception vector setup, MMU enablement, and 1:1 host mappings.
  The primary and secondary EL2 entry paths explicitly select `SP_EL2` with
  `SPSel=1`, so host scheduling, guest entry, and guest exit use the same EL2
  stack register.
- ARM64 guest entry/exit path for running EL1 guests under stage-2 translation.
- ARM64 vCPU entry builds the guest restore frame on the current vCPU thread
  stack and synchronizes guest-exit registers with `vcpu->arch.regs`, so
  scheduler context switches no longer treat the persistent register block as
  the live EL2 stack.
- ARM64 data-abort MMIO dispatch into the common MMIO vio path.
- Static vFDT generation for static ARM64 VMs, including CPU nodes, memory,
  PSCI, GICv3, timer, and PL011.
- ARM64 physical GIC sources are organized under `arch/arm64/gic/` with the
  imported FreeBSD file headers preserved. The default path builds
  `gicv3.c` and `gicv3_its.c`; `CONFIG_ARM64_GICV5=y` selects `gicv5.c`,
  `gicv5_its.c`, and `gicv5_iwb.c`. Active GICv3/GICv5 sources use BEAU
  static platform data instead of FreeBSD FDT/ACPI/device/bus attachment code.
- Raw-image loader support for ARM64 guest RAM start and FDT placement.
- PSCI virtualization for guest `CPU_ON`, `CPU_OFF`, `AFFINITY_INFO`,
  `SYSTEM_OFF`, and `SYSTEM_RESET`.
- BEAU shell `reboot` command wired to host PSCI system reset.
- BEAU shell supports command-name Tab completion. Empty-line Tab lists all
  commands, a unique command prefix completes the command and adds a trailing
  space, and ambiguous prefixes print matching command names before redrawing the
  prompt. Completion is intentionally limited to the first command token; command
  parameters remain command-specific and are not completed by the common shell.
- BEAU shell `symtab` command lists the built-in debug symbol table as
  `offset symbol` rows. `offset` is relative to `dbg_symbol_text_start`, which
  keeps symbol dumps comparable when the final text load address changes.
- ARM64 BEAU shell `vmstat` command lists all configured VMs without requiring a
  VM ID. It reports configured and runtime vCPU counts, affinity, VM/vCPU state,
  scheduler parameters and BVT runtime stats, guest RAM, console, GIC, ITS, and
  timer basics.
- PSCI-based host secondary CPU bring-up with `MAX_PCPU_NUM=8`.
- DTS `cpu-affinity` is preserved as an ordered vCPU-to-pCPU map while the
  existing bitmap remains available for set membership and scheduler sharing
  tests. In the default QEMU layout, VM0 uses `<1 2 3 5>`, VM1 Linux-1 uses
  `<4 2 5 6>`, and VM2 Linux-2 uses `<7 0 3 6>`. vCPU0 pCPUs are unique
  across the three VMs.
- Current QEMU BVT weights are VM0 Zephyr `128`, VM1 Linux-1 `64`, and VM2
  Linux-2 `128`. VM2 additionally enables bounded BVT warp on vCPU event
  requests to reduce shared-core wakeup latency. The optional VM1 LK path keeps
  LK at weight `16`.
- ARM64 vCPU switch-in/out now saves and restores guest EL1 translation,
  exception, timer, TPIDR, and vGIC state, so two VMs can time-share one pCPU
  without inheriting each other's EL1 address-space context.
- ARM64 local hypervisor timer setup enables CNTHP/PPI26 as the scheduler tick
  source on every pCPU. Guest CNTV/PPI27 and emulated CNTP/PPI30 state stays out
  of the host scheduler deadline path.
- ARM64 WFI/WFE trapping is disabled in the default QEMU scenario. Guest WFI/WFE
  stays on the virtual CPU interface so idle/spin paths do not exit to EL2 on
  every instruction; the trapped handler remains only as non-default debug
  plumbing.
- Shell running as a scheduler thread and VM launch gated to host BSP after all
  APs are running.
- Static autostart prepares all configured VM images before starting any VM
  whose BSP vCPU is pinned to the launcher pCPU. This keeps a launcher-pCPU
  guest from running before later static VMs are created and prepared.
- ARM64 physical IRQ return gives a pending guest IRQ or Linux hardirq/softirq
  context only a bounded forward-progress window before `schedule()` must run.
  This avoids starving another runnable vCPU on an RTDS-shared pCPU when a
  guest timer line stays deliverable across repeated exits.
- ARM64 HVC dispatch recognizes ACRN hypercall IDs separately from PSCI HVC
  calls. The current ARM64 implementation supports basic API/HW info and
  GPA-to-HPA translation; x86-specific VM/device management hypercalls return
  not-supported until their ARM64 dependencies exist.
- VM and vCPU debug commands:
  - `vcpus`
  - `threads`
  - `schedstat`
  - `mmap`
  - `xmem <addr, length>`
  - `irqstat`
  - `dumpstat [vm id]`
  - `vsh <vm id>`
- Press Tab in the BEAU shell to display `registered commands`; `help` is not
  registered as a BEAU console command.
- `vsh <vm id>` binds the host vuart to a VM vPL011/vUART console.
  Ctrl-D unbinds it and switches back to the BEAU shell.
- `schedstat` prints the scheduler algorithm and one physical-CPU row with
  `pcpu`, scheduler `busy%`, `timer` callbacks, context `switches`, `resched`
  requests, runnable-thread count, and current `thread`. It also prints CPU
  usage for non-idle scheduler threads from the delta between consecutive
  `schedstat` samples. When BVT is active, it also prints a thread table with
  BVT `weight`, `avt`, and `evt`.
  - `busy%` is the pCPU busy percentage over the previous `schedstat` sample
    window, derived from the idle thread runtime delta.
  - `timer` is the number of scheduler timer callbacks observed on that pCPU.
  - `switches` is the number of times `schedule()` actually selected a
    different thread.
  - `resched` counts requests raised through `make_reschedule_request()`,
    including tick, wake, yield, and remote reschedule paths.
  - `runqueue` is the current count of runnable threads bound to that pCPU.
  - `cpu%` and `run.us` in the CPU usage table are per-thread running-time
    deltas over the same sample window; the first `schedstat` capture has no
    previous sample, so it reports `0.0` and zero runtime deltas.
  - `weight`, `avt`, and `evt` expose BVT share and virtual-time ordering;
    running threads are sampled against current host ticks for a live view.

### BVT Scheduler Progress, 2026-06-22

Current BVT status:

- The ARM64 default scheduler is being switched to `sched_bvt` for the static
  QEMU/rk356x flow. A valid `schedstat` header should report
  `schedstat algorithm:sched_bvt mcu:1ms csa:5 weight:1-128 pcpus:8`.
- The BVT `schedstat` thread table exposes `weight`, `avt`, and `evt`.
  Lower virtual time has scheduling priority. `evt` is `avt` unless bounded
  warp is enabled for the thread by its platform `sched_params`.
- The user-provided `schedstat` samples show VM2 Linux vCPUs at `weight 128`.
  VM2/vCPU0 and VM2/vCPU1 advance by about 80k virtual-time ticks between the
  two samples; VM2/vCPU2 and VM2/vCPU3 move from runnable to running and also
  advance. This is not direct evidence of BVT starvation for VM2 Linux.
- If the observed lag is BEAU shell or `vsh 2` responsiveness, pCPU0 is a
  likely pressure point: `shell` currently falls back to BVT weight `1`, while
  `vm0:vcpu1` on the same pCPU has weight `128`. Raising only the shell
  thread's BVT weight is the narrowest way to test host-console responsiveness.
- If Linux still reports internal stalls while VM2 vCPUs continue to advance,
  keep debugging VM2 vtimer/vGIC forward progress with `dumpstat 2`, `irqstat`,
  `vmstat`, and consecutive `schedstat` captures.
- Earlier samples showed pCPU4 contention when VM0 also used pCPU4. The current
  QEMU static layout keeps VM2's pCPU1 and pCPU4 private while preserving four
  VM0 vCPUs; shared-core latency should now be checked on pCPU6 and pCPU7.

### ARM64 Static DTS Configuration Progress, 2026-07-07

Current status:

- QEMU and rk356x no longer hand-maintain `struct acrn_vm_config` tables in
  platform-specific `vm_config.c`. The source of truth is now each platform's
  `platform.dts`, parsed by common code in `sdk/bsp/arm64`.
- The DTS shape follows a standard ARM64 hardware tree at the root:
  `compatible`, `model`, `chosen`, `memory`, `cpus`, `psci`, `timer`, `soc`,
  `gic`, and `uart`. BEAU host/vFDT policy is grouped under `/beau,platform`.
  Guest policy is intentionally grouped under `/vm` with `generic` and `vm@N`
  children.
- The old `sos` node is not used. Hypervisor-running hardware is described by
  root hardware nodes and `soc`; guest defaults and images stay under `/vm`.
- Per-VM DTS entries describe the current static memory window
  (`beau,static-mem`), CPU affinity, guest flags, scheduler parameters, OS
  family, bootargs, kernel/ramdisk/device-tree modules, and per-VM device
  overrides such as QEMU VM2 vITS.
- The Makefile validates/compiles each `platform.dts` with `dtc`, then embeds
  `platform.dtb` through `platform.S`. Runtime BSP parsing emits the existing
  BEAU C ABI: `beau_config`, `vm_configs[]`, `service_vm_config`,
  `bare_boot_options[]`, and `n_bare_boot_options`.
- The Makefile wires `platform.dtb` and the generated static config header into
  the normal dependencies, so BSP boot code and symbol-table objects rebuild
  when the DTS changes.
- The per-VM Linux DTS files under `sdk/image/linux/vm1/` and
  `sdk/image/linux/vm2/` were not changed. They remain guest Linux DTB sources
  and still require explicit approval before edits.

Validation run on 2026-07-07:

- `dtc -I dts -O dtb` passed for both platform DTS files.
- `dtc` generated QEMU and rk356x `platform.dtb` files successfully.
- `git diff --check` passed.
- `make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)`
  passed with `BEAU_TOOLCHAINS=/home/beau/beau-cc/bin`.
- `make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)`
  passed with `BEAU_TOOLCHAINS=/home/beau/beau-cc/bin`.
- No QEMU runtime boot, regression run, or rk356x hardware validation was run
  for this DTS migration step.

Bounded BVT warp design:

- BVT keeps long-term fairness in `avt` and sorts runnable threads by `evt`.
  Normally `evt == avt`; when warp is active, `evt = avt - bvt_warp_value`, so
  the thread moves earlier in the runqueue without receiving permanent AVT
  credit.
- `bvt_warp_value` is the temporary EVT credit in MCU units. Larger values reduce
  event wake latency more aggressively, but can temporarily push peer threads
  further back.
- `bvt_warp_limit` is the maximum charged runtime for one warp window, also in
  MCU units. The window is charged only while the boosted thread actually runs.
- `bvt_unwarp_period` is the cooldown after a warp ends, in MCU units. It keeps
  an interrupt-heavy vCPU from re-entering warp immediately after every event.
- `request_thread_priority()` is the common scheduler entry point. It always
  raises `NEED_RESCHEDULE`; only schedulers that implement `.prioritize`, such
  as `sched_bvt`, attach extra ordering state. This keeps vCPU/GIC event paths
  from directly editing a scheduler runqueue while they may hold unrelated locks.
- VM2 QEMU currently uses `bvt_warp_value = 8`, `bvt_warp_limit = 2`, and
  `bvt_unwarp_period = 4`. These values are platform policy, not common scheduler
  defaults.

How to change BVT weight:

- BVT weight is per scheduler thread and is clamped to `1-128` by
  `sched_bvt_init_data()`. A zeroed `struct sched_params` therefore becomes
  weight `1`.
- To change all vCPU threads of one VM, update that VM's scheduler
  `bvt-weight` in `arch/arm64/platform/<platform>/platform.dts`.
  `sdk/bsp/arm64/platform_dts.c` parses it into
  `vm_configs[].sched_params.bvt_weight`, and `core/vcpu.c:init_vcpu_thread()`
  passes that value to every vCPU thread of that VM.

```dts
sched {
	bvt-weight = <64>;
};
```

- To change one non-VM scheduler thread, set the field in that thread's local
  `struct sched_params` before `init_thread_data()`. For example, the BEAU
  shell is created in `sdk/bsp/shell.c:shell_start()`:

```c
struct sched_params shell_params = {0U};

shell_params.prio = PRIO_LOW;
shell_params.bvt_weight = 64U;
init_thread_data(&shell_thread, &shell_params);
```

- To test one specific vCPU thread without adding a new platform config model,
  make a local copy of the VM scheduler parameters in
  `core/vcpu.c:init_vcpu_thread()`, override by VM/vCPU ID, and pass that copy
  to `init_thread_data()`:

```c
struct sched_params params = get_vm_config(vm->vm_id)->sched_params;

if ((vm->vm_id == 2U) && (vcpu->vcpu_id == 1U)) {
	params.bvt_weight = 64U;
}
init_thread_data(&vcpu->thread_obj, &params);
```

  This is a core scheduling behavior change and should be reviewed before it is
  kept. For a persistent per-vCPU policy, add an explicit per-vCPU scheduler
  configuration instead of hiding one-off overrides in the common vCPU path.
- `irqstat` prints host IRQ handler entry counts, not guest-visible interrupt
  injection or EOI counts. ARM64 maps ACRN IRQ numbers back to GIC SGI/PPI/SPI
  sources and names the EL2-owned SMP-call, CNTHP scheduler-timer PPI, CNTV
  guest-timer host PPI, and vGIC-maintenance interrupts. It suppresses IRQs
  that have neither an active handler nor any recorded count, so unused INTIDs
  do not fill the table. The compact table is `irq name active cpu0 ... cpuN`;
  `active` shows whether the IRQ is allocated in the common IRQ table. Per-pCPU
  counters saturate at `UINT64_MAX` instead of wrapping; saturated fields print
  as `sat`.
- `dumpstat [vm id]` prints all created vCPUs in the VM, including the saved
  ARM64 register image, scheduler state, current-thread status, recent vCPU
  exit reason, recent virtual IRQ injection, recent guest timer programming or
  injection, a raw guest stack trace from the VM register image, and the host
  stack saved by the vCPU thread on its bound pCPU. IRQ exits report `ec:n/a`
  because `ESR_EL2.EC` is only meaningful for synchronous exits. Guest stack
  entries are raw addresses because BEAU does not embed guest symbol tables. The
  debug image embeds the BEAU symbol table, so host stack return addresses are
  printed as `function+offset`. Offline vCPUs skip stack output.
  `guest-trace` is a small per-vCPU ring for the guest/EL2 boundary:

  ```text
  ┌────────────┐
  │ vCPU thread│
  └─────┬──────┘
        │ enter
        ▼
  ┌────────────┐      exit       ┌────────────┐
  │  EL1 guest │ ──────────────▶ │ EL2 handler│
  └────────────┘ ◀────────────── └────────────┘
        ▲           resume
        │
        ╰─ next guest run
  ```

  `enter` is the first handoff from the vCPU thread into EL1, `exit` is the
  guest trap or physical interrupt return to EL2, and `resume` is the point
  where EL2 has finished handling the exit and is about to `ERET` back to EL1.

### Shell Field Notes

Use these field meanings when comparing `vmstat`, `dumpstat`, and `schedstat`
captures during boot, reboot, or VM2 latency work:

- `schedstat` pCPU table:
  - `role`: `shared` means more than one vCPU is bound to the pCPU; `exclusive`
    means no vCPU sharing was detected there.
  - `scheduler`: active scheduler implementation on that pCPU.
  - `busy%`: pCPU busy percentage since the previous `schedstat` capture.
  - `timer`: scheduler timer callback count.
  - `switches`: context switches where `schedule()` selected a different
    thread.
  - `resched`: reschedule requests raised by tick, wake, yield, or remote
    reschedule paths.
  - `runqueue`: runnable threads currently bound to the pCPU.
  - `current`: thread currently selected on that pCPU.
- `schedstat` CPU usage table:
  - `window.us`: sample duration between the current and previous `schedstat`
    command.
  - `cpu%`: non-idle thread runtime delta divided by `window.us` on its bound
    pCPU.
  - `run.us`: non-idle thread runtime delta in microseconds for that sample.
    The first `schedstat` capture reports zero usage because no previous
    sample exists yet.
- `schedstat` BVT table:
  - `weight`: BVT CPU share. Higher weight advances virtual time more slowly.
  - `avt`: actual virtual time used for long-term fairness.
  - `evt`: effective virtual time used for runnable ordering; bounded warp can
    make `evt` lower than `avt` for a temporary wakeup boost.
- `schedstat` RTDS table:
  - `period.us`: replenishment period.
  - `budget.us`: CPU budget granted per period.
  - `remain.us`: budget left in the current period.
  - `deadline-in.us`: time until the current RTDS deadline.
- `vmstat` VM fields:
  - `configured`: vCPU count implied by the configured affinity mask.
  - `created`: vCPUs actually instantiated at runtime.
  - `state`: VM lifecycle state.
  - `flags`: guest flags from VM config.
  - `load`: load order value.
  - `affinity-config`: configured pCPU mask.
  - `runtime`: runtime pCPU mask currently held by the VM.
  - `root-s2`: whether the VM has a stage-2 root page table.
  - `bound`: whether `vsh` has bound the host vuart to this VM console.
  - `ring`: queued VM console bytes versus ring capacity.
  - `drain`: current bound-vuart output bytes allowed per console timer pass;
    `0` means the VM is not bound to the host vuart.
  - `pending`: VM console output is waiting to be drained.
- `vmstat` vCPU fields:
  - `sched`: scheduler class backing the vCPU thread.
  - `vcpu`: vCPU lifecycle state.
  - `thread`: scheduler thread state.
  - `cur`: whether this vCPU thread is currently selected on its pCPU.
  - `req-mask`: pending vCPU request bits.
  - `diag`: quick hints such as `cpu-wait`, `rtds-depleted`, and
    `rtds-overrun`.
  - `timer`: guest virtual timer state saved in the vCPU context.
  - `cpuif`: saved vGIC CPU-interface state and LR usage.
- `dumpstat` vCPU header:
  - `pcpu`: pCPU bound to the vCPU thread.
  - `sched`: scheduler-visible vCPU state.
  - `current`: whether the vCPU is the selected thread on that pCPU.
  - `live`: whether `dumpstat` captured live EL2 state through an IPI.
- `dumpstat` guest trace:
  - `src`: exit source, where `1` is synchronous trap and `2` is physical IRQ.
  - `ec`: ESR exception class for synchronous exits; `N/A` means no sync EC is
    available for that boundary.
  - `status`: handler return status.
  - `delta.us`: time since the previous printed guest-boundary row.
  - `tsc`: raw host tick timestamp.
  - `elr/esr/far/hpfar`: saved guest exception frame values.
- ARM64 host exception call traces resolve return addresses through the
  embedded BEAU symbol table and print `function+offset` beside the raw LR.
- VM vPL011 TX output is written into a Xen-style per-VM async console receive
  FIFO, using monotonic producer/consumer indexes and a 4KB power-of-two data
  area with 4095 bytes of usable capacity. `vsh` binds the host vuart to one VM
  console at a time; unbound VM output stays buffered until that VM is selected.
  The console timer path runs every `CONFIG_CONSOLE_KICK_TIMER_TIMEOUT`,
  default 2ms, and drains the bound VM to the host
  serial console with a small live-output slice. The raw slice starts from
  `CONFIG_VM_CONSOLE_DRAIN_BUDGET`, default 512, or
  `CONFIG_VM_CONSOLE_DRAIN_BURST_BUDGET`, default 2048, when the selected VM
  has at least half a ring queued and no host input is pending. Smaller live
  output slices are capped by `CONFIG_VM_CONSOLE_INTERACTIVE_DRAIN_BUDGET`,
  default 512. If host input was collected or remains queued in the same pass,
  the cap drops to `CONFIG_VM_CONSOLE_INPUT_PENDING_DRAIN_BUDGET`, default 256,
  so Ctrl-D and typed commands still get priority over guest TX backlog.
  Host-to-VM console input is first buffered in a small host backlog, then fed to
  the guest with `CONFIG_VM_CONSOLE_RX_BUDGET`, default 32 bytes per pass, and
  only while the guest RX FIFO is below `CONFIG_VM_CONSOLE_RX_LOW_WATERMARK`,
  default 1 byte. Ordinary input beyond the backlog can be dropped under
  sustained key repeat. Guest RX reads refill one queued byte while budget
  remains, so short commands stay ordered and Ctrl-D still switches back to the
  BEAU shell. This keeps a held key from monopolizing the console timer path or
  keeping Linux in continuous UART RX IRQ and shell echo handling.
- Non-selected VM console output is not replayed into the BEAU shell.
- VM exception logs have a separate 4KB/4095-byte per-VM ring reserved for VM
  trap/oops capture. The ring is internal debug plumbing and is no longer
  exposed as a BEAU shell command.
- vUART/vPL011 layering:
  - `vuart` owns the upper VM console interface and host console integration.
  - `vpl011` is the ARM64 PL011 backend implementation.
  - Backend notification is routed through `vuart_notify_rx()` instead of direct
    console-to-vPL011 calls.
- ARM64 vGICv3 keeps ordinary level IRQ pending while the sampled device line is
  asserted and clears it only through the device deassert path. vPL011 uses this
  to enter the vGIC injection path only on visible UART IRQ line changes, so a
  held console key no longer needs repeated `arch_trigger_level_intr(assert=true)`
  calls or vCPU event requests to redeliver RX interrupts. Guest UART MMIO exits
  on an already asserted line refresh the current vGIC LR in place so active RX
  interrupts can become active+pending without a remote wakeup storm.
- ARM64 vtimer diagnostics keep pending-only timer LR counters for the case
  where Linux reaches idle with EL1 IRQs still masked and CNTV remains
  host-masked. The old timer LR/WFI rescue state has been removed; CNTV/PPI27
  now follows ordinary vGIC software level delivery.
- ARM64 vPL011 vio for guest PL011 MMIO and RX interrupt notification.
- ARM64 vPL011 avoids running the vGIC level-deassert path for ordinary PL011
  polling reads, especially Linux's frequent `FR` reads around console output.
  RX reads, interrupt status reads, interrupt-mask writes, interrupt clears, and
  newly raised TX-ready state still update the virtual IRQ line. vPL011 keeps
  internal counters for TX bytes, TX-ready raises, pending state, and virtual
  IRQ assert/deassert transitions for temporary diagnostics.
- Initial vGICv3 model:
  - VM and vCPU vGIC state.
  - ICH LR save/load/sync/flush.
  - VMCR/HCR setup.
  - Virtual interrupt injection.
  - Maintenance IRQ handler.
  - SGI system-register trap handling through `ICC_SGI1R_EL1`.
  - GICD/GICR MMIO vio and 64-bit MMIO split handling.
  - Redistributor frames are addressed by guest vCPU slot and mapped to the
    VM's local IRQ bank.
  - GICD/GICR `IPRIORITYR` byte/halfword/word access support.
  - GICD/GICR `ICFGR` read/write support for SGI/PPI/SPI trigger type state.
  - GICD `IROUTER` low/high word access and SPI target-vCPU delivery.
  - Virtual timer PPI handling and injection back into the running guest vCPU.
- Initial QEMU GICv3 ITS/vGICv3 ITS support:
  - QEMU launch and regression use `-machine
    virt,virtualization=on,gic-version=3,its=on`.
  - Host GICv3 early init detects and quiesces the QEMU ITS at
    `0x08080000-0x0809ffff`.
  - VM2 Linux has a guest ITS window and DT `msi-controller@8080000` node.
    VM0 Zephyr and VM1 LK do not expose ITS/LPI capability in the QEMU
    scenario.
  - vGICv3 advertises LPIs only for VMs with `guest_its_size != 0`, including
    GICD `LPIS/NumLPIs/IDbits` and GICR `PLPIS`.
  - vITS models CTLR/IIDR/TYPER/CBASER/CWRITER/CREADR/BASERn/TRANSLATER and
    the GICR LPI registers needed by Linux ITS setup.
  - vITS command queue supports MAPD, MAPC, MAPI, MAPTI, MOVI, DISCARD, INT,
    CLEAR, INV, INVALL, SYNC, and MOVALL for the current software model.
  - `arm64_vgicv3_inject_msi()` provides the hypervisor-side entry point for
    injecting a mapped `(device_id, event_id)` as an LPI.
- ARM64 IRQ domains for CPU-local and GIC interrupts.
- ARM64 memory logging for host stage-1 and VM stage-2 mappings.
- ARM64 exception stack dumps print directly to the console without per-line
  log prefixes.
- BEAU log and shell/console strings are lowercase source literals; there is no
  output-layer lowercase conversion.
- ARM64 memory virtualization, interrupt virtualization, and vCPU
  virtualization code now includes English comments for module responsibilities,
  design boundaries, and key state transitions.

## QEMU Validation Notes

The following are the current QEMU `-smp 8` validation expectations for the
default `CONFIG_ENABLE_VM1_LK=0` RTOS + 2 Linux scenario:

- The hypervisor accepts Enter after boot and prints the `console:\>` shell
  prompt.
- VM0 Zephyr autostarts as the service VM.
- VM1 Linux-1 autostarts as a pre-launched VM.
- VM2 Linux-2 autostarts as a pre-launched VM.
- VM0 Zephyr vCPU0-vCPU3 bind to pCPU1, pCPU2, pCPU3, and pCPU5 in authored
  `cpu-affinity` order.
- VM1 Linux-1 vCPU0-vCPU3 bind to pCPU4, pCPU2, pCPU5, and pCPU6 in authored
  `cpu-affinity` order.
- VM2 Linux-2 vCPU0-vCPU3 bind to pCPU7, pCPU0, pCPU3, and pCPU6 in authored
  `cpu-affinity` order.
- VM0 Zephyr enters EL1 at `0x42000000`.
- VM1 Linux-1 enters EL1 at `0x58080000`.
- VM2 Linux-2 enters EL1 at `0x48080000`.
- Boot logs show each VM stage-2 RAM map as identity mapped:
  VM0 `ipa[0x42000000-0x48000000]:pa[0x42000000-0x48000000]`, VM1
  `ipa[0x58000000-0x60000000]:pa[0x58000000-0x60000000]`, and VM2
  `ipa[0x48000000-0x50000000]:pa[0x48000000-0x50000000]`.
- Before the default BVT switch, `schedstat` reported the configured scheduler,
  per-pCPU scheduler timer callbacks, reschedule requests, runnable-thread
  counts, and context switch counters. After the BVT switch, QEMU validation
  must confirm that it reports `sched_bvt`.
- `vcpus` reports all three VMs and twelve guest vCPUs. VM0 uses
  pCPU1/pCPU2/pCPU3/pCPU5, VM1 Linux-1 uses pCPU4/pCPU2/pCPU5/pCPU6, and VM2
  Linux-2 uses pCPU7/pCPU0/pCPU3/pCPU6.
- `vsh 0` enters the Zephyr console and reaches `zero ~>`.
- `vsh 1` enters the Linux-1 console and should reach the initramfs `uos` root
  shell.
- `vsh 2` enters the Linux-2 console and should reach the initramfs `uos` root
  shell. Linux shell regressions should verify root identity with `id` instead
  of using the old login flow.
- VM0 Zephyr `help` completes through the async VM console path. Linux-1 and
  Linux-2 shell commands remain part of the active timer/vGIC stability gate.
- VM0 Zephyr `symtab list` completes through `vsh 0` and returns to the
  `zero ~>` prompt with async batched VM console output.
- VM console output bypasses vUART TX FIFO forwarding for the selected `vsh`
  VM console.
- Ctrl-D returns from VM console mode to `console:\>`.
- Five repeated QEMU cold boots reached VM0/VM1/VM2 guest entry without
  `[cut here]` or `unexpected arm64 trap`, covering the prior `SP_EL2`/`SPSel`
  boot race.
- Five repeated QEMU cold boots also covered the later `vcpu_exit_return`
  restore-frame issue where EL2 `sp` could drift to guest RAM and fault at
  `far:0x80000000`.
- `mmap` shows VM0/VM1/VM2 stage-2 RAM identity mappings plus vGICD, vGICR,
  and vPL011 vio windows.
- Boot logs show each VM image copied to 1:1 RAM.
- `irqstat` uses a narrow-screen-friendly format and shows host CNTV PPI handler
  entries on Zephyr AP pCPUs.
- Zephyr no longer traps on `GICD_IPRIORITYR` byte writes.
- Zephyr AP virtual timer interrupts no longer hit the host unexpected IRQ path.
- The optional `CONFIG_ENABLE_VM1_LK=1` path still exists, but it is not the
  default QEMU 3OS validation scenario.
- The `reboot` shell command resets QEMU and restarts BEAU.
- `PLATFORM=rk356x` builds a hardware image. Boot correctness is pending manual
  flashing and serial-log validation.
- VM2 Linux no longer stops at `smp: Bringing up secondary CPUs ...` in the
  current 4-vCPU path: logs show CPU1, CPU2, and CPU3 entering EL1 and Linux
  reporting `smp: Brought up 1 node, 4 CPUs`.
- ITS regression `out/qemu_out/regress-its-vm2-only.log` was run after a
  successful QEMU build. VM0 Zephyr and VM1 LK passed the regression gate, and
  VM2 Linux enumerated the virtual ITS:
  `ITS [mem 0x08080000-0x0809ffff]`,
  `ITS: Using hypervisor restricted LPI range [8192]`, and allocated LPI
  pending tables for CPU0-CPU3. This predates the switch to the direct
  initramfs `uos` shell.
- On 2026-06-23 the physical GIC import was build-validated after adding
  `gicv5_iwb.c` to the `CONFIG_ARM64_GICV5=y` source list. Clean builds passed
  for QEMU and rk356x with both the default GICv3 path and the GICv5-selected
  path:

```bash
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- clean
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- CONFIG_ARM64_GICV5=y -j$(nproc)
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- clean
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- CONFIG_ARM64_GICV5=y -j$(nproc)
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- clean
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- clean
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

  This is compile validation only. QEMU boot, `scripts/regress.py`, and rk356x
  hardware validation still require explicit manual runs.

## Manual Regression

Use these commands when validating the QEMU 3OS console, vGIC, vtimer, and
vCPU-exit path manually. Set the toolchain path first:

```bash
export BEAU_TOOLCHAINS=$HOME/beau-cc/bin
```

For debug, shell, or console changes, remove stale debug artifacts before
building so `libdebug.a` and the final linked image are refreshed:

```bash
rm -f out/qemu_out/modules/libdebug.a \
      out/qemu_out/beau.out \
      out/qemu_out/beau.debug.out \
      out/qemu_out/beau.debug.bin
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

Lightweight checks before booting:

```bash
python3 -m py_compile scripts/regress.py
sh -n scripts/repack_initramfs.sh
git diff --check
```

Standard QEMU smoke regression:

```bash
BEAU_TOOLCHAINS=${BEAU_TOOLCHAINS} \
./scripts/regress.py --no-build --timeout 240
```

The smoke run should reach the BEAU shell, pass `vcpus`, `schedstat`, `mmap`,
`irqstat`, `dumpstat 0`, enter and leave `vsh 0`, `vsh 1`, and `vsh 2`, and
verify VM2 Linux root identity with `id` showing `gid=0`.

VM console switch and held-Enter pressure regression:

```bash
BEAU_TOOLCHAINS=${BEAU_TOOLCHAINS} \
./scripts/regress.py --no-build --timeout 300 \
  --stress-vsh-switch \
  --stress-rounds 4 \
  --stress-enters 80 \
  --no-terminal-replies
```

This stress run models the manual failure sequence: enter VM2 and press Enter
repeatedly, switch to VM1 and press Enter, switch to VM0, then switch back to
VM2 and run `id`. Passing means VM2 still reaches `uos ~`, `id` returns `gid=0`,
no Linux RCU stall appears, and Ctrl-D still returns to `console:\>`.

VM console switch and `help` command pressure regression:

```bash
BEAU_TOOLCHAINS=${BEAU_TOOLCHAINS} \
./scripts/regress.py --no-build --timeout 300 \
  --stress-vsh-help \
  --stress-help-rounds 100 \
  --no-terminal-replies
```

This stress run switches between VM0, VM1, and VM2 for each round, runs `help`
inside the selected guest shell, waits for the guest prompt to return, and then
uses Ctrl-D to return to the BEAU shell before switching to the next VM.

If a regression fails, the harness tries to return to the BEAU shell and capture
`vcpus`, `schedstat`, `vmstat`, `irqstat`, and `dumpstat <vmid>` into
`out/qemu_out/regress.log`. If reproducing manually through `./scripts/kick.py`,
run the same commands immediately after Ctrl-D:

```text
vcpus
schedstat
vmstat
irqstat
dumpstat 2
```

For the current VM2 issue, key failure indicators are Linux messages containing
`rcu_preempt kthread timer wakeup didn't happen`,
`Possible timer handling issue on cpu=0 timer-softirq=0`, VM2/vCPU0 stopped at
`elr:0xffff800080f0fe4c`, virtual timer `virq:27` pending, and a pending-only
timer LR such as `0x508000000000001b`.

## ARM64 vtimer/vIRQ/vGICv3/GICv3 Handoff Notes

This section is written for engineers taking over the ARM64 interrupt/timer
path. It describes the intended design flow rather than only the current bug
state. Keep it synchronized with `arch/arm64/gic/`,
`arch/arm64/gic/gicv3.c`, `arch/arm64/gic/gicv3_its.c`,
`arch/arm64/timer.c`, `arch/arm64/guest/virq.c`,
`arch/arm64/guest/vgicv3.c`, `include/arch/arm64/asm/guest/vgicv3.h`, and the
guest-exit path.

### Architecture Map

- Physical GICv3 host layer:
  `arch/arm64/gic/` contains the imported FreeBSD GICv3/GICv5/ITS/IWB source
  files with their original file headers preserved. `gicv3.c` and
  `gicv3_its.c` are trimmed to BEAU static-platform code: they initialize the
  real GICv3 Distributor, Redistributors, ITS state, MSI/MSI-X message mapping,
  SGI/SMP handling, and CPU interface using BEAU platform addresses, while
  keeping the `arm64_gicv3_*` ABI used by IRQ, timer, and vGIC code.
- Physical GICv5 host layer:
  `CONFIG_ARM64_GICV5=y` selects `arch/arm64/gic/gicv5.c`,
  `gicv5_its.c`, and `gicv5_iwb.c`. These files retain the FreeBSD file
  headers but remove FreeBSD FDT/ACPI/device/bus framework glue. `gicv5.c`
  provides the BEAU-facing `arm64_gicv3_*` ABI used by IRQ, timer, and vGIC
  code, with GICv5 CPU-interface setup, SMP SGI delivery through LPI pending
  commands, and separate PPI/SPI/LPI command paths. `gicv5_its.c` provides the
  current BEAU MSI/MSI-X allocation, release, and mapping boundary using the
  GICv5 ITS translate frame. `gicv5_iwb.c` provides static IWB register glue:
  firmware enable detection, wire-count discovery, all-wire disable during
  init, wire enable/disable, and level/edge programming helpers. IWB is still
  absent by default because the static platform API does not yet expose IWB
  base/size values.
- Host hypervisor timer layer:
  `arch/arm64/timer.c` uses CNTHP/hypervisor timer PPI26 as the EL2 scheduler
  tick. Guest CNTV/PPI27 and CNTP/PPI30 work must not steal CNTHP, because
  CNTHP is the host preemption and softirq source.
- Virtual interrupt API:
  `arch/arm64/guest/virq.c` is the small architecture-facing entry point used
  by common code and devices. For GIC INTIDs it delegates to VGICv3; the
  bitmap fallback is legacy/request plumbing for non-GIC virtual events.
- Virtual GICv3/vITS model:
  `arch/arm64/guest/vgicv3.c` owns guest-visible GICD/GICR/GITS MMIO,
  ICC_* sysreg traps, software IRQ descriptors, pending bitmaps, hardware
  list-register save/load/sync/flush, virtual SGI delivery, vtimer PPI
  delivery, and VM2-only ITS/LPI modeling.
- Core scheduler boundary:
  `core/schedule.c` knows threads, runqueues, ticks, and reschedule flags. It
  does not know GIC LR semantics. ARM64 code may ask for the narrow
  `sched_clear_reschedule_if_current_only()` helper when returning to the same
  guest is required to retire a pending virtual IRQ, but the scheduler must not
  grow architecture-specific timer logic.

### Interrupt Numbering

- ARM GIC INTIDs are guest-visible interrupt numbers:
  SGI0-SGI15, PPI16-PPI31, SPI32 and above, and LPIs from 8192.
- BEAU maps GIC INTIDs into the generic ACRN IRQ namespace through the ARM64
  IRQ domain. Do not pass raw GIC INTIDs to common IRQ APIs without this
  domain conversion.
- Current important physical INTIDs:
  - SGI0: EL2 SMP/reschedule call.
  - PPI25: vGIC maintenance interrupt.
  - PPI26: ARM hypervisor timer. Reserved for the EL2 scheduler tick.
  - PPI27: ARM virtual timer. Used as the hardware source for guest timer
    delivery while a vCPU is loaded.
  - PPI30: ARM physical timer. Reserved for guest physical-timer emulation.
- Current guest timer ABI:
  - Linux VM2 uses virtual timer PPI/INTID 27 from DTS `<1 13 4>`.
  - RTOS guests may expect physical-timer PPI30 as their guest-visible timer
    PPI. BEAU emulates guest CNTP with a vCPU shadow and a host software timer.

### Physical GICv3 Flow

1. `arm64_gicv3_init_early()` discovers Redistributor frames by matching
   `GICR_TYPER` affinity to per-pCPU MPIDR, initializes the Distributor, and
   quiesces a discovered physical ITS. Do not assume Redistributor frame order
   equals logical pCPU order.
2. The Distributor is reset into non-secure Group-1, affinity-routing mode.
   SPIs are disabled, pending/active state is cleared, priorities are set to
   `0x80`, and routes default to BSP until a richer routing policy exists.
3. The QEMU ITS is detected and quiesced, but physical ITS passthrough is not
   currently exposed. VM2 receives a software vITS model instead.
4. Every pCPU initializes its Redistributor and CPU interface. Local host
   enables include SGI0, PPI25, and PPI27. PPI26 is enabled by the host timer
   init path.
5. CNTV/PPI27 is the physical source event for guest virtual timer delivery.
   Do not disable PPI27 at the Redistributor as a replacement for vGIC level
   tracking, because the QEMU CPU model can then make Linux-visible
   CNTV_CTL.ISTATUS unreliable.

### vIRQ API Flow

The ARM64 vIRQ API deliberately stays thin:

1. Device or common code calls `vcpu_set_intr(vcpu, hwirq)` or
   `vcpu_clear_intr(vcpu, hwirq)`.
2. If `hwirq < ARM64_VGIC_IRQ_NUM`, the request is a guest GIC INTID and is
   routed to `arm64_vgicv3_inject_irq()` or `arm64_vgicv3_clear_irq()`.
3. VGICv3 syncs any current LR state before mutating the software descriptor,
   updates pending/active state, flushes into hardware LRs if the target vCPU
   is loaded, and signals `ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT`.
4. The vCPU request/event path wakes blocked vCPUs and makes the guest-return
   path revisit pending interrupt work before entering EL1.
5. Keep device-specific level semantics in the device backend. For example
   vPL011 should assert/deassert based on UART interrupt line state; VGICv3
   should model that line, not infer UART FIFO state itself.

### VGICv3 Lifecycle

The vGIC model has two copies of interrupt state:

- Software descriptors and bitmaps:
  guest-visible enable, pending, active, priority, level/edge, target vCPU, and
  LPI mappings. These are persistent across scheduler switches.
- Hardware list registers:
  transient GICv3 slots that present virtual IRQs to the currently loaded EL1
  vCPU. LRs must be saved/restored on vCPU switch and synchronized on
  maintenance IRQs, MMIO/sysreg traps, and selected guest-exit paths.

Normal flow:

1. Guest GICD/GICR/GITS MMIO or ICC_* sysreg access traps to EL2.
2. VGICv3 takes the VM vGIC lock, syncs live LR state if needed, and emulates
   the register access against software descriptors.
3. If a pending, enabled IRQ is deliverable, flush writes it into an LR.
4. Guest IAR/EOIR executes through the virtual CPU interface. Hardware reports
   completion through EISR/EOIcount or active-priority state.
5. Sync consumes that hardware evidence and updates descriptor pending/active
   state.
6. Level interrupts remain pending while their source line is high. Edge
   interrupts are delivery credits and are normally consumed after LR
   materialization, except SGI pending-only wake cases that must wait for EOI
   evidence.

Important rules:

- Do not clear an edge SGI only because a pending-only LR disappeared. Linux
  can wake from WFI with PSTATE.I masked, and the SGI handler may not run until
  after `local_irq_enable()`.
- Do not blindly mark pending-only timer LRs as EOI-maintained. In BEAU's
  current LR sync model that can produce maintenance storms or early boot
  stalls because a still-high CNTV level source immediately rebuilds the same
  LR.
- Do not manufacture Active+Pending for a pending-only vtimer LR. That can
  model an interrupt acknowledgement that EL1 has not actually performed.
- Do not infer HVC return-PC handling from `last-resume pc == last-exit elr`
  alone. Symbolicate the ELR and decode the current exit source first; an IRQ
  exit can resume at the same ordinary guest instruction many times without
  being an HVC replay loop.
- GICD/GICR MMIO windows must stay vio/MMIO, not stage-2 RAM mappings. A
  stage-2 data abort is the intended dispatch path for guest
  interrupt-controller register access.

### vtimer Lifecycle

BEAU separates host timekeeping from guest timer ABI:

1. Host scheduler tick:
   CNTHP/PPI26 drives `SOFTIRQ_TIMER` and the configured scheduler. It is local
   to each pCPU and must remain owned by EL2.
2. Loaded guest timer:
   Hardware CNTV is loaded with the guest vCPU timer state. When CNTV expires,
   physical PPI27 arrives at EL2 and `arm64_vgicv3_virtual_timer_irq_handler()`
   injects the guest-visible `timer_virq`.
3. Guest-visible PPI:
   `timer_virq` records the guest ABI. Linux currently sees INTID 27 through
   the CNTV path. Guest CNTP/PPI30 is separate software emulation and must not
   share the live CNTV/PPI27 delivery state.
4. Host masking:
   After CNTV expires and the vGIC owns the guest-visible timer line, BEAU masks
   the live CNTV comparator with CNTV_CTL.IMASK and keeps guest CNTV shadow state
   separate. Linux must still be able to observe an architecturally expired
   timer.
5. Reprogram/EOI:
   When the guest moves CVAL to the future, masks/disables CNTV, or EOIs a
   timer LR, VGICv3 samples live CNTV and lowers or keeps the software level
   line accordingly.
6. Offline/shared-pCPU backup:
   If a vCPU is switched out with a future timer deadline, VGICv3 can arm a
   host backup timer. The backup injects the same guest timer PPI only if the
   vCPU is still offline and the deadline is due.
7. CNTV/PPI27 delivery:
   CNTV is the physical event source for EL2, but guest PPI27 is delivered as
   an ordinary software level LR. BEAU no longer creates a hardware-backed PPI27
   LR or a separate LR/WFI rescue owner for the virtual timer.

### Scheduler Interaction

The scheduler should remain architecture-neutral:

- `make_reschedule_request()` records a request and uses SGI0 only when the
  target pCPU is remote.
- `need_reschedule()` is a flag check, not a promise that another thread will
  be selected.
- `sched_clear_reschedule_if_current_only()` is intentionally narrow. ARM64 can
  call it before a guest return when the current non-idle object is the only
  runnable object on that pCPU. It clears a no-op tick request that would have
  selected the same vCPU.
- Shared pCPUs still fall back to the configured scheduler's fairness model.
  Do not disable scheduler ticks to hide vtimer bugs.

### Debugging Checklist

When a guest appears stuck around timer or interrupt delivery, capture:

```text
vcpus
schedstat
irqstat
dumpstat 2
```

Read the dump in this order:

1. Confirm the target vCPU is running on the expected pCPU and is not offline.
2. Check `elr`, `spsr`, and DAIF. If IRQs are masked after WFI, a pending LR may
   be waiting for Linux to run a few more instructions before it can be handled.
3. Check `timer_virq`, CNTV_CTL/CVAL/CNTVCT, `cntv_el2_masked`, and whether the
   deadline is expired.
4. Check LR0/LR1 and software descriptor state for the same INTID:
   pending-only, active, active+pending, enabled, pending bitmap.
5. Check host PPI27 enabled/pending/active state. It is the EL2 source event,
   not the guest-visible interrupt lifecycle.
6. For SGI/SMP stalls, compare source and target SGI debug fields. A delivered
   SGI with no EOI evidence and Linux stuck in a CSD wait usually points to an
   edge lifecycle issue, not a timer issue.
7. If `vmstat` console/ring fields do not show host-to-guest input backlog,
   do not chase console input first; continue with vtimer/vGIC state.

### Safe Change Process

1. Preserve the current standard QEMU root-console baseline before each
   interrupt/timer experiment.
2. Do not change the per-VM Linux DTS files under `sdk/image/linux/vm1/` or
   `sdk/image/linux/vm2/` unless the task explicitly approves a DTS change.
3. Keep vtimer fixes in bounded sync points: vCPU switch-in/out,
   maintenance/EOI handling, timer IRQ injection, explicit sysreg/MMIO traps,
   or guest exits. Avoid always-on polling and broad backup timers unless a
   reviewed design proves they are needed.
4. Add comments in English at the ownership boundary being changed: physical
   GIC, vIRQ API, VGIC descriptor/LR sync, guest timer shadow state, or core
   scheduler helper.
5. Validate both the standard smoke path and the held-Enter/vsh-switch pressure
   path before treating a timer/vGIC change as accepted.

## VM2 Linux Debug Snapshot

Status as of 2026-06-18:

- Current baseline: VM2 Linux uses `sdk/image/linux/Initramfs.cpio.gz` and
  `rdinit=/init`, enters the initramfs `uos` root shell on PL011, and the
  regression checks root identity with `id`. This basic root-console path is
  working and is the baseline to preserve before making any further VM2 timer
  or vGIC change.
- The baseline was restored after reverting the stale pending-only timer LR
  drop experiment. That experiment made boot and VM console handoff visibly
  slower and must not be treated as the starting point for future work.
- `dumpstat` no longer includes the Linux CSD wait probe or any direct
  `call_single_data_t` parser. Keep `dumpstat` focused on generic vCPU state,
  EL1 state, timer/vGIC state, vtimer trace, SGI dst state, compact local IRQ
  state, and raw guest/host stacks. `dumpstat` output uses `src`/`dst` labels
  instead of `source`/`target` labels.
- Development hygiene: avoid broad, low-signal local `rg` searches over large
  source trees. Prefer known relevant files in this repository and targeted
  reads from explicitly named external references, such as the Linux 7.1 source
  tree used for VM2 symbolication.
- Keep VM2 Linux at 4 vCPUs for validation. `maxcpus=1` or `maxcpu=1` can hide
  the SMP/timer symptom and may be useful as a temporary comparison point, but
  it is not a fix for the 4-vCPU VM2 Linux path.
- Keep Linux assets under `sdk/image/linux`; the active Linux DT sources are
  `sdk/image/linux/vm1/beau-linux.dts` and
  `sdk/image/linux/vm2/beau-linux.dts`. These DTS files must only be changed by
  explicit manual edits. Do not let build scripts, regression scripts, or helper
  tools rewrite them implicitly.
- After manually changing either Linux DTS, regenerate the matching embedded DTB
  explicitly:
  `dtc -I dts -O dtb -o sdk/image/linux/vm1/beau-linux.dtb sdk/image/linux/vm1/beau-linux.dts`
  or
  `dtc -I dts -O dtb -o sdk/image/linux/vm2/beau-linux.dtb sdk/image/linux/vm2/beau-linux.dts`.
  The BEAU image must then be rebuilt because both per-VM Linux DTBs are
  included by `arch/arm64/platform/qemu/platform.S`.
- Timer DT interrupt IDs remain unchanged. The virtual timer still uses
  PPI/INTID 27 from `<1 13 4>`; current evidence does not require changing
  PPI27.
- WFI/WFE trapping is kept disabled by default. Reference checks:
  the croc reference traps WFE as yield and WFI as block, while the anoa
  reference traps WFE as yield and uses an IRQ-wait path for WFI with a yield
  threshold plus timeout. In BEAU QEMU, enabling TWI/TWE makes VM0/VM1/VM2
  noticeably slower, so the default relies on hardware/vGIC wakeup.
- The latest ITS validation keeps VM0/VM1 free of ITS/LPI exposure and exposes
  vITS only to VM2 Linux. This avoids perturbing RTOS guests while still
  allowing Linux to exercise GICv3 ITS and LPI table setup.
- vITS currently advertises a 14-bit LPI INTID space with a Linux-visible
  hypervisor restricted LPI range of 8192 IDs, but the in-hypervisor descriptor
  model stores only a compact 256-LPI active window. Mappings outside that
  active window are ignored until a dynamic LPI descriptor allocator is added.

### VM2 Linux RCU Stall Signature

- The remaining issue is post-login SMP runtime stability under VM console
  pressure, not pre-login boot: VM2 can reach the root console and run `id`, but
  the held-Enter plus `vsh 2 -> vsh 1 -> vsh 0 -> vsh 2` sequence can still
  trigger Linux RCU stall messages.
- The representative Linux log says
  `rcu_preempt kthread timer wakeup didn't happen`,
  `Possible timer handling issue on cpu=0 timer-softirq=0`, and shows target
  CPUs idling around `default_idle_call()`.
- The useful hypervisor-side evidence is centered on the virtual timer/vGIC
  path for VM2/vCPU0:
  - virtual timer PPI/INTID 27,
  - CNTV deadline/control state and `cntv_el2_masked`,
  - LR state for virtual INTID 27,
  - software vGIC descriptor state for virq 27,
  - host PPI27 enabled/pending/active state,
  - recent vtimer trace events and recent timer sysreg/inject records.
- A stale pending-only timer LR, such as `0x508000000000001b`, combined with an
  empty software descriptor is a diagnostic clue. It should not be repaired by
  blindly forcing EOI maintenance or by dropping the LR from broad sync paths.
- If the vtimer trace shows no timer sysreg writes while Linux is handling the
  interrupt, assume the CPU model may be allowing direct EL1 CNTV access despite
  the intended trap configuration. In that case, fixes must sample live CNTV
  state at real EL2 boundaries instead of relying only on `handle_timer_sysreg()`.

### VM2 Linux RCU Stall Progress, 2026-06-18

- Latest VM2 dumps show improvement from the earlier lost-LR symptom: vCPU0
  still stops around Linux `cpu_do_idle()` return, but the virtual timer LR is
  visible as pending-only, for example `live_lr0:0x508000000000001b` for
  virtual INTID 27. The remaining stall is therefore no longer primarily "timer
  LR disappeared"; it is a forward-progress window after WFI wakeup and before
  Linux handles the pending timer interrupt.
- Older console diagnostics from failing stress runs show the VM console input path is not
  the immediate stuck point after Ctrl-D diagnostics: host input backlog is
  empty, vUART RX is empty, vPL011 RX IRQ is not asserted, SPI33 is not pending,
  and the async TX ring may have drops only from the heavy Linux log stream.
  That points the active failure boundary back to VM2 vtimer/vGIC/vCPU-exit
  progress rather than lost host-to-guest console bytes.
- Symbolication for the active VM2 Linux image maps runtime
  `0xffff800080f0fe4c` to `cpu_do_idle+8`, the instruction after WFI and before
  `arch_cpu_idle()` returns toward `default_idle_call()`. Linux reaches this
  point with PSTATE.I still set, then must execute a few more instructions and
  reach `local_irq_enable()`/`daifclr` before the pending timer IRQ can be
  handled normally.
- Representative dump state for the remaining failure:
  - vCPU0 `elr:0xffff800080f0fe4c`, `lr:0xffff800080f0fe60`,
    `spsr:0x600000c5`, and live `DAIF` IRQ masked.
  - virtual timer virq 27 is enabled and pending in the vGIC, active is clear,
    and `cntv_el2_masked:yes`.
  - the vtimer trace repeatedly shows pending-only timer LR preservation rather
    than total loss of the timer interrupt.
  - some failing captures report `wfx:none`, meaning the observed stall can
    occur on the normal physical IRQ return path, not only inside the trapped
    WFI diagnostic path.
- Earlier local fix attempts were limited to ARM64 guest exit/vGIC code:
  - `handle_wfx()` no longer yields WFI when a virtual IRQ is already pending,
    even if the saved guest PSTATE still masks IRQs.
  - the sync-trap and physical-IRQ return paths now refresh the current vtimer
    and vGIC state before honoring host `need_reschedule()`.
  - if a pending guest IRQ is visible after that refresh, the return path skips
    the host reschedule once and returns to EL1 so Linux can leave the idle path
    and unmask interrupts. Physical IRQ exits still run host softirq processing
    before this refresh/schedule decision; skipping host softirqs made the BEAU
    console nearly unresponsive when VM2 kept virq27 pending.
- The 2026-06-18 follow-up modified `core/schedule.c` without changing the
  scheduler algorithm. The FreeBSD arm64 vmm I/O reference keeps vtimer delivery
  as a vGIC pending/notify problem: vtimer sync asserts or deasserts the virtual
  timer IRQ from the current timer condition, the vGIC tracks active/pending
  IRQs in a queue, and queued injection notifies the vCPU. BEAU therefore should
  not let a host scheduler tick repeatedly consume the guest return window when
  there is no other runnable thread on that pCPU.
- Validation status recorded during the 2026-06-23 investigation:
  `python3 -m py_compile scripts/regress.py`, `sh -n scripts/repack_initramfs.sh`,
  and `git diff --check` pass. The QEMU BEAU image rebuilds successfully with:

  ```bash
  PATH=${BEAU_TOOLCHAINS}:$PATH \
  make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
  ```

  Updated outputs are `out/qemu_out/beau.debug.out` and
  `out/qemu_out/beau.debug.bin`.
- Regression status after the PPI27 priority-mask repair and the
  `core/schedule.c` no-op reschedule clear: the standard VM0/VM1/VM2
  root-console path has passed, but the VM console pressure gate
  `--stress-vsh-switch --stress-rounds 4 --stress-enters 80
  --no-terminal-replies` remains the active failure reproducer. On 2026-06-23
  it still reached VM2 round 2 and then tripped Linux
  `rcu_preempt detected stalls`. Do not mark the VM2 RCU issue fixed until
  this pressure gate passes repeatedly.
- A one-shot experiment that cleared TWI after converting WFI rescue into LR
  rescue did not solve the failure and made the 40-enter stress fail earlier.
  Do not treat "clear TWI after LR rescue" by itself as the next root fix.
- Before the PPI27 priority-mask repair, the 2026-06-18 follow-up reproduced
  without changing the scheduler: CPU0/CPU1 were reported idling around
  `default_idle_call()`, RCU said `rcu_preempt kthread timer wakeup didn't
  happen`, and the warning remained `Possible timer handling issue on cpu=0
  timer-softirq=0`. This supported the narrower forward-progress hypothesis:
  the pending-only vtimer LR was present, but VM2/vCPU0 could still be
  preempted by host scheduling before Linux ran far enough past `cpu_do_idle+8`
  to execute `local_irq_enable()`/`daifclr`.
- A bounded ARM64 vGIC-only no-reschedule window for pending-only virtual timer
  LRs was tried and later removed from the submit direction. It improved the
  theory around the WFI masked-IRQ window, but it kept a separate timer rescue
  owner in BEAU that Xen does not need and did not solve VM2 WDT progress.
- The next failing dumps after that change showed an expired CNTV deadline,
  `cntv_el2_masked:Y`, a pending-only timer LR, and no later timer sysreg write.
  A live CNTV_CTL sample could be `0x1` even though EL2 computed the deadline as
  expired. That points to this QEMU CPU model not reliably trapping EL1 CNTV_CTL
  reads and not reliably reporting ISTATUS while the physical PPI27 line is
  disabled. The current repair keeps CNTV/PPI27 as the EL2 source event but
  delivers guest PPI27 as a vGIC software level LR. Linux can still observe the
  architecturally expired CNTV timer, while BEAU avoids binding guest timer
  lifecycle to a hardware PPI27 LR.
- The core scheduling follow-up adds a narrow no-op reschedule clear:
  `sched_clear_reschedule_if_current_only()` clears `NEED_RESCHEDULE` only when
  the current non-idle thread is the only runnable/running non-blocking object
  on that pCPU. This remains a narrow scheduler hygiene change, but the timer
  fix must live in the ARM64 vtimer/vGIC lifecycle rather than in a rescue
  budget layered on top of scheduling.
- A later two-layer experiment added a stronger BVT urgent boost for vtimer
  events and raised `VTIMER_LR_RESCUE_RESCHED_BUDGET` from 4 to 16. It did not
  remove the VM2 RCU stall under the pressure gate and expanded common scheduler
  ABI without proving root-cause coverage. Keep that experiment reverted in
  submit-ready patches.
- The retained narrow code path is an ARM64-local final refresh before returning
  to EL1: sample the current CNTV line, update the vGIC timer descriptor, and
  flush current LRs immediately before ERET. This is compatible with the Xen
  model because the guest-visible PPI27 line is rebuilt from software vGIC state
  at bounded EL2 sync points.
- A dynamic tick-gating experiment reduced pCPU1 host timer callbacks
  but still failed the VM2 Linux stress path and changed timing rather than
  fixing the underlying vtimer/vGIC lifecycle.

### VM2 Linux RCU Stall Current Analysis, 2026-06-23

The most likely stall chain is:

1. VM2 Linux enters the idle path and executes WFI with saved guest DAIF.I still
   masked.
2. The virtual timer deadline is already due, so BEAU has a pending virtual
   timer IRQ for guest INTID 27 and may have a pending-only timer LR resident.
3. QEMU can use that pending-only LR only as the architectural WFI wake event.
   Linux has returned from WFI, but it has not yet reached
   `local_irq_enable()`/`daifclr`, so the EL1 timer handler and timer softirq
   have not run.
4. A host timer tick, shared-pCPU scheduling point, or VM console
   switch can take the vCPU away during this short masked-IRQ window.
5. On a later sync, the live LR may be gone without EOI evidence while CNTV is
   still expired and `cntv_el2_masked` remains true. If EL2 does not rebuild a
   guest-visible timer LR before the next guest idle point, Linux can keep
   sleeping with its timer softirq starved.
6. Linux eventually reports RCU stalls such as
   `rcu_preempt kthread timer wakeup didn't happen` and
   `Possible timer handling issue on cpu=0 timer-softirq=0`.

This is related to vCPU scheduling latency, but the failed urgent-BVT
experiment shows that scheduler preference alone is not sufficient. The root
fix should preserve the virtual timer line across the pending-only-LR/WFI
window and rebuild guest-visible delivery at bounded vGIC/vtimer sync points.

Selected repair direction after comparing Xen:

1. Do not keep extending TWI/LR rescue. A pending-only timer LR is normal vGIC
   software level state, not a separate timer owner.
2. Keep the pre-ERET vtimer/vGIC refresh so a loaded vCPU returns to EL1 with
   current CNTV source state reflected in the software descriptor and LR array.
3. Remove the hardware-backed PPI27 LR binding. CNTV/PPI27 is the EL2 physical
   source event; guest PPI27 is delivered through ordinary vGIC software level
   semantics.
4. Keep guest CNTV_CTL shadow state separate from EL2's live CNTV IMASK. When
   EL2 masks the physical comparator after sampling an expired CNTV, line
   sampling must ignore only that EL2-owned mask, matching Xen's
   `CNTV_CTL & ~MASK` logic.
5. Keep common scheduler changes out of the timer fix unless later evidence
   proves VM2 vCPUs are runnable but not selected for long enough to explain the
   RCU warning.

### RCU Stall Repair Strategy

1. Preserve the fast root-console baseline first. Before any VM2 RCU experiment,
   run the standard QEMU regression and require VM0 Zephyr plus the configured
   Linux guests to pass root identity checks. In the optional
   `CONFIG_ENABLE_VM1_LK=1` path, require VM1 LK console progress instead.
2. Keep the per-VM Linux DTS files under `sdk/image/linux/vm1/` and
   `sdk/image/linux/vm2/` unchanged. Core scheduler changes require explicit
   confirmation; the approved scope for this follow-up is the
   `core/schedule.c` no-op reschedule clear, not a scheduler algorithm switch.
3. Reproduce the stall from VM2 root shell with the 4-vCPU configuration. On
   the first RCU warning, return to the BEAU shell and capture `dumpstat 2`,
   `vcpus`, `schedstat`, and `irqstat`.
4. In `dumpstat 2`, compare vCPU0 against the other VM2 vCPUs: live CNTV_CTL,
   CNTV_CVAL, CNTVCT, `cntv_el2_masked`, timer virq, LR0/LR1, vGIC descriptor
   pending/active/level bits, AP registers, and host PPI27 state.
5. Fix in the ARM64 vGIC/vtimer lifecycle, not in Linux bootargs. The likely
   repair boundary is a narrow synchronization point where EL2 already knows a
   timer LR completed or a loaded vCPU is leaving/entering guest execution.
6. Keep guest-visible CNTV state and EL2 host-mask state separate. EL2 may need
   to throttle host PPI delivery, but Linux's timer handler must still be able
   to observe an architecturally expired CNTV timer when it handles PPI27.
7. If a fix samples live CNTV, do it only at bounded points such as vCPU
   switch-in/out, timer IRQ injection, maintenance/EOI handling, or explicit
   guest exits. Avoid background backup timers or always-on polling in the QEMU
   3OS path unless a later design proves they are required.
8. Treat the RCU fix as complete only when VM2 reaches the initramfs `uos` root
   shell, remains responsive long enough to cover the earlier RCU stall window,
   emits no `timer-softirq=0` RCU warning, and VM0/VM1 still pass the regression
   gate.

### Experiments Not To Repeat

- Marking pending-only timer LRs with EOI caused maintenance storms or early
  boot stalls in BEAU's current vGIC model. Xen's newer vGIC sets EOI for
  software level IRQs, but it also has AP-list/LR fold semantics that BEAU does
  not yet fully mirror. Do not reintroduce timer EOI maintenance without that
  broader sync model.
- Requesting underflow maintenance (`HCR.UIE`) for rescued pending-only timer
  LRs caused an underflow maintenance storm: dumps showed `HCR.UIE`, `MISR.U`,
  one still-valid pending timer LR, and hundreds of thousands of vGIC
  maintenance interrupts while VM2 Linux timed out. Do not use UIE as a
  re-flush mechanism for this rescued timer window.
- Replacing EL2's live CNTV mask with broad host PPI27 enable/disable logic
  slowed or stalled RTOS console validation and was reverted. Disabling PPI27
  at the redistributor can also make Linux-visible CNTV_CTL.ISTATUS unreliable
  on the QEMU CPU model used here. The current accepted variant keeps the PPI
  enabled and masks only host priority while a vGIC timer LR owns delivery.
- Dropping stale pending-only timer LRs from WFI or broad vGIC sync paths made
  startup visibly slower and was reverted.
- Rewriting the WFI pending-only timer LR as Active+Pending made forward
  progress worse and was reverted.
- Re-enabling WFI/WFE traps slowed the QEMU 3OS scenario and affected VM0/VM1.
  Keep HCR_EL2.TWI/TWE clear unless a specific diagnostic run requires trapped
  idle instructions.
- Porting anoa's full switch-out backup timer model directly to BEAU caused a
  large VM2 slowdown. Do not arm a backup timer on every vCPU unload in the
  current QEMU 3OS path.
- Skipping host softirq processing on physical IRQ return when a guest IRQ is
  pending starved the BEAU shell/console during the VM2 held-Enter scenario and
  was reverted. Keep `do_softirq_no_irqenable()` on the physical IRQ exit path.
- Dynamically disabling scheduler ticks when only the current thread is runnable
  is not the right fix. It can hide pCPU tick pressure but does not guarantee
  VM2 timer forward progress and should stay reverted unless a broader scheduler
  design is reviewed separately.
- Expanding the vITS software model to a static 8192-LPI descriptor array
  caused QEMU to stop producing useful shell output for more than 60 seconds.
  Keep the compact active-window model unless dynamic allocation is added.
- Expanding the generated QEMU vFDT PL011 clocks to include both `uartclk` and
  `apb_pclk` did not affect VM2. VM2 uses its embedded per-VM Linux DTB under
  `sdk/image/linux/vm2/`, whose DTS already has both clocks.

### Current WDT/vtimer Progress, 2026-06-26

User-visible status in that 2026-06-26 run:

- VM1 LK keeps kicking the BEAU watchdog.
- VM0 Zephyr can kick several times, then its watchdog path can stop.
- VM2 Linux still normally kicks only once.
- The Linux BEAU watchdog driver is not the likely root cause. The common
  failing boundary remains BEAU's CNTV/PPI27 to vGIC/LR delivery model.

Observed timer/interrupt ownership:

```text
CNTV deadline/ISTAT
        │
        ▼
EL2 vtimer source sampler
        │
        ▼
VGIC descriptor: pending/active/level
        │
        ▼
ICH list register + CPU interface state
        │
        ▼
guest IAR/EOIR/DIR and timer handler
```

- VM2 Linux uses guest-visible virtual timer INTID/PPI27. Current evidence does
  not support changing the Linux DTS timer interrupt.
- Zephyr should also be treated as a PPI27/CNTV guest in the current model.
  The earlier PPI27/PPI30 confusion has been corrected conceptually: PPI26 is
  the EL2 hypervisor timer source and must remain host-owned, while PPI30 is
  guest physical-timer emulation.
- Linux failing dumps place vCPUs around the instruction after `wfi` in
  `cpu_do_idle()`: Linux was woken from WFI, but it has not progressed far
  enough to run the timer handler/softirq path.
- Representative dumps show guest virq27 pending/level, host PPI27 enabled, and
  shadow/live LR divergence. One useful pattern is a shadow pending HW timer LR
  while the live LR has become invalid without clear guest EOI evidence.
- Host PPI27 active-bit synchronization, live PMR synchronization, pre-ERET
  vtimer/vGIC refresh, and keeping the live CNTV mask separate from guest
  CNTV_CTL were all useful diagnostics, but none alone is a proven fix for Linux
  WDT.

Runtime check on 2026-06-26 17:40+08 after moving the host scheduler tick to
CNTHP/hypervisor timer:

- Reproduced the VM2 Linux failure. The BEAU WDT line reported
  `event:timeout vm2: Linux status: stuck kick: 1` at about 40.2s. VM0 Zephyr
  and VM1 LK continued to kick every 5s afterward, proving the BEAU shell,
  watchdog thread, and WDT hypercall path were still alive.
- `vmstat` showed all VM2 vCPUs still running/current on their pCPUs, not
  blocked waiting for CPU time. VM2 vCPU0 and vCPU1 stayed on exclusive pCPUs,
  and vCPU2/vCPU3 stayed current on their shared pCPUs. This rules out "Linux
  did not get scheduled" as the first explanation for the missing second kick.
- `schedstat` and `irqstat` showed a CNTHP host scheduler-timer ownership
  problem on VM2 pCPUs: the CNTHP PPI entry count stayed at 1 on pCPU1 and
  pCPU4, and stayed at 29/17 on pCPU6/pCPU7 across later samples. In the same
  interval, CNTHP counts on pCPU0/pCPU3/pCPU5 kept increasing by tens of
  thousands. The scheduler timer counters for pCPU6/pCPU7 also stayed flat at
  24/13 while their RTDS deadlines were already overdue.
- `irqstat` showed host CNTV PPI delivery did not progress for VM2: total CNTV
  PPI entry count stayed at 4, with zero counts on VM2's pCPU1/pCPU4/pCPU6/pCPU7.
- `dumpstat 2` showed each VM2 vCPU had an expired guest CNTV deadline, live
  `CNTV_CTL` with enable+mask, `el2_masked:Y`, a vGIC timer line still
  pending/level, and a timer LR present. Representative vCPU0 values:
  `guest_delta:-11970241636`, `cntv_irq en:Y pend:N act:N`,
  `vgic enabled:Y pending:Y active:N level:Y`, live LR0 pending, and
  `el2-mask active:Y max-age.us:191500331`.
- The guest trace had very large gaps between last resume and the next exit
  on VM2 vCPUs, for example vCPU0 showed about 187.5s. That means EL1 was
  running for a long time without another useful timer exit/EOI/reprogram
  boundary, even though EL2 kept seeing the vGIC timer line as pending/level.
- Older console diagnostics showed no input backlog and a full VM2 console ring. The VM2
  console path was congested because Linux kept printing or had printed enough
  to fill the ring, but there was no shell-input blockage causing the WDT miss.
- A later per-vCPU dump initially looked like a repeated HVC because older
  trace entries contained `EC=0x16`, but the live `last-exit` source was the
  IRQ path and its EC field was invalid diagnostic filler. Symbolicating
  `elr:0xffff8000804ef9ec` against the VM2 Linux image maps it to
  `tioclinux()` in `drivers/tty/vt/vt.c`, not to the WDT HVC site. The WDT
  driver's actual `hvc #0` is in `beau_wdt_timerfn()`.
- In that same dump, VM2 had only vCPU0/vCPU1 running while vCPU2/vCPU3 were
  still `init/blocked`. vCPU0 had `requests:pending:0x2`, the virtual timer
  line was `enabled:Y pending:Y active:N level:Y`, and LR0 was
  `0x500002000000001b`: a pending-only timer LR with the EOI bit still set.
  The maintenance snapshot showed `MISR.EOI` and the pending-only timer
  preserve counters were in the millions. This stale EOI-marked timer LR can
  keep EL2 in a maintenance loop while the live CNTV source remains masked.
- A later dump after clearing the stale EOI bit still showed no Linux WDT
  growth. The timer offset relation was valid: live `CNTVCT` matched
  `CNTPCT - CNTVOFF_EL2` within the expected sampling window, so the counter
  offset formula was not the active fault. The remaining bad state was
  `CNTV_CTL=0x3` in the live register, `cntv_el2_masked:Y`, PPI27
  pending/level in vGIC state, and no new guest timer sysreg writes.
- That points to a guest-visible timer-control problem: BEAU masks the live
  CNTV comparator with `CNTV_CTL.IMASK` while vGIC owns PPI27, but VM2 Linux
  reads `CNTV_CTL` in `arch_timer_handler_virt()` before running its clockevent
  handler. If Linux sees EL2's private IMASK instead of guest-visible
  `ISTATUS`, it treats the interrupt as not handled and never reprograms the
  next deadline.

Conclusion from this run:

- The single Linux WDT kick proves the guest driver reached
  `HC_VM_WDT_KICK` once, but the supplied dump does not prove an HVC replay
  loop. A generic `handle_hvc64()` ELR advance experiment made 3OS startup
  abnormal and must not be kept as the VM2 WDT fix.
- The actionable bad state remains timer/vGIC state. Clearing stale
  `ICH_LR_EOI` from pending-only timer LRs is necessary, but it was not
  sufficient: Linux still did not reprogram the timer after seeing a pending
  PPI27.
- The next repair is to keep `CNTVCT_EL0` direct, preserving
  `CNTVCT = CNTPCT - CNTVOFF_EL2`, while trapping guest CNTV control/compare
  registers through `CNTHCTL_EL2.EL1TVT`. The trap path returns the guest
  shadow `CNTV_CTL` with computed `ISTATUS`, hiding EL2's private live IMASK
  from Linux.
- Console backlog, Linux WDT code, broad scheduler starvation, generic HVC PC
  advancement, and timer-offset sign are not the primary explanations supported
  by these dumps.

Reference-model findings to preserve:

- Timer expiry should be treated as a source line that feeds the vGIC model,
  not as an ad hoc wakeup side channel.
- On every relevant exit/sync point, rebuild software state from the live list
  registers before deciding whether an IRQ was delivered, lost, still pending,
  or already active.
- Local SGI/PPI LRs may need different priority and active handling from SPIs,
  but a priority-only local LR experiment compiled and still did not fix Linux.
- WFI handling in the reference model is tied to the interrupt tracker and timer
  source state. A broad "trap every WFI" experiment in BEAU caused many WFI
  exits and still did not fix Linux, so keep WFI trapping as narrow diagnostic
  plumbing rather than the main repair.

2026-06-26 local-tree status, superseded by the 2026-07-08 section below:

- The vGICv3 ITS code has been split out of the main vGICv3 file in the local
  tree. Treat that as organization, not as a WDT fix.
- The vtimer helper recognizes loaded guest CNTV/PPI27, emulated guest
  CNTP/PPI30, and CNTHP/PPI26 as three separate roles. CNTHP remains reserved
  for the EL2 scheduler timer path.
- Guest CNTV/PPI27 now follows the Xen-style model: the physical CNTV interrupt
  triggers EL2 to sample and mask the source, then vGIC injects guest PPI27 as a
  software level LR. BEAU no longer sets `desc->hw`/`desc->pirq` for PPI27 and
  no longer forces the physical PPI27 active bit before publishing LRs.
- The local tree contains extra diagnostics around host PPI27 state, LR state,
  and vtimer/vGIC trace points. Keep these diagnostics until the WDT fix is
  validated.
- QEMU builds have passed after the recent local experiments, and a rk356x
  build passed earlier in the investigation. The latest QEMU runtime check still
  failed WDT progress, so no current patch should be described as accepted.

Experiments now considered insufficient:

- Repeated TWI rescue/rearm around lost pending-only LRs.
- Globally enabling WFI trapping for normal guest idle loops.
- Treating active-bit synchronization alone as the root fix.
- Treating PMR synchronization alone as the root fix.
- Lowering local SGI/PPI LR priority alone as the root fix.
- Changing Linux watchdog code, Linux bootargs, or Linux timer DTS data.

Xen comparison and selected fix:

1. Xen programs CNTHP for the host scheduler timer and handles guest CNTV expiry
   separately in `vtimer_interrupt()`.
2. Xen masks the physical CNTV comparator on EL2 entry and injects a normal vGIC
   line for the current vCPU. It does not use a hardware LR that binds guest
   PPI27 to physical PPI27.
3. Xen's vtimer update ignores the EL2-only mask bit when deciding whether the
   guest-visible virtual timer line is asserted.
4. BEAU should therefore keep CNTV as the source sampler and inject guest PPI27
   through ordinary vGIC software level semantics. The code now removes the
   PPI27 hardware LR special case, removes timer LR/WFI rescue owner state, and
   lets normal LR sync/EOI/reprogram paths update the sampled level.
5. A direct attempt to mark BEAU's pending-only software timer LR as
   EOI-maintained made VM2 Linux fail earlier during boot. Treat that as
   disproven for the current BEAU vGIC sync model, even though Xen's newer
   vGIC can set EOI for software level IRQs.
6. Xen's HVC paths are not proof that BEAU should generically advance ELR in
   `handle_hvc64()`: Xen's SMCCC HVC C handler does not explicitly advance the
   PC, and its Xen-hypercall path only rewinds PC for preempted calls. Xvisor's
   PSCI HVC path does advance PC inside its PSCI emulation. BEAU must validate
   its saved-ELR convention with a direct HVC-site dump before changing this
   path again.
7. Xvisor's GICv3 LR encoder writes `ICH_LR_EOI` only when the software LR
   flags explicitly request `VGIC_LR_EOI_INT`; BEAU should therefore clear an
   accidentally preserved EOI bit from software pending-only vtimer LRs instead
   of treating it as guest EOI evidence.
8. Xen also updates level-triggered emulated device lines on guest-exit
   boundaries before syncing LR state. That remains useful reference material,
   but the current VM2 dump first requires fixing the stale EOI-marked
   pending-only timer LR.
9. Xen can mask the hardware CNTV comparator in EL2 because its vtimer update
   treats the mask as hypervisor-private. BEAU now follows the same principle by
   trapping guest CNTV timer registers and leaving only `CNTVCT_EL0` direct.

Historical validation direction from this 2026-06-26 analysis:

1. Build QEMU BEAU and run `git diff --check` before runtime testing.
2. Boot VM0/VM1/VM2 and confirm Linux WDT continues beyond the previous single
   kick. Manual acceptance target: LK, Zephyr, and Linux all continue beyond at
   least 20 watchdog kicks without Linux RCU/timer-softirq warnings.
3. On failure, capture `vmstat`, `vcpus`, `schedstat`, `irqstat`, and
   `dumpstat 2`. Expected post-fix dumps should show no hardware-backed PPI27
   LR and no timer rescue state; guest PPI27 state should be readable from the
   software descriptor and LR state.

### QEMU 3OS WDT/RTDS Progress, 2026-07-08

Current validated status:

- The default QEMU baseline now keeps Zephyr, Linux-1, and Linux-2 at four
  vCPUs each. The current result is not achieved by reducing guest vCPU counts.
- QEMU `cpu-affinity` is ordered: entry N maps vCPU N to that pCPU. Example:
  `<1 6>` means vCPU0 runs on pCPU1 and vCPU1 runs on pCPU6.
- The current QEMU mapping is VM0 `<1 2 3 5>`, VM1 Linux-1 `<4 2 5 6>`, and
  VM2 Linux-2 `<7 0 3 6>`. VM vCPU0 pCPUs are unique: pCPU1, pCPU4, and pCPU7.
  Secondary vCPUs may share pCPUs; pCPU0 is used only by a secondary vCPU to
  avoid three-way RTDS contention on the secondary pool.
- The July timeout was not simply a vCPU-count problem. Diagnostics showed VM0
  vCPU0 running on its exclusive pCPU while VM0 secondary vCPUs on shared RTDS
  pCPUs stayed runnable with `cpu-wait,rtds-overrun` and zero runtime; Linux
  VMs kept kicking in that sample.
- ARM64 physical IRQ return now bounds the guest IRQ forward-progress bypass.
  A pending guest IRQ or Linux IRQ context can defer host scheduling only for a
  short budget, then `schedule()` must run and let a shared-pCPU peer run.

The scheduling handoff is:

```text
physical IRQ exit
    -> do_softirq_no_irqenable()
    -> refresh current CNTV/vGIC
    -> pending guest IRQ or Linux IRQ context?
        yes -> short bounded return-to-guest window
        no  -> schedule peer vCPU when reschedule is pending
    -> budget exhausted: schedule peer vCPU
    -> pre-ERET vtimer/vGIC flush
```

Validation run on 2026-07-08:

- `git diff --check` passed for the ARM64 vCPU-exit, VM creation, DTS parser,
  VM config, and QEMU DTS changes.
- QEMU and rk356x builds passed with the ARM64 bare-metal toolchain.
- A QEMU watchdog observation with the default 4+4+4 vCPU layout ran for about
  60 seconds without a BEAU WDT timeout; VM0, VM1, and VM2 all continued to kick
  through kick 13.
- A later `vmstat` observation showed all three VMs alive, zero BEAU WDT
  timeouts, and VM0 secondary vCPUs no longer reporting
  `cpu-wait,rtds-overrun`.

## Code Commenting Guidelines

New ARM64 virtualization code should use English comments for design intent,
not line-by-line narration. The goal is to make the virtualization model
auditable when memory, interrupt, and CPU state crosses an EL2/EL1 boundary.
Comment style: keep the wording concise and easy to understand, and combine
short text with `sdk/box-drawing.md`-style diagrams for complex state machines or
ownership handoffs.

- Add a module-level comment when a file owns an architectural subsystem or a
  cross-layer boundary, such as host stage-1 mappings, guest stage-2 mappings,
  vGIC state, physical GIC routing, vCPU entry/exit, or trap dispatch.
- Add structure comments for state that mirrors hardware or guest-visible
  state, especially EL2 control registers, vGIC list registers, interrupt
  descriptors, VM layout data, and deferred trap state.
- Add comments before non-obvious state transitions: syncing hardware list
  registers into software state, flushing pending virtual IRQs into LRs,
  translating a stage-2 abort into an MMIO request, loading or unloading EL2
  guest context, switching between host IRQ and guest IRQ paths, and validating
  VM memory isolation.
- For vGICv3/vtimer paths, comments should identify the owner at each step:
  guest-visible CNTV/CNTP state, EL2 shadow state, live CNTV IMASK, vGIC
  descriptor pending/active bits, pending bitmap scan state, and hardware LRs.
  Use a compact diagram when a fix crosses more than two of these owners.
- For abort/trap handling code, comments must distinguish instruction aborts
  from data aborts and from broader memory abort terminology. State the trigger
  scenario being handled, such as instruction fetch from unmapped or
  execute-never memory, load/store MMIO data aborts, stage-2 translation faults,
  permission faults, or unexpected host EL2 aborts.
- Document the design invariant and failure mode when code enforces isolation,
  ordering, or ownership. Examples: QEMU RTOS stage-2 RAM is identity-mapped,
  vGIC MMIO is protected by the VM vGIC lock, and physical INTIDs must pass
  through the ARM64 IRQ domain before common IRQ dispatch.
- Avoid comments that restate a simple assignment, branch condition, or function
  name. Prefer clear names for local mechanics and reserve comments for intent,
  architecture rules, and concurrency assumptions.
- Keep comments close to the code that enforces the rule. If a later patch
  changes the rule, update the comment in the same patch.

## Current Limitations

The ARM64 port is now capable of booting Zephyr, Linux-1, and Linux-2 on QEMU,
but it is still a bring-up target rather than a complete architectural
virtualization port. The optional `CONFIG_ENABLE_VM1_LK=1` path remains useful
for RTOS comparison, but it is not the default QEMU 3OS baseline.

- vGICv3 is sufficient for the current Zephyr and 4-vCPU Linux boot path, and
  now has initial Linux vITS/LPI support for ITS enumeration. It is not a
  complete GICv3/ITS model yet: active-state stress cases, deeper redistributor
  behavior, PCI/MSI requester identity plumbing, physical ITS passthrough,
  dynamic LPI allocation, and richer distributor coverage still need work.
- `GITS_TRANSLATER` direct MMIO injection currently uses device ID 0 as a local
  test path. Real passthrough/MSI code should call `arm64_vgicv3_inject_msi()`
  after requester identity and event mapping are plumbed.
- QEMU and rk356x VM layout are now DTS-authored, but the DTS is still a
  build-time static configuration source. Runtime host FDT parsing is not yet
  used to derive VM layout.
- Zephyr is an RTOS raw image and boots with `GUEST_FLAG_NO_FW`. Linux-1 and
  Linux-2 use the loader/module path for `Image`, `Initramfs.cpio.gz`, and
  per-VM Linux-on-BEAU DTBs.
- Linux-1 and Linux-2 run as 4-vCPU guests. Both paths should reach the
  initramfs `uos` shell during QEMU validation. The held-Enter and `vsh` switch
  pressure tests remain part of the acceptance gate because they previously
  reproduced RCU/vtimer and console-forward-progress failures.
- Linux-2 currently omits `earlycon=pl011,0x09000000` and uses `loglevel=4` to
  reduce shared-core console pressure during latency investigation.
- VM console output from SMP guests can interleave because multiple guest CPUs
  write concurrently to the same PL011 console.
- Stage-2 mapping is still static for the QEMU `virt` platform.
- The regression harness covers the core multi-VM boot sequence and has a manual
  `--stress-vsh-switch` path for the current console pressure scenario. That
  stress path previously reproduced the VM2 RCU/vtimer failure and should remain
  part of the acceptance gate for future fixes.

## Next Phase Target: ACRN-DM Android Bring-Up

The next major target is to move from the current static VM2 Linux validation
path toward an ACRN-style post-launched Android User VM. `sdk/udev` contains the
ACRN Device Model source, but the DM path expects a Linux Service VM with
`/dev/acrn_hsm` or `/dev/acrn_vhm` and ioctl-backed VM creation, memory mapping,
vCPU setup, interrupt injection, and ioreq completion. The current ARM64 QEMU
scenario still uses Zephyr as the Service VM, sets `CONFIG_HAS_HSM=0` and
`MAX_POST_VM_NUM=0`, and the ARM64 hypercall dispatcher returns `-ENOTTY` for
the VM-management, ioreq, and IRQ operations that `acrn-dm` needs.

Do not treat Android boot as a simple replacement of VM2 Linux images. The
ACRN-compatible route is to first close the Linux Service VM, HSM, and
`acrn-dm` control loop, then launch Android as a post-launched User VM through
the device model.

Target architecture:

1. Provide a QEMU scenario variant with a Linux Service VM that can run
   `sdk/udev/acrn-dm`.
2. Enable one Android post-launched VM slot for the ARM64 QEMU platform by
   introducing the required `CONFIG_HAS_HSM` and `MAX_POST_VM_NUM` support.
3. Implement the minimal ARM64 HSM path needed by `sdk/udev/core/vmmapi.c`:
   `HC_CREATE_VM`, `HC_DESTROY_VM`, `HC_START_VM`, `HC_PAUSE_VM`,
   `HC_CREATE_VCPU`, `HC_SET_VCPU_REGS`, `HC_VM_SET_MEMORY_REGIONS`,
   `HC_SET_IOREQ_BUFFER`, `HC_NOTIFY_REQUEST_FINISH`, `HC_INJECT_MSI`, and
   `HC_SET_IRQLINE`.
4. Add the Service VM Linux userspace/kernel side needed to expose
   `/dev/acrn_hsm` or `/dev/acrn_vhm` and translate the ACRN ioctls used by
   `acrn-dm` into ARM64 hypercalls.
5. Add an ARM64 raw-image loader path to `sdk/udev` for Android. The existing
   `-k` path enters the x86 bzImage loader, so Android needs a separate loader
   that copies an ARM64 `Image`, Android ramdisk, and DTB into guest RAM, sets
   vCPU0 entry to the kernel load address, and enters the ARM64 boot ABI with
   `x0` pointing at the guest DTB.
6. Start with a minimal Android ramdisk acceptance target: Android kernel logs
   appear on PL011, `/init` starts, and `androidboot.*` boot parameters are
   visible before requiring full `system`, `vendor`, or `userdata` mounts.
7. Add virtual devices only after the minimal Android init path works. The first
   required devices are `virtio-blk` for Android partitions and a console path;
   `virtio-net`, `virtio-input`, `virtio-gpu`, RPMB, and Trusty should follow as
   separate enablement steps.

A future Android launch command should be shaped like ACRN-DM rather than the
current static QEMU loader:

```bash
acrn-dm \
  -m 2048M \
  --cpu_affinity 1,2,3,4 \
  --arm64_dtb sdk/image/android/beau-android.dtb \
  -k sdk/image/android/Image \
  -r sdk/image/android/ramdisk.cpio.gz \
  -B "console=ttyAMA0 earlycon=pl011,0x09000000 loglevel=7 androidboot.console=ttyAMA0" \
  -s 2,virtio-blk,sdk/image/android/super.img \
  -s 3,virtio-blk,sdk/image/android/userdata.img \
  Android
```

The `--arm64_dtb` option does not exist yet; it represents the intended
`sdk/udev` interface for the ARM64 Android loader. Until that path exists, the
fastest debug-only experiment remains replacing the current VM2 Linux
`Image`/`Initramfs.cpio.gz`/DTB inputs with Android kernel and ramdisk assets,
but that is only a kernel/DTB/timer/GIC/PL011 smoke test and is not the final
ACRN-DM Android launch model.

## Next Steps

1. Promote the default QEMU 3OS WDT result into a regression gate. Run repeated
   cold boots, longer watchdog observations, Linux-1/Linux-2 shell commands,
   held-Enter, and `vsh` switching, and save `vmstat`, `schedstat`, `irqstat`,
   and `dumpstat` snapshots for failures.
2. Make RTDS and bounded guest-IRQ scheduling behavior more observable. Useful
   follow-ups include per-vCPU counters for forward-progress budget expiry,
   DTS-authored RTDS period/budget policy, and shell output that correlates
   `cpu-wait`, `rtds-depleted`, and `rtds-overrun` with recent scheduling
   decisions.
3. Continue reconciling BEAU's CNTV/PPI27 handling with the selected software
   level model under stress. Avoid more isolated TWI/rescue knobs; live LR, AP,
   EOI/DIR, host PPI27, and software descriptor ownership must stay consistent.
4. Extend `scripts/regress.py` with more Linux initramfs-shell commands, reboot
   coverage, repeated cold boots, WDT-kick thresholds, and saved log artifacts
   suitable for CI.
5. Complete the Linux `virtio_console` path under `sdk/bsp/virtio` while
   keeping RTOS console traffic on vPL011. This keeps the intended split:
   RTOS -> `vpl011.c`, Linux -> `virtio_console.c`.
6. Keep reducing platform-specific static C configuration. Platform
   directories should converge on `platform.dts` plus `platform.S`; use static
   C headers only for compile-time limits or information that cannot come from
   DTS.
7. Audit ARM64 abort handling to confirm whether guest instruction aborts are
   trapped and diagnosed correctly. Cover instruction fetch aborts separately
   from data abort MMIO paths, document the expected ESR/FAR/HPFAR trigger
   scenarios, and update comments so instruction abort, data abort, and broader
   memory abort terminology are not conflated.
8. Start the ACRN-DM Android bring-up by creating a Linux-Service-VM QEMU
   scenario variant and defining the minimal ARM64 HSM/ioctl/hypercall contract
   needed by `sdk/udev`.
9. Add the ARM64 raw-image Android loader interface to `sdk/udev`, including an
   explicit DTB argument and vCPU0 register setup for the ARM64 boot ABI.
10. Extend the static DTS model toward host-FDT-derived memory and device data
   where it helps reduce board assumptions without weakening the current static
   VM placement invariants.
11. Bring up rk356x hardware manually, then capture the validated RAM, UART,
   GIC, and boot-image placement assumptions back into the platform files.
