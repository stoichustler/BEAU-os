# BEAU Zephyr Guest Shell Commands

This directory keeps the BEAU Zephyr guest validation shell sources that are
also installed in:

```text
zephyr/samples/subsys/shell/shell_module/src
```

The current source set is:

- `hcall.h` / `hcall.c`: common BEAU HVC IDs, IPC ABI structs, and HVC helpers.
- `beau_wdt.c`: BEAU VM watchdog heartbeat validation thread.
- `beau_ipc.c`: BEAU static IPC query, notify, ack, and ping shell commands.
- `beau_rpmsg.c`: OpenAMP `RPMSG_REMOTE` endpoint for the static VM0 <-> VM3
  remoteproc transport. It retains `rpmsg-raw` payload echo and publishes the
  dedicated `beau-rpmsg-peer` endpoint for `rpmsg status` and
  `rpmsg send <payload>` full-duplex validation.
- `beau_rpmsg.overlay`, `beau_rpmsg.conf`, `beau_rpmsg.cmake`: QEMU endpoint
  DT, configuration, and upstream OpenAMP/libmetal CMake integration.

When porting into a Zephyr shell sample, add `src/hcall.c`, `src/beau_wdt.c`,
and `src/beau_ipc.c` to the application `target_sources()` list.

For static remoteproc/RPMsg, apply `beau_rpmsg.overlay` and
`beau_rpmsg.conf`, then include `beau_rpmsg.cmake` after the application's
`find_package(Zephyr)`. The application must provide the upstream OpenAMP and
libmetal sources at `modules/lib/open-amp` and `modules/hal/libmetal`. The
transport owns the resource table and standard RPMsg endpoint only; EL2 owns
the shared-memory mapping and doorbell routing.

The Linux peer must attach remoteproc and publish READY before `rpmsg send` is
available. A successful command validates the ACK sequence and payload; an
unready peer, invalid payload, send error, ACK mismatch, or timeout fails the
command without changing the existing `rpmsg-raw` behavior.
