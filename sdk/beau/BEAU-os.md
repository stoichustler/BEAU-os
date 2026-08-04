# BEAU OS 整体框架与源码学习指南

> 文档基线：2026-07-16 当前工作区，`main` 分支，BEAU OS `0.0.7`。
>
> 本文以 [`sdk/sdk.md`](sdk/sdk.md) 为最高约束，并以当前源码、
> `platform.dts`、Kconfig、构建脚本和 QEMU 回归脚本互相校验。当前工作区包含
> 尚未提交的电源管理改动，因此本文描述的是工作区现状，不只是 `HEAD` 提交。

BEAU OS 是一个运行在 ARM64 EL2 的静态分区型 Hypervisor。它面向 QEMU `virt`
和 rk356x，重点不是提供通用云虚拟化，而是用较小且可追踪的代码，承载 RTOS 与
Linux 混合客体，并把 CPU、内存、IRQ、DMA、设备和电源状态的所有权边界写清楚。

本文同时承担四类用途：

- **教程**：从构建、启动到看懂第一条 VM 启动链。
- **解释**：说明 BEAU 为什么采用静态 DTS、Stage-2 trap、vGIC、SMMU 和分区调度。
- **参考**：逐模块列出能力、关键结构、关键函数、状态机和代码路径。
- **操作指南**：说明如何增加 VM、设备、Hypercall，如何调试启动、IRQ、virtio 和 STR。

## 1. 阅读约定

### 1.1 实现状态标记

| 标记 | 含义 |
|---|---|
| 当前路径 | 默认 QEMU/rk356x 配置会编译或运行的代码 |
| 可选路径 | 源码存在，需要 Kconfig、DTS 或硬件能力才能启用 |
| 保留路径 | 为移植或兼容保留，当前 ARM64 静态平台不使用 |

### 1.2 地址术语

| 术语 | 含义 |
|---|---|
| VA/HVA | EL2 主机虚拟地址 |
| PA/HPA | 主机物理地址 |
| IPA/GPA | 客体看到的中间物理地址 |
| GVA | 客体虚拟地址 |
| Stage-1 | EL2 自身 VA 到 PA 的转换 |
| Stage-2 | 客体 IPA 到 PA 的转换，也是 VM CPU 内存隔离边界 |

当前 ARM64 静态平台强制客体 RAM 使用 `IPA == HPA` 的 1:1 Stage-2 映射。
这让 bring-up 和日志更直观，但不是通用虚拟化内存模型。

## 2. 五分钟建立整体模型

```text
firmware / QEMU / U-Boot
        |
        v
+--------------------------- BEAU EL2 -----------------------------+
|                                                                  |
|  embedded platform.dtb                                           |
|    |                                                             |
|    +--> host: CPU/RAM/GIC/ITS/UART/PCIe/SMMU/PM                  |
|    +--> policy: VM RAM/image/vCPU affinity/device ownership      |
|    |                                                             |
|    v                                                             |
|  arch/arm64                        core                          |
|  - EL2 entry/vector                - VM/vCPU lifecycle           |
|  - host Stage-1                    - per-pCPU scheduler          |
|  - guest Stage-2                   - timer/softirq/event         |
|  - GIC/vGIC/vITS/vtimer            - IRQ/ptdev/hypercall         |
|  - SMMUv3/PSCI/SVE                 - watchdog/PM/trace           |
|           |                              |                       |
|           +---------------+--------------+                       |
|                           v                                      |
|                    sdk/bsp device/control plane                  |
|        console, shell, DTS, vFDT, vPL011, virtio, vPCI, PM       |
+---------------------------+--------------------------------------+
                            |
          +-----------------+------------------+
          |                 |                  |
          v                 v                  v
       RTOS VM          backend Linux      frontend Linux
     vPL011/IPC         KBE/HVC/vPCI       virtio-console/proxy
```

从运行角度看，BEAU 只有四条必须先记住的主线：

1. **CPU 执行**：vCPU 是固定绑定到某个 pCPU 的调度线程；切入时装载
   VTTBR、EL1 系统寄存器、vGIC、vtimer，切出时反向保存。
2. **CPU 内存访问**：客体 RAM 由 Stage-2 映射；设备 IPA 故意不映射，访问时
   Data Abort 到 EL2，再按 MMIO 地址分派给 vGIC、vPL011、virtio 或 vPCI。
3. **设备 DMA**：直通设备必须先把 StreamID 绑定到 VM 的 SMMUv3 domain，
   再允许客体看到配置空间和 BAR。
4. **中断**：物理 GIC/ITS 负责宿主中断，软件 vGIC/vITS 维护客体状态，最终把
   virtual INTID 放入硬件 List Register，再 `ERET` 给客体。

## 3. 设计约束与核心不变量

这些规则来自 `sdk/sdk.md` 和实现中的设计注释，是理解代码顺序的关键。

### 3.1 静态策略先于运行时对象

`arch/arm64/platform/<platform>/platform.dts` 是平台静态事实源。DTS 先经过
`sdk/bsp/arm64/platform_dts.c` 完整校验，再被归一化为普通 C 结构。后续模块不再
各自解析 FDT。

```text
platform.dts
    |
    v
validate + normalize
    |
    +--> beau_config / platform_info
    +--> vm_configs[]
    +--> bare_boot_options[]
    +--> sched_platform_config
    +--> passthrough / IPC / PM policy
```

配置缺失、内存重叠、CPU pool 冲突、设备所有者冲突或无效预算都会在客体运行前
失败，而不是把错误带入运行时。

### 3.2 CPU 与 DMA 必须使用同一所有权模型

VM 的 CPU 访问受 Stage-2 控制。直通设备 DMA 应复用该 VM 的 Stage-2 根表建立
SMMUv3 domain。设备对 VM 可见之前，StreamID 必须已完成 STE 编程和 CMDQ 同步。

代码中保留了硬件不支持 SMMU Stage-2 时的 bypass 兼容分支。该分支适合 bring-up，
不应视为满足严格 DMA 隔离的安全模式。

量产平台应在 `/beau,platform` 中设置 `beau,dma-isolation = "required"`。
只要 DTS 配置了直通 PCI 设备，SMMUv3 必须在 VM 创建前完成 Stage-2 capability
校验并进入 assignment-ready 状态；每个 PCI 配置还必须显式给出与静态
`/passthrough` owner policy 匹配的 `beau,stream-id`。缺失 SMMU、能力不符、StreamID
策略不匹配或同步失败都会停止启动，而不是隐藏设备后继续运行。严格模式禁止
`beau,optional` PTDEV。未列入静态 policy 的 StreamID 保持 SMMU ABORT 默认状态。

直通设备的发布和回收顺序为：

```text
BME=0 + IRQ masked
    -> S2 STE body/valid publish + CMD_SYNC
    -> vPCI config/BAR/MSI routing visible
    -> Guest may enable BME

teardown: BME=0 -> BAR hidden -> ABORT STE + CMD_SYNC -> release owner
```

### 3.3 发布顺序必须 fail closed

典型顺序如下：

```text
validate policy
    |
    v
build private state
    |
    v
program and synchronize hardware isolation
    |
    v
publish software ownership / guest visibility
```

撤销时反向执行：先停止客体可见性和中断，再撤销 DMA，最后释放软件所有权。

### 3.4 控制台按客体类型分流

- RTOS 使用 vPL011，适合简单 MMIO 串口和早期启动。
- Linux 优先使用内建 virtio-console，避免大量 PL011 寄存器轮询导致 VM-exit。
- 两种前端最终都进入 BEAU 的 per-VM console ring，由 `vcon` 和宿主 shell 消费。

### 3.5 安全导向的 C 规则

[`sdk/beau/ISO26262.md`](sdk/beau/ISO26262.md) 规定：外部输入必须校验，隔离错误
必须关闭访问，所有权转移必须显式，清理必须确定，硬件等待必须有界，诊断应包含
VMID、vCPUID、StreamID、BDF、IRQ 或地址等对象身份。

## 4. 仓库与构建框架

### 4.1 顶层目录

| 目录 | 角色 | 主要输出/消费者 |
|---|---|---|
| `arch/arm64` | EL2 启动、CPU、MMU、GIC、SMMU、客体虚拟化 | `host.a`、`vp-base.a` |
| `core` | 跨架构 VM/vCPU、调度、timer、IRQ、PM、WDT | `core.a` |
| `include` | 公共、架构、BSP、ABI 头文件 | 所有 C/汇编模块 |
| `lib` | libc 子集、页表辅助、libfdt、SHA/HKDF | `core.a` |
| `sdk/bsp` | 平台解析、设备模型、shell、console、vPCI、virtio | `libbsp.a` |
| `sdk/imgs` | 稳定的 Zephyr、LK、Linux、DTB、initramfs | 链接或 QEMU loader |
| `sdk/kbe` | Linux 客体 KBE 驱动保留副本 | 移植进 Linux 内核树 |
| `sdk/zsh` | Zephyr shell/HVC/WDT/IPC 验证代码 | 移植进 Zephyr sample |
| `sdk/ube` | ACRN userspace device model 保留副本 | 当前 BEAU 不使用 |
| `scripts` | 配置生成、链接、符号、QEMU 启动和回归 | 开发/验证工具 |

### 4.2 构建产物

```text
Kconfig + platform Bconfig
        |
        +--> out/<platform>_out/configs/.config
        +--> generated/autoconf.h
        +--> bconfig.h
sdk/imgs
        |
        +--> bimage.h
        |
platform.dts --cpp/dtc--> platform.dtb --incbin--+
guest images / guest DTBs -----------------------+--> platform.o
                                                   |
core/*.c + lib + boot/BSP common ------------------+--> core.a
arch/arm64 host -----------------------------------+--> host.a
arch/arm64 guest + platform.o ---------------------+--> vp-base.a
sdk/bsp -------------------------------------------+--> libbsp.a
                                                   |
                                                   v
                                              beau.out
                                              beau.debug.out
                                              beau.debug.bin
```

`Makefile` 默认只支持：

```text
ARCH=arm64 PLATFORM=qemu
ARCH=arm64 PLATFORM=rk356x
```

关键生成步骤：

- `scripts/checkconfig.py`：要求每个 `Bconfig` 覆盖所有 Kconfig symbol，并检查平台一致性。
- `scripts/gen_offset_header.py`：从 ELF 绝对符号生成 C/汇编共享结构偏移，依赖
  `pyelftools`。
- `scripts/genld.sh`：把配置值代入 `arch/lds/link_ram.ld.in`。
- `scripts/gen_symtab.py`：从未剥离 ELF 生成运行时符号表，供 `symtab` 和栈回溯使用。
- `platform.S`：用 `.incbin` 把平台 DTB、RTOS image 和客体 DTB 嵌入镜像。

### 4.3 构建与启动教程

前置条件：

- GNU Make、Python 3、`pyelftools`、`dtc`。
- `aarch64-none-elf-gcc` 交叉工具链。
- QEMU ARM64，且机器模型支持 GICv3、ITS 和 SMMUv3。

构建并启动 QEMU：

```sh
python3 scripts/kick.py --build
```

脚本等价地执行：

```sh
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- clean
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- Bconfig
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j"$(getconf _NPROCESSORS_ONLN)"
```

再以如下关键参数启动 QEMU：

```text
-machine virt,virtualization=on,gic-version=3,its=on,iommu=smmuv3
-global arm-smmuv3.stage=2
-cpu cortex-a57 -smp 8 -m 1536M
-kernel out/qemu_out/beau.debug.out
```

Linux VM1/VM2/VM3 image 和共享 initramfs 由 QEMU `loader` 放入 DTS 约定的
staging 地址；Zephyr、LK 和客体 DTB 则直接嵌入 BEAU 镜像。

预期第一个可见结果是：

```text
BEAU OS (HYPERVISOR) v0.0.7 ...
...
console:\>
```

## 5. 当前平台拓扑

### 5.1 QEMU `virt`

主机：8 pCPU、1.5 GiB RAM、GICv3+ITS、PL011、PCIe ECAM、SMMUv3。

| VM | 角色 | RAM | vCPU 到 pCPU | 客体入口 | 控制台/设备 |
|---|---|---|---|---|---|
| VM0 Zephyr | service VM、RTBE 预留 | `0x42000000`, 128 MiB | `0->0`, `1->4` | `0x42000000` | vPL011、IPC、WDT |
| VM1 Linux-1 | secure/KBE backend | `0x60000000`, 256 MiB | `0->1`, `1->5`, `2->6`, `3->7` | `0x60200000` | Trusty client、KBE、vPCI 直通 |
| VM2 Linux-2 | prelaunch frontend | `0x80000000`, 256 MiB | `0->2`, `1->4`, `2->5`, `3->6` | `0x80200000` | virtio-console、virtio proxy 前端 |
| VM3 Linux-3 | prelaunch frontend | `0x90000000`, 256 MiB | `0->3`, `1->5`, `2->6`, `3->7` | `0x90200000` | virtio-console、virtio proxy 前端 |

这里的 `cpu-affinity` 是有序数组，不是无序 bitmap。数组第 N 项就是 vCPU N 的
pCPU。运行时仍保留 bitmap，用于集合、共享和权限检查。

QEMU 的 CPU pool：

```text
pCPU0..3  exclusive  BVT  各承载一个 VM 的 vcpu0
pCPU4..7  shared     CBS  period=2000us
VM0 secondary        CBS  budget=800us
VM1/VM2/VM3 secondary CBS budget=600us
```

`strict-placement` 在 DTS 解析时验证：pool 不重叠且覆盖全部 pCPU，secondary vCPU
不能进入 exclusive pool，共享 pCPU 最多承载 3 个 VM/3 个 vCPU。

QEMU virtio-proxy 当前拓扑：

```text
VM2/VM3 standard virtio frontend
        |
        | virtio-mmio + descriptor chain
        v
BEAU virtio-proxy transport
        |
        | HC_VIRTIO_PROXY_BACKEND ABI v3
        v
VM1 Linux KBE backend
```

| 设备 | Virtio ID | VM2/VM3 MMIO | 队列 | VM1 后端 |
|---|---:|---:|---|---|
| net | 1 | `0x0a001a00` / `0x0a000a00` | RX/TX, 256 entries | VM1 uplink netdev |
| blk | 2 | `0x0a001600` / `0x0a000600` | 1, 64 entries | 每 frontend 1 MiB RAM disk |
| rng | 4 | `0x0a001400` / `0x0a000400` | 1, 64 entries | Linux RNG |
| fs | 26 | `0x0a001200` / `0x0a000200` | hiprio/request, 64 entries | 隔离的 `/var/beau/vm2`、`vm3` |
| i2c | 34 | `0x0a001800` / `0x0a000800` | 1, 64 entries | 每 frontend 独立 0x50 EEPROM |

### 5.2 rk356x

rk356x 使用同一框架，但当前 DTS 是较小的 bring-up 配置：

| VM | 角色 | RAM | pCPU | 入口 |
|---|---|---|---:|---|
| VM0 Zephyr | service RTOS | `0x12000000`, 96 MiB | 0 | `0x12000000` |
| VM1 LK | prelaunch RTOS | `0x10000000`, 32 MiB | 1 | `0x10100000` |
| VM2 Linux | prelaunch Linux | `0x18000000`, 128 MiB | 2 | `0x18080000` |

当前 rk356x DTS 未启用 PCIe/SMMU/virtio-proxy，PM policy 为 disabled；所有有效
pCPU 使用 BVT。rk356x 的验证以硬件手工 bring-up 为主，QEMU 回归只覆盖 QEMU 平台。

## 6. 启动主链

### 6.1 EL2 汇编入口

入口：`arch/arm64/boot/entry.S::_start`。

```text
firmware/QEMU
    |
    v
_start
    +--> mask DAIF
    +--> require CurrentEL == EL2
    +--> select SP_EL2
    +--> VBAR_EL2 = arm64_exception_vectors
    +--> clear BSS
    +--> use temporary boot stack
    +--> x0=MPIDR, x1=DTB PA
    v
init_primary_pcpu()
```

`CONFIG_AARCH64_IMAGE_HEADER` 在 rk356x 默认启用，使镜像可由 U-Boot `booti`
识别。重定位框架存在，但 `relocate()` 仍为空，默认 `CONFIG_RELOC=n`。

### 6.2 BSP 全局初始化

入口：`arch/arm64/init.c::init_primary_pcpu()`。

```text
per-CPU MPIDR / active bitmap / stack canary
    |
    v
arm64_platform_init()
    - copy embedded platform DTB
    - parse host geometry and board policy
    |
    v
early console
    |
    v
arm64_platform_init_post_console()
    - parse vm_configs[]
    - install PM and wake policy
    |
    v
boot info -> EL2 Stage-1 -> cache -> serial -> early GIC
    |
    v
switch temporary stack to BSP per-CPU stack
```

### 6.3 BSP/AP 汇合初始化

`init_pcpu_comm_post()` 在每个 pCPU 上执行：

```text
BSP-only: ITS -> SMMU -> PCI discovery
    |
all pCPU: IRQ -> SMP call -> timer -> ptdev -> scheduler
    |
BSP-only: shell and console timer
    |
mark pCPU RUNNING
    |
BSP starts APs through PSCI CPU_ON
    |
wait until every AP scheduler is ready
    |
BSP: cpufreq -> rttest -> shell -> VM watchdog
    |
release VM launch barrier
    |
launch_vms() -> run_idle_thread()
```

AP 从 `_start_secondary_psci` 进入，使用 PSCI context 中的 per-CPU stack，启用 BSP
已创建的 EL2 Stage-1 页表，然后进入同一个 common post 路径。

## 7. VM 创建、装载与启动

### 7.1 配置生成

`sdk/bsp/arm64/platform_dts.c` 解析 `/vm`、`generic` 和每个 `vm@N`，填充：

- `vm_configs[N]`：名称、load order、OS family、guest flags、调度参数。
- `arch_vm_config`：RAM、vGIC、vITS、UART、virtio-console、virtio-proxy、SVE。
- `cpu_affinity_order[]` 和集合 bitmap。
- boot module tag、源地址、load/entry 地址。
- PCI 设备的 physical/virtual BDF、BAR 和 optional 属性。

### 7.2 Ready-first 启动

入口：`core/vm.c::launch_vms()`。

```text
all pCPU scheduler instances ready
    |
    +--> BSP clears prior Service VM publication
    +--> release VM launch barrier
    |
    v
for each configured service/prelaunch VM whose vCPU0 belongs to this pCPU
    |
    +--> create_vm()
    |     - init common object/locks
    |     - arch_init_vm()
    |     - create_vcpu() in authored affinity order
    |     - VM_CREATED
    |     - Service VM pointer is release-published only after this point
    |
    +--> PREPARING: init_vm_boot_info() + prepare_os_image()
    |     - failure tears down the partial VM and returns to VM_POWERED_OFF
    |
    v
start this VM's BSP immediately
    - only VCPU_INIT may transition to VCPU_RUNNING
    - launch denial returns the VM to VM_CREATED
    -> VM_RUNNING
```

每个 pCPU 仅处理 `cpu_affinity_order[0]` 属于自己的静态 VM。同一 pCPU 仍按配置表顺序
prepare 和启动，不同 pCPU 可并行推进；任意 VM 的失败不会阻塞无关 VM。Service VM 指针不是
预分配对象地址，而是在其锁、vPCI 和 vCPU 基础状态完整后 release-publish；消费者必须把空指针
视为可重试的未就绪状态。跨 VM 服务依赖仍由既有协议处理，本路径不根据 VM ID 或镜像装载顺序
推断依赖。

### 7.3 `arch_init_vm()` 的隔离顺序

入口：`arch/arm64/guest/vm.c::arch_init_vm()`。

```text
1. init_stage2_identity_map()
2. arm64_vgicv3_init_vm()
3. arm64_vipc_init_vm()
4. init vPL011 or virtio-console
5. init virtio-proxy devices when configured
6. init_vpci() when PCI devices exist
7. register MMIO trap ranges
8. build synthetic service VM vFDT when no external DTB is configured
```

先建立 Stage-2 再创建设备，保证虚拟设备注册时已经有明确的 RAM 和 trap 边界。
`VM_CREATING` 期间的 VIPC、remoteproc 等首建图尚未被任何 vCPU VTTBR 或 SMMU DMA
domain 使用，因此仅完成页表写回；进入 `VM_CREATED` 后的每次 Stage-2 更新都必须完成
CPU VMID TLBI 与 SMMU CMD_SYNC，失败保持 fail-closed。

### 7.4 镜像装载

当前默认是 raw image：

```text
init_vm_boot_info()
    |
    +--> locate kernel/ramdisk/FDT module by tag
    v
prepare_os_image()
    |
    v
rawimage_loader()
    +--> validate kernel range and Linux alignment
    +--> validate initramfs range and overlap
    +--> choose FDT GPA without overlap
    +--> copy_to_gpa(kernel/initramfs)
    +--> record kernel_entry_addr
    v
load FDT module
```

所有目标地址都必须落在 VM 的 RAM window。Linux kernel、initramfs 与 FDT 之间也
必须无重叠。bzImage 和 ELF loader 是可选/保留路径，默认 ARM64 Bconfig 不启用。

### 7.5 BSP vCPU 初始寄存器

`start_vm()` 调用 `arch_vm_prepare_bsp()`，后者设置：

- `ELR_EL2` 对应的客体入口地址。
- `x0` 为初始 context/MPIDR 约定。
- `x1` 或 context 参数携带客体 FDT GPA。
- `SPSR_EL2` 选择 AArch64 EL1h 并设置初始 DAIF。

只启动 vCPU0。Linux/Zephyr 的 secondary vCPU 由客体 PSCI `CPU_ON` 触发。

### 7.6 Linux Guest 崩溃通知

Linux `drivers/virt/beau/crash.c` 在 panic notifier 与 ARM64 `DIE_OOPS` notifier
中构造固定 320-byte 的 V2 `beau_vm_crash_report`，并通过 `HC_VM_CRASH_REPORT`
向 BEAU Host 通知。报文包含 ABI magic/version/size、事件类型、
vCPU、序号、PC、错误码、触发进程/线程、寄存器快照及最多 16 个原始 guest stack PC，
并含最多 95 个 ASCII 诊断字符。

```text
Linux panic / DIE_OOPS
    -> one versioned fixed-size HVC report per event kind
    -> validate guest GPA and ABI, then sanitize and checksum
    -> LOG_ERR host log ring (visible through dmesg)
    -> per-VM eight-entry retained history ring
    -> crash <vmid> / crash <vmid> erase
```

Host 根据 HVC 发起者确定 VM ID，不接受客体提供的 VM 身份。报文
必须含有一致的序号寄存器参数、完整 ABI 和 NUL 结尾的消息；静态 VM
以外、无效 GPA 或无效报文均被拒绝。dmesg 仅记录简洁的 KE 通知；完整 task、寄存器和原始
guest stack 诊断只由 `crash <vmid>` 输出。每个 VM 保留最近八条可校验的
崩溃记录：一致的 HVC 重试不消耗新槽，而新事件仅在缓存已满时淘汰最早记录。
`crash <vmid>` 按捕获顺序显示 guest history 与已有 EL2 guest-fault 信息，`erase` 清除两者。

## 8. vCPU、上下文切换与 VM-exit

### 8.1 vCPU 是调度线程

`core/vcpu.c::create_vcpu()`：

```text
assign vcpu_id by creation order
    |
bind to one per-pCPU sched_ctl
    |
build thread_object and EL2 stack frame
    |
initialize events and architecture state
    |
VCPU_INIT
```

`launch_vcpu()` 原子地把状态改成 `VCPU_RUNNING`，再 `wake_thread()`。状态转换受到
`vcpu_transition_allowed()` 限制，避免随意跨越生命周期阶段。

```text
OFFLINE -> INIT -> RUNNING -> PAUSED -> INIT/OFFLINE
                    |
                    +--> POWERED_OFF -> RUNNING/PAUSED
```

### 8.2 切入与切出

`arch_context_switch_in()` 调用 `load_vcpu()`：

- 安装 `VTCR_EL2`、`VTTBR_EL2`，选中 VM Stage-2。
- 恢复 EL1 系统寄存器。
- 按 vMPU policy 装载 SVE。
- 恢复 vtimer 和 vGIC List Register 状态。

`arch_context_switch_out()` 调用 `unload_vcpu()`，按相反顺序保存并清空
`HCR_EL2/VTTBR_EL2`，防止下一线程继承客体状态。

### 8.3 进入客体

```text
arch_vcpu_thread()
    |
    +--> process pending exception/IRQ requests
    +--> arm64_run_vcpu(vcpu->arch)
    |       copy durable regs to temporary EL2 stack frame
    v
vcpu_exit_return
    +--> ELR_EL2/SPSR_EL2/SP_EL1/GPR
    v
ERET -> guest EL1
```

持久寄存器保存在 `vcpu->arch.regs`，汇编入口/退出使用临时 `cpu_regs` 栈帧。这样
调度线程栈与客体寄存器所有权不会混在一起。

### 8.4 同步异常分派

入口：`arch/arm64/guest/vcpu_exit.c::vcpu_exit_handler()`。

| ESR exception class | 处理 |
|---|---|
| Data Abort from lower EL | 构造 MMIO ioreq 并调用 `emulate_io()` |
| Instruction Abort | 记录 IPA/ESR/ELR 并输出 VM 栈，不当作 MMIO |
| HVC64 | ACRN Hypercall 或 PSCI |
| SMC64 | PSCI |
| trapped sysreg | vMPU/vSVE、vGIC CPU interface、SGI、vtimer |
| SVE trap | vMPU 拒绝未授权 SVE |
| WFI/WFE | 检查 pending vIRQ/vtimer，再决定是否 yield |
| SError | 记录异步错误上下文并暂停 vCPU |

处理失败会暂停当前 vCPU。成功后仍会检查 host reschedule、处理 pending request、
同步 vtimer/vGIC，并把最终被选中 vCPU 的寄存器写回 trap frame。

### 8.5 物理 IRQ 退出

`dispatch_vcpu_irq()` 的顺序：

```text
save guest regs
    -> host IRQ acknowledge/handler
    -> softirq
    -> optional schedule
    -> process selected vCPU requests
    -> poll/update vtimer
    -> flush vGIC state
    -> restore selected guest regs
    -> ERET
```

如果当前客体已经有可见 pending vIRQ，代码会保留一个有限的返回窗口，让 Linux
退出 WFI idle path 并打开 PSTATE.I，避免 host tick 把它反复切走而无法处理中断。

## 9. 内存管理模块

### 9.1 EL2 Stage-1

**定位**：`arch/arm64/mmu.c` 提供 ARM64 descriptor；`core/mmu.c` 提供通用页表
walker、页分配、拆大页和增删映射。

**能力**：

- 为 Hypervisor image、RAM、平台 MMIO 建立 EL2 映射。
- 主核创建页表，AP 复用并启用。
- 提供 cache/TLB flush 与页表池统计。

**原理**：当前 host 采用易于 bring-up 的 identity map。正常 RAM 与 device MMIO
使用不同内存属性。

`arch/arm64/copy.S` 提供 ARM64 的 `arch_memcpy()`、
`arch_memcpy_backwards()` 与 `arch_memset()`。其 forward copy 适配自 OpenBSD
ARM64 `kcopy()` 的普通内核复制循环，并保留 OpenBSD copyright 和许可；BEAU 不移植
OpenBSD 的 `copyin/copyout`、user address check 或 PCB onfault ABI。`memcpy`、
`memset` 与 `memmove` 的 C 接口是稳定语义契约；`arch_*` 是可替换的 ARM64 后端，
不得把接口收窄为对齐地址。调用者必须传入已验证的 normal-memory 范围；汇编先以
字节访问使端点对齐，只有两端都 8-byte 对齐才使用宽访问。重叠区间由 `memmove()`
选择逆向 copy，汇编不访问 guest VA 或 MMIO。

EL2 安全能力在 BSP 上先采集 PAC、BTI 和 RNDR，再与每个已启动 pCPU 的能力取交集。
启用 `CONFIG_ARM64_PTRAUTH` 时，BEAU 只有在所有在线 pCPU 同时具备 PAC 与 RNDR、PAC
算法字段 `APA/API/APA3` 完全一致，且
强随机源已发布后，才为每个 host scheduler thread 生成独立 APIA key，并在进入 idle 的
不返回边界设置 `SCTLR_EL2.EnIA`。调度汇编在恢复已运行线程的 LR 前装载该线程 key 并
认证返回地址；能力或熵条件不足时 PAC 指令保持 HINT fallback，系统继续以未启用 PAC
的路径运行。访客的 ID 寄存器始终隐藏 PAC、BTI、RNDR；进入 EL1 前 BEAU 清除
`HCR_EL2.API` 与 `HCR_EL2.APK`，因此访客无法执行 PAC 指令或访问 PAC key，也不会
读取、修改或使用 EL2 key。RNDR/RNDRRS 仅在 ID 寄存器声明支持时执行，并须在 BSP
成功取得种子且全部 pCPU 支持后才发布强熵接口；没有硬件熵时，栈 cookie 可使用现有
计数器混合降级值，但该值绝不宣称为密码学随机数。

Linux VM 的 4 MiB guest ramoops 输入由 EL2 复制到各自的双 bank retained snapshot：
每个 bank 均可保存完整 4 MiB dmesg/console payload，另保留 256 KiB live console。
capture 总是写入 inactive bank，完成 cache clean、payload checksum 及
`magic/~magic`、`length/~length` descriptor 后才切换 active bank。4 KiB ramlog
metadata page 同时保留两个 2 KiB header copy，valid marker 最后写入 inactive copy，
避免 header publication 被中断时丢失两个 payload bank。因此客体 GPA 读取失败、
Hypervisor 重启或中断 capture 不会覆盖上一份可读记录。启动恢复只选择 descriptor 和
payload checksum 均有效的最新 bank；`ramlog <vmid>` 可重复导出 active 记录，并显示
两个 bank 的状态与最近失败原因。

ramlog 保持固定 5 槽 ABI，平台 DTS 只决定 RTOS/Linux 的槽位拆分。QEMU 4OS 配置使用
1 个 RTOS 槽和 4 个 Linux 槽：VM0 使用 RTOS 槽，VM1 至 VM3 使用前三个 Linux 槽，
最后一个 Linux 槽为未来 VM4 预留。对应保留区为
`[0x7debf000, 0x80000000)`，总大小 `0x02141000`。rk356x 继续使用既有的 2+3 拆分，
公共解析器只校验总槽数等于 5，不改变其平台 DTS。

`devmap` 从实际 leaf descriptor 解码属性。Host Stage-1 通过 AttrIdx 查询当前
`MAIR_EL2`，VM Stage-2 读取 `MemAttr[3:0]`；输出统一使用 13 字符 memory-type
子列和固定 4-bit 编码，例如 `Device-nGnRE  [0b0100]`、
`Normal Memory [0b1111]`。没有 Stage-2 leaf 的模拟设备区间显示
`Unmapped (IPA->HPA)`。

### 9.2 VM Stage-2

**定位**：`arch/arm64/guest/vm.c`。

**能力**：

- 每 VM 创建独立 Stage-2 root。
- 使用 4 KiB translation granule；配置 RAM 优先使用 2 MiB block，地址或长度边缘
  自动回退为 4 KiB page，不生成 1 GiB Guest Stage-2 leaf。
- IPA 0 额外映射一页只读 zero page。
- 为 PCI BAR、MSI doorbell 等受控设备窗口动态 map/unmap。
- 导出每 VM 页表级别和 page-pool 统计。

**不变量**：默认静态平台要求 RAM 起点、后端 HPA 和大小满足 4 KiB 对齐，且
`guest_ram_hpa == guest_ram_start`。Host EL2 Stage-1 仍可使用 1 GiB、2 MiB 和
4 KiB 映射，不受 Guest Stage-2 上限影响。

动态删除 2 MiB block 中的局部 4 KiB 范围时，walker 先创建并填充 512 个
4 KiB leaf，再执行 break-before-make。页表更新由 VM 私有事务所有权串行化，
`stg2pt_lock` 只保护短暂的软件描述符临界区：

```text
invalidate old descriptor
    -> CPU VMID TLBI on configured pCPUs
    -> bound SMMU TLBI_S12_VMALL + CMD_SYNC
    -> publish replacement descriptor
    -> apply final leaf deletion
    -> CPU/SMMU synchronization
    -> release retired table pages to the MTE-aware pool
```

空子页表在最终 CPU 和 SMMU 同步完成前只记录在私有 retire bitmap 中，不清零、
不 retag，也不返回共享 pool。CPU shootdown、domain root 校验或 SMMU CMDQ 同步
失败属于不可恢复的隔离错误：页表页保持占用并进入 panic，不允许带着不一致的
CPU/DMA translation 继续运行。

该机制依据 ARM 大页拆分、break-before-make 和硬件 walker 同步后再回收页表页
的原则，针对 BEAU 的静态 page pool、VMID、SMP call 和 SMMUv3 broker 独立
实现，新代码使用 BEAU BSD-3-Clause 许可。当前不做碎片化 4 KiB 表恢复一致后的
自动 2 MiB 合并。

### 9.3 客体 copy 边界

`arch/arm64/guest/memory.c` 的 `copy_to_gpa()`/`copy_from_gpa()` 先验证整个范围，
再做 GPA 到 HPA 的 offset 转换。当前限制：

- `gva2gpa()` 返回 `-ENOSYS`。
- `copy_to_gva()`/`copy_from_gva()` 返回 `-ENOSYS`。
- 因此 Hypercall ABI 应传 GPA，而不是任意 GVA。

### 9.4 EL2 Memory Tagging Extension

`CONFIG_ARM64_MTE` 默认启用编译支持，但运行时只有
`ID_AA64PFR1_EL1.MTE >= FEAT_MTE2` 才开启 EL2 同步 tag checking。当前覆盖范围是
链接器隔离的 host Stage-1 和 VM Stage-2 页表池，不覆盖 EL2 栈、普通全局变量、
guest RAM 或编译器插桩对象。

```text
page-pool allocate
    -> select next non-zero 4-bit generation
    -> set tags and clear all 16-byte granules
    -> publish tagged pointer

page-pool free
    -> validate owner, range and current tag
    -> restore allocation tag 0 and clear data
    -> release bitmap ownership
```

页表描述符、`TTBR0_EL2` 和 `VTTBR_EL2` 只保存无 tag 物理地址；软件从 HPA
恢复页表指针时根据 pool 元数据重新附加当前 tag。Guest 的 MTE、MTE fractional 和
MTEX ID fields 均隐藏，且 `HCR_EL2.ATA` 保持清零，因此本阶段不提供 Guest MTE。

该实现依据 ARM MTE2 capability gate 和 page-tag synchronization 原则，针对
BEAU EL2 page-pool 所有权模型独立设计，不引入通用 OS 的 thread、pmap 或
`vm_page` 模型，新代码使用 BEAU BSD-3-Clause 许可。

QEMU 当前默认 Cortex-A57、rk356x Cortex-A55 以及 Cortex-A72 均不支持 MTE，运行
时保持原 Normal-WB identity path。MTE runtime 验证需要支持该扩展的 CPU，并在
QEMU `virt` machine 上同时启用 MTE。

## 10. 调度模块

### 10.1 公共调度框架

**定位**：`core/schedule.c`。

**能力**：

- 每 pCPU 一个固定 scheduler 和 runqueue。
- vCPU、idle、shell、watchdog、测试线程统一为 `thread_object`。
- 统计 runtime、wait latency、context switches、reschedule 次数和直方图。
- 支持 thread freeze/thaw epoch，为透明 STR 保存 wake 边沿。

**原理**：线程不迁移。线程在创建时绑定 pCPU，并按该 pCPU 的 policy 初始化私有
scheduler data。跨 pCPU 迁移需要重建私有数据，当前不支持。

### 10.2 Policy 对比

| Policy | 原理 | 适用 | 关键参数 |
|---|---|---|---|
| `noop` | 记住唯一 runnable 对象 | 保证只有一个非 idle 对象的独占核 | 无 |
| `iorr` | 固定 slice 轮转 | 同类简单共享负载 | 当前 10 ms slice |
| `bvt` | 选最小 effective virtual time | 独占核、低开销比例公平 | weight、warp、MCU |
| `cbs` | 每线程 Q/T server，EDF 选最早 deadline | burst/wakeup 较多的共享 vCPU | period、budget |
| `rtds` | 固定周期预算，EDF，耗尽等待周期边界 | 周期实时负载 | period、budget |
| `prio` | 固定优先级排序 | 小型且优先级关系明确的集合 | priority |

### 10.3 BVT

`core/sched/bvt.c` 用实际运行时间推进 AVT。权重越大，虚拟时间增长越慢，
长期获得的 CPU share 越高。wake/event 可以开启有界 warp：

```text
EVT = AVT - warp_value
```

warp 只改变短期排序，不修改长期公平账本；`warp_limit` 和 `unwarp_period` 防止
高频事件长期霸占 CPU。

BEAU host shell 使用 2 ms console timer 观察 PL011 RX-ready。timer 只在存在输入
或 VM ownership 返回 host 时锁存事件并唤醒 shell；shell wake 使用
`weight=1, warp_value=8, warp_limit=1, unwarp_period=4`，处理完输入后重新阻塞。
事件 latch 和阻塞后的 RX 重检关闭 timer 与 shell sleep 之间的 lost-wakeup 窗口，
同时不引入 host UART IRQ 路径。

### 10.4 CBS

`core/sched/cbs.c` 把每个线程建模为 Constant Bandwidth Server：

```text
budget Q, period T, absolute deadline D
    |
runtime consumes remaining budget
    |
budget depleted -> D += T, remaining = Q
    |
EDF picks smallest D
```

唤醒时执行 inactive-server 规则，避免睡眠线程囤积旧预算。初始化还会按 pCPU 汇总
Q/T，超过 100% 时 panic，阻止不可调度配置进入运行时。

### 10.5 RTDS

RTDS 使用固定周期边界。每个周期重新获得预算，未用预算不结转。预算耗尽的线程只在
没有有预算线程时借用 slack，因此更适合周期负载，而不是 CBS 的 burst 模型。

## 11. IRQ、GIC、vGIC、ITS 与 vtimer

### 11.1 Host IRQ 框架

`arch/arm64/irq.c` 建立 IRQ domain，把 GIC INTID/LPI 转成 common ACRN IRQ；
`core/irq.c` 管理 descriptor、handler、计数和 latency；`core/softirq.c` 延后 timer、
ptdev 等不能在硬中断内完成的工作。

### 11.2 物理 GICv3/ITS

`arch/arm64/gic/gicv3.c`：

- BSP 初始化 Distributor 和 Redistributor discovery。
- 每 pCPU 初始化 Redistributor、CPU interface 和优先级。
- 提供 ack/EOI、mask/unmask、affinity、SGI 和 PM save/restore。

`arch/arm64/gic/gicv3_its.c`：

- 配置 ITS BASER、command queue 和 LPI property/pending table。
- 为 PCI MSI/MSI-X 分配 LPI。
- 建立 DeviceID/EventID 到 LPI/collection 的映射。
- 维护 command queue timeout、stall 和错误统计。

GICv5 源码是实验性可选路径，默认 `CONFIG_ARM64_GICV5=n`。

### 11.3 vGICv3/vITS

**定位**：`arch/arm64/guest/vgicv3.c`、`vgicv3_its.c`。

**能力**：

- 模拟 GICD/GICR/ITS MMIO。
- 模拟 ICC CPU interface sysreg 与 SGI。
- 维护 SGI/PPI/SPI/LPI enable、pending、active、route 和 priority。
- 把可交付中断装入 `ICH_LR<n>_EL2`。
- 在切换、EOI、maintenance IRQ、WFI 和 VM reset 时同步软件/硬件状态。

**代码流**：

```text
device/ptdev/timer asserts virq
    |
    v
arm64_vgicv3_inject_irq()
    - select target vCPU
    - set descriptor pending/level
    - record assert latency
    - make vCPU request / wake
    |
    v
arm64_process_vcpu_requests()
    |
    v
arm64_vgicv3_flush_current_vcpu()
    - choose deliverable IRQ
    - build LR
    |
    v
ERET -> guest IRQ
    |
    v
guest EOI / maintenance -> sync active/pending state
```

### 11.4 虚拟定时器

`arch/arm64/guest/vtimer.c` 同时处理：

- CNTV：客体虚拟 timer，通常通过硬件 PPI27。
- trapped CNTP：软件影子和 PPI30。
- vCPU 离线时的 host backup timer。
- STR suspend/resume 时的 deadline 保存、恢复和过期补发。

CNTV 的关键闭环：

```text
guest programs CNTV
    -> save/live shadow + arm backup deadline
host PPI27 or backup expires
    -> sample CNTV and mask EL2 source
    -> assert vGIC PPI27
before ERET/WFI/schedule return
    -> poll/update timer line and flush LR
guest EOI or reprogram
    -> resample and deassert/preserve level
```

backup timer 不是第二个客体时钟，只是确保 loaded/unloaded vCPU 都能在 deadline 后
重新进入 EL2，同步 PPI27。

## 12. MMIO 与虚拟设备分派

### 12.1 Trap 原理

设备 IPA 不建立 Stage-2 leaf，因此客体 load/store 触发 Data Abort：

```text
guest device access
    -> HPFAR_EL2/FAR_EL2 reconstruct IPA
    -> ESR gives width/direction/source GPR
    -> build ACRN_IOREQ_TYPE_MMIO
    -> sdk/bsp/ioreq.c range lookup
    -> handler read/write
    -> read result extended into guest GPR
    -> advance ELR by 4
```

MMIO handler 在 VM 创建时注册，范围不能含糊重叠。访问宽度、offset 和 direction
由具体设备再次校验。

### 12.2 vPL011

**定位**：`arch/arm64/guest/vpl011.c`。

**能力**：模拟 PL011 寄存器、FIFO、IRQ 和 reset；把 RTOS 字节接入 per-VM
console vUART。

**数据流**：

```text
RTOS UART MMIO
    -> Stage-2 abort
    -> arm64_vpl011_mmio_handler()
    -> console_vm_tx_put()/RX refill
    -> console ring
    -> vcon/host terminal
```

### 12.3 virtio-mmio 公共层

**定位**：`sdk/bsp/virtio/virtio_mmio.c`。

**能力**：

- 模拟 virtio-mmio feature、status、queue 和 interrupt register。
- 保存可信 queue shadow。
- 校验 descriptor/avail/used ring 的 GPA、索引和数量。
- 由 device-specific ops 解释 payload。
- PM gate 时把 QueueNotify 合并进 bounded per-queue bitmap，resume 后重放一次。

guest 拥有 vring 内存，但不拥有 BEAU 的 queue shadow。设备语义只有在 descriptor
验证后才能执行。

### 12.4 内建 vhost-console backend

**定位**：`sdk/bsp/vhost/vhost_console.c`。

它不是 virtio-proxy。BEAU 的 vhost-console backend 自己完成 Linux
virtio-console frontend 的 console 路径：

```text
guest TX queue -> copy readable descriptors -> console_vm_tx_put()
host input     -> console_vm_rx_refill() -> fill guest RX descriptors
used ring      -> virtio MMIO IRQ -> vGIC
```

方向从设备视角命名：RX 是 BEAU 到客体，TX 是客体到 BEAU。

### 12.5 virtio-proxy

**定位**：`sdk/bsp/virtio/virtio_proxy.c`。

**能力**：

- DTS 最多描述 32 个 protocol-neutral virtio device。
- 从 VM2/VM3 vring 复制一个有界 descriptor chain 到各自 pending slot。
- VM1 backend 通过 HVC 注册、poll、reply 和 heartbeat。
- DTS 的必填 `beau,backend-vmid` 固定 backend 所有者，错误调用者返回 `-EPERM`。
- ABI v3 支持 wait hint、stats、batch 和 shared batch buffer。
- high-throughput 设备可用最多 4 条 batch；低吞吐设备走单请求。
- backend 超时、backpressure、pending full、busy 和 empty 都有独立统计。

完整请求流：

```text
VM2/VM3 QueueNotify
    -> virtio_proxy_notify_queue()
    -> validate and copy descriptor chain
    -> pending slot

VM1 KBE worker
    -> HC ... REGISTER
    -> HC ... POLL/BATCH_POLL
    -> protocol handler in VM1
    -> HC ... REPLY/BATCH_REPLY

BEAU
    -> copy output into对应 frontend writable descriptors
    -> update used ring
    -> inject virtio IRQ
```

BEAU 不解析 FUSE、block、I2C 或 Ethernet 协议。协议语义位于 VM1 KBE，这减少了
Hypervisor 可信计算基中的设备协议代码。

更多virtio前后端驱动信息，参考[virtio.md](virtio.md).


## 13. Hypercall 与 IPC

### 13.1 HVC 分流

`vcpu_exit.c` 先检查 x0：

```text
standard PSCI function ID -> local PSCI virtualization
top byte == HC_ID(0x80)   -> arm64_dispatch_hypercall()
```

ARM64 dispatcher 自身也是权限表：

- permission flags 为 0 的管理调用只允许 service VM，并解析 relative VMID。
- 带 `GUEST_FLAG_STATIC_VM` 的调用只允许静态 VM 对自身执行。
- 未实现、权限错误或目标错误返回 `-ENOTTY`/负 errno，不借用其他 VM 所有权。

### 13.2 当前 ARM64 有效 Hypercall

| 调用 | ID 低字节 | 调用者 | 用途 |
|---|---:|---|---|
| `HC_GET_API_VERSION` | `0x00` | service VM | 读取 ABI `1.0` |
| `HC_VM_GPA2HPA` | `0x41` | service VM | 查询目标 VM GPA 到 HPA |
| `HC_GET_HW_INFO` | `0x63` | service VM | 读取 pCPU 数量 |
| `HC_VM_WDT_KICK` | `0x64` | static VM | 提交 VM-wide 或 per-vCPU watchdog token |
| `HC_VIRTIO_PROXY_BACKEND` | `0x65` | static VM | backend register/poll/reply/heartbeat |
| `HC_IPC` | `0x66` | static VM | IPC query/notify/ack |

公共头 `include/public/acrn_hv_defs.h` 还定义了 VM 动态创建、ioreq、PCI、Trusty、PM
等 ACRN ABI，但 ARM64 dispatch table 当前大多不挂接或明确返回 `-ENOTTY`。

### 13.3 Trusty TEE 版本查询

#### 整体启动流程

启用 QEMU `--tee` 时，以下流程按源码执行顺序连接 secure boot、BEAU EL2 初始化、静态
VM 创建和客体首次执行。图中的 `path:function()` 是后续跟踪代码时应打开的实际位置。

- Boot Loader stage 1 (`BL1`) AP Trusted ROM
- Boot Loader stage 2 (`BL2`) Trusted Boot Firmware
- Boot Loader stage 3-1 (`BL31`) EL3 Runtime Software
- Boot Loader stage 3-2 (`BL32`) Secure-EL1 Payload (optional)
- Boot Loader stage 3-3 (`BL33`) Non-trusted Firmware

```text
                                    Resident                         Noraml
      Trusted                       Runtime                          World
      Bootstrap                     Firmware                         Bootloader
 ┌──► EL3                      ┌──► EL3                         ┌──► EL2
 │                             │                                │
BL1  ──►  BL2  ──►  BL1  ──►  BL31  ──►  BL32  ──►  BL31  ──►  BL33
           │                              │
           └──► Trusted                   └──► Trusted OS
                Bootloader                     EL1S
                EL1S
```

`bl1_entrypoint` implemented in bl1_entrypoint.S:

- do primary CPU (CPU0) cold boot
- secondary CPUs stay in `wfe` loop


##### 1. Secure boot 到 BEAU EL2

```text
TF-A BL1 / BL2 / BL31 -> Trusty LK BL32 -> BEAU BL33
                                              |
                                              v
arch/arm64/boot/entry.S:_start                 boot pCPU
    |
    +--> msr daifset, #0xf                     mask Debug, SError, IRQ, FIQ
    |
    +--> mrs x2, CurrentEL; cmp x2, EL2
    |       |
    |       `--> not EL2: b _start_hang -> wfi -> b _start_hang
    |
    +--> msr spsel, #1; isb                    select SP_EL2
    |
    +--> adrp/add arm64_exception_vectors
    |    -> msr vbar_el2, x2; isb              install EL2 vector base
    |
    +--> _bss_start .. _bss_end
    |    -> str xzr, [x2], #8 loop             zero C global/static storage
    |
    +--> adrp/add _boot_stack_end; mov sp, x4  install temporary SP_EL2
    |
    +--> Image-header fallback: x1 == 0 ? x1 = x0 : keep x1
    |       raw-loader ABI: x1 is DTB physical address
    |
    +--> mrs x0, mpidr_el1                     x0 is boot pCPU affinity
    `--> bl init_primary_pcpu(x0, x1)
            |
            `--> arch/arm64/init.c:init_primary_pcpu(mpidr, fdt_paddr)
```

`_start` owns only the state needed before C code may run: masked asynchronous exceptions,
EL2 stack selection, EL2 vector base, zeroed BSS and a temporary boot stack. `bl` 返回后不会
继续启动另一条路径；`init_primary_pcpu()` 若意外返回，入口同样落入 `_start_hang` 的低功耗
`WFI` 循环。TF-A/Trusty 不创建 BEAU VM；它们把控制权交给 BL33，之后由 BEAU 完成 host 与
guest 初始化。

```text
PSCI CPU_ON
    |
    v
arch/arm64/boot/entry.S:_start_secondary_psci  secondary pCPU
    |
    +--> msr daifset, #0xf
    +--> msr spsel, #1; isb
    +--> VBAR_EL2 = arm64_exception_vectors; isb
    +--> mov sp, x0                            PSCI-provided EL2 stack
    +--> mrs x0, mpidr_el1
    `--> bl init_secondary_pcpu(x0)
            |
            `--> arch/arm64/init.c:init_secondary_pcpu(mpidr)
```

次核不重复清 BSS，也不重新选择平台策略；它只建立本核的异常与栈上下文，然后加入
`init.c` 的公共 pCPU 初始化路径。主核在后续 `start_pcpus()` 发起 `PSCI CPU_ON`，所有 AP
到达运行态后，才释放静态 VM 的启动。

##### 2. BEAU EL2 初始化到静态 VM 创建

```text
init_primary_pcpu()
    |
    +--> sdk/bsp/arm64/platform.c:arm64_platform_init_early()
    |     -> embedded platform.dts -> CPU topology
    |
    +--> arm64_platform_init()
    |     -> host memory, GIC, SMMU, PCI, and board policy
    |
    +--> arm64_platform_init_post_console()
    |     -> sdk/bsp/arm64/vconfig.c:arm64_parse_vm_config_from_dts()
    |     -> vm_configs[]: RAM, vCPU affinity, images, entry, virtual devices
    |
    +--> init_paging() + arm64_gicv3_init_early()
    `--> init_pcpu_comm_post()
            |
            +--> init_interrupt() + timer_init() + init_sched()
            +--> BSP: start_pcpus() -> wait_pcpus_running()
            +--> BSP: vm_publish_static_boot_state()
            `--> init_guest_mode() -> core/vm.c:launch_vms(pcpu_id)
                    |
                    | per configured VM BSP pCPU, for VM0..VM3
                    v
                    create_vm()
                    +--> arch/arm64/guest/vm.c:arch_init_vm()
                    |     -> stage-2 identity map, vGIC, virtual devices/MMIO
                    +--> create_vm_vcpus() -> core/vcpu.c:create_vcpu()
                    |     -> one scheduler thread for each configured pCPU
                    +--> sdk/bsp/boot/guest/vboot_info.c:init_vm_boot_info()
                    +--> core/vm_load.c:prepare_os_image()
                    |     -> rawimage_loader() copies image/FDT into guest RAM
                    `--> core/vm.c:start_vm()
                          -> arch_vm_prepare_bsp() -> launch_vcpu(BSP)
```

`launch_vms()` is reached by every online pCPU, but each VM is constructed only by its
configured BSP pCPU. A failure in VM creation or image preparation prevents `start_vm()`
and tears down the partial VM.

##### 3. vCPU 首次进入、客体 EL1 与 EL2 回流

```text
launch_vcpu(BSP)
    |
    v
scheduler selects vCPU thread
    |
    +--> arch/arm64/guest/vcpu.c:arch_context_switch_in()
    |     -> load_vcpu(): VTTBR/VTCR, EL1 sysregs, vtimer, vGIC
    |
    +--> arch_vcpu_thread()
    |     -> arm64_process_vcpu_requests()
    `--> arch/arm64/guest/entry.S:arm64_run_vcpu()
            -> copies vcpu->arch.regs to EL2 stack frame
            -> arch/arm64/vector.S:vcpu_exit_return
            -> restores ELR_EL2/SPSR_EL2/SP_EL1 and GPRs
            -> ERET
                |
                v
            guest EL1 first instruction
                +--> VM0 Zephyr       service RTOS
                +--> VM1 Linux-1      secure/KBE backend
                +--> VM2 Linux-2      prelaunch frontend
                `--> VM3 Linux-3      prelaunch frontend

guest trap / IRQ
    -> arch/arm64/vector.S:guest_vector_entry
    -> arch/arm64/guest/vcpu_exit.c:dispatch_vcpu_trap() or dispatch_vcpu_irq()
    -> emulate, inject virtual IRQ, process request, or schedule
    -> vcpu_exit_return -> ERET to the selected guest EL1 context
```

按图阅读时，先追 `entry.S` 和 `init.c` 以理解 EL2 启动顺序；再追 `platform.c`、
`vconfig.c` 和 `vm.c` 以理解配置如何成为 VM；最后追 `vcpu.c`、`guest/entry.S`、
`vector.S` 和 `vcpu_exit.c` 以理解每次 EL1/EL2 往返。`vcpu->arch` 保存可迁移的客体状态，
EL2 栈中的寄存器帧仅用于本次进入或退出。

QEMU 的 `--tee` 启动链为 `TF-A -> Trusty LK -> BEAU`。BEAU shell 的 `tee version`
使用 Trusty `SMC_FC_GET_VERSION_STR` 读取实际 LK 构建字符串；先查询受限长度，再逐字符
读取，且只接受最多 128 个可打印 ASCII 字符。它不调用会锁定 Trusty 全局协商状态的
`SMC_FC_API_VERSION`，也不传递任何指针、共享内存或客体参数。

`tee dump` 不重复构建字符串。它只通过 `SMC_FC_GET_SMP_MAX_CPUS` 获取 Trusty 的最大
SMP CPU 数，并读取 BEAU 从 VM1 成功 API 协商后原子缓存的 ABI 版本。尚无成功协商记录时
显示 `smc.api-version: N/A`；dump 不主动调用 API-version SMC，因而不会改变
Trusty 的全局协商状态。CPU 查询失败时输出 `Trusty TEE Information: N/A`，
不会输出部分 dump。

两个命令只在 QEMU 平台尝试 secure-world SMC；未使用 Trusty firmware、SMC 返回未知值或
字符串校验失败时失败关闭。VM1 的 `trusty-client` API-version 白名单独立于这些诊断命令，
其他客体 SMC 仍保持拒绝。

### 13.4 静态 IPC

**定位**：`arch/arm64/guest/vipc.c`。

### 13.5 静态 remoteproc/RPMsg

`arch/arm64/guest/vrproc.c` 提供与 HVC IPC 分离的静态 transport。平台 DTS 定义
两端 VM、共享 GPA、两个 doorbell GPA、vIRQ 和 vring 参数；EL2 仅将共享页映射给
两个 endpoint，并对 doorbell 宽度、地址与 vqid 做失败关闭校验。当前 QEMU 配置为
VM0 Zephyr OpenAMP remote 与 VM3 Linux `RPROC_DETACHED` attach-only remoteproc；
guest DTS 的 `GIC_SPI 48/49` 对应 EL2 注入的 GIC INTID `80/81`。
Linux attach 会创建标准 virtio-rpmsg bus；它不能加载 firmware、启动、停止或复位
VM0。`ipcstat` 用于查看 HVC IPC 与 remoteproc 的映射、kick、IRQ 和拒绝访问统计。

QEMU DTS 当前定义 VM0 与 VM3 的 channel 0，GPA `0x0b100000`，共享区为
`0x100000` 字节，vring 参数为 `256/0x1000`。每个 channel 有两条单生产者/单消费者 ring：

```text
endpoint0 -> endpoint1
endpoint1 -> endpoint0
```

`QUERY` 返回 channel、peer、GPA、ring 数和可选 virq；`NOTIFY` 更新 doorbell 统计并
唤醒 peer；`ACK` 记录接收进度。热路径不解析 payload，只管理共享 ring 元数据和
通知。ABI version 为 1，ring magic 为 `0x42495043`。

## 14. PCI、vPCI、直通与 SMMUv3

### 14.1 Host PCI

`sdk/bsp/pci/pci.c` 通过 ECAM 扫描物理设备，建立 `pci_pdev` 表，解析 BAR、MSI、
MSI-X 和 capability。QEMU 默认附加：

- `edu,addr=0x1`，BDF `00:01.0`，StreamID `0x0008`。
- modern virtio-net PCI，BDF `00:02.0`，StreamID `0x0010`，可选。

### 14.2 vPCI

**定位**：`sdk/bsp/vpci`。

| 文件 | 责任 |
|---|---|
| `vpci_core.c` | per-VM vPCI、vBDF lookup、config dispatch、domain 生命周期 |
| `vpci_pt.c` | 直通 config/BAR shadow、Stage-2 BAR map/unmap |
| `vpci_msi.c` | MSI/MSI-X capability virtualize 和 remap |
| `vpci_rc.c` | virtual root complex |
| `vpci_sriov.c` | SR-IOV capability 辅助 |

配置空间坚持 virtual-first：只有白名单字段到达物理函数；BAR 只有在校验并建立
Stage-2 device mapping 后才对客体有效；客体写入的 MSI message 被视为路由请求，
必须重写成 host ITS/LPI message。

### 14.3 SMMUv3

**定位**：

- `arch/arm64/iommu/iommu.c`：domain 生命周期、StreamID ownership broker。
- `arch/arm64/iommu/smmu.c`：物理 SMMUv3、STE/CMDQ/EVTQ、故障 containment。
- `arch/arm64/guest/vsmmu.c`：guest synthetic registers/queues；不访问物理寄存器。

**能力**：

- probe IDR，建立 stream table、CMDQ、EVTQ。
- 默认将 STE 初始化为 ABORT。
- 以 VM Stage-2 root 创建 `iommu_domain`。
- 将 StreamID 原子地 assign/unassign 到 domain。
- CMDQ sync、TLBI、event fault、quarantine 和 PM restore。

赋权顺序：

```text
DTS owner policy
    -> passthrough table validation
    -> create VM IOMMU domain from Stage-2 root
    -> write S2 STE(StreamID, VMID, VTTBR/VTCR)
    -> CMDQ sync
    -> publish stream->domain and VM ownership
    -> expose vPCI config/BAR
```

撤销时先写 ABORT STE 并同步，再释放 software owner。

### 14.4 MSI/MSI-X 直通

```text
guest programs virtual MSI/MSI-X
    -> vPCI validates capability/table entry
    -> host ITS allocates LPI
    -> map DeviceID/EventID -> LPI
    -> map hidden per-VM MSI doorbell IOVA to host ITS TRANSLATER page
    -> rewrite physical device MSI address/data
    -> physical LPI -> ptdev softirq -> vGIC/vITS injection
```

CPU MMIO、DMA 和 IRQ 三条链必须同时成立，单独映射 BAR 不等于完成直通隔离。

## 15. Watchdog 与健康监测

### 15.1 VM WDT

**定位**：`core/vm_wdt.c`。

Linux/Zephyr 客体周期性调用 `HC_VM_WDT_KICK`。`param2=0` 保留旧的 VM-wide
heartbeat；Linux 可设置 `ACRN_VM_WDT_KICK_F_PER_VCPU_V1`，由 Hypervisor 根据 HVC
caller 绑定 vCPU 身份，不能由客体参数伪造。per-vCPU 模式激活后不允许退回 VM-wide
模式，重复 counter 或降级 kick 均返回负 errno 且不刷新 deadline。旧 SMCCC wrapper
未保证 `x2` 为零，所以 V1 使用精确 64-bit selector；模式激活前的其他值按 legacy
处理以兼容旧二进制，激活后则作为降级 kick 拒绝。
Hypervisor 不用普通 VM-exit 代替 heartbeat，因为客体 watchdog worker 卡死时仍可能
发生其他 VM-exit。

监测信号：

- VM-wide heartbeat age，或每个 RUNNING vCPU 的 age/counter 与
  expected/started/stalled mask。
- vCPU runtime stall。
- IRQ storm。
- console queue stuck。
- virtio pending/queue stuck。

状态和原因：

```text
UNUSED/OFFLINE/UNKNOWN/ALIVE/STUCK
cause: HEARTBEAT/TIMEOUT/VCPU_STALL/GUEST_VCPU_STALL/IRQ_STORM/CONSOLE_STUCK/VIRTIO_STUCK
```

可配置 VM 进入有界恢复：

```text
detect stuck
    -> QUIESCING: async pause and wait all vCPU switch out
    -> RESETTING: queue WDT-owned cold reset to the VM BSP pCPU
    -> BSP pCPU idle: reset architecture/device state, reload image, start BSP
    -> report reset completion to the WDT control thread
    -> VERIFYING: wait for new heartbeat
    -> success or bounded retry/failure
```

`CONFIG_VM_WDT_RESTART_VM_MASK` 选择允许恢复的 VM，service VM 永不自动重启；
`CONFIG_VM_WDT_RESTART_MAX=5` 将每个 VM 实例的自动恢复限制为最多 5 次，防止无限重启。
WDT 控制线程保持在 pCPU0，只负责检测、
排队和验证；每个 VM 的 cold reset 在其独占 BSP pCPU 上执行。因此一个 Linux VM 的
镜像重载不会累积 BVT runtime 并延迟其他 VM 的恢复。若目标 pCPU 未完成 reset，该 VM 保持
`RESETTING` 可观测状态，其他 VM 的检测和恢复仍可继续。

per-vCPU 模式只监测 `VCPU_RUNNING`。新进入 RUNNING 的 vCPU 从当前时刻获得完整超时
宽限，进入 INIT/OFFLINE/POWERED_OFF 的 vCPU 立即从 expected mask 移除；VM reset 清空
模式和全部 counter。已纳入 expected 的短暂 PAUSED 状态保留原 deadline，但 PAUSED
vCPU 不会被新纳入。system/VM STR 分别冻结每个 expected vCPU 的剩余 deadline，resume
后恢复原 age，因此 suspend 时间不会制造误报。任一 stalled bit 都进入同一个有界整 VM
恢复状态机；QEMU 的 `CONFIG_VM_WDT_RESTART_VM_MASK=0xe` 因而允许 VM1/VM2/VM3 自动重启。

### 15.2 超时现场保留

`sdk/bsp/arm64/hwtdbg.c` 在每次 heartbeat timeout 转换发生时、VM quiesce/restart
之前冻结现场。首次启动一直未 kick 和运行后长期未 kick 使用同一采集路径：

```text
timeout false -> true
    -> WDT lock 下排队轻量 metadata，立即解锁
    -> per-vCPU 模式选择 stalled mask；legacy/空 mask 回退全部 created vCPU
    -> durable selected-vCPU GPR/异常寄存器、调度状态和 pCPU owner snapshot
    -> 对 selected vCPU 执行一次 batched pCPU live capture，等待上限 1 ms
    -> guest/host stack 各最多 16 frame
    -> pending request/IRQ、IRQ 和 virtio 汇总
    -> checksum + barrier，最后发布 valid
    -> VM recovery；按事件 sequence 回写 verified 或失败原因
```

每个受监控 VM 在 BSS 中保留 4 个事件，写满后覆盖最老事件。远端回调只写
per-pCPU generation mailbox；超时后的晚到回调不能修改事件槽。`hwtdbg` 无参数，
按 VM 分组并按 sequence 从旧到新打印所有 checksum 有效事件；读取不清除数据，
但 Hypervisor 重启后数据不保留。没有超时事件时输出
`hwtdbg: no watchdog timeout events`。

为避免超时报告被高频轨迹和底层寄存器细节淹没，事件不保留 guest exit、vGIC
或 vtimer trace，也不冻结 vGIC/vtimer context。per-vCPU 模式只为 stalled mask 中的
vCPU 保留完整 GPR/异常寄存器、guest/host stack、调度延迟、pCPU 当前 owner 和 pending
request/IRQ；多个 stalled bit 全部采集，legacy 或空/无效 stalled mask 回退为全量采集。
VM 级仍保留 WDT/recovery、全部 expected vCPU 的 heartbeat mask/age/token、IRQ 和 virtio
信息。架构持续态只维护
`vmstat/health` 使用的
`vtimer_diag` 聚合计数，不维护 trace ring。

### 15.3 客体驱动

- 外部 Linux `vwdt.c`（验证后同步到 `sdk/kbe`）：`core_initcall` 注册 hotplug-aware
  per-CPU 普通优先级线程；pinned hrtimer 默认每 5 秒只负责唤醒，线程实际获得调度后
  才递增 counter 并 HVC。
- `sdk/zsh/beau_wdt.c`：Zephyr 静态线程，默认 5 秒周期。

该机制证明 vCPU 上的 process-context scheduling progress，覆盖 IRQ 仍响应但长期不能
调度的常见 RCU stall/CPU starvation。它不解析 Linux RCU 状态；若 RCU 子系统异常而
所有 heartbeat 线程仍正常调度，则不会仅凭本机制判定为 RCU stall。

## 16. 透明电源管理与 STR

### 16.1 设计目标

`core/pm.c` 实现 BEAU-owned transparent suspend。客体不参与 PM transaction，也
不暴露 guest PSCI system suspend。框架支持 system scope 和 single-VM scope。

```text
request
    -> gate target VM I/O
    -> prepare/drain hooks
    -> freeze vCPU scheduler threads by epoch
    -> wait every active vCPU switch out
    -> suspend device/architecture hooks
    -> SUSPENDED
    -> platform wake or explicit VM resume
    -> reverse-order restore hooks
    -> thaw threads and replay deferred wakes
    -> ungate I/O
```

### 16.2 状态机

```text
RUNNING
  -> PREPARING
  -> FREEZING_HOST
  -> SUSPENDED
  -> RESTORING_HOST
  -> RUNNING

active phase error -> ABORTING -> RUNNING
rollback cannot prove isolation -> PM_FAILED
```

每次 transaction 使用非零 epoch。hook 按 priority 正序 prepare/suspend，按反序
resume/abort，并用 completed mask 精确回滚已完成步骤。

### 16.3 Retention hook

`sdk/bsp/pm.c` 注册并编排：

- vtimer
- vGIC
- vPCI
- watchdog
- virtio-console/virtio-proxy
- passthrough IRQ/DMA
- SMMUv3

Stage-2 和设备所有权只有在 vCPU 全部 switch-out 后才能保存。恢复失败时保持 I/O
gate 和 freeze，避免把部分恢复状态暴露给客体。

### 16.4 Host context 与 QEMU 模式

`arch/arm64/pm.c` 保存 EL2 sysreg、GIC、timer 和 secondary CPU 状态。
`arch/arm64/suspend.S` 是 strict PSCI SYSTEM_SUSPEND 的低级保存/恢复入口。

QEMU 默认 `qemu-mode = "simulated"`：不声称虚拟硬件真正掉电，但执行同一 PM
transaction、retention 顺序和 wake policy。PL011 RX/timeout IRQ 被临时注册为 wake
source，终端输入唤醒 BSP 的 WFI loop。rk356x 当前 PM disabled。

Shell 暴露：

```text
pm status
pm suspend <vmid>
pm resume <vmid>
pm reboot <vmid>
pmstat
```

`pm reboot <vmid>` 把 cold restart 排队到目标 VM BSP 所在 pCPU 的 idle thread，
然后立即把 BEAU shell prompt 还给用户。目标 pCPU 完成 vCPU pause、device reset、
image reload 和 BSP restart；PM topology gate 在完成或失败前阻止冲突的 STR/reset。

## 17. CPU 特性、SVE 与 CPUFreq

### 17.1 vMPU/vSVE

这里的 MPU 是 VM CPU feature policy unit，不是物理 Memory Protection Unit。

```text
host ID register capability
    AND VM DTS requests feature
    AND product/OS policy allows
    AND requested VL <= host VL
    -> expose feature to guest
```

当前只管理 SVE：

- RTOS 被 policy 拒绝，以保持较小且确定的上下文。
- Linux DTS 可请求 SVE 和最大 VL，当前配置 128 bit。
- ID register trap 隐藏未授权 SVE。
- `CPTR_EL2.TZ` 控制是否允许执行。
- vCPU switch 保存/恢复 Z0-Z31、P0-P15、FFR、FPCR/FPSR、ZCR。

如果 host 不支持 SVE，Linux VM 的配置存在但 feature 不会 active。

### 17.2 CPUFreq

`sdk/bsp/cpufreq.c` 从 DTS 解析 domain、CPU mask、P-state、min/max，当前只支持
performance policy，并选最大 P-state。QEMU 和 rk356x platform backend 目前都是
`stub`：记录 transition，但不改真实时钟。这是策略框架，不是完成的 DVFS 驱动。

## 18. Console、Shell、日志与 Trace

### 18.1 Console ownership

`sdk/bsp/console.c` 管理：

- host shell 输入。
- per-VM TX history ring。
- host 到 VM 的 RX queue。
- backpressure、drain budget 和 prefixed output。
- `Ctrl-D` 从 VM console 返回 BEAU shell。

host shell 无输入时保持 blocked。首次输入发现延迟由 2 ms timer 周期限定；唤醒后
最多连续处理 64 字节，再重新经过 scheduler，以限制单次 console service 时间。

`sdk/bsp/shell.c` 提供行编辑、历史、Tab completion、异步输出保护和命令分发；
`sdk/bsp/cmds/` 按功能提供 common 和架构诊断命令。

`core/logmsg.c` 在输出到 console、memory 或 NPK 前写入独立的 host dmesg record
ring。它不依赖当前 console 所属 VM，因此 VM 占用串口时出现的 EL2 instruction abort、
SError 和 MMIO abort 仍可在返回 BEAU shell 后通过 `dmesg` 查看。容量由
`CONFIG_HOST_DMESG_RING_SIZE` 在平台 `Bconfig` 中以字节配置，默认 64 KiB；记录满后
覆盖最旧的完整记录并显示覆盖计数。该 ring 位于 host BSS，仅保留本次启动期间的日志，
不改变 guest `ramlog` 或跨 host reboot 的留存格式。

所有 ARM64 host trap、MTE/RAS fault 与 guest vCPU exit 的诊断都使用 `LOG_*`，因此
保留完整的 host 前缀和 dmesg 记录。异常行包含可用的 pCPU、VM、vCPU、ESR、ELR、FAR
或 IPA 身份，寄存器 dump 不再手工重复日志前缀。`daemon_log()` 是独立的后台状态输出
接口，只显示 `[κ][HH:MM:SS.mmm,uuu]` 前缀并且仅在 BEAU shell 拥有 console 时写出；
它不进入 dmesg、memory 或 NPK。每次有效 VM WDT HVC kick 在原 heartbeat 更新完成后
排入每 VM 一个有界 daemon display slot，并由 `vm-wdt` 后台线程输出 HVC gap 和 token；
slot 忙时只合并显示事件，不影响 timeout 或 recovery。WDT 以独立的
`heartbeat_started` 状态区分首个 HVC 与运行期心跳，`vmstat` 显示 daemon 的
merge/drop 计数。`CONFIG_VM_WDT_PRINT_PERIOD_MS` timer 继续完整扫描 timeout 与
recovery，但不批量输出 alive 状态。severity 只用于 VT100 色彩，不会显示在 daemon 前缀中。
WDT recovery 与 failure 事件继续使用 `LOG_*` 保留为故障证据。

### 18.2 命令参考

| 命令 | 用途 |
|---|---|
| `version` | 版本、build type、commit 状态 |
| `clear` | 清屏 |
| `symtab` | 查看运行时符号表 |
| `loglevel [console [mem [npk]]]` | 修改 0..6 日志级别 |
| `dmesg [count]` | 输出最近的 BEAU host 日志；缺省输出整个 host 环 |
| `vcpus` | 列出 vCPU、pCPU、状态和 switch 信息 |
| `ps` | scheduler thread 状态、current owner 和相邻命令间 CPU% |
| `schedstat` | pCPU policy、busy%、runqueue、BVT/CBS 统计 |
| `irqstat` | host IRQ 和 guest vIRQ latency |
| `vsh <vmid>` | 切到客体 console，`Ctrl-D` 返回 |
| `devmap` | host Stage-1 与每 VM Stage-2 map |
| `memstat` | 页表池和 Stage-2 ownership |
| `walkpt <vmid> <ipa>` | 只读输出指定 IPA 的逐级 Stage-2 descriptor、映射与属性 |
| `health` | host/VM 运行健康摘要与 findings |
| `hwtdbg` | 打印所有已保留 WDT 超时现场，读取不清除 |
| `coredump <print\|erase>` | 查看或清除最近一次 ARM64 panic/异常快照 |
| `vmstat` | VM 配置、状态、affinity、boot、timer、WDT |
| `cachestat` | cache topology 和 LLC domain |
| `ipcstat` | HVC IPC channel、ring、notify/ack/drop，以及 static remoteproc/rpmsg channel、doorbell、vIRQ 和拒绝计数 |
| `virtiostat` | proxy device、queue、backend、吞吐和延迟 |
| `smmustat` | SMMU/ITS queue、STE、event fault |
| `pcistat` | host/vPCI/BAR/MSI-X/StreamID ownership |
| `cpufreq` | policy、domain、P-state 和 backend |
| `rttest` | 每 pCPU 1000 次 EL2 timer latency test |
| `trace <...>` | start/stop/status/clear/dump per-pCPU trace |
| `perf <...>` | 在 `CONFIG_PERF=y` 时采样并输出 EL2 Host 调用栈 |
| `reboot` | 立即重启 host |
| `pm reboot <vmid>` | 异步 cold restart 非 service VM |
| `pm ...` / `pmstat` | VM STR/reboot 控制与 transaction 诊断 |

`health` 的 vCPU utilization 表使用相邻两次命令之间的 scheduler runtime 差值。
列数取所有已配置 VM 中最大的 vCPU 数；某 VM 未配置的列显示 `NC`，已配置但 VM
或 vCPU 不处于 running 生命周期时显示 `NA`。首次命令只建立基线并以 `--` 表示
running vCPU；后续命令仅对当前快照为 running 的 vCPU 显示百分比。`total` 是该 VM
各 running vCPU 百分比之和，可能超过 100%。利用率表示调度时间，不替代 WDT
heartbeat 对 guest forward progress 的判断。

`ps` 把 idle、shell、helper 和 vCPU thread 放在同一张表中，`cpu%/run.us` 使用相邻
两次 `ps` 的独立 runtime 快照；首次采样、线程首次出现、计数回退或快照容量不足
时显示 `--`。执行 `schedstat` 不会改变 `ps` 的采样窗口。`schedstat` 只保留 pCPU
busy%、policy 和 BVT/RTDS/CBS 诊断，不再重复 per-thread CPU usage。

### 18.3 Trace 与符号化

`core/trace.c` 为每 pCPU 分配固定 32-byte record ring，默认 256 条，满后覆盖最老
记录并计数。trace 默认 idle，需 shell 启动。当前类别包括 timer、scheduler、HVC、
VM enter/exit 和 vIRQ inject。scheduler dump 将 vCPU 显示为 `v<vmid>:<vcpu>`，
host thread 显示为两字符 tag，例如 `v0:2 -> sh`。一次 capture 使用
`IDLE -> CAPTURING -> DRAINING -> READY` 状态机：
stop 先关闭 producer fast path，随后等待全部 per-pCPU writer publish window 退出；
只有 `READY` session 可 dump。若等待超时，状态保留为 `DRAINING`，拒绝 dump、clear
和下一次 start；再次 `trace stop` 可继续完成 drain。`trace status` 输出 session、
readable、last-stop、per-pCPU overwritten 和 writer 状态，便于区分正常覆盖与未完成
快照。

ARM64 build 保留 frame pointer；debug image 由 `gen_symtab.py` 生成地址到名称表。
`arch/arm64/coredump.c` 只在已登记的 thread、per-pCPU 或 boot stack 边界内读取
frame record，并限制原始栈快照和回溯深度；panic 与同步异常共用该 fail-closed
host 栈回溯路径。每个 pCPU 保留一个带版本和校验和的内存快照，shell 可通过
`coredump print` 查看最新快照或通过 `coredump erase` 清除；`hwtdbg` 提供重启前
冻结的 guest/vCPU WDT 超时证据。

### 18.4 Perf

`CONFIG_PERF` 默认关闭。启用后，`perf record <duration-ms> <frequency-hz>` 使用
一个 controller pCPU timer 产生 `1..1000 Hz` 的采样节拍，并通过 Host SGI 请求
其他 pCPU 在各自 IRQ 入口记录现场。每个 pCPU 独占一个固定 128 条的 sample ring；
满后覆盖最老记录并累计 `overwritten`，采样路径不分配内存，也不执行符号解析。

Host 样本从异常入口保存的 ELR、SP 和 x29 开始，只在已登记的 thread、per-pCPU
或 boot stack 范围内回溯，最多保留 12 层。Guest-origin IRQ 只记录 VM/vCPU owner，
不会读取 guest stack。采样自动结束后可执行 `perf dump [count]`，由 shell 使用内置
符号表解析地址；`perf status` 查看各 pCPU 的 captured、missed、no-stack 和 overwrite
计数，`perf stop` 提前停止，`perf clear` 清空已停止的 rings。

QEMU 只能验证控制流、SMP ring ownership 和回溯边界，不能作为真实热点或采样开销
结论；性能分析结果需要在目标硬件上确认。

## 19. SDK 模块

### 19.1 `sdk/imgs`

用于固定回归输入：

- `zephyr.bin`、`lk.bin`。
- VM1/VM2/VM3 Linux 默认复用同一 `Image`，亦可独立指定。
- VM1/VM2/VM3 Linux DTB。
- 共享 `Initramfs.cpio.gz`。

`scripts/repack_initramfs.sh` 可把 KBE 驱动与测试工具重新打包进默认 initramfs。

### 19.2 `sdk/kbe`

这些文件不是 BEAU Hypervisor 本体的一部分，而是移植到 Linux
`drivers/virt/beau` 的保留副本。

| 模块 | 能力与边界 |
|---|---|
| `hcall.*` | WDT、virtio-proxy、IPC 的 AArch64 HVC wrapper |
| `virtio-proxy-backend.*` | register、poll、batch、reply、heartbeat、adaptive wait |
| `virtio-fs-backend.c` | VM2/VM3 隔离目录下的有限 FUSE 操作，不是完整 virtiofsd |
| `virtio-rng-backend.c` | Linux RNG 填充各 frontend writable buffer |
| `virtio-blk-backend.c` | 每 frontend 1 MiB 非持久 RAM disk，有限单段请求 |
| `virtio-i2c-backend.c` | 每 frontend 独立 0x50、256-byte 内存 EEPROM |
| `virtio-net-backend.c` | VM1 uplink 转发与独立 RX 队列，关闭 offload/MQ/RSS |
| `vwdt.c` | 早期、hotplug-aware per-vCPU scheduling-progress heartbeat |
| `edu-test.c` | QEMU edu 直通与 IRQ 验证 |
| `ipc-test.c` | Linux IPC query/ring/notify/ack endpoint |

所有 proxy backend 只在 DT model 表明自己是 VM1 时启动，并为 VM2/VM3 分配独立实例。

### 19.3 `sdk/zsh`

- `hcall.*`：Zephyr HVC、IPC 与 AI scheduler advisor ABI。
- `beau_wdt.c`：heartbeat thread。
- `beau_ipc.c`：`beau ipc query/ping` 风格的共享 ring 验证命令。
- `beau_ai_sched.*`：VM0 advisor service，启动注册后通过 `HC_AI_SCHED`
  获取快照；未训练模型不生成 proposal。

移植到 Zephyr shell sample 时，把三个 C 文件加入 application `target_sources()`。

### 19.4 `sdk/ube`

这是继承自 ACRN 的 userspace device model，覆盖 PCI、virtio、ACPI、USB、TPM、
图形等大量设备。`sdk/sdk.md` 明确标记为“not used yet”，顶层 BEAU `Makefile` 也不
编译它。学习当前 BEAU 数据面时应先忽略，只有设计 future userspace backend 时再读。

## 20. 回归与验证

### 19.5 AI-assisted scheduling SDK

`sdk/ai-sched` retains emlearn and a host-side training/export flow for an
experimental scheduler advisor. It is not part of the EL2 image. The
BEAU shell command `schedai snapshot` emits `AI_SCHED` telemetry records;
the VM0 advisor registers through `HC_AI_SCHED`, then uses a boot-bound
capability for bounded snapshot and proposal requests. A proposal contains a
variable-length `{vmid, budget_us}` entry list and is validated against the
DTS envelope before it is recorded as observe-only.

The scheduler and HVC ABI are platform-neutral. The current QEMU DTS is the
test configuration: it permits only shared CBS pCPUs, budgets from 500 to 1500
microseconds, steps no larger than 100 microseconds, and a 100 millisecond
minimum update interval. No proposal changes a reservation in this revision.
All future apply paths must reuse the same validation before touching scheduler
state.

### 19.6 Arm SPE

`CONFIG_ARM64_SPE` builds an EL2-owned Arm Statistical Profiling Extension
collector. The feature is disabled by default and requires a platform
`/vm/generic/spe-ppi` property plus a CPU with `PMSVer`. BEAU owns the SPE
registers and static buffers; guest SPE identification and sysreg accesses are
hidden. `spestat` reports state and provides bounded capture control. The
current QEMU configuration has no SPE PPI and reports the unavailable path.

### 20.1 自动回归

```sh
python3 scripts/regress.py
```

主要检查：

- BEAU prompt 和 fatal pattern。
- PM policy、`vcpus`、hybrid `schedstat`、`rttest`。
- `vmstat`、`devmap`、`memstat`、`health`、`irqstat`。
- `virtiostat` 的 VM2/VM3 fs/rng/blk/i2c/net proxy。
- 正常启动时 `hwtdbg` 无事件；WDT smoke 后保留 guest regs、栈和恢复结果。
- VM0 Zephyr、VM1 Linux KBE、VM2/VM3 Linux frontend shell。
- VM1 KBE startup、VM2/VM3 virtio-proxy 与跨 frontend 隔离。
- 可选 VM console stress、help stress、WDT recovery、STR cycles 和故障注入。

常用变体：

```sh
python3 scripts/regress.py --no-build
python3 scripts/regress.py --stress-vsh-switch
python3 scripts/regress.py --stress-vsh-help --stress-help-rounds 100
python3 scripts/regress.py --wdt-restart-smoke
python3 scripts/regress.py --str-cycles 10 --str-vmid 3
python3 scripts/regress.py --dry-run
```

### 20.2 手工最小 smoke

```text
console:\> health
console:\> vmstat
console:\> schedstat
console:\> irqstat
console:\> virtiostat
console:\> vsh 2
uos ~ dmesg | grep -i 'BEAU virtio-'
<Ctrl-D>
console:\> vsh 3
uos ~ dmesg | grep -i virtio
```

## 21. 常见扩展操作

### 21.1 增加或修改静态 VM

1. 修改目标平台 `platform.dts` 的 `vm@N`：RAM、ordered `cpu-affinity`、OS family、
   boot module、SVE 和 scheduler 参数。
2. 确保 VM RAM 与其他 VM、Hypervisor、staging image 不重叠，并满足 Stage-2
   block alignment。
3. 把可复用 image/DTB 放到 `sdk/imgs`；若是嵌入资产，更新 `platform.S` 和依赖。
4. 更新平台 `Bconfig` 的 VM 数量和资源上限。
5. 执行 `make ... checkconfig`、完整构建和 QEMU regression。

验证点：`vmstat` 中 configured/created vCPU 一致，`devmap` 中只有本 VM RAM，
客体 AP 能经 PSCI CPU_ON 启动。

### 21.2 增加 virtio-proxy 设备

1. 在 `/vm/generic` 增加 `beau,virtio-proxy` 节点，配置唯一 MMIO/IRQ/device ID、
   frontend VM、backend VM、queue num/size、pending num、tag 和 throughput。
2. 确保 frontend guest DTB 含对应 `virtio,mmio` 节点。
3. 在 `sdk/kbe` 增加 VM1 protocol backend，复用 common worker并隔离各 frontend 状态。
4. 只在 backend 中解析协议；Hypervisor 只处理 descriptor transport。
5. 更新 Kconfig/Makefile、initramfs 和 regression。

验证点：`virtiostat` 显示 queue ready、ABI 3、heartbeat 和 request/reply 增长。

### 21.3 增加 Hypercall

1. 在 `include/public/acrn_hv_defs.h` 分配 ABI ID 和固定布局结构。
2. 在 `arch/arm64/guest/hcall.c` 增加显式 dispatch entry 和 permission flags。
3. 使用 `copy_from_gpa()`/`copy_to_gpa()`，不要直接解引用客体地址。
4. 在 KBE/ZSH wrapper 中同步 ID 和结构。
5. 测试正常、无权限、错误 ABI、越界 GPA 和目标 VM 不存在路径。

### 21.4 增加直通设备

1. 在 host DTS 中声明 passthrough owner、StreamID、writable、可选 SPI policy。
2. 在 VM `pci-devices` 中声明 pbdf/vbdf 和 BAR policy。
3. 确认 SMMU 支持 Stage-2；安全目标下不要依赖 bypass compatibility。
4. 验证 assignment 顺序为 SMMU 先、VM visibility 后。
5. 用 `pcistat`、`smmustat`、`irqstat` 和客体驱动共同验证 config、BAR、DMA、IRQ。

## 22. 故障定位手册

### 22.1 客体不启动

按顺序检查：

```text
host banner/pCPU all running
    -> vmstat: VM_CREATED or VM_RUNNING
    -> boot kernel/load/entry/FDT address
    -> devmap: RAM Stage-2 exists
    -> vcpus: BSP runnable/running
    -> hwtdbg: 若已发生 WDT timeout，检查重启前 ELR/ESR/FAR/HPFAR
```

常见原因：image staging 地址错误、load range 越界、FDT overlap、entry 未对齐、
affinity 指向未初始化 scheduler。

### 22.2 客体卡在 WFI 或 timer

查看：

```text
hwtdbg
irqstat
vmstat
schedstat
```

`hwtdbg` 重点查看超时前 GPR/异常寄存器、guest/host stack、`pcpu-owner`、pending
request/IRQ 和 vCPU wait latency；`vmstat` 查看 CNTV ctl/cval、PPI descriptor、
backup/poll/PPI count、EL2 mask 和 pre-ERET flush 聚合；`irqstat` 核对 host/guest
IRQ 是否持续推进。Linux 有 arch_timer IRQ 不代表 softirq 一定推进，需要结合三类
证据判断是 vCPU 未获调度、guest 执行卡死还是 timer delivery 停滞。

### 22.3 virtio-proxy 不工作

```text
virtiostat
vsh 2 -> dmesg | grep -i 'BEAU virtio-'
vsh 3 -> dmesg | grep -i virtio
```

区分：frontend queue 未 ready、backend 未 register、heartbeat stale、pending full、
`-ENODATA` 正常空轮询、`-EBUSY` in-flight slot，以及 descriptor copy/bounds 错误。

### 22.4 PCI 设备存在但 DMA/IRQ 不工作

```text
pcistat
smmustat
irqstat
```

必须分别确认：vBDF/config、BAR Stage-2、StreamID owner/STE、CMDQ sync、ITS
DeviceID/EventID/LPI、ptdev entry 和 vGIC delivery。不要用“BAR 可读”推断 DMA 已隔离。

### 22.5 STR 卡住

```text
pmstat
ps
schedstat
virtiostat
smmustat
```

检查当前 epoch、phase、completed hook mask、I/O gate、last error 和 phase duration。
`pmstat` 使用 box-drawing 框架按列输出 VM 基本状态；全部 vCPU mask 为零时显示
`vCPU masks: none`，否则以两张对齐表完整显示 gated/active 和 frozen/wake-owned mask。
`PM_FAILED` 表示恢复隔离无法证明，不应强行解冻客体。

## 23. 当前实现边界

1. 只支持 ARM64 静态平台 `qemu` 和 `rk356x`。
2. 客体 RAM 强制 1:1 Stage-2，尚无通用非 identity backing。
3. GVA page walk/copy 未实现，Hypercall 主要使用 GPA ABI。
4. post-launched VM、HSM I/O、ACPI 默认关闭；Trusty 仅支持 QEMU POC 的
   `TF-A -> Trusty LK -> BEAU` 启动链和只读 `tee <version|dump>` 诊断，不提供通用 TEE、
   FF-A、共享内存或 RK 平台支持。
5. ARM64 动态 VM 管理与多数继承的 ACRN Hypercall 未接通。
6. relocation 函数未实现，默认固定链接地址。
7. CPUFreq backend 为 stub；rk356x PM 当前 disabled。
8. GICv5 是实验路径，默认 GICv3。
9. SMMU 无 Stage-2 时存在 bypass 兼容分支，不满足严格 DMA 隔离目标。
10. KBE fs/blk/i2c/net 都是验证级 backend，不是完整生产设备服务。
11. `sdk/ube` 尚未接入。
12. 自动回归集中在 QEMU；rk356x 需要硬件验证。

## 24. 推荐源码学习顺序

### 第一阶段：跑起来并理解所有权

```text
sdk/sdk.md
README.md
arch/arm64/platform/qemu/platform.dts
Makefile
arch/arm64/Makefile
scripts/kick.py
scripts/regress.py
```

目标：能说清四个 VM、两类 CPU pool、image staging 和 frontend/backend 拓扑。

### 第二阶段：从 reset 到 guest EL1

```text
arch/arm64/boot/entry.S
arch/arm64/init.c
sdk/bsp/arm64/platform.c
sdk/bsp/arm64/platform_dts.c
core/vm.c
core/vcpu.c
core/vm_load.c
arch/arm64/guest/vm.c
arch/arm64/guest/vcpu.c
arch/arm64/guest/entry.S
```

目标：手工画出 `_start -> launch_vms -> ERET`。

### 第三阶段：异常、IRQ 与 timer

```text
arch/arm64/vector.S
arch/arm64/guest/vcpu_exit.c
sdk/bsp/ioreq.c
arch/arm64/gic/gicv3.c
arch/arm64/guest/vgicv3.c
arch/arm64/guest/vtimer.c
```

目标：能从一个 guest MMIO 或 PPI27 追到最终 LR/ERET。

### 第四阶段：设备与 DMA

```text
sdk/bsp/virtio/virtio_mmio.c
sdk/bsp/vhost/vhost_console.c
sdk/bsp/virtio/virtio_proxy.c
sdk/kbe/virtio-proxy-backend.c
sdk/bsp/pci/pci.c
sdk/bsp/vpci/*
sdk/bsp/passthrough.c
arch/arm64/iommu/iommu.c
arch/arm64/iommu/smmu.c
arch/arm64/guest/vsmmu.c
```

目标：能分别解释 CPU MMIO、DMA、MSI 三条所有权链。

### 第五阶段：实时性、健康与系统状态

```text
core/schedule.c
core/sched/bvt.c
core/sched/cbs.c
core/vm_wdt.c
core/pm.c
arch/arm64/pm.c
sdk/bsp/pm.c
sdk/bsp/cmds/shell_pm.c
```

目标：能用 `schedstat/hwtdbg/health/pmstat` 给出有代码证据的故障判断。

## 25. 关键结构速查

| 结构 | 所有者 | 含义 |
|---|---|---|
| `acrn_vm_config` | platform DTS parser | 静态 VM policy |
| `arch_vm_config` | platform DTS parser | ARM64 RAM/设备/SVE policy |
| `acrn_vm` | `core/vm.c` | VM runtime object、Stage-2、vGIC、IOMMU |
| `acrn_vcpu` | `core/vcpu.c` + arch guest | 生命周期、thread、durable guest state |
| `thread_object` | scheduler | 固定 pCPU 的调度实体 |
| `sched_control` | per-CPU | 当前 policy、runqueue、current、统计 |
| `arm64_vgicv3` | guest vGIC | VM 级中断描述符、ITS、lock |
| `virtio_mmio_dev` | virtio common | MMIO register 和 queue shadow |
| `virtio_proxy_dev` | proxy | frontend queue、pending、backend、统计 |
| `iommu_domain` | SMMU/vPCI | VMID、Stage-2 root、StreamID owner |
| `beau_pm_snapshot` | PM core | epoch、scope、phase、hook/vCPU mask、错误 |
| `vm_wdt_entry` | WDT core | heartbeat、QUIESCING/RESETTING/VERIFYING 状态与恢复次数 |


## 26. 计划

BEAU OS后续开发计划参照[porting.md](porting.md).


## 27. 总结

BEAU OS 的核心不是单个虚拟设备，而是四个一致的所有权边界：

```text
CPU time     -> per-pCPU scheduler + ordered vCPU affinity
CPU memory   -> per-VM Stage-2
device DMA   -> SMMUv3 StreamID -> VM domain
interrupt    -> GIC/ITS -> ptdev/vGIC/vITS -> guest LR
```

`platform.dts` 在启动前定义这些边界，`core` 管理生命周期，`arch/arm64` 实现 EL2
机制，`sdk/bsp` 把平台策略和设备控制面接起来。学习时只要始终追问“状态由谁拥有、
何时发布、失败时如何撤销”，就能从启动、调度、MMIO、virtio、PCI 一直追到 WDT
和 STR，而不会把表面上的函数调用误当成真正的隔离边界。
