# SMMU DTS Policy Implementation Plan

> For implementer: keep the first step small and fail closed. Validate with
> platform DTS compilation and ARM64 builds.

**Goal:** Parse static passthrough device policy from ARM64 platform DTS and
register it with the existing BSP passthrough ownership layer.

**Architecture:** The parser stays in `sdk/bsp/arm64/platform_dts.c`, where
static platform policy is already consumed. A `/beau,platform/passthrough`
node may contain `beau,passthrough-device` children with StreamID, optional
SPI mapping, and writable policy. The parser calls the existing
`passthrough_register_device()` and `passthrough_register_spi()` APIs.

**Tech Stack:** C99, libfdt, BEAU BSP passthrough API, static platform DTS.

---

## Task 1: Parse Passthrough Policy

**Files:**
- Modify: `sdk/bsp/arm64/platform_dts.c`
- Modify: `sdk/bsp/include/arm64_platform_dts.h`

**Behavior:**
- Missing `/beau,platform/passthrough` is valid and changes nothing.
- Each child compatible with `beau,passthrough-device` must provide one
  StreamID through `iommus = <&smmu N>` or `beau,stream-id = <N>`.
- `beau,writable` controls writable passthrough policy.
- Optional `interrupts` plus `beau,guest-irq` registers a SPI mapping.
- Duplicate StreamIDs and duplicate SPIs rely on the passthrough layer to
  reject registration.

**Verification:**
- `dtc -I dts -O dtb` passes for QEMU and rk356x platform DTS.
- QEMU and rk356x builds pass.
- `git diff --check` passes.
