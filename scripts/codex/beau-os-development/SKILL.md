---
name: beau-os-development
description: Plan, implement, compile, validate, and submit BEAU OS ARM64 hypervisor changes in this repository through explicit human approval gates. Use when Codex scopes or designs a BEAU OS change; edits C, assembly, headers, Kconfig, DTS, Makefiles, SDK documentation, or regression code; diagnoses build or runtime failures; runs qemu or rk356x builds; validates SMMU, vPCI, vGIC, scheduler, watchdog, console, or virtio behavior; or prepares a BEAU OS commit or push.
---

# BEAU OS Development

Use the repository's code ownership, safety rules, build entry points, and
commit format consistently. Keep edits narrowly scoped and preserve unrelated
worktree changes.

## Enforce Phase Gates

Treat approval as scoped to the phase and artifacts shown to the user. Do not
infer approval from silence, an earlier approval, or a general request to finish
the task. Read-only inspection needed to prepare the current phase is allowed.

### Phase 1: Confirm Requirements and Boundaries

Before designing or editing, state:

- the requested outcome and explicitly excluded work;
- the affected subsystem and target platform;
- allowed and prohibited file or directory boundaries;
- the expected behavior, failure contract, and measurable success criteria;
- for guest OS changes, the human-provided source root, editable paths, and
  guest source to BEAU retained-copy mapping;
- assumptions, ambiguities, and decisions still needed.

Ask for explicit human approval. Do not enter Phase 2 until the requirements and
boundaries are approved. If the requirements later change, return to Phase 1.

### Phase 2: Design the Change

After Phase 1 approval, inspect the implementation and present:

- the exact files expected to change and their ownership layers;
- the control flow, data flow, state ownership, and cleanup path;
- the implementation sequence and required design comments;
- per-code-tree file changes, build targets, validation, and worktree handling;
- the estimated change cost and impact analysis: implementation scope and
  dependencies; ABI, state, configuration, compatibility, CPU, memory,
  latency, and user-visible effects; affected platforms, guests, and
  subsystems; risks, mitigations, and validation evidence. Quantify known
  values such as files, bytes, records, or build targets. For unknown values,
  state a low/medium/high estimate with its rationale. Explicitly state when a
  dimension has no expected impact;
- the validated guest source files to copy into BEAU, when guest OS code changes;
- the build target, automated-test proposal, risks, and rollback approach.

Do not edit implementation files during this phase. When requested, the only
allowed write is an AI-generated development plan under `docs/plan/`. Treat
these plans as local process artifacts: they may describe the complete workflow
but must remain outside every code submission. Ask for explicit human approval
before entering Phase 3. If implementation requires a new file, subsystem, ABI
change, or other unapproved scope, stop and return to Phase 2.

### Phase 3: Implement the Approved Design

Modify only the approved files and behavior. Follow the repository rules below
and report any deviation before making it. After implementation, summarize the
actual diff and continue to the approved compilation checks in Phase 4.

### Phase 4: Compile

Compile every affected target in every approved code tree specified by the
design. Compilation, compiler diagnostics, `git diff --check`, and other
non-test integrity checks do not require a separate test approval. Do not
silently add or run automated tests as part of a build command.

### Phase 5: Plan and Approve Testing

#### Automated Testing

Treat unit, integration, regression, smoke, and runtime test scripts as
automated tests. Before changing a test script, show the proposed files and
coverage change and obtain explicit human approval. After any approved test
changes are complete, obtain separate approval before enabling or running tests.

The execution approval request must list:

- test type and script path;
- target code tree;
- exact command and target platform;
- estimated duration and environment impact;
- whether the test modifies images, generated files, devices, or runtime state.

Approval to implement or compile does not authorize test modification or test
execution. If the test plan changes, stop and request approval again.

#### Manual Testing

When the user selects manual testing or validation, obtain explicit confirmation
before the final test handoff. After confirmation, direct the human to run this
exact command from the repository root:

```sh
./scripts/kick.py --build --tee
```

The human must interact directly with BEAU OS in that session and validate the
behavior affected by the current change. Keep the handoff concise. Do not embed
a task-specific test script, fixed command sequence, exhaustive evidence list,
or generic prerequisite and cleanup template in this skill. Provide additional
interaction suggestions or pass/fail details only when the user requests them.

Do not execute, automate, substitute, start, or monitor this manual acceptance
on the user's behalf. After handing off the command, stop AI-side test activity
and wait for the human result unless the user requests specific assistance.

Accept an explicit human pass or fail report as the authoritative test result
without performing corroborating work. Enter Phase 6 after a reported pass.
Record a reported failure and wait for further direction. Treat ambiguous or
incomplete feedback as unverified and continue waiting; do not infer a result.

### Phase 6: Submit

After validation, report the changed files, diff summary, build and approved
test results, known limitations, and proposed commit message for each code tree.
Obtain explicit human approval before creating each commit. Obtain separate
approval before pushing each commit or creating/updating a remote review. Never
apply approval for one code tree to another.

Never request, accept, store, or pass a Gitee, GitHub, or Gerrit password or
access token in source files, helper files, prompts, logs, commit messages, or
command-line arguments. Ask the user to configure local SSH, a credential
manager, or the provider's interactive login when authentication is unavailable.

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
   Do not search for or inspect local external projects unless the user provides
   the source root and approves its role in the task.

## Handle External and Guest OS Code

1. Operate only in the current BEAU repository by default. Do not discover,
   search, or infer another source tree from local directories, images, build
   outputs, or neighboring workspaces.
2. Treat a human-provided external reference project as read-only unless it is
   explicitly approved as the guest OS implementation target. Adapt reference
   ideas to BEAU ownership and failure rules instead of copying assumptions.
3. Before changing guest OS code, confirm its exact source root and editable
   paths in Phase 1 and approve its file-level design in Phase 2. After both
   gates pass, modify the approved guest OS files directly.
4. Keep task-specific external project names and absolute paths in task context;
   do not add them to this skill.
5. Inspect and preserve user-owned changes separately in every approved code
   tree. Build, test, commit, and push each tree under its own applicable rules
   and approval gates.
6. `sdk/kbe/` retains BEAU general-purpose Linux drivers. `sdk/zsh/` retains
   BEAU general-purpose Zephyr applications. After every required build and
   automated or manual test in the approved design passes, copy only the
   approved reusable source, header, and integration files according to the
   Phase 2 mapping.
7. Compare the retained copies with their validated guest source files after
   copying. Do not retain a complete external tree, build outputs, logs,
   temporary files, or images.
8. Do not update `sdk/kbe/` or `sdk/zsh/` when required validation is skipped,
   fails, remains incomplete, or lacks the required evidence.

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
- Put retained BEAU general-purpose Linux drivers in `sdk/kbe/` and retained
  BEAU general-purpose Zephyr applications in `sdk/zsh/` only after validation.
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

Apply the `FIXME` state rules from `sdk/sdk.md`:

- For `UNSOLVED`, include `FIXME` only and omit `METHOD`.
- Mark an issue `FIXED` only when the same change includes `METHOD` and the
  corresponding code implementation.
- Never mark an issue `FIXED` in a comment-only change.

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
3. Propose `python3 -m py_compile scripts/regress.py` when regression code
   changes, and run it only after Phase 5 execution approval.
4. Propose durable success and failure coverage in `scripts/regress.py` for new
   runtime behavior. Modify it only after Phase 5 modification approval.
5. After Phase 5 execution approval, run a targeted smoke first, then the
   broader regression required by the blast radius.

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

Commit only after the Phase 6 commit approval. A request to implement, finish,
or submit a change does not replace this approval.

Use the steps below for the BEAU repository. For an approved guest OS source
tree, read and follow its repository-local submission rules, then request its
commit and push approvals separately.

1. Re-read `git status --short` and `git diff` after validation.
2. Determine the next two-digit count from recent commits for the active BEAU
   version.
3. Stage an explicit path list. Leave unrelated modified and untracked files
   unstaged. Never stage AI-generated files under `docs/plan/`.
4. Run `git diff --cached --check` and review `git diff --cached --stat` plus
   the staged diff. Verify `git diff --cached --name-only` contains no
   `docs/plan/` path before committing.
5. Use the exact commit structure:

```text
[beau 0007] (xx)

Purpose of the change.
```

6. Verify the resulting commit and worktree. Push only after the separate Phase
   6 push approval, using authentication already configured by the user.

Report the commit hash, pushed branch when applicable, validation performed,
known limitations, and any preserved user-owned changes.
