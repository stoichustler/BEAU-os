# Coordinated Guest STR Design

## Goal

Add complete coordinated suspend-to-RAM (STR) and resume support for the BEAU
ARM64 QEMU topology. VM0 Zephyr, VM1 RT-Thread, and VM2/VM3 Linux must suspend
cooperatively, BEAU must quiesce and retain EL2-owned state, and every guest
must resume its existing memory image and execution context after an allowed
wakeup event.

The design uses standard guest-visible PSCI semantics, a transaction-owned
all-VM barrier, deterministic device suspend/resume hooks, and a replaceable
platform PM backend. QEMU is the first implementation and validation target.

## Reference Decisions

The design combines the useful ownership rules from two reference systems
without copying their implementation structure.

- Xen ARM treats `CPU_SUSPEND` as an event-blocked vCPU operation. Its
  `SYSTEM_SUSPEND` validates that non-calling vCPUs are offline, records a
  resume entry and context ID, suspends the domain, and rebuilds the wake vCPU
  context during domain resume.
- The MT8678 Nebula implementation tracks guest PM state across vCPUs, saves
  EL2 and timer context before physical PSCI suspend, and evolved toward an
  all-VM barrier with ordered device notifications. Its history also shows
  that pending IRQs, virtual timers, GIC state, WDT handling, and concurrent
  wakeups are primary STR failure sources.
- BEAU already saves EL1 system registers, vtimer state, and vGIC list
  registers on vCPU switch-out. STR will reuse that state rather than create a
  second guest-context format.

Three implementation routes were considered:

1. Xen-style per-VM suspend is small but does not provide whole-vehicle
   coordination or device dependency ordering.
2. An implicit Nebula-style "last vCPU suspends the host" rule is concise but
   makes timeout, rollback, and concurrent wake ownership fragile.
3. A transaction-owned coordinator retains the standard PSCI guest ABI while
   adding an explicit epoch, participant masks, ordered hooks, timeout, and
   rollback. This is the selected route.

## Scope

The first complete release includes:

- guest `CPU_SUSPEND` standby and powerdown semantics;
- guest `SYSTEM_SUSPEND` with validated resume entry and context;
- an authorized PM-controller VM and a versioned PM hypercall ABI;
- required and optional VM participants selected from `platform.dts`;
- provider/consumer suspend and resume ordering;
- WDT, vtimer/vGIC, virtio, vPCI/passthrough, SMMUv3, console, scheduler,
  host timer, GIC/ITS, and pCPU handling;
- QEMU simulated STR using QMP `stop`/`cont` and an allowed virtual wake IRQ;
- strict QEMU PSCI `SYSTEM_SUSPEND` validation using a maintained QEMU patch;
- Linux, Zephyr, and RT-Thread guest PM agents and regression coverage.

The checked-in `sdk/ube` implementation is not part of the active BEAU runtime
and will not be used as the PM control path.

## State Model

VM and vCPU lifecycle state remains separate from PM state. The existing vCPU
lifecycle design is a prerequisite and must first split management pause from
guest PSCI power-off.

```text
enum beau_pm_system_state

RUNNING
  -> PREPARING
  -> GUESTS_QUIESCED
  -> FREEZING_HOST
  -> SUSPENDED
  -> RESTORING_HOST
  -> RESUMING_GUESTS
  -> RUNNING

PREPARING / GUESTS_QUIESCED / FREEZING_HOST
  -> ABORTING
  -> RUNNING

Unrecoverable host isolation restore failure
  -> FAILED
```

Each active VM also has an independent `beau_vm_pm_state`:

```text
VM_PM_RUNNING
  -> VM_PM_PREPARE_SENT
  -> VM_PM_SUSPEND_PENDING
  -> VM_PM_SUSPENDED
  -> VM_PM_RESUMING
  -> VM_PM_RUNNING

Any transition may publish VM_PM_FAILED with an owned error record.
```

`CPU_SUSPEND` blocks a scheduler thread without overloading the vCPU lifecycle
state. `SYSTEM_SUSPEND` records VM PM state and a BSP resume context. AP vCPUs
remain powered off after resume until their guest invokes `CPU_ON`.

## Coordinator Ownership

`core/pm.c` owns one statically allocated `beau_pm_transaction`. It contains a
monotonic epoch, system state, participant masks, initiator, wake reason, phase
timestamps, completed hook mask, and one fixed VM PM record per configured VM.
There is no late runtime allocation.

The global PM spinlock protects only state publication, masks, and snapshots.
No device callback, guest-memory access, wait, or cross-pCPU operation runs
while the lock is held. VM locks are acquired one VM at a time in ascending VM
ID order. IRQ handlers may only atomically latch a wake reason and kick the BSP.

A guest vPSCI exit never performs host suspend in its live trap frame. The last
required VM to become ready sets `NEED_SYSTEM_SUSPEND`; the BSP idle thread
claims the epoch and performs the transition. This follows the ownership model
already used by deferred VM reset.

## Required Source Layout

- `include/common/hv_pm.h`: common coordinator, hook, snapshot, and platform PM
  interfaces. The new API is deliberately named `hv_pm.h`, not `host_pm.h`.
- `core/pm.c`: transaction state machine, barrier, hook ordering, abort, wake
  ownership, and diagnostics snapshot producer.
- `arch/arm64/guest/vpsci.c`: guest CPU/system suspend validation and resume
  context capture.
- `arch/arm64/guest/vcpu_exit.c`: PSCI ID decode and `PSCI_FEATURES` reporting.
- `arch/arm64/pm.c` and an ARM64 resume assembly path: retained host context,
  secondary pCPU rendezvous, and common architecture resume.
- `arch/arm64/platform/qemu/pm.c`: simulated and strict QEMU platform backends.
- `sdk/bsp/pm.c`: PM hypercall, shell control, DTS policy, and guest event
  notification.
- existing WDT, timer, interrupt, SMMU, vPCI, virtio, and console modules:
  subsystem-owned PM callbacks.
- `scripts/regress.py`: QMP control, phase expectations, wake injection, fault
  injection, and stress loops.

## Required Code Comment

The following full ASCII sequence, with short ownership rules, must appear as
the design comment immediately before the coordinator implementation in
`core/pm.c`:

```c
/* [20260715] Coordinated guest STR transaction
 *
 * PM controller       Guest OSes          BEAU PM owner          Platform
 *      |                    |                    |                    |
 *      | request(epoch)     |                    |                    |
 *      +---------------------------------------->| PREPARING          |
 *      |                    |<-- prepare IRQ ----|                    |
 *      |                    | freeze OS/devices  |                    |
 *      |                    | offline AP vCPUs   |                    |
 *      |                    | SYSTEM_SUSPEND     |                    |
 *      |                    +------------------->| save entry/context |
 *      |                    |     BSP blocked    | mark VM ready      |
 *      |                    |                    |                    |
 *      |                    | all required ready |                    |
 *      |                    |                    | FREEZING_HOST      |
 *      |                    |                    | quiesce hooks      |
 *      |                    |                    | stop secondary CPU |
 *      |                    |                    | save EL2 context   |
 *      |                    |                    +------------------->|
 *      |                    |                    |     suspended      |
 *      |                    |                    |<---- wake source --|
 *      |                    |                    | restore host       |
 *      |                    |<-- entry/x0 -------| resume providers   |
 *      |                    | resume OS/devices  |                    |
 *      |                    |-- resume complete->| resume consumers   |
 *      |                    |                    | RUNNING            |
 *
 * Key rules:
 *   - the BSP idle thread is the only owner allowed to freeze or restore EL2;
 *   - vPSCI exits publish guest readiness before blocking the calling BSP;
 *   - suspend callbacks run in dependency order and rollback in reverse order;
 *   - a pending wake event aborts before platform entry and is never dropped.
 */
```

## PM ABI

The existing public PM hypercall namespace gains one versioned command:

```text
HC_PM_CONTROL
  ACRN_PM_QUERY_CAPS
  ACRN_PM_REQUEST_SUSPEND
  ACRN_PM_GET_EVENT
  ACRN_PM_ABORT
  ACRN_PM_GET_STATUS
  ACRN_PM_GET_WAKE_REASON
  ACRN_PM_RESUME_COMPLETE
```

The argument structure includes ABI version, structure size, operation, epoch,
VM ID, state, status, and wake reason. Compile-time assertions fix its layout.
All caller-supplied sizes, VM IDs, epochs, pointers, and reserved fields are
validated before use.

Only the VM named by `controller-vm` in `platform.dts` may request or abort a
system transaction. Other guests may query their own event/status, complete
resume, and invoke PSCI. An unauthorized management request fails closed.

## Platform Policy

`platform.dts` remains the source of truth and gains a bounded PM policy node:

```dts
beau,power-management {
    compatible = "beau,system-pm";
    controller-vm = <2>;
    required-vms = <0 1 2 3>;
    prepare-timeout-ms = <5000>;
    resume-timeout-ms = <5000>;
    wakeup-irqs = <...>;
    qemu-mode = "simulated";
};
```

The implementation validates uniqueness, VM ranges, timeout bounds, wake IRQ
ranges, and dependency cycles. The participant set is snapshotted at the start
of an epoch. VM create/destroy, device assignment, and a second PM transaction
are rejected until the epoch completes.

## Guest PSCI Semantics

`CPU_SUSPEND` supports both architected forms:

- standby blocks until a deliverable event and continues after the HVC/SMC;
- powerdown validates and stores entry/context, then resumes at the entry with
  `x0` set to the context ID.

`SYSTEM_SUSPEND` is accepted only from the VM BSP, only during the matching
transaction, and only when all other vCPUs are powered off. The resume entry
must be aligned and executable in that VM's stage-2 mapping. Once accepted, a
successful or aborted system transaction resumes through the supplied entry;
it does not return through the original PSCI call.

`PSCI_FEATURES` reports a suspend function only after its implementation and
tests are present. Invalid state, address, CPU, or concurrent operation returns
the matching PSCI error and does not partially publish PM state.

## Guest Coordination

The PM controller begins an epoch through `HC_PM_CONTROL`. BEAU publishes a
prepare event and injects each participant's configured virtual PM IRQ.

- A Linux agent writes the selected suspend mode to `/sys/power/state`. Linux
  freezes tasks and devices, offlines APs, and finally invokes PSCI.
- Zephyr and RT-Thread agents perform their native device/CPU preparation and
  invoke the same PSCI ABI.
- After returning through the OS resume path, each agent calls
  `ACRN_PM_RESUME_COMPLETE` for the active epoch.

Provider/consumer relationships define ordering. Consumers suspend before
providers; providers resume and acknowledge before consumers resume. A cycle
or missing required provider is a configuration error.

## Device PM Hooks

`hv_pm.h` defines a fixed-capacity, statically registered hook table with
`prepare`, `suspend`, `resume`, and `abort` callbacks plus a stable priority.
Suspend runs in forward dependency order. Resume and abort run in exact reverse
order. The transaction records a completed bit only after a callback succeeds.

The initial suspend order is:

1. close the PM I/O gate and reject new management operations;
2. latch pending wake sources;
3. freeze VM WDT deadlines and record their remaining time;
4. stop new virtio requests and drain bounded in-flight work;
5. suspend consumers before backend/provider channels;
6. mask passthrough IRQs and stop device DMA;
7. synchronize SMMUv3 CMDQ while unmatched streams remain aborting;
8. save live vtimer/vGIC state and cancel host backup timers;
9. suspend console, scheduler, and per-pCPU CNTHP timers;
10. rendezvous secondary pCPUs and enter the platform backend on the BSP.

Host resume first restores CPU/EL2 context, then GIC/ITS, SMMUv3, timers and
secondary pCPUs. It then reverses all completed device hooks before resuming
provider and consumer guests.

## Timer And Wake Rules

In QEMU simulated mode the ARM counter is treated as stopped while QMP has
paused execution. Guest CNTV CVAL and CNTVOFF values are retained. Host backup
timers are canceled before platform entry and recomputed after resume. An
already expired timer is asserted once through the vGIC; it must not generate a
resume interrupt storm.

WDT deadlines are rebased from the remaining pre-suspend interval, so time in
STR cannot create a false watchdog reset. A future hardware backend may use an
always-on counter or RTC to account for elapsed suspend time without changing
the coordinator contract.

Wake IRQ handlers latch the first source and a complete source bitmap before
clearing hardware state. Only allowlisted sources may initiate resume. Multiple
wake events coalesce into one transaction, and an atomic state transition gives
one pCPU resume ownership. The wake reason becomes queryable before the target
virtual wake IRQ is injected.

## QEMU Backends

The installed QEMU 8.2.2 TCG implementation treats `CPU_SUSPEND` as WFI and
does not advertise PSCI `SYSTEM_SUSPEND`. BEAU therefore provides two modes.

`simulated` is the default CI mode. After BEAU reaches `PM_SUSPENDED`, the
regression controller pauses QEMU through a separate QMP socket, waits, resumes
it with `cont`, and injects an allowlisted UART or RTC event. This validates all
BEAU and guest retention behavior but is not by itself proof of platform PSCI
STR.

`strict` uses a maintained QEMU patch that implements PSCI `SYSTEM_SUSPEND`,
resume entry/context behavior, the QEMU suspended runstate, and QMP
`system_wakeup`. Complete QEMU STR support is claimed only after strict mode
passes along with simulated stress coverage.

## Failure And Rollback

- A guest prepare timeout or refusal resumes every already-ready guest through
  its stored context and returns the transaction to `RUNNING`.
- A wake IRQ observed before platform entry records its reason and aborts in
  reverse hook order.
- A virtio, DMA, or SMMU quiesce failure keeps isolation fail-closed and rolls
  back only successfully completed hooks.
- A platform suspend call that returns an error restores pCPUs and hooks and
  publishes a platform-stage error.
- GIC or SMMU host restore failure is unrecoverable: guest DMA remains blocked
  and the configured fatal/reset path executes.
- An optional guest resume timeout isolates or restarts that guest. A required
  guest timeout enters the configured degraded/fatal policy.
- A concurrent suspend request returns busy with the current epoch. Repeated
  wake IRQs update the wake bitmap but cannot claim a second resume owner.

All error records include epoch, phase, VM/vCPU or device identity, and status.
No error path silently returns success.

## Diagnostics

The BEAU shell gains `pm suspend`, `pm status`, `pm abort`, `pm wake`, and
`pmstat`. Status reports epoch, phase, owner, participant masks, completed hook
mask, last error, wake source, per-phase duration, and completed/aborted cycle
counts. A retained trace ring records phase boundaries after the console has
been suspended.

## Verification

The implementation follows TDD at each layer and covers:

- public ABI layouts, PSCI IDs, DTS validation, permissions, and all state
  transitions;
- vPSCI standby, powerdown, invalid entry, non-BSP system suspend, AP-online
  rejection, and duplicate requests;
- single-VM context retention and all-VM barrier ordering;
- WDT, vtimer/vGIC, virtio, vPCI/passthrough, SMMU, IPC, console, and network
  behavior across resume;
- pending IRQ races, prepare timeout, hook failure, platform failure, duplicate
  wake, provider failure, and required/optional guest recovery;
- all four configured guest OS types and their PM agents.

The normal CI target runs 20 QEMU STR cycles. Nightly validation runs at least
1,000 cycles with randomized suspend duration, wake source, and phase-boundary
IRQ injection. Every resumed guest must maintain a heartbeat for 60 seconds.
The run fails on panic, false WDT recovery, RCU stall, timer loss, IRQ storm,
DMA isolation error, memory growth, or a non-empty transaction mask.

`pmstat` records P50, P99, and maximum phase latency. The first version does not
claim an unmeasured hard latency target.

## Delivery Order

1. Land the existing vCPU lifecycle state-machine prerequisite.
2. Add `hv_pm.h`, coordinator state, the required code diagram, and diagnostics.
3. Implement and test guest `CPU_SUSPEND`.
4. Implement and test per-VM `SYSTEM_SUSPEND` resume context.
5. Add the epoch/barrier, public PM ABI, DTS policy, and guest notifications.
6. Add subsystem PM hooks and deterministic rollback.
7. Add ARM64 host context, pCPU rendezvous, and the QEMU simulated backend.
8. Extend QEMU regression control and fault injection.
9. Integrate Linux, Zephyr, and RT-Thread agents and validate all four guests.
10. Add strict QEMU PSCI support, 1,000-cycle stress, and the final report.

Complete QEMU STR support is declared only after every delivery item and the
strict-mode acceptance suite pass.
