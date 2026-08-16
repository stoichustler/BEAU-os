# VMOS Runtime Console Design

## Scope

The QEMU smoke protection domain becomes an interactive runtime console for
the standalone ARM64 VMOS build. It accepts commands from the QEMU serial
terminal after the existing loader, seL4, CapDL initialiser, and Microkit
monitor have started.

All new functional code is Rust. Vendored Microkit and seL4 implementation
files remain unchanged. The console is intentionally a diagnostic shell, not a
general-purpose terminal, debugger, or privileged system-management service.

## Architecture

The console protection domain owns a single uncached mapping of the QEMU PL011
UART page at physical address `0x0900_0000` and IRQ 33. Its output continues to
use `microkit_dbg_putc`, so kernel, monitor, and console messages share the
existing seL4 debug-output path without a second UART transmitter.

```text
QEMU serial input
    |
    v
PL011 RX IRQ 33 --> Microkit notification channel 0
                        |
                        v
                 Rust line editor
                        |
                        v
                 command dispatcher --> seL4 debug output --> QEMU serial
```

`init()` configures PL011 receive and receive-timeout interrupts, prints the
banner and prompt, then returns to the Microkit event loop. `notified(0)` drains
the receive FIFO, acknowledges PL011 and Microkit interrupt state, and feeds
each byte to a fixed-size line editor. No polling loop or heap allocation is
used.

## Console behavior

The line editor accepts printable ASCII, carriage return or line feed, and
backspace/delete. Input is echoed. Empty lines only redraw the prompt. A line
longer than 127 bytes is rejected deterministically, cleared at newline, and
does not execute a truncated command.

The initial command set is:

- `help`: list commands and their syntax;
- `version`: report VMOS, ARM64, seL4/Microkit, and Rust console identity;
- `ping`: print `pong` as a minimal liveness check;
- `echo <text>`: reproduce the supplied text, including an empty argument;
- `selftest`: run deterministic parser, buffer, and arithmetic checks and
  print a single `PASS` or `FAIL` result;
- `stats`: report successfully executed, unknown, and overflowed line counts.

Commands are exact, case-sensitive ASCII tokens. Leading/trailing whitespace
and repeated spaces between command and argument are accepted. An unknown
command prints an error followed by a hint to use `help`.

## Code structure

`vmos/qemu-smoke` becomes a small Cargo crate that can compile in two modes:

- host tests use `std` only through the Rust test harness and exercise the
  line editor and dispatcher without QEMU;
- the AArch64 production binary remains `no_std`/`no_main`, links against the
  existing `libmicrokit.a`, and implements volatile PL011 access.

Hardware-independent parsing and state live in `src/console.rs`. The runtime
adapter in `src/main.rs` implements output through the Microkit debug ABI and
input through volatile PL011 registers. The existing direct `rustc`/linker
build remains usable and does not add a network dependency.

## Error handling

The IRQ handler drains all available UART bytes before returning. Unexpected
notification channels are reported and acknowledged. PL011 interrupt status
is cleared before calling `microkit_irq_ack(0)`.

No command can write arbitrary memory or invoke a reboot. Buffer overflow,
invalid UTF-8/non-ASCII bytes, and unknown commands are handled locally and
return to a fresh prompt. A Rust panic remains a terminal spin because there is
no allocator, unwinder, or safe recovery context in the protection domain.

## Verification

1. Rust host unit tests cover every command, whitespace, unknown commands,
   editing, CR/LF handling, overflow recovery, and statistics.
2. Existing Make tests verify that the QEMU image build uses Cargo tests and
   contains the PL011 mapping and IRQ configuration.
3. `make -f Makefile.vmos test` and `verify-source` remain green.
4. An actual `make -f Makefile.vmos qemu` PTY session sends all six commands
   and checks their responses after confirming EL2, seL4, monitor, and console
   startup messages.

