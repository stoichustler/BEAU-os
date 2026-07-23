# BEAU Zephyr Guest Shell Commands

This directory keeps the BEAU Zephyr guest validation shell sources that are
also installed in:

```text
zephyr/samples/subsys/shell/shell_module/src
```

The current source set is:

- `hcall.h` / `hcall.c`: common BEAU HVC IDs and wrappers for WDT, HVC IPC,
  and the AI scheduler advisor ABI.
- `beau_wdt.c`: BEAU VM watchdog heartbeat thread; successful kicks are silent
  while failed kicks are logged.
- `beau_ipc.c`: BEAU HVC IPC shell commands: `hipc status` and
  `hipc send <payload>`.
- `beau_ai_sched.c` / `beau_ai_sched.h`: VM0 AI scheduler advisor. It registers
  through `HC_AI_SCHED`, retains the boot-bound capability, then requests a
  snapshot every 100 ms.
- `beau_ai_model.h`: reviewed model configuration consumed by the advisor. The
  retained default is untrained and does not issue proposals.
- `beau_rpmsg.c`: OpenAMP `RPMSG_REMOTE` endpoint for the static VM0 <-> VM3
  remoteproc transport. It retains `rpmsg-raw` payload echo and publishes the
  dedicated `beau-rpmsg-peer` endpoint for `rpmsg status` and
  `rpmsg send <payload>` full-duplex validation.
- `beau_rpmsg.overlay`, `beau_rpmsg.conf`, `beau_rpmsg.cmake`: QEMU endpoint
  DT, configuration, and upstream OpenAMP/libmetal CMake integration.

When porting into a Zephyr shell sample, add `src/hcall.c`, `src/beau_wdt.c`,
`src/beau_ipc.c`, and `src/beau_ai_sched.c` to the application
`target_sources()` list. Keep `beau_ai_sched.h` and `beau_ai_model.h` beside
the source files.

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

`hipc send` requires a 1..192-byte payload. It uses the independent static
VM0 <-> VM2 HVC IPC ring, drains stale replies, sends a notify, and waits up to
1500 ms for the VM2 reply.
