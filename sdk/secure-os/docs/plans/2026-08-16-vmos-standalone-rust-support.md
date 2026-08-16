# VMOS Standalone Rust Support Implementation Plan

> **For implementer:** Use TDD throughout. Write failing test first. Watch it fail. Then implement.

**Goal:** Remove active dependency on `../microkit` and publish ARM64 ELF outputs at `build/vmos/<board>/<config>/elf/` using new Rust-only support code.

**Architecture:** A standalone `vmos/support` Rust crate verifies vendored sources against a checked-in SHA-256 manifest and publishes required ELF artifacts from the internal Microkit SDK. The legacy Python builder remains unchanged except for prior compatibility fixes; Make orchestrates both layers.

**Tech Stack:** Rust 1.94, standard library, `sha2`, Cargo tests, GNU Make.

---

### Task 1: Standalone vendored-source integrity verification

**Files:**

- Create: `vmos/support/Cargo.toml`
- Create: `vmos/support/Cargo.lock`
- Create: `vmos/support/src/main.rs`
- Create: `vmos/UPSTREAM_SHA256`
- Delete: `vmos/UPSTREAM_FILES`
- Delete: `vmos/tests/test_upstream_sync.py`

**Step 1: Write failing Rust tests**

Add unit tests for manifest parsing and verification:

- accept a sorted `64-hex-digest + two spaces + relative-path` entry;
- reject malformed hashes, duplicate paths, unsorted paths, absolute paths,
  and `..` components;
- report a missing file;
- report a digest mismatch;
- verify a valid temporary file without accessing any sibling directory.

**Step 2: Run tests — confirm RED**

Command:

```shell
cargo test --manifest-path vmos/support/Cargo.toml
```

Expected: FAIL until parsing and SHA-256 verification are implemented.

**Step 3: Implement minimal verifier**

Implement a `verify --root PATH --manifest PATH` command using `sha2`. Error
messages must name the offending manifest line or source path. It must only
join safe relative paths beneath `--root`.

Generate `UPSTREAM_SHA256` from the current 78 vendored files in the existing
manifest, sorted by relative path, before deleting the path-only manifest and
Python sibling-comparison test.

**Step 4: Verify GREEN and standalone behavior**

Commands:

```shell
cargo test --manifest-path vmos/support/Cargo.toml
cargo run --quiet --manifest-path vmos/support/Cargo.toml -- \
    verify --root vmos --manifest vmos/UPSTREAM_SHA256
```

Expected: PASS without reading `../microkit`.

**Step 5: Commit**

```shell
git add vmos/support vmos/UPSTREAM_SHA256 vmos/UPSTREAM_FILES \
    vmos/tests/test_upstream_sync.py
git commit -m '[beau 0007] (11)' \
    -m 'verify vendored VMOS sources without an external checkout'
```

### Task 2: Shallow ELF publication

**Files:**

- Modify: `vmos/support/src/main.rs`
- Modify: `Makefile.vmos`
- Modify: `vmos/tests/test_build_vmos.py`

**Step 1: Write failing Rust publisher tests**

Add tests that:

- reject a missing internal SDK ELF and name it;
- copy exactly `sel4.elf`, `loader.elf`, `monitor.elf`, and
  `initialiser.elf` into `<output>/elf`;
- preserve source contents;
- succeed when the destination already exists.

Update the legacy layout test to describe the internal SDK only and remove
assertions that contain sibling Microkit paths.

**Step 2: Run tests — confirm RED**

Command:

```shell
cargo test --manifest-path vmos/support/Cargo.toml
```

Expected: FAIL because `publish` is not implemented.

**Step 3: Implement publisher and Make integration**

Implement:

```text
publish --sdk PATH --board NAME --config NAME --output PATH
```

The source is `<sdk>/board/<board>/<config>/elf`; destination is
`<output>/elf`.

After `build_vmos.py` succeeds, `Makefile.vmos build` runs the Rust publisher
with output `$(BUILD_DIR)/$(BOARD)/$(CONFIG)`. `test` runs Cargo tests;
`verify-source` runs the Rust verifier. Cargo target output stays under
`build/vmos/.support-target`.

**Step 4: Verify GREEN**

Commands:

```shell
make -f Makefile.vmos test
make -f Makefile.vmos verify-source
make -f Makefile.vmos build
```

Expected: PASS and all four files exist under
`build/vmos/qemu_virt_aarch64/debug/elf/`.

**Step 5: Commit**

```shell
git add vmos/support/src/main.rs Makefile.vmos vmos/tests/test_build_vmos.py
git commit -m '[beau 0007] (12)' -m 'publish VMOS ELF outputs at a shallow path'
```

### Task 3: Standalone documentation and final validation

**Files:**

- Modify: `vmos/README.md`
- Modify: `docs/plans/2026-08-16-vmos-hypervisor-design.md`

**Step 1: Add an active-reference audit**

Use `rg` to confirm active build, test, Rust, and README files contain no
filesystem dependency on `../microkit`. Historical implementation plans may
retain provenance text.

**Step 2: Update user documentation**

Document:

- the repository is standalone;
- new project functionality must be Rust;
- `UPSTREAM_SHA256` is the vendored-source baseline;
- primary ELF output is `build/vmos/<board>/<config>/elf/`;
- the deeper SDK path is internal compatibility state.

**Step 3: Run complete verification**

```shell
make -f Makefile.vmos test
make -f Makefile.vmos verify-source
make -f Makefile.vmos build
file build/vmos/qemu_virt_aarch64/debug/elf/*.elf
git diff --check
git status --short
```

Expected: all tests and build pass; four shallow outputs are ELF64 AArch64;
only the user's unrelated `.gitignore` and `AGENTS.md` remain outside intended
commits.

**Step 4: Commit**

```shell
git add vmos/README.md docs/plans/2026-08-16-vmos-hypervisor-design.md
git commit -m '[beau 0007] (13)' -m 'document standalone Rust VMOS outputs'
```

