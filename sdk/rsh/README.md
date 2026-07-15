# RT-Thread Coordinated STR Agent

`beau_str.c` is the retained source for the BEAU QEMU VM1 suspend-to-RAM
agent. The integration source lives at:

```text
~/nebula/rt-thread/bsp/qemu-virt64-aarch64/applications/beau_str.c
```

The QEMU BSP must enable these RT-Thread PM options:

```text
CONFIG_RT_USING_PM=y
CONFIG_PM_ENABLE_SUSPEND_SLEEP_MODE=y
```

The agent negotiates the versioned `HC_PM_CONTROL` ABI, receives prepare
events on SPI 60, enters the native RT-Thread device suspend/resume path, and
acknowledges the epoch only after native resume callbacks finish. On SMP
builds, the application overrides the weak secondary idle hook so the AP can
enter PSCI `CPU_OFF`; the BSP restores it with `CPU_ON` after
`SYSTEM_SUSPEND` returns.

Build the retained image with the RT-Thread CI AArch64 bare-metal toolchain:

```sh
cd ~/nebula/rt-thread/bsp/qemu-virt64-aarch64
RTT_EXEC_PATH=/path/to/aarch64-none-elf/bin scons -j"$(nproc)"
cp rtthread.bin ~/nebula/beau/sdk/image/rtthread.bin
```

The committed `sdk/image/rtthread.bin` was built from the matching retained
source with RT-Thread 5.3.0 and an `aarch64-none-elf` toolchain.
