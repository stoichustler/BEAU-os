# BEAU Zephyr Guest Shell Commands

This directory keeps the BEAU Zephyr guest validation shell sources that are
also installed in:

```
/home/beau/nebula/zephyr/samples/subsys/shell/shell_module/src
```

The current source set is:

- `hcall.h` / `hcall.c`: common BEAU HVC IDs, IPC ABI structs, and HVC helpers.
- `beau_wdt.c`: BEAU VM watchdog heartbeat validation thread.
- `beau_ipc.c`: BEAU static IPC query, notify, ack, and ping shell commands.

When porting into a Zephyr shell sample, add `src/hcall.c`, `src/beau_wdt.c`,
and `src/beau_ipc.c` to the application `target_sources()` list.
