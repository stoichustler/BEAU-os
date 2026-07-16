---
name: beau-os-development
description: Implement, compile, validate, and submit BEAU OS ARM64 hypervisor changes in this repository. Use when Codex edits BEAU OS C, assembly, headers, Kconfig, DTS, Makefiles, SDK documentation, or regression code; diagnoses build or runtime failures; runs qemu or rk356x builds; validates SMMU, vPCI, vGIC, scheduler, watchdog, console, or virtio behavior; or prepares a BEAU OS commit.
---

# BEAU OS Development

Use the repository's code ownership, safety rules, build entry points, and
commit format consistently. Keep edits narrowly scoped and preserve unrelated
worktree changes.

## Establish Context

1. Resolve the repository root with `git rev-parse --show-toplevel` and operate
   from that directory.
2. State that `scripts/codex/beau-os-development/SKILL.md` is being used.
3. Read `sdk/sdk.md` completely before inspecting or changing implementation.
4. Read `sdk/beau/ISO26262.md` completely before changing C, assembly, hardware
   policy, VM isolation, or shared runtime state.
5. Read only the relevant sections of `sdk/beau/BEAU-os.md` and the task-specific
   documents under `sdk/beau/`.
6. Inspect `git status --short`, the recent commit headers, and overlapping
   diffs. Treat existing changes as user-owned unless their origin is known.
7. Inspect the current implementation and its callers before selecting a fix.
   Use external Nebula or Zephyr trees only as references; adapt their design to
   BEAU ownership and failure rules instead of copying platform assumptions.

## Assign Ownership

Place behavior in the narrowest existing layer:

- Put EL2 entry, traps, CPU state, stage-1/stage-2, GIC/ITS, SMMU, and vCPU
  architecture mechanics in `arch/arm64/`.
- Put VM/vCPU lifecycle, scheduling, timers, IRQ dispatch, memory ownership, and
  hypercalls in `core/`.
- Put public contracts in `include/`; keep hardware and ABI fields fixed-width
  and layout-stable.
- Put platform discovery, shell commands, vPCI, passthrough policy, virtio
  proxy, watchdog, image loading, and guest-facing DT construction in `sdk/bsp/`.
- Put retained Linux and Zephyr validation sources in `sdk/kbe/` and `sdk/zsh/`
  only after validation.
- Put reusable guest images in `sdk/image/`.
- Put retained automated coverage in `scripts/regress.py`; never submit
  `scripts/test_*.py`.

Split a feature when the layers have different owners. Keep policy, guest
emulation, and physical hardware programming separate when that makes ownership
and cleanup explicit.

## Implement Safely

1. Define the success and failure contract before editing. For isolation paths,
   keep the device, memory, IRQ, or DMA object hidden until all validation and
   synchronization succeeds.
2. Validate pointers, indexes, sizes, alignment, address arithmetic, enum
   values, register capabilities, and externally supplied data before use.
3. Make shared-state ownership and publication ordering explicit. Use the
   matching lock, atomic primitive, barrier, cache operation, or hardware sync.
4. Keep cleanup deterministic and reverse acquisition order. Undo only state
   acquired by the current transition.
5. Keep loops bounded, handle validation and allocation return values, and emit
   actionable diagnostics containing the failing object and owner.
6. Follow existing helpers and local style. Avoid new abstractions unless they
   remove real duplication or enforce an ownership boundary.
7. Use `apply_patch` for manual edits. Do not rewrite or revert unrelated files.
8. Keep generic code and comments platform-neutral. Put platform names and
   quirks only in the matching platform implementation or documentation.

For non-trivial C logic, use the dated design-comment form required by
`sdk/sdk.md`:

```c
/* [YYYYMMDD] Topic
 *
 * source state
 *     |
 *     v
 * validate and synchronize
 *     |
 *     +--> fail closed
 *     |
 *     v
 * publish ownership
 *
 * Key rule:
 *   - identify the state owner;
 *   - state required before/after ordering;
 *   - state the failure or isolation violation being prevented.
 */
```

Do not add comments that merely repeat assignments or branch conditions. Use
plain ASCII in low-level C diagrams and fatal diagnostics.

## Build

Select the platform from the task or current configuration. Do not silently
substitute qemu validation for a requested hardware build.

Verify required tools when the environment is not already proven:

```sh
command -v aarch64-none-elf-gcc
command -v dtc
python3 -c 'import elftools'
```

Use the documented qemu build sequence for a clean final build:

```sh
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- clean
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- Bconfig
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j"$(getconf _NPROCESSORS_ONLN)"
```

Use `PLATFORM=rk356x` for the rk356x build. Set `PATH` or `CROSS_COMPILE` from
the actual environment; do not hardcode a developer home directory into source
or scripts. Use an incremental `make` while iterating, then perform the clean
build before submission.

Before cleaning or launching, inspect active build and QEMU processes. Do not
terminate sessions not started by the current task. Use an isolated
`HV_OBJDIR` when another task owns the default output; pass that image to
validation with `scripts/regress.py --kernel <image>`.

## Validate

Scale validation to the changed contract:

1. Run `git diff --check` after edits.
2. Compile every affected target with warnings treated as errors.
3. Run `python3 -m py_compile scripts/regress.py` when regression code changes.
4. Add durable success and failure coverage to `scripts/regress.py` for new
   runtime behavior.
5. Run a targeted smoke first, then the broader regression required by the
   blast radius.

Use the repository entry points:

```sh
python3 scripts/kick.py --build
python3 scripts/regress.py
python3 scripts/regress.py --no-build
python3 scripts/regress.py --smmu-passthrough-smoke
python3 scripts/regress.py --smmu-no-s2-smoke
python3 scripts/regress.py --wdt-restart-smoke
```

Select only relevant variants, but cover both success and fail-closed behavior
for isolation-sensitive changes. Treat timeouts and retries as evidence to
investigate; do not hide a product failure by only increasing a timeout. Re-run
after a transient failure and preserve the first failure's diagnostic context.

## Submit

Commit only when requested or when the active task explicitly requires
submission.

1. Re-read `git status --short` and `git diff` after validation.
2. Determine the next two-digit count from recent commits for the active BEAU
   version.
3. Stage an explicit path list. Leave unrelated modified and untracked files
   unstaged.
4. Run `git diff --cached --check` and review `git diff --cached --stat` plus
   the staged diff.
5. Use the exact commit structure:

```text
[beau 0007] (xx)

Purpose of the change.
```

6. Verify the resulting commit and worktree. Push only when the user requested
   remote submission. Never place credentials in source, helper files, commit
   messages, or command-line arguments.

Report the commit hash, pushed branch when applicable, validation performed,
known limitations, and any preserved user-owned changes.
