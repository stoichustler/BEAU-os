# BEAU ARM64 DDB

BEAU DDB is a read-only EL2 debugger built into the full ARM64 image. The Host
shell enters it through:

```text
ddb <passwd>
```

The shell masks the password argument and excludes the command from history.
The credential itself is managed out of band and is represented in the image
only by its SHA-256 digest.

Developers may also call `arm64_ddb_break()` from Host code. The reserved
`BRK #0x0DDB` immediate identifies a manual break, while `BRK #0x0DDC`
identifies a panic. Guest exceptions and all other Host traps retain their
normal handling.

An ARM64 Host `panic()` persists its coredump and then enters DDB through the
dedicated `BRK #0x0DDC` panic entry. This path does not use Shell authentication.
The banner reports `reason:panic`; `continue` returns to the panic path's
permanent halt instead of resuming normal execution.

## FreeBSD Design Origin

The BEAU ARM64 DDB mechanism is derived from the FreeBSD ARM64 KDB/DDB design.
Its primary design references are:

- `sys/arm64/arm64/trap.c` and `sys/arm64/include/db_machdep.h` for
  trap-driven debugger entry and breakpoint-PC advancement;
- `sys/arm64/arm64/db_interface.c` for register exposure and fault-recoverable
  debugger memory inspection;
- `sys/arm64/arm64/db_trace.c` for frame-pointer-based ARM64 stack unwinding.
- `sys/kern/kern_shutdown.c` and `sys/sys/kdb.h` for debugger entry with the
  explicit `KDB_WHY_PANIC` reason after panic reporting.

No FreeBSD source file is incorporated. The mechanism is reimplemented for
BEAU Host EL2 with Guest isolation, an authenticated shell entry, a reserved
Host BRK immediate, read-only Normal-memory probing, PL011 ownership, and
bounded SMP sampling.

Commands:

```text
help
regs
bt
x <hex-address> [1-256 byte-count]
symbol <hex-address>
cpu
continue
c
reboot
```

`x` accepts only readable Normal-memory mappings and uses a recoverable
single-byte probe. It rejects MMIO and unmapped addresses. `bt` follows at most
32 validated frame-pointer records inside the current known EL2 stack. `cpu`
uses a bounded SMP sample and does not park remote CPUs.

The debugger is intentionally read-only. It does not provide memory writes,
software or hardware breakpoints, watchpoints, instruction stepping, or a
stop-the-world mode. Other pCPUs continue to run, so memory and CPU output is a
diagnostic sample rather than a globally consistent snapshot.
