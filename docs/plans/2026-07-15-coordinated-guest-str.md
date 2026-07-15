# Coordinated Guest STR Implementation Plan

> **For implementer:** Use TDD throughout. Write failing test first. Watch it fail. Then implement.

**Goal:** Implement complete coordinated suspend-to-RAM and resume for all four BEAU QEMU guests, including standard vPSCI, a transaction-owned all-VM barrier, deterministic device rollback, and simulated plus strict QEMU validation.

**Architecture:** `core/pm.c` owns a fixed-size epoch transaction and the BSP idle thread is the only host transition owner. Guest kernels use PSCI for CPU and system suspend, while a versioned PM hypercall and virtual IRQ provide management coordination. Subsystems register ordered PM hooks through `hv_pm.h`; QEMU provides a default QMP-controlled retention backend and a strict patched-PSCI backend.

**Tech Stack:** ARM64 EL2 C/assembly, PSCI/SMCCC, BEAU scheduler/vGIC/vtimer/SMMUv3/virtio/vPCI, DTB policy, Python `unittest`, QEMU 8.2.2 TCG and QMP, Linux/Zephyr/RT-Thread guest agents.

---

## Execution Rules

- Read `docs/plans/2026-07-15-coordinated-guest-str-design.md` before every task that changes ownership or ordering.
- Run the named RED command and observe the expected failure before editing production code.
- Keep the public ABI fixed-width and compile-time asserted. Do not allocate in a suspend/resume path.
- Do not hold the PM spinlock while calling a hook, waiting, accessing guest memory, or sending a cross-pCPU request.
- Commit only the files named by the current task after its GREEN command succeeds.
- Never modify `~/nebula/xen` or the MT8678 reference workspace.

### Task 1: Land The vCPU Lifecycle Prerequisite

**Files:**
- Follow: `docs/plans/2026-07-15-vcpu-lifecycle-state-machine.md`
- Modify: `include/common/vcpu.h`
- Modify: `core/vcpu.c`
- Modify: `core/vm.c`
- Modify: `arch/arm64/guest/vcpu.c`
- Modify: `arch/arm64/guest/vcpu_exit.c`
- Modify: `arch/arm64/guest/vgicv3.c`
- Modify: `arch/arm64/guest/vgicv3_its.c`
- Modify: `arch/arm64/guest/vtimer.c`
- Modify: `sdk/bsp/ioreq.c`
- Modify: `sdk/bsp/shell.c`
- Modify: `sdk/bsp/arm64/shell.c`
- Test: `scripts/regress.py`

**Step 1: Write and run the failing lifecycle regression**

Implement Task 1 from the referenced lifecycle plan, including the VM2
CPU1 offline/online assertion.

Command:

```sh
./scripts/regress.py --no-build --timeout 120
```

Expected: FAIL because `VCPU_ZOMBIE` does not distinguish management pause
from PSCI power-off and CPU1 cannot reliably restart.

**Step 2: Implement only the approved lifecycle graph**

Use these states and no PM-specific state in the lifecycle enum:

```c
enum vcpu_state {
	VCPU_OFFLINE = 0U,
	VCPU_INIT,
	VCPU_RUNNING,
	VCPU_PAUSED,
	VCPU_POWERED_OFF,
};
```

Make the ARM64 vCPU thread wait outside its guest-run loop while not running.
PSCI `CPU_OFF` publishes `POWERED_OFF`; `CPU_ON` may claim only `INIT` or
`POWERED_OFF`. VM pause/reset uses `PAUSED`.

**Step 3: Run the lifecycle regression**

```sh
make ARCH=arm64 PLATFORM=qemu Bconfig
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
./scripts/regress.py --no-build --timeout 120
```

Expected: PASS, including VM2 CPU1 `poweroff -> running` and all four guest
boots.

**Step 4: Commit**

```sh
git add include/common/vcpu.h core/vcpu.c core/vm.c \
  arch/arm64/guest/vcpu.c arch/arm64/guest/vcpu_exit.c \
  arch/arm64/guest/vgicv3.c arch/arm64/guest/vgicv3_its.c \
  arch/arm64/guest/vtimer.c sdk/bsp/ioreq.c sdk/bsp/shell.c \
  sdk/bsp/arm64/shell.c scripts/regress.py
git commit -m "arm64: make vCPU lifecycle restartable"
```

### Task 2: Add The PM Contract Test And Rename `host_pm.h`

**Files:**
- Create: `scripts/test_hv_pm.py`
- Rename: `include/common/host_pm.h` -> `include/common/hv_pm.h`
- Rename: `include/arch/arm64/asm/host_pm.h` -> `include/arch/arm64/asm/hv_pm.h`
- Modify: `core/vm.c`
- Modify: `arch/arm64/trap.c`
- Modify: `arch/arm64/pm.c`
- Modify: `sdk/bsp/arm64/shell.c`
- Modify: `Makefile`
- Create: `core/pm.c`

**Step 1: Write the failing contract test**

Create `scripts/test_hv_pm.py` with:

```python
#!/usr/bin/env python3
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def source(path):
    target = pathlib.Path(path)
    if not target.is_absolute():
        target = ROOT / target
    return target.read_text(encoding="utf-8") if target.is_file() else ""


class HvPmContractTest(unittest.TestCase):
    def test_hv_pm_header_replaces_host_pm(self):
        self.assertTrue((ROOT / "include/common/hv_pm.h").is_file())
        self.assertFalse((ROOT / "include/common/host_pm.h").exists())
        self.assertTrue((ROOT / "include/arch/arm64/asm/hv_pm.h").is_file())
        self.assertNotIn("#include <host_pm.h>", source("core/vm.c"))

    def test_coordinator_owns_required_sequence_comment(self):
        pm = source("core/pm.c")
        for text in (
            "[20260715] Coordinated guest STR transaction",
            "PM controller       Guest OSes",
            "request(epoch)",
            "SYSTEM_SUSPEND",
            "FREEZING_HOST",
            "resume complete",
            "the BSP idle thread is the only owner",
        ):
            self.assertIn(text, pm)


if __name__ == "__main__":
    unittest.main()
```

**Step 2: Run RED**

```sh
python3 scripts/test_hv_pm.py -v
```

Expected: FAIL because `hv_pm.h` and `core/pm.c` do not exist.

**Step 3: Rename headers and add the minimal coordinator file**

Use `git mv` for both headers, change all common and ARM64 includes to
`<hv_pm.h>` / `<asm/hv_pm.h>`, and preserve `shutdown_host()` and
`reset_host()` unchanged. Add `core/pm.c` to `COMMON_C_SRCS` in `Makefile`.

Copy the exact approved ASCII sequence from the design document to the top of
`core/pm.c`. After the comment, add only:

```c
#include <types.h>
#include <hv_pm.h>

const char *hv_pm_state_to_str(enum beau_pm_system_state state)
{
	static const char *const names[] = {
		[PM_RUNNING] = "running",
		[PM_PREPARING] = "preparing",
		[PM_GUESTS_QUIESCED] = "guests-quiesced",
		[PM_FREEZING_HOST] = "freezing-host",
		[PM_SUSPENDED] = "suspended",
		[PM_RESTORING_HOST] = "restoring-host",
		[PM_RESUMING_GUESTS] = "resuming-guests",
		[PM_ABORTING] = "aborting",
		[PM_FAILED] = "failed",
	};

	return (state < ARRAY_SIZE(names)) ? names[state] : "invalid";
}
```

Define the enum and prototype in `include/common/hv_pm.h`.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add -A include/common include/arch/arm64/asm core/pm.c core/vm.c \
  arch/arm64/trap.c arch/arm64/pm.c sdk/bsp/arm64/shell.c Makefile \
  scripts/test_hv_pm.py
git commit -m "pm: establish hypervisor PM interface"
```

### Task 3: Implement The Fixed Transaction And State Transitions

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/common/hv_pm.h`
- Modify: `core/pm.c`

**Step 1: Add one failing test per transition behavior**

Add these methods to `HvPmContractTest`:

```python
    def test_transaction_is_static_and_epoch_owned(self):
        header = source("include/common/hv_pm.h")
        pm = source("core/pm.c")
        self.assertIn("struct beau_pm_transaction", header)
        self.assertIn("vm[CONFIG_MAX_VM_NUM]", header)
        self.assertIn("required_vm_mask", header)
        self.assertIn("completed_hook_mask", header)
        self.assertIn("static struct beau_pm_transaction pm_transaction", pm)
        self.assertNotIn("malloc(", pm)
        self.assertNotIn("calloc(", pm)

    def test_request_rejects_concurrent_epoch(self):
        pm = source("core/pm.c")
        self.assertIn("int32_t hv_pm_request_suspend", pm)
        self.assertIn("-EBUSY", pm)
        self.assertIn("PM_RUNNING", pm)
        self.assertIn("PM_PREPARING", pm)

    def test_snapshot_does_not_expose_live_lock(self):
        pm = source("core/pm.c")
        self.assertIn("void hv_pm_get_snapshot", pm)
        self.assertIn("spinlock_irqsave_obtain", pm)
        self.assertIn("memcpy_s", pm)
```

**Step 2: Run RED**

```sh
python3 -m unittest \
  scripts.test_hv_pm.HvPmContractTest.test_transaction_is_static_and_epoch_owned \
  scripts.test_hv_pm.HvPmContractTest.test_request_rejects_concurrent_epoch \
  scripts.test_hv_pm.HvPmContractTest.test_snapshot_does_not_expose_live_lock -v
```

Expected: FAIL because the transaction does not exist.

**Step 3: Add the minimal public data model**

Define fixed-width VM records, the transaction, snapshot, phase error, and
these interfaces in `hv_pm.h`:

```c
int32_t hv_pm_request_suspend(uint16_t initiator_vmid);
int32_t hv_pm_abort(uint64_t epoch, int32_t reason);
void hv_pm_get_snapshot(struct beau_pm_snapshot *snapshot);
bool hv_pm_io_is_gated(void);
```

Initialize one static transaction and spinlock in `core/pm.c`. Request must
validate the controller, require `PM_RUNNING`, increment a nonzero epoch,
snapshot configured participants, clear all per-epoch fields, and publish
`PM_PREPARING`. Abort is accepted only for the active epoch and moves through
`PM_ABORTING` back to `PM_RUNNING` while preserving last-result diagnostics.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add include/common/hv_pm.h core/pm.c scripts/test_hv_pm.py
git commit -m "pm: add epoch-owned STR transaction"
```

### Task 4: Add Ordered PM Hooks And Reverse Rollback

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/common/hv_pm.h`
- Modify: `core/pm.c`

**Step 1: Add the failing hook-order contract**

```python
    def test_hooks_are_bounded_and_rollback_only_completed_entries(self):
        header = source("include/common/hv_pm.h")
        pm = source("core/pm.c")
        self.assertIn("HV_PM_MAX_HOOKS", header)
        self.assertIn("struct beau_pm_ops", header)
        self.assertIn("prepare", header)
        self.assertIn("suspend", header)
        self.assertIn("resume", header)
        self.assertIn("abort", header)
        self.assertIn("completed_hook_mask", pm)
        self.assertIn("for (idx = count; idx > 0U; idx--)", pm)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_hooks_are_bounded_and_rollback_only_completed_entries -v
```

Expected: FAIL.

**Step 3: Implement minimal hook registration and execution**

Use a fixed array, reject null/duplicate names, full capacity, zero priority,
and registration after boot finalization. Stable-sort by priority at
registration. `hv_pm_run_prepare()` and `hv_pm_run_suspend()` set a bit only
after success. `hv_pm_run_resume()` and `hv_pm_run_abort()` walk set bits in
reverse and clear each bit after its callback.

Never hold `pm_transaction.lock` across a callback.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add include/common/hv_pm.h core/pm.c scripts/test_hv_pm.py
git commit -m "pm: add deterministic subsystem hooks"
```

### Task 5: Parse QEMU PM Policy And Add Shell Diagnostics

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `sdk/bsp/include/arm64_platform_dts.h`
- Modify: `sdk/bsp/arm64/platform_dts.c`
- Modify: `sdk/bsp/arm64/platform.c`
- Modify: `arch/arm64/platform/qemu/platform.dts`
- Modify: `arch/arm64/platform/rk356x/platform.dts`
- Create: `sdk/bsp/pm.c`
- Modify: `sdk/bsp/arm64/shell.c`

**Step 1: Add failing DTS and shell tests**

```python
    def test_platform_dts_has_bounded_pm_policy(self):
        qemu = source("arch/arm64/platform/qemu/platform.dts")
        parser = source("sdk/bsp/arm64/platform_dts.c")
        for text in ("beau,power-management", "controller-vm",
                     "required-vms", "prepare-timeout-ms",
                     "resume-timeout-ms", "wakeup-irqs", "qemu-mode"):
            self.assertIn(text, qemu + parser)
        self.assertIn("CONFIG_MAX_VM_NUM", parser)

    def test_pm_shell_exposes_transaction_diagnostics(self):
        shell = source("sdk/bsp/arm64/shell.c")
        for text in ('"pm"', '"pmstat"', "hv_pm_get_snapshot",
                     '"suspend"', '"status"', '"abort"', '"wake"'):
            self.assertIn(text, shell)
```

**Step 2: Run RED**

```sh
python3 -m unittest \
  scripts.test_hv_pm.HvPmContractTest.test_platform_dts_has_bounded_pm_policy \
  scripts.test_hv_pm.HvPmContractTest.test_pm_shell_exposes_transaction_diagnostics -v
```

Expected: FAIL.

**Step 3: Implement parser and commands**

Add a fixed `arm64_platform_pm_config` to the parsed platform info. Validate
node presence, exact cell counts, controller and VM ranges, nonzero bounded
timeouts, wake IRQ ranges, unique VM IDs, and mode string. QEMU uses all four
VMs as required and UART as its first wake source. RK356x disables system STR
until its backend exists.

Add `pm suspend|status|abort|wake` and `pmstat`; `status` uses a copied snapshot
and prints epoch, phase, owner, masks, error, wake reason, and durations.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu Bconfig
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
make ARCH=arm64 PLATFORM=rk356x Bconfig
make ARCH=arm64 PLATFORM=rk356x -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add sdk/bsp/include/arm64_platform_dts.h sdk/bsp/arm64/platform_dts.c \
  sdk/bsp/arm64/platform.c arch/arm64/platform/qemu/platform.dts \
  arch/arm64/platform/rk356x/platform.dts sdk/bsp/pm.c \
  sdk/bsp/arm64/shell.c scripts/test_hv_pm.py
git commit -m "pm: parse platform STR policy"
```

### Task 6: Implement Guest `CPU_SUSPEND`

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/arch/arm64/asm/guest/vcpu.h`
- Modify: `include/arch/arm64/asm/guest/vm_reset.h`
- Modify: `arch/arm64/guest/vpsci.c`
- Modify: `arch/arm64/guest/vcpu_exit.c`
- Modify: `arch/arm64/guest/virq.c`

**Step 1: Add failing vPSCI tests**

```python
    def test_vpsci_cpu_suspend_supports_standby_and_powerdown(self):
        vpsci = source("arch/arm64/guest/vpsci.c")
        exit_c = source("arch/arm64/guest/vcpu_exit.c")
        for text in ("PSCI_0_2_FN_CPU_SUSPEND",
                     "PSCI_0_2_FN64_CPU_SUSPEND"):
            self.assertIn(text, exit_c)
        for text in ("arm64_vpsci_cpu_suspend", "power_state",
                     "entry_point", "context_id", "PSCI_RET_DENIED"):
            self.assertIn(text, vpsci)
        self.assertIn("handle_psci_features", exit_c)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_vpsci_cpu_suspend_supports_standby_and_powerdown -v
```

Expected: FAIL because the ID is defined but not handled or advertised.

**Step 3: Implement minimal architected behavior**

Add a per-vCPU PM record containing mode, resume entry/context, and blocked
flag. Validate PSCI power-state reserved bits and aligned executable entry for
powerdown. Standby advances ELR before blocking and continues after an event.
Powerdown blocks without advancing its old ELR and rebuilds entry/x0 after a
wake event. Pending deliverable virtual IRQs cancel blocking.

Advertise both 32-bit and 64-bit calls only after the handler is connected.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
./scripts/regress.py --no-build --timeout 120
```

Expected: PASS; existing Linux/RTOS boot behavior remains intact.

**Step 5: Commit**

```sh
git add include/arch/arm64/asm/guest/vcpu.h \
  include/arch/arm64/asm/guest/vm_reset.h arch/arm64/guest/vpsci.c \
  arch/arm64/guest/vcpu_exit.c arch/arm64/guest/virq.c \
  scripts/test_hv_pm.py
git commit -m "arm64: virtualize PSCI CPU_SUSPEND"
```

### Task 7: Implement Per-VM `SYSTEM_SUSPEND` Resume Context

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/common/hv_pm.h`
- Modify: `include/arch/arm64/asm/guest/vm.h`
- Modify: `include/arch/arm64/asm/guest/vm_reset.h`
- Modify: `arch/arm64/guest/vpsci.c`
- Modify: `arch/arm64/guest/vcpu_exit.c`
- Modify: `arch/arm64/guest/vcpu.c`
- Modify: `core/pm.c`

**Step 1: Add the failing context contract**

```python
    def test_system_suspend_requires_bsp_and_offline_aps(self):
        vpsci = source("arch/arm64/guest/vpsci.c")
        for text in ("arm64_vpsci_system_suspend", "is_vcpu_bsp",
                     "VCPU_POWERED_OFF", "PSCI_RET_DENIED",
                     "resume_entry", "resume_context"):
            self.assertIn(text, vpsci)

    def test_system_resume_rebuilds_entry_and_x0(self):
        pm = source("core/pm.c")
        self.assertIn("arm64_vpsci_resume_vm", pm)
        self.assertIn("resume_entry", pm)
        self.assertIn("resume_context", pm)
```

**Step 2: Run RED**

```sh
python3 -m unittest \
  scripts.test_hv_pm.HvPmContractTest.test_system_suspend_requires_bsp_and_offline_aps \
  scripts.test_hv_pm.HvPmContractTest.test_system_resume_rebuilds_entry_and_x0 -v
```

Expected: FAIL.

**Step 3: Implement the Xen-compatible VM contract**

Accept `SYSTEM_SUSPEND` only from a required VM BSP during `PM_PREPARING`,
after every other vCPU is `POWERED_OFF`. Validate entry alignment and stage-2
execute permission. Copy entry/context into the VM architecture PM record,
mark the VM ready under the PM lock, then block the BSP.

`arm64_vpsci_resume_vm()` prepares the BSP at the stored entry with x0 set to
context, keeps APs powered off, and consumes the context exactly once.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add include/common/hv_pm.h include/arch/arm64/asm/guest/vm.h \
  include/arch/arm64/asm/guest/vm_reset.h arch/arm64/guest/vpsci.c \
  arch/arm64/guest/vcpu_exit.c arch/arm64/guest/vcpu.c core/pm.c \
  scripts/test_hv_pm.py
git commit -m "arm64: add guest SYSTEM_SUSPEND context"
```

### Task 8: Defer The All-VM Barrier To The BSP Idle Owner

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/common/per_cpu.h`
- Modify: `include/common/hv_pm.h`
- Modify: `core/pm.c`
- Modify: `core/schedule.c`

**Step 1: Add the failing ownership test**

```python
    def test_last_ready_guest_only_queues_idle_work(self):
        pm = source("core/pm.c")
        sched = source("core/schedule.c")
        self.assertIn("NEED_SYSTEM_SUSPEND", pm)
        self.assertIn("make_system_suspend_request", pm)
        self.assertIn("hv_pm_process_from_idle", sched)
        self.assertNotIn("platform_pm_enter", source("arch/arm64/guest/vpsci.c"))
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_last_ready_guest_only_queues_idle_work -v
```

Expected: FAIL.

**Step 3: Add the bounded idle transition**

Allocate a unique pCPU flag bit. The last required ready VM queues the flag on
the BSP pCPU and kicks it. The idle handler atomically claims
`GUESTS_QUIESCED -> FREEZING_HOST`, verifies all suspended BSP threads are
blocked, rechecks wake-pending state, runs hooks, and enters the platform.

On any pre-entry failure, set `PM_ABORTING`, reverse completed hooks, resume
stored guest contexts in provider order, clear the I/O gate, and return to
`PM_RUNNING`.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add include/common/per_cpu.h include/common/hv_pm.h core/pm.c \
  core/schedule.c scripts/test_hv_pm.py
git commit -m "pm: defer STR transition to BSP idle"
```

### Task 9: Add The Versioned PM Hypercall And Guest Events

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/public/acrn_hv_defs.h`
- Modify: `arch/arm64/guest/hcall.c`
- Modify: `sdk/bsp/pm.c`
- Modify: `sdk/bsp/arm64/vfdt.c`
- Modify: `arch/arm64/platform/qemu/platform.dts`

**Step 1: Add the failing ABI test**

```python
    def test_pm_hypercall_is_versioned_and_permission_checked(self):
        abi = source("include/public/acrn_hv_defs.h")
        hcall = source("arch/arm64/guest/hcall.c")
        pm = source("sdk/bsp/pm.c")
        for text in ("HC_PM_CONTROL", "ACRN_PM_ABI_VERSION",
                     "ACRN_PM_REQUEST_SUSPEND", "ACRN_PM_GET_EVENT",
                     "ACRN_PM_RESUME_COMPLETE", "struct acrn_pm_ioc"):
            self.assertIn(text, abi)
        self.assertIn("HC_IDX(HC_PM_CONTROL)", hcall)
        self.assertIn("controller_vmid", pm)
        self.assertIn("-EPERM", pm)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_pm_hypercall_is_versioned_and_permission_checked -v
```

Expected: FAIL.

**Step 3: Implement the minimal ABI**

Add one 64-byte aligned fixed-layout IOC with version, size, op, status, epoch,
VM ID, PM state, wake reason, and reserved zero fields. Add a compile-time size
assertion. Copy it through validated guest GPA helpers. Controller-only ops are
request/abort; every VM may query its own event/status/reason and acknowledge
resume for the matching epoch.

Expose one virtual PM IRQ per VM through vFDT. A transaction request publishes
the event before asserting the IRQ.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
```

Expected: PASS.

**Step 5: Commit**

```sh
git add include/public/acrn_hv_defs.h arch/arm64/guest/hcall.c sdk/bsp/pm.c \
  sdk/bsp/arm64/vfdt.c arch/arm64/platform/qemu/platform.dts \
  scripts/test_hv_pm.py
git commit -m "pm: add coordinated STR guest ABI"
```

### Task 10: Freeze WDT, vtimer, And vGIC State

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/common/vm_wdt.h`
- Modify: `core/vm_wdt.c`
- Modify: `include/arch/arm64/asm/guest/vtimer.h`
- Modify: `arch/arm64/guest/vtimer.c`
- Modify: `include/arch/arm64/asm/guest/vgicv3.h`
- Modify: `arch/arm64/guest/vgicv3.c`
- Modify: `sdk/bsp/pm.c`

**Step 1: Add failing state-retention tests**

```python
    def test_wdt_and_virtual_timer_have_epoch_pm_hooks(self):
        wdt = source("core/vm_wdt.c")
        timer = source("arch/arm64/guest/vtimer.c")
        vgic = source("arch/arm64/guest/vgicv3.c")
        for text in ("remaining_ticks", "suspend_epoch", "vm_wdt_pm_resume"):
            self.assertIn(text, wdt)
        for text in ("arm64_vtimer_suspend_vm", "arm64_vtimer_resume_vm"):
            self.assertIn(text, timer)
        for text in ("arm64_vgicv3_suspend_vm", "arm64_vgicv3_resume_vm"):
            self.assertIn(text, vgic)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_wdt_and_virtual_timer_have_epoch_pm_hooks -v
```

Expected: FAIL.

**Step 3: Implement retention hooks**

WDT suspend records each monitored VM's remaining deadline and disarms its
timer; resume rebases from the remaining interval. vtimer suspend first forces
the loaded vCPU through its existing save path, then cancels CNTV/CNTP backup
timers. Resume restores the shadows and rearms once; expired conditions assert
one virtual line. vGIC suspend requires no LR owner and snapshots pending,
active, priority, and virtual CPU-interface state before host GIC shutdown.

Register hooks in WDT -> vtimer -> vGIC order and reverse them on resume.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
./scripts/regress.py --no-build --timeout 120
```

Expected: PASS with no false WDT recovery or lost Linux timer tick.

**Step 5: Commit**

```sh
git add include/common/vm_wdt.h core/vm_wdt.c \
  include/arch/arm64/asm/guest/vtimer.h arch/arm64/guest/vtimer.c \
  include/arch/arm64/asm/guest/vgicv3.h arch/arm64/guest/vgicv3.c \
  sdk/bsp/pm.c scripts/test_hv_pm.py
git commit -m "pm: retain WDT vtimer and vGIC state"
```

### Task 11: Quiesce virtio, vPCI, Passthrough, And SMMUv3

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `sdk/bsp/include/virtio_mmio.h`
- Modify: `sdk/bsp/include/virtio_proxy.h`
- Modify: `sdk/bsp/virtio/virtio_mmio.c`
- Modify: `sdk/bsp/virtio/virtio_proxy.c`
- Modify: `include/bsp/vpci.h`
- Modify: `sdk/bsp/vpci/vpci_core.c`
- Modify: `sdk/bsp/passthrough.c`
- Modify: `include/arch/arm64/asm/smmu.h`
- Modify: `arch/arm64/smmu/smmuv3.c`
- Modify: `sdk/bsp/pm.c`

**Step 1: Add the failing I/O isolation test**

```python
    def test_io_pm_hooks_gate_requests_before_dma(self):
        proxy = source("sdk/bsp/virtio/virtio_proxy.c")
        vpci = source("sdk/bsp/vpci/vpci_core.c")
        smmu = source("arch/arm64/smmu/smmuv3.c")
        self.assertIn("virtio_proxy_pm_suspend", proxy)
        self.assertIn("hv_pm_io_is_gated", proxy)
        self.assertIn("vpci_pm_suspend", vpci)
        self.assertIn("arm_smmu_pm_suspend", smmu)
        self.assertIn("ARM_SMMU_GBPA_ABORT", smmu)
        self.assertIn("arm_smmu_cmdq_sync", smmu)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_io_pm_hooks_gate_requests_before_dma -v
```

Expected: FAIL.

**Step 3: Implement bounded quiesce and reverse restore**

Reject new queue notifications after the PM I/O gate closes. Drain a bounded
count of already-owned proxy requests; timeout aborts before DMA changes. Mask
passthrough MSI/MSI-X and physical IRQ routing before disabling device bus
mastering. Synchronize SMMU CMDQ, preserve stream-table ownership, and leave
GBPA abort set. Resume SMMU first, then device DMA/IRQs, vPCI, and virtio.

Each hook must be idempotent for the same epoch and reject a different epoch
while suspended.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
./scripts/regress.py --no-build --timeout 120
```

Expected: PASS, including existing edu PCI, virtio, IPC, and network checks.

**Step 5: Commit**

```sh
git add sdk/bsp/include/virtio_mmio.h sdk/bsp/include/virtio_proxy.h \
  sdk/bsp/virtio/virtio_mmio.c sdk/bsp/virtio/virtio_proxy.c \
  include/bsp/vpci.h sdk/bsp/vpci/vpci_core.c sdk/bsp/passthrough.c \
  include/arch/arm64/asm/smmu.h arch/arm64/smmu/smmuv3.c \
  sdk/bsp/pm.c scripts/test_hv_pm.py
git commit -m "pm: quiesce virtual IO and DMA"
```

### Task 12: Add ARM64 Host Context And QEMU Simulated Platform Entry

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `include/common/hv_pm.h`
- Modify: `include/arch/arm64/asm/hv_pm.h`
- Create: `arch/arm64/suspend.S`
- Modify: `arch/arm64/pm.c`
- Modify: `arch/arm64/psci.c`
- Modify: `arch/arm64/gic/gicv3.c`
- Modify: `arch/arm64/gic/gicv3_its.c`
- Modify: `arch/arm64/timer.c`
- Create: `arch/arm64/platform/qemu/pm.c`
- Modify: `arch/arm64/platform/qemu/Bconfig`
- Modify: `arch/arm64/platform/rk356x/Bconfig`
- Modify: `arch/arm64/Makefile`
- Modify: `sdk/bsp/console.c`
- Modify: `core/schedule.c`

**Step 1: Add failing host-order tests**

```python
    def test_host_resume_order_restores_isolation_before_guests(self):
        pm = source("arch/arm64/pm.c")
        for text in ("arm64_restore_el2_context", "arm64_gicv3_pm_resume",
                     "arm_smmu_pm_resume", "arch_pm_resume_timer",
                     "arch_pm_resume_secondary_cpus"):
            self.assertIn(text, pm)
        self.assertLess(pm.index("arm64_gicv3_pm_resume"),
                        pm.index("arm_smmu_pm_resume"))
        self.assertLess(pm.index("arm_smmu_pm_resume"),
                        pm.index("hv_pm_resume_guests"))

    def test_qemu_backend_has_simulated_and_strict_modes(self):
        qemu = source("arch/arm64/platform/qemu/pm.c")
        self.assertIn("QEMU_PM_SIMULATED", qemu)
        self.assertIn("QEMU_PM_STRICT", qemu)
        self.assertIn("psci_system_suspend", qemu)
        self.assertIn("asm_wfi", qemu)
```

**Step 2: Run RED**

```sh
python3 -m unittest \
  scripts.test_hv_pm.HvPmContractTest.test_host_resume_order_restores_isolation_before_guests \
  scripts.test_hv_pm.HvPmContractTest.test_qemu_backend_has_simulated_and_strict_modes -v
```

Expected: FAIL.

**Step 3: Implement host save/resume and platform modes**

The assembly trampoline saves callee GPRs, SP, and a physical resume target.
The C context covers VBAR/SCTLR/TCR/TTBR/MAIR/HCR/VTCR/VTTBR/CPTR/CNTHCTL/
CNTVOFF/MDCR/TPIDR EL2 state. Secondary pCPUs stop local timer and GIC CPU
interfaces, acknowledge a bounded rendezvous, and remain in WFI in simulated
mode. The BSP logs the suspended marker before console shutdown and waits for
an allowlisted wake IRQ.

Strict mode calls the new host `psci_system_suspend(resume_pa, context)` and
handles every returned PSCI error by restoring the pre-entry state.

Resume order is EL2/MMU, GIC/ITS, SMMUv3, timer, secondary pCPUs, device hooks,
then guests. Do not enable guest DMA on a failed GIC/SMMU restore.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu Bconfig
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
make ARCH=arm64 PLATFORM=rk356x Bconfig
make ARCH=arm64 PLATFORM=rk356x -j"$(nproc)"
```

Expected: PASS. RK356x compiles with an unsupported platform backend that
returns `-ENOTSUP` before quiescing guests.

**Step 5: Commit**

```sh
git add include/common/hv_pm.h include/arch/arm64/asm/hv_pm.h \
  arch/arm64/suspend.S arch/arm64/pm.c arch/arm64/psci.c \
  arch/arm64/gic/gicv3.c arch/arm64/gic/gicv3_its.c arch/arm64/timer.c \
  arch/arm64/platform/qemu/pm.c arch/arm64/platform/qemu/Bconfig \
  arch/arm64/platform/rk356x/Bconfig arch/arm64/Makefile \
  sdk/bsp/console.c core/schedule.c scripts/test_hv_pm.py
git commit -m "arm64: retain host state across QEMU STR"
```

### Task 13: Extend The QEMU Regression With QMP STR Control

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Modify: `scripts/regress.py`

**Step 1: Add failing regression source assertions**

```python
    def test_regression_uses_separate_qmp_and_checks_full_cycle(self):
        regress = source("scripts/regress.py")
        for text in ("--str-cycles", "--qmp-socket", "qmp_capabilities",
                     '"execute": "stop"', '"execute": "cont"',
                     "PM_SUSPENDED", "PM_RESUMING", "wake_reason",
                     "pm status"):
            self.assertIn(text, regress)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_regression_uses_separate_qmp_and_checks_full_cycle -v
```

Expected: FAIL.

**Step 3: Add QMP support and one-cycle behavior**

Add `-qmp unix:<path>,server=on,wait=off` without multiplexing it with serial.
Implement a newline-delimited JSON QMP client with bounded connect/read
timeouts. For every cycle:

1. issue `pm suspend`;
2. wait for every VM ready marker and `PM_SUSPENDED`;
3. issue QMP `stop`, assert `query-status` reports paused;
4. wait the requested interval, issue `cont`, send the UART wake byte;
5. require `PM_RESUMING`, all resume acknowledgements, `PM_RUNNING`;
6. run `pm status`, `health`, `irqstat`, `virtiostat`, `pcistat`, and all guest
   heartbeat checks.

Add fault switches for prepare timeout, pending wake, hook failure, platform
failure, duplicate wake, and resume timeout.

**Step 4: Run GREEN**

```sh
python3 -m py_compile scripts/test_hv_pm.py scripts/regress.py
python3 scripts/test_hv_pm.py -v
./scripts/regress.py --no-build --str-cycles 1 --timeout 180
```

Expected: PASS for one complete simulated STR cycle.

**Step 5: Commit**

```sh
git add scripts/test_hv_pm.py scripts/regress.py
git commit -m "test: automate QEMU STR through QMP"
```

### Task 14: Add Linux And Zephyr PM Agents

**Files:**
- Modify: `scripts/test_hv_pm.py`
- Create: `/home/beau/nebula/linux-7.1.1/drivers/virt/beau/beau-pm.c`
- Create: `/home/beau/nebula/linux-7.1.1/drivers/virt/beau/beau-pm.h`
- Modify: `/home/beau/nebula/linux-7.1.1/drivers/virt/beau/Kconfig`
- Modify: `/home/beau/nebula/linux-7.1.1/drivers/virt/beau/Makefile`
- Create: `sdk/kbe/beau-pm.c`
- Create: `sdk/kbe/beau-pm.h`
- Modify: `sdk/kbe/Makefile`
- Create: `/home/beau/nebula/zephyr/samples/subsys/shell/shell_module/src/beau_pm.c`
- Modify: `/home/beau/nebula/zephyr/samples/subsys/shell/shell_module/CMakeLists.txt`
- Modify: `/home/beau/nebula/zephyr/samples/subsys/shell/shell_module/prj.conf`
- Create: `sdk/zsh/beau_pm.c`
- Copy after validation: `sdk/image/linux/vm2/Image`
- Copy after validation: `sdk/image/zephyr.bin`
- Modify: `scripts/repack_initramfs.sh`
- Modify: `scripts/regress.py`

**Step 1: Add failing guest-agent contracts**

```python
    def test_linux_and_zephyr_agents_share_the_public_pm_abi(self):
        linux = source("sdk/kbe/beau-pm.c")
        zephyr = source("sdk/zsh/beau_pm.c")
        for body in (linux, zephyr):
            self.assertIn("HC_PM_CONTROL", body)
            self.assertIn("ACRN_PM_GET_EVENT", body)
            self.assertIn("ACRN_PM_RESUME_COMPLETE", body)
            self.assertIn("ACRN_PM_GET_WAKE_REASON", body)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_linux_and_zephyr_agents_share_the_public_pm_abi -v
```

Expected: FAIL.

**Step 3: Implement minimal agents**

The Linux driver exposes a pollable event device and validates ABI/caps. A
small initramfs service reads prepare events, writes `mem` to
`/sys/power/state`, and acknowledges after the write returns. The Zephyr agent
waits on the PM IRQ, invokes the architecture PSCI suspend path, then
acknowledges and prints the wake reason. Neither agent may acknowledge a stale
epoch.

Implement and validate in the real guest trees first. The Linux 7.1.1 tree
already integrates `drivers/virt/beau`; preserve its existing files and add the
PM driver to its Kconfig/Makefile. The Zephyr shell sample is already the VM0
image source and targets `qemu_cortex_a53/qemu_cortex_a53/smp`; work with its
existing local BEAU additions instead of replacing them. Copy only the reviewed
agent sources and successfully built images back into BEAU.

**Step 4: Run GREEN**

```sh
python3 scripts/test_hv_pm.py -v
make -C /home/beau/nebula/linux-7.1.1 ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- O=/tmp/beau-linux-str defconfig
/home/beau/nebula/linux-7.1.1/scripts/config \
  --file /tmp/beau-linux-str/.config -e BEAU -e BEAU_PM
make -C /home/beau/nebula/linux-7.1.1 ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- O=/tmp/beau-linux-str olddefconfig Image
cp /tmp/beau-linux-str/arch/arm64/boot/Image sdk/image/linux/vm2/Image
ZEPHYR_BASE=/home/beau/nebula/zephyr cmake -S \
  /home/beau/nebula/zephyr/samples/subsys/shell/shell_module \
  -B /home/beau/nebula/zephyr/build-beau-str -GNinja \
  -DBOARD=qemu_cortex_a53/qemu_cortex_a53/smp
cmake --build /home/beau/nebula/zephyr/build-beau-str
cp /home/beau/nebula/zephyr/build-beau-str/zephyr/zephyr.bin \
  sdk/image/zephyr.bin
./scripts/repack_initramfs.sh sdk/image/linux/Initramfs.cpio.gz
./scripts/regress.py --no-build --str-cycles 1 --timeout 180
```

Expected: PASS for VM0, VM2, and VM3 agent participation.

**Step 5: Commit the guest-tree integrations separately**

The Linux source directory is an extracted validation tree rather than a Git
worktree, so retain the reviewed Linux source in BEAU. In the Zephyr repository,
commit only the STR files and the exact CMake/prj.conf hunks added by this task;
do not absorb pre-existing local BEAU changes.

In `~/nebula/zephyr`:

```sh
git add samples/subsys/shell/shell_module/src/beau_pm.c \
  samples/subsys/shell/shell_module/CMakeLists.txt \
  samples/subsys/shell/shell_module/prj.conf
git commit -m "arm64: add BEAU coordinated STR agent"
```

In BEAU:

```sh
git add sdk/kbe/beau-pm.c sdk/kbe/beau-pm.h sdk/kbe/Makefile \
  sdk/zsh/beau_pm.c sdk/image/linux/vm2/Image sdk/image/zephyr.bin \
  scripts/repack_initramfs.sh scripts/regress.py scripts/test_hv_pm.py
git commit -m "guest: add Linux and Zephyr STR agents"
```

### Task 15: Add The RT-Thread PM Agent In Its Own Repository

**Files:**
- Create: `/home/beau/nebula/rt-thread/bsp/qemu-virt64-aarch64/applications/beau_str.c`
- Copy after validation: `sdk/image/rtthread.bin`
- Retain source after validation: `sdk/rtthread/beau_str.c`
- Create: `sdk/rtthread/README.md`

**Step 1: Write the failing RT-Thread shell test**

Add `beau_str status` and `beau_str suspend` expectations to
`scripts/regress.py`, then run:

```sh
./scripts/regress.py --no-build --str-cycles 1 --timeout 180
```

Expected: FAIL because VM1 has no PM agent command.

**Step 2: Implement and build the external agent**

Use RT-Thread native PM preparation, HVC `HC_PM_CONTROL`, and PSCI
`SYSTEM_SUSPEND`. Validate epoch on prepare and resume, keep APs offline before
system suspend, acknowledge after native resume callbacks, and print the wake
reason.

Build the existing BEAU QEMU RT-Thread target, update the stable image only
after its standalone tests pass, and copy the exact reviewed source into
`sdk/rtthread` for reproducibility.

The BSP `applications/SConscript` already includes every application `*.c`, so
no build-file change is required. Preserve the current local `.config`,
`rtconfig.h`, `applications/main.c`, and `applications/beau_wdt.c` changes.

**Step 3: Run GREEN**

```sh
cd /home/beau/nebula/rt-thread/bsp/qemu-virt64-aarch64
scons -j"$(nproc)"
cp rtthread.bin /home/beau/nebula/beau/sdk/image/rtthread.bin
cd /home/beau/nebula/beau
./scripts/regress.py --no-build --str-cycles 1 --timeout 180
```

Expected: PASS with VM1 included in the required mask.

**Step 4: Commit separately in each repository**

In `~/nebula/rt-thread`:

```sh
git add bsp/qemu-virt64-aarch64/applications/beau_str.c
git commit -m "arm64: add BEAU coordinated STR agent"
```

In BEAU:

```sh
git add sdk/rtthread sdk/image/rtthread.bin scripts/regress.py
git commit -m "sdk: retain validated RT-Thread STR agent"
```

### Task 16: Add Strict QEMU PSCI `SYSTEM_SUSPEND`

**Files:**
- Create: `scripts/qemu/0001-arm-tcg-implement-psci-system-suspend.patch`
- Modify: `scripts/regress.py`
- Modify: `scripts/test_hv_pm.py`
- Reference implementation paths in QEMU 8.2.2: `target/arm/tcg/psci.c`, `system/runstate.c`

**Step 1: Add the failing strict-mode contract**

```python
    def test_strict_qemu_patch_exposes_system_suspend_and_wakeup(self):
        patch = source("scripts/qemu/0001-arm-tcg-implement-psci-system-suspend.patch")
        regress = source("scripts/regress.py")
        for text in ("QEMU_PSCI_1_0_FN64_SYSTEM_SUSPEND",
                     "qemu_system_suspend_request", "resume_entry",
                     "context_id"):
            self.assertIn(text, patch)
        self.assertIn("--str-strict", regress)
        self.assertIn('"execute": "system_wakeup"', regress)
        self.assertIn('"status": "suspended"', regress)
```

**Step 2: Run RED**

```sh
python3 -m unittest scripts.test_hv_pm.HvPmContractTest.test_strict_qemu_patch_exposes_system_suspend_and_wakeup -v
```

Expected: FAIL.

**Step 3: Implement the QEMU patch**

Against exact tag `v8.2.2`, add 32/64-bit `SYSTEM_SUSPEND` IDs and
`PSCI_FEATURES` reporting. Require the caller to be the last powered-on CPU,
validate entry alignment, store entry/context, and request QEMU system
suspend. Register a wake notifier that resets the suspended CPU's execution
state to the saved resume entry with x0 set to context before QEMU resumes.

The regression strict mode must require QMP `SUSPEND`, `query-status` equal to
`suspended`, invoke `system_wakeup`, require `WAKEUP`, and then run the same
post-resume guest checks as simulated mode.

**Step 4: Run GREEN**

```sh
git -C /tmp/qemu-v8.2.2-src apply --check \
  "$PWD/scripts/qemu/0001-arm-tcg-implement-psci-system-suspend.patch"
python3 scripts/test_hv_pm.py -v
./scripts/regress.py --qemu <patched-qemu-system-aarch64> \
  --no-build --str-strict --str-cycles 1 --timeout 180
```

Expected: PASS with QEMU `SUSPEND` and `WAKEUP` events.

**Step 5: Commit**

```sh
git add scripts/qemu/0001-arm-tcg-implement-psci-system-suspend.patch \
  scripts/regress.py scripts/test_hv_pm.py
git commit -m "qemu: add strict ARM PSCI STR validation"
```

### Task 17: Fault Injection, Stress, And Final Verification

**Files:**
- Modify: `scripts/regress.py`
- Modify: `scripts/test_hv_pm.py`
- Modify: `sdk/bsp/pm.c`
- Modify: `sdk/bsp/arm64/shell.c`
- Modify: `sdk/sdk.md`
- Create: `docs/plans/2026-07-15-coordinated-guest-str-report.md`

**Step 1: Add failing fault-matrix expectations**

Extend `scripts/test_hv_pm.py` to require named regression cases for:

```text
unauthorized-request
concurrent-request
prepare-timeout
ap-online-denied
pending-wake-race
virtio-drain-timeout
smmu-sync-failure
platform-enter-failure
duplicate-wake
provider-resume-timeout
required-resume-failure
wdt-boundary
vtimer-boundary
```

**Step 2: Run RED**

```sh
python3 scripts/test_hv_pm.py -v
```

Expected: FAIL until every named case is wired to a deterministic injection
point and assertion.

**Step 3: Implement injection points and retained reporting**

Compile injection code only in non-release QEMU builds. Each injection accepts
one epoch and phase, fires once, and records an explicit error identity. Add
`pmstat` P50/P99/max phase durations and completed/aborted/failed cycle counts.
Document simulated versus strict support accurately in `sdk/sdk.md`.

**Step 4: Run all static and build verification**

```sh
python3 -m py_compile scripts/test_hv_pm.py scripts/regress.py
python3 scripts/test_hv_pm.py -v
make ARCH=arm64 PLATFORM=qemu Bconfig
make ARCH=arm64 PLATFORM=qemu -j"$(nproc)"
make ARCH=arm64 PLATFORM=qemu checkconfig
make ARCH=arm64 PLATFORM=rk356x Bconfig
make ARCH=arm64 PLATFORM=rk356x -j"$(nproc)"
make ARCH=arm64 PLATFORM=rk356x checkconfig
git diff --check
```

Expected: all commands succeed.

**Step 5: Run behavior and stress gates**

```sh
./scripts/regress.py --no-build --str-cycles 20 --timeout 180
./scripts/regress.py --no-build --str-fault-matrix --timeout 180
./scripts/regress.py --qemu <patched-qemu-system-aarch64> \
  --no-build --str-strict --str-cycles 20 --timeout 180
./scripts/regress.py --qemu <patched-qemu-system-aarch64> \
  --no-build --str-strict --str-cycles 1000 --str-randomize --timeout 180
```

Expected: every cycle returns all required VM masks and completed hook masks to
zero; every guest maintains heartbeat for 60 seconds; no panic, false WDT,
RCU stall, lost timer, IRQ storm, isolation error, or memory growth occurs.

**Step 6: Write the report and commit**

Record QEMU version/commit, guest image hashes, build commands, all pass/fail
counts, latency distribution, wake-source counts, and known non-QEMU platform
limitations in the report.

```sh
git add scripts/regress.py scripts/test_hv_pm.py sdk/bsp/pm.c \
  sdk/bsp/arm64/shell.c sdk/sdk.md \
  docs/plans/2026-07-15-coordinated-guest-str-report.md
git commit -m "test: qualify coordinated guest STR"
```

## Completion Gate

Do not describe BEAU as having complete QEMU guest STR support until Tasks
1-17 are committed, the strict QEMU path passes, all four guest agents resume
their retained images, and the 1,000-cycle report contains no unresolved
isolation, timer, interrupt, or watchdog failure.
