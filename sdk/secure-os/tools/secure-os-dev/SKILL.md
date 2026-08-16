---
name: secure-os-dev
description: Guarded development workflow for the secure-os seL4 and VMOS repository. Use for feature development, refactoring, debugging, build changes, reviews, or commits that touch secure-os, especially seL4 core paths such as src/, libsel4/, include/, configs/, or VMOS Rust code under vmos/. Enforces strict seL4 review, modular Rust-only VMOS additions, scored design alternatives, and separate human approval gates for requirements, design, scoring, implementation, and code submission.
---

# Secure OS Development

Apply conservative change control to secure-os. Keep the current workflow stage visible and never treat approval for one stage as approval for the next.

## Non-negotiable rules

1. Treat seL4 kernel and ABI code as high risk. Do not casually modify `src/`, `libsel4/`, `include/`, `configs/`, root CMake logic, architecture code, platform code, generated-interface inputs, or equivalent seL4-owned paths.
2. Implement new VMOS functionality in Rust. Do not add new C, C++, assembly, or Python feature logic unless the user explicitly approves an exception.
3. Keep VMOS Rust code modular. Give each source file one clear responsibility and split parsing, state, hardware access, commands, protocols, and tests into coherent modules when responsibilities differ.
4. Require separate human confirmation after each workflow stage: requirements, design, design scoring, implementation, and code submission.
5. Never stage, commit, amend, rebase, push, submit, or discard changes without the approval required for that exact action. Earlier approval is not transferable.
6. Preserve unrelated worktree and index changes. Stage only the explicitly approved files.

## Classify the change first

Before proposing edits, inspect the repository instructions and worktree, then classify every affected file:

- **seL4 core:** `src/`, `libsel4/`, `include/`, `configs/`, kernel CMake and architecture/platform implementation.
- **Vendored VMOS/Microkit:** copied upstream code under `vmos/` that is not project-owned Rust.
- **VMOS Rust:** project-owned Rust runtime, tools, libraries, tests, and modules.
- **Build or documentation:** Makefiles, build orchestration, manifests, README, and supporting metadata.

If a task crosses classes, apply the strictest applicable rule.

## seL4 core review gate

Default to leaving seL4 core unchanged. Before requesting permission to edit it, provide:

- the concrete reason the change cannot live in VMOS Rust or build glue;
- the exact files and symbols affected;
- at least one lower-risk alternative and why it is insufficient;
- ABI, capability, scheduling, memory-safety, architecture, and verification risks;
- upstream provenance or comparison when relevant;
- the focused tests, full ARM64 build, and QEMU validation that will be run.

Wait for explicit approval before editing. Keep an approved patch minimal and avoid cleanup unrelated to the stated requirement. After implementation, present the seL4 diff separately for human review.

## VMOS Rust structure

For new VMOS code:

- use Rust and preserve applicable `no_std` constraints;
- define module boundaries before implementation;
- keep public APIs narrow and place hardware-specific unsafe code behind a small reviewed interface;
- avoid mixing command parsing, device access, state management, formatting, and transport logic in one file;
- colocate focused unit tests with their module and add integration or QEMU tests for cross-module behavior;
- prefer a directory module when a feature has multiple responsibilities, for example `console/commands.rs`, `console/completion.rs`, and `console/process.rs`;
- do not grow an existing monolithic file merely because it is convenient.

## Mandatory gated workflow

### Gate 1: Clarify requirements

Restate:

- goal and user-visible behavior;
- scope and non-goals;
- platform and language constraints;
- acceptance criteria and validation environment;
- files or subsystems likely to be affected.

Resolve one material ambiguity at a time. Ask the user to confirm the requirements. Do not design or edit code before confirmation.

### Gate 2: Design alternatives

After requirement approval, present two or three viable designs. For each design include:

- architecture and data flow;
- Rust module and file layout;
- seL4 or vendored-code impact;
- error handling and safety boundaries;
- testing strategy;
- trade-offs and migration cost.

Recommend one design, but do not select it for the user. Ask for design confirmation. Do not score or implement before confirmation.

### Gate 3: Score the approved designs

Score the alternatives on a 100-point scale using this default weighting:

| Criterion | Weight |
| --- | ---: |
| seL4 and system safety | 30 |
| Rust modularity and maintainability | 25 |
| requirement fit | 20 |
| testability and observability | 15 |
| implementation cost and complexity | 10 |

Explain every material deduction. Identify the highest-scoring design and any blocking risk. Ask the user to confirm the selected design and score before implementation.

### Gate 4: Implement

After scoring approval:

1. Create an implementation plan with exact files and tests.
2. Add or update a failing test first when practical.
3. Implement only the approved design in modular Rust.
4. Run focused tests, full relevant tests, formatting, linting, ARM64 build, and QEMU validation proportional to risk.
5. Run `git diff --check` and review the complete diff, separating seL4 core changes from VMOS changes.
6. Report changed files, behavior, test evidence, known limitations, and any deviation from the approved design.

Ask the user to confirm the implementation. Do not stage or commit at this gate.

If implementation reveals a design or requirement change, stop and return to the affected earlier gate.

### Gate 5: Submit code

Only after implementation approval:

1. Show the intended file list, commit message, current `git status`, and test summary.
2. Identify any pre-existing staged or unrelated changes and exclude them.
3. Ask for explicit confirmation to create the commit.
4. After confirmation, stage only approved paths, inspect `git diff --cached`, then commit with the approved message.
5. Report the commit hash and final status.

Treat pushing, creating a review, merging, or submitting as separate external actions requiring separate explicit approval.

## Confirmation protocol

- End each gate with one explicit confirmation request.
- Accept confirmation only for the named current gate.
- Do not combine multiple gates into one approval request.
- Summarize any changed assumptions before asking again.
- When approval is denied or changes are requested, remain at the current gate.
- When the user asks only for analysis or review, do not infer permission to implement.

## Completion checklist

Before declaring work ready for submission, verify:

- requirements, design, scoring, and implementation each have recorded user confirmation;
- any seL4 core edit has a necessity statement and separate diff review;
- all new VMOS feature code is Rust and has coherent module boundaries;
- relevant tests and ARM64/QEMU validation pass;
- unrelated worktree and index changes remain untouched;
- no commit or external submission occurred without explicit approval.
