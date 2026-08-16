# VMOS Runtime Console Implementation Plan

> **For implementer:** Use TDD throughout. Write failing test first. Watch it fail. Then implement.

**Goal:** Turn the ARM64 QEMU smoke protection domain into an interrupt-driven Rust runtime console with six diagnostic commands.

**Architecture:** Hardware-independent command parsing and line editing live in `vmos/qemu-smoke/src/console.rs` and are host-unit-tested. The `no_std` adapter in `main.rs` receives PL011 IRQ 33 through Microkit, drains UART input, and writes responses through the existing seL4 debug console. No vendored seL4 or Microkit source is modified.

**Tech Stack:** Rust 1.94 `core`, Cargo host tests, PL011 MMIO, Microkit SDF/IRQ notifications, GNU Make, QEMU AArch64.

---

### Task 1: Command parser and dispatcher

**Files:**

- Create: `vmos/qemu-smoke/Cargo.toml`
- Create: `vmos/qemu-smoke/src/console.rs`
- Modify: `vmos/qemu-smoke/src/main.rs`

**Step 1: Create the test-only crate entry and failing command tests**

Add an empty-workspace Cargo manifest with one binary at `src/main.rs`. Make
`main.rs` use `std` only under `cfg(test)` and declare `mod console`.

In `console.rs`, add tests with an in-memory output sink for these exact
responses:

```text
help       -> commands: help version ping echo <text> selftest stats
version    -> VMOS runtime console | ARM64 | seL4/Microkit | Rust
ping       -> pong
echo hello -> hello
echo       -> an empty output line
bad        -> ERROR: unknown command 'bad'; use 'help'
```

Also test leading/trailing/repeated ASCII whitespace and case sensitivity.

**Step 2: Run RED**

```shell
cargo test --manifest-path vmos/qemu-smoke/Cargo.toml
```

Expected: compilation fails because the parser and dispatcher do not exist.

**Step 3: Implement the minimal dispatcher**

Implement a borrowed `Command<'a>` enum, `parse_command(&str)`, and an
allocation-free output trait. `echo` keeps all text after its separating
whitespace, after trimming outer whitespace. Dispatch exact lowercase ASCII
tokens only.

**Step 4: Run GREEN**

```shell
cargo test --manifest-path vmos/qemu-smoke/Cargo.toml
```

Expected: all command tests pass.

**Step 5: Commit**

```shell
git add vmos/qemu-smoke
git commit -m '[beau 0007] (16)' -m 'add VMOS runtime console commands'
```

### Task 2: Line editor, self-test, and statistics

**Files:**

- Modify: `vmos/qemu-smoke/src/console.rs`

**Step 1: Write failing line-editor tests**

Add one focused test for each behavior:

- the startup banner and `vmos> ` prompt;
- printable input echo and execution on CR or LF;
- one execution for a CRLF pair;
- backspace and delete editing with terminal erase output;
- empty-line prompt refresh;
- rejection and recovery after a line exceeds 127 bytes;
- rejection of non-ASCII input;
- `stats` counts recognized, unknown, and overflowed commands;
- `selftest` produces `selftest: PASS`.

**Step 2: Run RED**

```shell
cargo test --manifest-path vmos/qemu-smoke/Cargo.toml
```

Expected: failures because `Console`, its fixed buffer, and statistics do not
exist.

**Step 3: Implement the minimal line editor**

Add `Console<W>` containing its output sink, `[u8; 128]`, current length,
discard/invalid flags, CRLF state, and three `u64` counters. Implement
`const fn new`, `start`, and `input`. All number formatting uses a fixed stack
buffer; no allocator is introduced.

The runtime self-test checks command classification, the line capacity
constant, and a small deterministic byte checksum before emitting one PASS or
FAIL line.

**Step 4: Run GREEN and formatting**

```shell
cargo test --manifest-path vmos/qemu-smoke/Cargo.toml
cargo fmt --manifest-path vmos/qemu-smoke/Cargo.toml -- --check
cargo clippy --manifest-path vmos/qemu-smoke/Cargo.toml -- -D warnings
```

Expected: all tests and lints pass.

**Step 5: Commit**

```shell
git add vmos/qemu-smoke/src/console.rs
git commit -m '[beau 0007] (17)' -m 'handle VMOS console input and statistics'
```

### Task 3: PL011 interrupt runtime and build integration

**Files:**

- Modify: `vmos/qemu-smoke/src/main.rs`
- Modify: `vmos/qemu-smoke/qemu-smoke.system`
- Modify: `Makefile.vmos`
- Modify: `vmos/tests/test_build_vmos.py`

**Step 1: Write failing integration-shape tests**

Extend the root Make tests to require:

- `make test` includes `cargo test --manifest-path vmos/qemu-smoke/Cargo.toml`;
- the smoke SDF contains physical `0x0900_0000`, uncached mapping, IRQ 33,
  channel ID 0, and level triggering;
- the production Rust source contains volatile MMIO, PL011 receive and timeout
  interrupt masks, and calls `microkit_irq_ack`.

**Step 2: Run RED**

```shell
python3 -m unittest vmos.tests.test_build_vmos -v
```

Expected: the new assertions fail against the output-only smoke PD.

**Step 3: Implement PL011 and Microkit notification handling**

Map the 4 KiB UART page uncached at virtual address `0x1000_0000` and declare
IRQ 33 as level-triggered ID 0. In Rust, use `read_volatile`/`write_volatile`
for PL011 `DR`, `FR`, `IMSC`, `MIS`, and `ICR` registers.

`init()` clears pending UART interrupts, enables RX and receive-timeout masks,
prints the console banner/prompt, and returns. `notified(0)` drains the RX FIFO,
feeds bytes to the static console, clears asserted UART interrupts, and calls
`microkit_irq_ack(0)`. Other channels print an error without acknowledging a
capability the PD does not own.

Add the smoke Cargo tests to the top-level `test` target, using a target
directory below `build/vmos`.

**Step 4: Run GREEN and package the image**

```shell
make -f Makefile.vmos test
make -f Makefile.vmos verify-source
make -f Makefile.vmos qemu-image
```

Expected: tests pass and Microkit packages `loader.img` with the UART mapping
and IRQ capability.

**Step 5: Commit**

```shell
git add Makefile.vmos vmos/qemu-smoke vmos/tests/test_build_vmos.py
git commit -m '[beau 0007] (18)' -m 'drive the VMOS console from the QEMU UART interrupt'
```

### Task 4: QEMU interaction validation and documentation

**Files:**

- Modify: `vmos/README.md`

**Step 1: Update user documentation**

Document the prompt, six commands, overflow behavior, successful startup line,
and QEMU exit sequence. State that the target is a diagnostic console and has
no reboot or arbitrary-memory command.

**Step 2: Run a real PTY session**

```shell
make -f Makefile.vmos qemu
```

After `vmos> ` appears, send:

```text
help
version
ping
echo runtime-ok
selftest
stats
```

Expected: each documented response is present, `selftest: PASS` is printed,
and the statistics reflect the executed commands with zero errors.

**Step 3: Run final regression checks**

```shell
make -f Makefile.vmos test
make -f Makefile.vmos verify-source
cargo fmt --manifest-path vmos/qemu-smoke/Cargo.toml -- --check
cargo clippy --manifest-path vmos/qemu-smoke/Cargo.toml -- -D warnings
git diff --check
git status --short
```

Expected: all checks pass; only the user's pre-existing `.gitignore` change and
untracked `AGENTS.md` remain outside intended commits.

**Step 4: Commit**

```shell
git add vmos/README.md
git commit -m '[beau 0007] (19)' -m 'document the VMOS runtime console'
```

