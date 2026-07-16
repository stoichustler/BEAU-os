# BEAU SMMUv3 Strict Mode Implementation Plan

> **For implementer:** Use TDD throughout. Write failing regression coverage first, watch it fail, then implement the minimum production change.

**Goal:** Complete PR 1 from `sdk/beau/SMMUv3.md` so physical passthrough DMA can only use a validated Stage-2 SMMUv3 domain and every capability, command, assignment, or PM failure ends in ABORT.

**Architecture:** Keep the current single-instance ownership model for this PR, but replace its implicit ready flag with an explicit capability/state gate. Probe first establishes `GBPA.ABORT`, validates the IHI 0070 capabilities used by BEAU, programs CR1/CR2 while disabled, and only then enables queues and translation. Stream publication remains two-phase; any command failure disables assignment and returns the entire instance to disabled global ABORT.

**Tech Stack:** ISO C99 freestanding ARM64 EL2, Arm SMMUv3 IHI 0070 H.a sections 6.3.1, 6.3.2, 6.3.6, 6.3.11, 6.3.12, 6.3.15 and 6.3.28, Python QEMU regression, GNU Make/Kconfig.

---

### Task 1: Strict QEMU Regression Contract

**Files:**
- Modify: `scripts/regress.py`

**Step 1: Write the failing regression**

Add `--smmu-no-s2-smoke`, make the normal machine explicitly use QEMU SMMUv3 Stage-2, and add structured `smmustat` checks.

Normal Stage-2 expectations:

```text
strict:Y caps.valid:Y state:ready
s2p:Y
assignment:Y
stream[0x0008] sw-owner:vm2 ste-vm:vm2
cfg:s2(6)
stream[0x0010] sw-owner:vm2 ste-vm:vm2
cfg:s2(6)
```

Forbidden output:

```text
cfg:bypass
quarantine:Y
```

No-S2 smoke expectations:

```text
strict:Y caps.valid:N state:abort
s2p:N
assignment:N
```

**Step 2: Run regression and confirm RED**

Commands:

```bash
PATH=/home/beau/beau-cc/bin:$PATH python3 scripts/regress.py --no-build
PATH=/home/beau/beau-cc/bin:$PATH python3 scripts/regress.py --no-build --smmu-no-s2-smoke
```

Expected: both fail because the current driver has no strict capability/state report and the no-S2 path publishes BYPASS.

### Task 2: Fail-Closed Capability Gate

**Files:**
- Modify: `arch/arm64/smmu/smmuv3.c`
- Modify: `include/arch/arm64/asm/vtd.h`
- Modify: `sdk/bsp/arm64/shell.c`

**Step 1: Implement capability validation**

Require and record:

```text
S2P == 1
TTF includes VMSAv8-64
TTENDIAN is mixed or little-endian
GRAN4K == 1
COHACC == 1 for the initial WB/inner-shareable implementation
STALL_MODEL is not force-stall
non-preset/non-relative tables and queues
CMDQ and EVTQ support the configured 64 entries
OAS covers BEAU RAM, SMMU-owned structures, ITS doorbell, and domain roots
VMID width covers vm_id + 1
```

Decode OAS encodings instead of treating unknown values as 48-bit. Use the discovered OAS encoding for STE.S2PS while retaining BEAU's 48-bit IPA input and 4 KiB Stage-2 format.

**Step 2: Establish safe probe ordering**

```text
discover MMIO
    -> set GBPA.ABORT
    -> disable CR0 and wait for ACK
    -> validate mandatory capabilities
    -> program CR1/CR2 and verify readback
    -> initialize Stream Table/CMDQ/EVTQ
    -> enable CMDQ/EVTQ/SMMU
    -> publish READY
```

On any failure, retain `GBPA.ABORT`, keep `assignment_ready == false`, and expose an actionable capability failure bitmap/status through `smmustat`.

**Step 3: Build and run the focused positive regression**

```bash
PATH=/home/beau/beau-cc/bin:$PATH make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
PATH=/home/beau/beau-cc/bin:$PATH python3 scripts/regress.py --no-build
```

Expected: build passes and strict Stage-2 SMMU checks pass.

### Task 3: Remove Physical BYPASS and Close Failure Paths

**Files:**
- Modify: `arch/arm64/smmu/smmuv3.c`
- Modify: `include/arch/arm64/asm/vtd.h`
- Modify: `sdk/bsp/arm64/shell.c`

**Step 1: Remove BYPASS production paths**

Delete the BYPASS STE encoder, endpoint/SID0 compatibility publication, no-S2 MSI doorbell success, and no-S2 ITS alias behavior. Guest-visible bypass is out of scope for this PR; physical assignment without Stage-2 returns `-EOPNOTSUPP` and leaves ABORT.

**Step 2: Add deterministic rollback/degraded handling**

```text
STE update failure
    -> write ABORT
    -> CFGI_STE + CMD_SYNC
    -> publish no software owner

CMDQ CERROR/timeout or rollback failure
    -> assignment gate closed
    -> GBPA.ABORT set
    -> CR0 disabled and ACKed
    -> state DEGRADED
```

Do not clear a domain/owner after an unassign, destroy, or PM operation unless hardware ABORT synchronization succeeded. PM resume publishes READY only after all retained streams are rebuilt successfully.

**Step 3: Run the no-S2 negative regression**

```bash
PATH=/home/beau/beau-cc/bin:$PATH python3 scripts/regress.py --no-build --smmu-no-s2-smoke
```

Expected: PASS with S2P absent, assignment disabled, no BYPASS STE, and the instance in global ABORT.

### Task 4: QEMU Defaults and Release Verification

**Files:**
- Modify: `scripts/kick.py`
- Modify: `scripts/regress.py`

**Step 1: Keep developer and regression launchers aligned**

Add QEMU's explicit `-global arm-smmuv3.stage=2` to normal launchers. The negative regression alone selects stage 1.

**Step 2: Verify debug and release builds**

```bash
PATH=/home/beau/beau-cc/bin:$PATH make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- clean
PATH=/home/beau/beau-cc/bin:$PATH make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
PATH=/home/beau/beau-cc/bin:$PATH make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- RELEASE=1 clean
PATH=/home/beau/beau-cc/bin:$PATH make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- RELEASE=1 -j$(nproc)
```

Expected: both builds pass with `-Werror` and no physical BYPASS implementation remains.

**Step 3: Run retained regressions**

```bash
PATH=/home/beau/beau-cc/bin:$PATH python3 scripts/regress.py
PATH=/home/beau/beau-cc/bin:$PATH python3 scripts/regress.py --no-build --smmu-no-s2-smoke
```

Expected: both pass. No temporary `scripts/test_*.py` file is created.

### Task 5: Review and Finish

**Files:**
- Review all files changed since the plan commit.

**Step 1: Spec review**

Confirm every PR 1 requirement in `sdk/beau/SMMUv3.md` is implemented and that no synthetic vSMMU capability is advertised.

**Step 2: Code-quality review**

Check ownership publication, error rollback, register ordering, overflow-safe range validation, diagnostics, comments, and absence of copied Nebula implementation text.

**Step 3: Final verification**

Re-run debug/release builds and both QEMU SMMU regressions after review fixes.
