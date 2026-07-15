# BEAU HYPERVISOR

```text
_________ _________  _____   ___ ___     ________   ________
\_____   \\_   ___/ /  _  \ /   |   \    \_____  \ /  _____/
 |   |  _/ |   ___)/  /_\  \\   |   /     /   |   \\____  \
 |______ \ |_____ \\___|___ \\_____/      \_____  //____  /
        \/       \/        \/                   \/      \/ (2026)
```

## Introduction

BEAU is a compact ARM64 hypervisor bring-up project for QEMU `virt` and rk356x
hardware. It runs at EL2 and focuses on a small, readable virtualization base
for mixed RTOS and Linux guests.

```text
┌──────────────────────────────┐
│ BEAU Hypervisor · ARM64 EL2  │
├──────────────┬───────────────┤
│ VM0 Zephyr   │ service VM    │
│ VM1 RT-Thread│ prelaunch VM  │
│ VM2 Linux    │ prelaunch VM  │
│ VM3 Linux    │ prelaunch VM  │
└──────────────┴───────────────┘
```

### Quick Run

```sh
./scripts/kick.sh --build
```

## Learning Path

Start with [walkthrough.md](sdk/beau/walkthrough.md) for the ARM64
implementation flow from EL2 entry through VM creation, vCPU entry/exit,
stage-2 memory, vGIC/vtimer virtualization, and console debugging.

## Source Base And License

- [LICENSE](LICENSE)
- [NOTICE.md](NOTICE.md)

---

Hustle Embedded OS.
