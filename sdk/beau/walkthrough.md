# BEAU ARM64 Walkthrough

This guide is the first reading path for the ARM64 implementation. It explains
how the QEMU/rk356x bring-up is connected from the first EL2 instruction to a
running guest VM, and points to the source files that own each step.

## Big Picture

BEAU runs as an ARM64 EL2 hypervisor. The current learning target is a static
platform model: platform code describes the host layout, VM layout, guest RAM,
and guest devices before runtime.

```text
firmware / QEMU
      |
      v
BEAU EL2 host
      |
      +--> VM0 Zephyr service VM
      +--> VM1 LK pre-launched VM
      +--> VM2 Linux pre-launched VM
```

The implementation stays small by keeping four ownership boundaries clear:

```text
platform config  -> board and VM policy
core/            -> VM, vCPU, scheduler, timer, and boot-image flow
arch/arm64/      -> EL2 CPU, MMU, IRQ, timer, and trap mechanics
arch/arm64/guest -> stage-2, vCPU entry/exit, vGIC, vtimer, and vPL011
```

## Reading Order

Read the code in this order when learning the ARM64 path:

```text
Makefile / arch/arm64/Makefile
  -> arch/arm64/boot/cpu_entry.S
  -> arch/arm64/init.c
  -> arch/arm64/mmu.c
  -> core/schedule.c / core/timer.c / core/softirq.c
  -> core/vm.c
  -> core/vcpu.c
  -> core/vm_load.c
  -> arch/arm64/platform/qemu/vm_config.c
  -> arch/arm64/guest/vm.c
  -> arch/arm64/guest/guest_memory.c
  -> arch/arm64/guest/vcpu.c
  -> arch/arm64/guest/guest_entry.S
  -> arch/arm64/vector.S
  -> arch/arm64/guest/vcpu_exit.c
  -> arch/arm64/guest/vgicv3.c
  -> arch/arm64/guest/vtimer.c
  -> arch/arm64/guest/vpl011.c
```

## Build Shape

`Makefile` chooses a static ARM64 platform path when the command uses
`ARCH=arm64 PLATFORM=qemu` or `ARCH=arm64 PLATFORM=rk356x`. The ARM64 makefile
then selects host EL2 files, guest virtualization files, and platform VM config
files.

```text
ARCH=arm64 PLATFORM=qemu
      |
      v
arch/arm64/Makefile
      |
      +--> host EL2 code: init, MMU, IRQ, timer, GIC
      +--> guest code: stage-2, vCPU, vGIC, vtimer, vPL011
      +--> platform code: qemu VM layout and image tags
```

QEMU is the normal learning platform. rk356x follows the same structure, but
hardware validation is manual.

## EL2 Host Boot

The first ARM64 source file is `arch/arm64/boot/cpu_entry.S`. It does only the
minimum architectural setup needed before C code can run:

```text
_start
  -> mask exceptions with DAIF
  -> verify CurrentEL == EL2
  -> select SP_EL2
  -> install VBAR_EL2
  -> clear BSS
  -> set the boot stack
  -> init_primary_pcpu()
```

`arch/arm64/init.c` then builds the host world in two phases. The BSP performs
global initialization first; after the host MMU and interrupt controller are
ready, all pCPUs converge on the common per-CPU initialization path.

```text
BSP init_primary_pcpu()
  -> percpu identity
  -> early console
  -> EL2 stage-1 MMU
  -> early GIC
  -> switch to per-CPU stack
  -> init_pcpu_comm_post()

AP init_secondary_pcpu()
  -> percpu identity from MPIDR
  -> enable existing EL2 stage-1 MMU
  -> init_pcpu_comm_post()
```

The common post path installs IRQs, SMP calls, host timers, the scheduler, shell
support, and finally launches static VMs before entering the idle thread.

## Host Stage-1 MMU

`arch/arm64/mmu.c` owns EL2 stage-1 mappings. It is intentionally separate from
guest memory isolation.

```text
EL2 VA/HVA
    |
    v
stage-1 page table
    |
    v
host PA/HPA
```

The current bootstrap model identity-maps host RAM and platform MMIO so early
code, exception vectors, memory copies, and device access use the same numeric
address as the physical address placed by firmware or QEMU.

The generic walker lives in `core/mmu.c`. ARM64 supplies descriptor callbacks
for stage-1 and stage-2, while the common walker owns page allocation, hierarchy
walking, and large-page splitting.

## Scheduler And Timers

BEAU models a vCPU as a scheduler thread. `core/schedule.c` owns partitioned
per-pCPU scheduling, and architecture code owns the guest state saved or loaded
when a vCPU thread is switched.

```text
scheduler picks vCPU thread
      |
      v
arch_context_switch_in()
  -> load guest EL1 sysregs
  -> load stage-2 root
  -> load vtimer and vGIC state
      |
      v
guest runs at EL1
      |
      v
arch_context_switch_out()
  -> save guest EL1 sysregs
  -> save vtimer and vGIC state
```

`core/timer.c` owns common software timers. `arch/arm64/timer.c` programs the
EL2 hypervisor timer `CNTHP` for host deadlines. Guest `CNTV` and trapped guest
`CNTP` are virtualized in `arch/arm64/guest/vtimer.c`; they must not own the
host scheduler tick.

## VM Creation

The platform VM table is the source of policy. Common VM code consumes that
policy without adding board-specific ordering rules.

```text
arch/arm64/platform/qemu/vm_config.c
      |
      v
core/vm.c: launch_vms()
      |
      v
create_vm()
  -> arch_init_vm()
  -> create one vCPU per cpu_affinity bit
      |
      v
VM_CREATED
      |
      v
init_vm_boot_info() / prepare_os_image()
      |
      v
start_vm()
  -> arch_vm_prepare_bsp()
  -> launch_vcpu(BSP)
      |
      v
VM_RUNNING
```

`VM_CREATED` means the VM object, vCPU objects, stage-2 root, and virtual device
state exist. `VM_RUNNING` starts only after the BSP vCPU thread is made runnable.
Guest AP vCPUs are later started through the guest-visible PSCI `CPU_ON` path.

## Image Loading And Guest Memory

`core/vm_load.c` is a dispatcher. Boot discovery code fills `vm->sw` with source
modules and load addresses, the selected loader copies or places the kernel, and
ARM64 guest-memory helpers enforce the configured RAM contract.

```text
bare_boot_options / loader modules
      |
      v
init_vm_boot_info()
      |
      v
prepare_os_image()
      |
      v
rawimage_loader()
      |
      v
copy_to_gpa()
      |
      v
gpa2hpa()
```

For the current static ARM64 platforms, guest RAM is intentionally simple:

```text
guest IPA/GPA == host PA/HPA
```

Device windows such as vGIC and vPL011 are not mapped as RAM. They are left
unmapped in stage-2 so guest accesses trap to EL2 as MMIO.

## Stage-2 VM Memory

`arch/arm64/guest/vm.c` owns VM stage-2 setup. The host stage-1 map lets BEAU
access memory; each VM stage-2 map controls what a guest can access.

```text
guest IPA/GPA
      |
      v
stage-2 page table
      |
      v
host PA/HPA
```

The current QEMU/rk356x bring-up validates the identity RAM invariant before
building the map. That makes raw-image loading and early debugging readable:
the address printed by the guest is the same numeric address BEAU uses for the
configured guest RAM window.

## vCPU Entry

`core/vcpu.c` creates a common `thread_object` for each vCPU. ARM64 code owns
the durable guest state stored in `vcpu->arch`.

```text
vcpu->arch.regs
      |
      v
arch/arm64/guest/guest_entry.S
  -> build temporary cpu_regs frame on the vCPU thread stack
      |
      v
arch/arm64/vector.S: vcpu_exit_return
  -> program ELR_EL2 / SPSR_EL2 / SP_EL1
      |
      v
ERET to guest EL1
```

The persistent register block is not the live EL2 stack. Guest entry and guest
exit copy between `vcpu->arch.regs` and a temporary stack frame so scheduler
context switches stay independent from guest register mechanics.

## Guest Exit

Guest exits enter `arch/arm64/vector.S`, which saves the live CPU state into a
temporary `cpu_regs` frame. `arch/arm64/guest/vcpu_exit.c` then classifies the
exit by `ESR_EL2`.

```text
EL1 guest
  |
  +--> data abort   -> fault IPA -> common MMIO emulation
  +--> HVC          -> PSCI or ACRN hypercall
  +--> sysreg trap  -> vGIC or vtimer emulation
  +--> IRQ          -> host IRQ/softirq, vtimer/vGIC sync
  +--> WFI/WFE      -> guest idle handling and optional reschedule
```

The normal return path updates the durable vCPU state, processes pending vCPU
requests, flushes deliverable virtual interrupts, and returns through the same
`vcpu_exit_return` assembly used by first entry.

## MMIO, PSCI, And Hypercalls

Data-abort MMIO exits use the guest fault IPA from `HPFAR_EL2` and `FAR_EL2`.
The common IO layer dispatches the request by GPA range to ARM64 handlers such
as vGICv3 and vPL011.

```text
guest load/store to unmapped device IPA
      |
      v
stage-2 data abort
      |
      v
vcpu_exit.c builds MMIO request
      |
      v
sdk/bsp/io_req.c range lookup
      |
      +--> arm64_vgicv3_mmio_handler()
      +--> arm64_vpl011_mmio_handler()
```

HVC exits are split into two ABIs:

```text
HVC function ID
      |
      +--> PSCI ID       -> vcpu_exit.c CPU/power emulation
      +--> ACRN HC_ID    -> arch/arm64/guest/hcall.c
```

ARM64 keeps the ACRN hypercall table small. x86-only operations return
`-ENOTTY` until the required ARM64 dependencies exist.

## Interrupts And vGICv3

`arch/arm64/irq.c` maps ARM64 source IDs into generic ACRN IRQ numbers so common
IRQ code does not need to know GIC INTIDs. The guest-visible interrupt
controller is virtualized in `arch/arm64/guest/vgicv3.c`.

```text
event source / guest GIC write
      |
      v
software IRQ descriptor
      |
      v
pending bitmap
      |
      v
flush into ICH_LR<n>
      |
      v
guest EL1 takes IRQ
      |
      v
EOI / maintenance / sync
      |
      v
software descriptor is authoritative again
```

List registers are a delivery cache for the currently loaded vCPU. The
authoritative interrupt state lives in software descriptors so a vCPU can be
switched out and later reconstructed on another guest entry.

## Timer Virtualization

The ARM timer split is deliberate:

```text
CNTHP -> host EL2 scheduler timer
CNTV  -> guest virtual timer, delivered as PPI27
CNTP  -> trapped guest physical timer, emulated as PPI30
```

`arch/arm64/guest/vtimer.c` saves/restores live CNTV state on vCPU switches and
uses host software timers to keep guest timer deadlines observable even when a
vCPU is not currently running. Guest CNTP is software-emulated and cannot change
the host CNTHP scheduler path.

## Console And Debug

The BEAU shell is the main inspection surface after boot. Useful commands:

```text
vcpus       VM/vCPU binding and state
schedstat   scheduler and per-pCPU runtime state
mmap        host stage-1 and VM stage-2 mappings
irqstat     interrupt routing and counts
dumpstat    saved VM/vCPU state
vsh <id>    switch to a VM PL011 console
```

`arch/arm64/guest/vpl011.c` backs the guest PL011 MMIO window. `vsh <vmid>`
switches interactive ownership to one VM console; non-selected VM output is
buffered and replayed when selected.

```text
guest PL011 MMIO
      |
      v
stage-2 data abort
      |
      v
vPL011 emulation
      |
      +--> selected VM console: interactive path
      +--> other VM console: async buffer
```

Ctrl-D returns from the VM console to the BEAU shell.

## Validation Milestones

For code changes, follow the validation rules in `sdk/sdk.md`. For reading and
manual diagnosis, these are the milestones to look for:

```text
EL2 entry reaches BEAU banner
host stage-1 MMU enables
GIC and per-pCPU timers initialize
secondary pCPUs reach running state
BEAU shell reaches console:\>
VM0/VM1/VM2 are created and started
stage-2 RAM maps match platform config
vcpus shows expected pCPU affinity
vsh 0 reaches Zephyr
vsh 1 reaches LK
vsh 2 reaches Linux initramfs shell
```

When debugging, isolate the first missing milestone. That keeps ARM64 EL2
bring-up, common VM flow, guest image loading, and guest runtime failures from
being mixed together.
