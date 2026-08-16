# VMOS ARM64 Hypervisor Implementation Plan

> **For implementer:** Use TDD throughout. Write failing test first. Watch it fail. Then implement.

**Goal:** Add an ARM64-only VMOS hypervisor component copied unchanged from the adjacent Microkit tree and provide a root-level build that produces seL4 and all required Microkit runtime/tool artifacts.

**Architecture:** `vmos/` contains a byte-identical, architecture-pruned dependency closure of Microkit loader, initialiser, runtime library, monitor, and host tool sources. A root `build_vmos.py` orchestrates the existing seL4 CMake build and unchanged upstream Make/Cargo builds into a Microkit-compatible SDK layout; `Makefile.vmos` is the stable user entry point.

**Tech Stack:** Python 3 standard library and `unittest`, CMake/Ninja, GNU Make, AArch64 bare-metal GCC, Rust/Cargo, seL4, Microkit.

---

### Task 1: Define and copy the upstream ARM64 source closure

**Files:**

- Create: `vmos/tests/test_upstream_sync.py`
- Create: `vmos/UPSTREAM_FILES`
- Create: `vmos/Cargo.toml`
- Create: `vmos/Cargo.lock`
- Create: `vmos/VERSION`
- Create: `vmos/LICENSE.md`
- Create: `vmos/LICENSES/*`
- Create: `vmos/loader/**`
- Create: `vmos/initialiser/**`
- Create: `vmos/libmicrokit/**`
- Create: `vmos/monitor/**`
- Create: `vmos/tool/microkit/**`

**Step 1: Write the failing tests**

Create `vmos/tests/test_upstream_sync.py` with tests that:

1. read non-empty, sorted, unique relative paths from `vmos/UPSTREAM_FILES`;
2. require each listed destination and `../microkit/<path>` to be a regular file;
3. compare every pair with `filecmp.cmp(..., shallow=False)`;
4. require the manifest to include the workspace files and each of the five components;
5. reject `loader/src/riscv`, `monitor/src/riscv`, `monitor/src/x86_64`,
   `libmicrokit/src/riscv`, `libmicrokit/src/x86_64`, all `example/`, `docs/`,
   and upstream `tests/` paths;
6. allow only `aarch64`, common, license, version, and Cargo workspace files.

The test must derive the repository root from `Path(__file__).resolve()` and must not depend on the caller's working directory.

**Step 2: Run test — confirm it fails**

Command: `python3 -m unittest vmos.tests.test_upstream_sync -v`

Expected: FAIL because `vmos/UPSTREAM_FILES` does not exist.

**Step 3: Copy the minimal unchanged source closure**

Populate the manifest and copy exactly these categories from `../microkit`:

- workspace: `Cargo.toml`, `Cargo.lock`, `VERSION`, `LICENSE.md`, `LICENSES/**`;
- loader: `Makefile`, `aarch64.ld`, common headers/C sources, `src/aarch64/**`;
- initialiser: `Cargo.toml`, `src/**`;
- libmicrokit: `Makefile`, `microkit.ld`, `include/**`, common C sources,
  `src/aarch64/**`;
- monitor: `Makefile`, common headers/C sources, `src/aarch64/**`;
- tool: `Cargo.toml`, `object_sizes.h`, `address_space_constants.h`, `src/**`.

Do not edit copied bytes. Keep `UPSTREAM_FILES` sorted and relative to both
`vmos/` and `../microkit/`.

**Step 4: Run test — confirm it passes**

Command: `python3 -m unittest vmos.tests.test_upstream_sync -v`

Expected: PASS; every copied implementation file matches upstream.

**Step 5: Commit**

```shell
git add vmos
git commit -m '[beau 0007] (03)' -m 'import the ARM64 Microkit hypervisor source closure'
```

Do not add the unrelated root `AGENTS.md`.

### Task 2: Build configuration and seL4 SDK preparation

**Files:**

- Create: `vmos/tests/test_build_vmos.py`
- Create: `build_vmos.py`

**Step 1: Write failing configuration tests**

Load `build_vmos.py` with `importlib.util.spec_from_file_location`. Add isolated
tests for these public behaviors:

- `parse_int("0x70000000") == 0x70000000` and decimal input works;
- invalid and negative addresses raise `argparse.ArgumentTypeError`;
- `BoardConfig.default()` selects `qemu_virt_aarch64`, `qemu-arm-virt`,
  `cortex-a53`, loader address `0x70000000`, and four CPUs;
- `BoardConfig.validate()` rejects an empty board/platform/CPU, a non-aligned
  loader address, and fewer than one CPU;
- `kernel_definitions(board, "debug")` contains
  `KernelSel4Arch=aarch64`, `KernelIsMCS=ON`,
  `KernelArmHypervisorSupport=ON`, `KernelPlatform=qemu-arm-virt`,
  `KernelDebugBuild=ON`, and `KernelPrinting=ON`;
- release definitions do not enable debug/printing;
- an unsupported configuration name raises `ValueError`;
- `cmake_args()` points `-S` at the current repository, uses Ninja, the
  AArch64 cross prefix, and never references `../microkit`.

**Step 2: Run tests — confirm they fail**

Command: `python3 -m unittest vmos.tests.test_build_vmos -v`

Expected: FAIL because `build_vmos.py` does not exist.

**Step 3: Implement the tested configuration API**

Create a standard-library-only Python module containing:

- immutable `BoardConfig(name, kernel_platform, gcc_cpu,
  loader_link_address, cpus, extra_cmake)`;
- the default qemu configuration above;
- `parse_int`, `kernel_definitions`, `cmake_args`, and `run_checked`;
- CLI options `--board`, `--kernel-platform`, `--gcc-cpu`,
  `--loader-link-address`, `--cpus`, `--config {debug,release}`,
  repeatable `--cmake-def KEY=VALUE`, `--build-dir`, and `--jobs`;
- explicit checks for Python, CMake, Ninja, Make, Cargo/Rust, and the
  `aarch64-none-elf-` toolchain;
- `configure_and_build_sel4()` that configures, builds, and installs the
  current source tree under the selected output directory.

Commands must be lists passed to `subprocess.run(check=True)`; do not construct
shell strings. All paths must be absolute before subprocess execution.

**Step 4: Run tests — confirm they pass**

Command: `python3 -m unittest vmos.tests.test_build_vmos -v`

Expected: PASS.

**Step 5: Commit**

```shell
git add build_vmos.py vmos/tests/test_build_vmos.py
git commit -m '[beau 0007] (04)' -m 'add the ARM64 VMOS kernel build configuration'
```

### Task 3: Build and package all VMOS components

**Files:**

- Modify: `vmos/tests/test_build_vmos.py`
- Modify: `build_vmos.py`

**Step 1: Add failing artifact-layout tests**

Using `tempfile.TemporaryDirectory`, add tests that:

- `BuildLayout.create()` produces work, install, SDK `bin`, and
  `board/<board>/<config>/{elf,include,lib}` directories;
- `required_artifacts()` lists `sel4.elf`, `loader.elf`, `monitor.elf`,
  `initialiser.elf`, `libmicrokit.a`, `microkit.ld`, and `bin/microkit`;
- `verify_artifacts()` reports every missing path in one `BuildError`;
- `copy_sel4_sdk()` copies the kernel, generated invocations/platform JSON,
  and all four seL4 installed include trees without modifying sources;
- `parse_constant_header()` accepts `name: integer` preprocessor output and
  evaluates the single subtraction form used by upstream headers;
- all generated component commands use only `vmos/` sources and the selected
  output directory, never `../microkit`.

**Step 2: Run the focused tests — confirm they fail**

Command: `python3 -m unittest vmos.tests.test_build_vmos -v`

Expected: FAIL because layout, packaging, and component functions are absent.

**Step 3: Implement minimal component orchestration**

Extend `build_vmos.py` to:

1. create a Microkit-compatible SDK directory and copy `vmos/VERSION`;
2. copy the installed seL4 kernel, generated JSON metadata, and installed
   headers into the SDK board/config directory;
3. preprocess `vmos/tool/microkit/{object_sizes,address_space_constants}.h`
   with `aarch64-none-elf-cpp` and write JSON constants;
4. run Cargo from `vmos/` to test/build `microkit-tool` and build `initialiser`
   for `aarch64-unknown-none`, with `SEL4_PREFIX` set to the seL4 install;
5. invoke the unchanged loader, monitor, and libmicrokit Makefiles with
   `ARCH=aarch64`, the selected CPU, loader address, SDK include path, and
   AArch64 target triple;
6. copy resulting files into the SDK `elf`, `lib`, `include`, and `bin`
   directories;
7. verify all artifacts and check `kernel/gen_config/kernel/gen_config.h`
   contains `CONFIG_ARM_HYPERVISOR_SUPPORT 1`;
8. preserve partial builds only under the requested build directory and return
   non-zero with an actionable message on failure.

**Step 4: Run the full Python tests — confirm they pass**

Command: `python3 -m unittest discover -s vmos/tests -v`

Expected: PASS.

**Step 5: Commit**

```shell
git add build_vmos.py vmos/tests/test_build_vmos.py
git commit -m '[beau 0007] (05)' -m 'build and package the VMOS runtime components'
```

### Task 4: Root build entry point and end-to-end verification

**Files:**

- Create: `Makefile.vmos`
- Create: `vmos/README.md`
- Modify: `.gitignore`
- Modify: `vmos/tests/test_build_vmos.py`

**Step 1: Add failing entry-point tests**

Add tests that run `make -f Makefile.vmos help` and assert it documents
`build`, `test`, `verify-source`, `clean`, `BOARD`, `CONFIG`, and `JOBS`.
Run `make -n -f Makefile.vmos build` and assert it calls root
`build_vmos.py`, defaults to `qemu_virt_aarch64/debug`, and forwards variables.
Assert `.gitignore` ignores `/build/vmos/`.

**Step 2: Run tests — confirm they fail**

Command: `python3 -m unittest discover -s vmos/tests -v`

Expected: FAIL because the root Makefile and ignore rule are absent.

**Step 3: Add the minimal entry point and usage documentation**

`Makefile.vmos` must provide:

- `build`: invoke `python3 build_vmos.py` with all selected variables;
- `test`: run `python3 -m unittest discover -s vmos/tests -v`;
- `verify-source`: run only the upstream sync test;
- `clean`: remove only `build/vmos`;
- `help`: print variables, defaults, targets, and output location.

`vmos/README.md` must explain component ownership, the build flow, exact
commands, prerequisites, output layout, and the byte-identical upstream source
rule. It may document code but must not modify copied implementation files.

**Step 4: Verify tests and build as far as the environment permits**

Commands:

```shell
python3 -m unittest discover -s vmos/tests -v
make -f Makefile.vmos verify-source
make -f Makefile.vmos build
```

Expected: unit/source tests PASS. The full build PASSes when Cargo/Rust
1.94 plus the AArch64 Rust target and network/cache dependencies are available;
otherwise it must stop before mutation with a precise missing-tool diagnostic.
Do not claim a full build passed unless all required artifacts exist.

Also run:

```shell
git diff --check
git status --short
```

Expected: no whitespace errors; only intended files/commits plus the user's
untracked `AGENTS.md` are present.

**Step 5: Commit**

```shell
git add Makefile.vmos vmos/README.md .gitignore vmos/tests/test_build_vmos.py
git commit -m '[beau 0007] (06)' -m 'provide the top-level VMOS build workflow'
```
