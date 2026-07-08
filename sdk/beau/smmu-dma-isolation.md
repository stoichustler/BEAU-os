# BEAU SMMU/DMA Isolation Design

## Goal

Add production-grade DMA isolation for BEAU ARM64 passthrough devices while
preserving the current static QEMU/rk356x VM model.

The safety property is:

```text
guest vCPU load/store: IPA -- CPU stage-2  --> PA
device DMA request:    IPA -- SMMU stage-2 --> PA
```

A passed-through device must only DMA through the same stage-2 permissions that
the owning VM's vCPUs use. If BEAU cannot prove that a device stream is isolated,
the device must not be assigned.

## Current State

The repository already has the first isolation framework:

- `arch/arm64/smmu/smmuv3.c`
  - creates one `iommu_domain` per VM from the VM stage-2 root HPA;
  - rejects stream assignment while `arm_smmu_hw_ready == false`;
  - tracks one active owner per StreamID;
  - contains ARM64 MSI/MSI-X remap plumbing for GICv3 ITS/vGIC LPI injection.
- `include/arch/arm64/asm/vtd.h`
  - keeps x86 VT-d compatibility names mapped to ARM64 SMMUv3 concepts.
- `sdk/bsp/passthrough.c`
  - records MMIO, IRQ, and DMA ownership as one device contract;
  - assigns SMMU stream ownership before publishing VM ownership;
  - deassigns SMMU stream ownership before returning a device to the host pool.
- `sdk/bsp/vpci/vpci.c`
  - creates an IOMMU domain from `vm->root_stg2ptp`;
  - moves PCI requester-derived streams during passthrough vPCI setup.

The missing production pieces are:

- no real SMMUv3 hardware discovery/init path;
- no stream table allocation or STE programming;
- no command queue, event queue, or fault reporting;
- no DTS/IORT parsing for platform SMMU nodes and platform-device StreamIDs;
- no static per-VM passthrough policy in `platform.dts`;
- no shell/debug command to prove stream ownership and fault state at runtime.

## Non-Goals For The First Implementation

- Do not add dynamic device assignment policy.
- Do not bypass SMMU for any passthrough device.
- Do not create an independent DMA page table for each VM.
- Do not edit Linux guest DTS files for this stage.
- Do not make QEMU passthrough required for normal QEMU boot regression.

## Static Configuration Model

Keep policy in each platform DTS.

Host hardware SMMU description should live under `/soc`:

```dts
smmu0: iommu@fd800000 {
	compatible = "arm,smmu-v3";
	reg = <0x0 0xfd800000 0x0 0x20000>;
	interrupts = <0x0 0x70 0x4>,
		     <0x0 0x71 0x4>,
		     <0x0 0x72 0x4>,
		     <0x0 0x73 0x4>;
	#iommu-cells = <1>;
	dma-coherent;
};
```

BEAU passthrough policy should live under `/beau,platform` or a dedicated child
such as `/beau,platform/passthrough`. Example:

```dts
passthrough {
	#address-cells = <2>;
	#size-cells = <2>;

	device@fe2b0000 {
		compatible = "beau,passthrough-device";
		beau,name = "can0";
		beau,owner-vm = <2>;
		iommus = <&smmu0 0x123>;
		reg = <0x0 0xfe2b0000 0x0 0x10000>;
		interrupts = <0x0 0x45 0x4>;
		beau,guest-irq = <45>;
		beau,writable;
	};
};
```

For PCIe devices, StreamID should be derived from requester ID unless platform
firmware declares an override. For platform devices, the DTS `iommus` property
is the source of truth.

## SMMUv3 Driver Architecture

The SMMUv3 driver should be split into four layers:

1. **Discovery**
   - parse SMMUv3 base, size, interrupts, feature registers, StreamID width,
     VMID width, and queue sizes;
   - reject unsupported mandatory features early;
   - keep hardware disabled or in abort-default mode until tables are ready.

2. **Table and Queue Setup**
   - allocate aligned stream table memory;
   - initialize every STE as abort/fault, never bypass;
   - allocate command queue and event queue;
   - enable command processing and event/fault reporting;
   - issue `CMD_SYNC` after table updates.

3. **Domain Binding**
   - use the VM CPU stage-2 root HPA for SMMU stage-2 translation;
   - program STE fields for VMID, S2TTB, IPA width, memory attributes, and
     shareability;
   - invalidate cached STE/context state after changes;
   - keep one active StreamID owner.

4. **Fault Handling**
   - consume event queue records;
   - record StreamID, fault IPA, permission, and event reason;
   - rate-limit logs;
   - expose counters and last-fault state through a shell command.

## Assignment Ordering

Device assignment must remain fail-closed:

```text
register policy
     |
     v
validate StreamID, MMIO, IRQ, owner VM
     |
     v
program SMMU STE to VM stage-2
     |
     v
install IRQ/MSI remap
     |
     v
map/expose MMIO to guest
     |
     v
mark device owned by VM
```

Deassignment must reverse the order:

```text
quiesce guest access
     |
     v
hide/unmap MMIO
     |
     v
remove IRQ/MSI remap
     |
     v
replace STE with ABORT
     |
     v
CMD_SYNC and invalidate
     |
     v
mark device host/free
```

The existing `sdk/bsp/passthrough.c` order already follows the most important
rule: SMMU stream ownership is programmed before VM ownership is published.

## IRQ And MSI Policy

SPI passthrough:

- platform policy must explicitly map physical SPI to guest virtual IRQ;
- shared host-owned SPIs must be rejected;
- level-triggered SPI delivery must preserve device line semantics.

MSI/MSI-X passthrough:

- physical device MSI writes target host ITS `GITS_TRANSLATER`;
- host ITS maps `(DeviceID, EventID)` to a host LPI;
- BEAU ptdev softirq injects the guest-visible `(DeviceID, EventID)` through
  `arm64_vgicv3_inject_msi()`;
- SMMU stream table still controls DMA isolation; ITS remap is not a substitute
  for DMA isolation.

## Observability

Add a shell command after the hardware path exists:

```text
smmustat
```

Suggested fields:

- hardware ready state;
- SMMU base and feature summary;
- stream table entry count and default mode;
- per StreamID owner VM and state: `abort`, `assigned`, `faulted`;
- domain VMID, stage-2 root HPA, IPA width;
- command queue sync count;
- event/fault counters;
- last fault: StreamID, IPA, reason, permission, VM owner.

Keep output narrow enough for serial logs.

## Implementation Plan

1. **Document the static policy**
   - add this design document;
   - update `sdk/sdk.md` with the current SMMU/DMA isolation status.

2. **Parse SMMU and passthrough DTS policy**
   - parse `/soc` `arm,smmu-v3` nodes into platform config;
   - parse BEAU passthrough device nodes into a BSP passthrough policy table;
   - register devices with `passthrough_register_device()` and
     `passthrough_register_spi()`;
   - validate duplicate StreamIDs and duplicate physical SPIs.

3. **Bring up SMMUv3 hardware**
   - add `arm_smmu_init_from_dts()` or an equivalent platform registration API;
   - read ID registers and reject unsupported configurations;
   - allocate stream table, command queue, and event queue;
   - initialize all STEs to abort;
   - set `arm_smmu_hw_ready = true` only after abort-default protection is
     active.

4. **Program stage-2 STEs**
   - implement `arm_smmu_assign_stream()` hardware programming;
   - implement `arm_smmu_unassign_stream()` as abort replacement plus sync;
   - share VM stage-2 root HPA, VMID, IPA width, and attributes with CPU
     stage-2 setup.

5. **Wire faults and diagnostics**
   - register SMMU event/fault IRQs;
   - consume event queue records;
   - add `smmustat`;
   - include SMMU state in failure reports when passthrough validation fails.

6. **Validate on QEMU and hardware**
   - QEMU without SMMU: assignments must fail closed with `-ENODEV`;
   - QEMU normal 3OS boot must remain unchanged;
   - rk356x build must pass after DTS/parser changes;
   - hardware: assign one non-safety platform device to a Linux VM, verify
     valid DMA works and invalid DMA reports an SMMU fault without corrupting
     another VM.

## Validation Matrix

Build checks:

```bash
export BEAU_TOOLCHAINS=$HOME/beau-cc/bin
./scripts/kick.py --build --dry-run
./scripts/kick.py --build
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
git diff --check
```

Runtime checks:

- QEMU default boot still reaches `console:\>`.
- QEMU default boot logs do not show accidental passthrough assignment.
- `smmustat` reports no hardware SMMU on QEMU unless a QEMU SMMU node is added.
- rk356x hardware logs show SMMUv3 ready before any passthrough assignment.
- Assigning a device before SMMU ready fails.
- Assigning the same StreamID to two VMs fails.
- DMA outside the owning VM RAM causes an SMMU fault and does not corrupt memory.

## Open Questions

- Which rk356x SMMU instance and StreamIDs are available for the first hardware
  test device?
- Should BEAU reserve a fixed VMID per VM ID, or allocate VMIDs from SMMU
  hardware width at boot?
- Does the first target device need MSI/MSI-X, or is SPI-only enough for the
  initial hardware validation?
- Should MMIO mapping for platform passthrough be added to the guest stage-2
  table during assignment, or remain pre-authored as static VM memory/device
  policy?
