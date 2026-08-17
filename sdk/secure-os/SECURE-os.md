# MicroKernel seL4

> 本文是一份面向学习者的 Secure OS 架构与源码导读。它描述的是本仓库当前的
> ARM64/QEMU 配置，不代表一套可直接用于生产环境的安全操作系统。

`secure-os` 是一个建立在 seL4 微内核之上的 ARM64 虚拟化学习工程。它把 seL4、
Microkit 风格的静态系统构建、CapDL 初始化、故障监控和 Rust `no_std` 运行时组合
在同一个源码树中，便于从“镜像怎样生成”一直追踪到“一个字符怎样经过中断进入
Rust 控制台”。

本文希望回答五个问题：

1. seL4、Microkit、CapDL 和 VMOS 各自负责什么？
2. `loader.img` 是如何构建出来并在 EL2 启动的？
3. 静态系统描述如何变成 seL4 对象、能力和保护域？
4. PL011 中断与同步 IPC 如何穿过各层？
5. 初学者应该按照什么顺序阅读本仓库？

## 1. 先建立正确的边界

### 1.1 这是学习工程，不是安全产品

本仓库演示的是一条可读、可构建、可在 QEMU 上运行的技术路径。它没有提供完整的
产品安全策略、安全启动、密钥管理、安全更新、设备全虚拟化、认证或长期维护承诺。

尤其要注意：

- 上游 seL4 的形式化验证结论有严格的架构、配置和构建假设；
- 本仓库的 VMOS 集成、Microkit 组件、Rust Runtime 和系统描述不自动继承这些结论；
- 当前构建明确设置 `KernelVerificationBuild=OFF`；
- `vmos/UPSTREAM_SHA256` 是本地源码完整性基线，不是代码正确性或安全性证明。

项目的正式范围说明见 [README.md](README.md)。

### 1.2 当前配置与可扩展能力

本仓库默认目标是：

| 项目 | 当前默认值 |
| --- | --- |
| 架构 | AArch64 |
| 机器 | QEMU `virt` |
| CPU | 4 个 Cortex-A53 |
| 内存 | 2 GiB |
| Loader 地址 | `0x7000_0000` |
| seL4 调度模型 | MCS |
| Hypervisor 支持 | 开启 |
| Runtime | Rust `no_std` |

seL4 内核和 Microkit 工具具有 ARM VCPU、Guest VM 与 Stage-2 地址转换能力，但当前
[vmos/runtime/runtime.system](vmos/runtime/runtime.system) 只实例化两个普通保护域
（Protection Domain，PD）：

- `vmos_runtime`：中断驱动的 PL011 控制台和 IPC Benchmark 发起端；
- `ipc_benchmark_responder`：被动的同步 IPC 响应端。

因此，本文会解释 VCPU/Stage-2 在整体架构中的位置，但不会把它们误画成当前 QEMU
Runtime 已经创建的 Guest VM。

## 2. 核心概念

### 2.1 微内核

传统宏内核往往把驱动、文件系统、网络栈和大量策略放在内核态。微内核尽量只保留
最小机制，例如：

- 线程和调度；
- 地址空间；
- IPC；
- 中断交付；
- 内核对象与访问控制。

服务和策略尽可能放入相互隔离的用户态组件。这样可以缩小高权限可信计算基，但也
意味着系统设计者必须明确组件边界、能力分发和故障处理。

### 2.2 seL4

seL4 是本工程的微内核。对本文最重要的内核对象包括：

| 对象 | 学习时可理解为 |
| --- | --- |
| TCB | 一个线程的内核执行上下文 |
| CNode / CSpace | 保存能力的对象及其组成的能力地址空间 |
| VSpace / Page Table | 一个组件的虚拟地址空间 |
| Frame | 可被映射的物理内存页 |
| Endpoint | 同步 IPC 端点 |
| Notification | 异步事件聚合与通知对象 |
| IRQ Handler | 将硬件中断接入 Notification 的能力 |
| Scheduling Context | MCS 模型中的调度预算与周期 |
| VCPU | ARM Guest 虚拟 CPU 状态；当前 Runtime 未实例化 |

seL4 内核提供机制，不替系统设计者决定哪个组件可以访问哪个设备。访问权通过
**能力（Capability）**显式表达。

### 2.3 Capability：对象的引用也是权限

能力不是一个普通整数句柄，而是“指向某个内核对象、同时携带允许操作”的受控引用。
组件只有在自己的 CSpace 中持有相应能力，才能对对象执行被允许的操作。

可以把核心思想记成：

```text
No capability -> no way to name an object -> no kernel operation on that object
```

能力可以复制、派生和限制权限，但不能由用户态随意伪造。真正的安全边界不仅取决于
内核，还取决于系统构建时是否只把必要能力交给必要组件。

### 2.4 Protection Domain

Microkit 的 PD 是一个静态配置的隔离组件。一个 PD 通常对应：

- 一个程序镜像；
- 独立的地址空间与 CSpace；
- 一个或多个线程及调度配置；
- 显式声明的内存映射、中断和通信通道；
- 一个指向 Monitor 的故障处理关系。

PD 是 Microkit 的系统建模概念，最终仍会展开为 seL4 的 TCB、CNode、VSpace、Frame、
Endpoint、Notification 等对象和能力。

### 2.5 System Description、CapDL 与 Initialiser

本工程不是在运行时动态发现并启动任意服务，而是在构建时读取 XML System
Description，生成一份静态对象图：

```text
+------------------------+
| runtime.system         |
| System intent          |
+-----------+------------+
            |
            v
+------------------------+
| Microkit Tool          |
| Parse and validate     |
+-----------+------------+
            |
            v
+------------------------+
| CapDL object/cap spec  |
| System mechanism       |
+-----------+------------+
            |
            v
+------------------------+
| Initialiser ELF        |
| Embedded init data     |
+-----------+------------+
            |
            v
+------------------------+
| seL4 kernel calls      |
| Create objects/caps    |
+-----------+------------+
            |
            v
+------------------------+
| Monitor and PDs run    |
+------------------------+
```

三者的分工是：

- System Description：描述“需要哪些组件、映射、中断和通道”；
- CapDL：描述“需要哪些 seL4 对象以及能力如何连接”；
- Initialiser：在启动时使用初始任务获得的资源，将静态规格落实为真实对象。

## 3. 总体框架

系统要分成两个世界理解：Host 上的离线构建世界，以及 QEMU/目标板中的运行世界。

```text
+--------------------------------------------------------------------------+
| HOST: build and package                                                  |
|                                                                          |
|  +---------------+      +---------------+                                |
|  | Makefile.vmos |----->| build_vmos.py |                                |
|  +---------------+      +-------+-------+                                |
|                                 |                                        |
|                    +------------+------------+                           |
|                    |                         |                           |
|                    v                         v                           |
|          +------------------+      +----------------------+              |
|          | CMake + Ninja    |      | Cargo + Make         |              |
|          | build seL4       |      | build VMOS parts     |              |
|          +--------+---------+      +----------+-----------+              |
|                   |                           |                          |
|                   +-------------+-------------+                          |
|                                 |                                        |
|                                 v                                        |
|                     +-----------------------+                            |
|                     | Internal VMOS SDK     |                            |
|                     +-----+------------+----+                            |
|                           |            |                                 |
|                           v            v                                 |
|                    +-----------+  +------------------+   +--------------+|
|                    | rustc     |  | Microkit Tool    |<--|runtime.system||
|                    | build PDs |->| objects + image  |   +--------------+|
|                    +-----------+  +--------+---------+                   |
|                                            |                             |
|                                            v                             |
|                               +-------------------------+                |
|                               | loader.img + report.txt |                |
|                               +------------+------------+                |
+--------------------------------------------|-----------------------------+
                                             |
                                             v
+--------------------------------------------------------------------------+
| TARGET: QEMU virt / ARM64 EL2                                            |
|                                                                          |
|  +--------+    +-------------+    +-------------------+                  |
|  | Loader |--->| seL4 Kernel |--->| CapDL Initialiser |                  |
|  +--------+    +-------------+    +---------+---------+                  |
|                                             |                            |
|                  +--------------------------+----------------------+     |
|                  |                          |                      |     |
|                  v                          v                      v     |
|          +---------------+       +-------------------+    +-------------+|
|          | Monitor       |<......| Rust Runtime PD   |<-->| Passive IPC ||
|          | fault handler |<......| PL011 console     |    | Responder   ||
|          +---------------+       +---------+---------+    +-------------+|
|                                            ^                             |
|                                            | IRQ 33                      |
|                                      +-----+-----+                       |
|                                      | PL011 UART |                      |
|                                      +-----------+                       |
+--------------------------------------------------------------------------+
```

这里最容易混淆的两点是：

1. Microkit Tool 是 Host 上运行的 Rust 工具，不是目标机常驻服务。
2. Initialiser 只负责把静态规格变成内核对象；系统建立后，Monitor 才负责接收 PD
   故障，而不是继续代替 PD 执行业务逻辑。

## 4. 仓库分层与源码归属

```text
secure-os/
|-- src/ include/ libsel4/ configs/     seL4 kernel and ABI
|-- build_vmos.py                       ARM64 VMOS build orchestration
|-- Makefile.vmos                       Public build entry point
`-- vmos/
    |-- loader/                         EL2 Loader
    |-- initialiser/                    CapDL Initialiser wrapper
    |-- monitor/                        PD/VCPU fault Monitor
    |-- libmicrokit/                    Target-side PD runtime library
    |-- tool/microkit/                  Host system builder and packager
    |-- support/                        Artifacts and source integrity
    `-- runtime/                        Project-owned Rust Runtime
```

| 层 | 主要职责 | 修改风险 |
| --- | --- | --- |
| seL4 Core | 内核对象、系统调用、调度、ARM 虚拟化与 ABI | 最高，默认保持不变 |
| Vendored VMOS/Microkit | Loader、Monitor、libmicrokit、Host Tool | 上游拷贝，逻辑默认保持不变 |
| VMOS Rust | 控制台、协议、Benchmark、构建支持 | 本项目功能扩展首选层 |
| Build / Documentation | 构建入口、说明和元数据 | 仍需与真实行为保持同步 |

新功能优先写在模块化 Rust 中，而不是为了方便直接修改 seL4 或 Vendored C 代码。
相关仓库规则见 [tools/secure-os-dev/SKILL.md](tools/secure-os-dev/SKILL.md)。

## 5. 构建和镜像生成流程

### 5.1 Make 目标

入口是 [Makefile.vmos](Makefile.vmos)：

| 命令 | 结果 |
| --- | --- |
| `make -f Makefile.vmos build` | 构建 seL4 与 VMOS 基础组件并发布 ELF |
| `make -f Makefile.vmos qemu-image` | 再构建两个 Rust PD，生成 `loader.img` |
| `make -f Makefile.vmos qemu` | 构建镜像并启动 QEMU |
| `make -f Makefile.vmos test` | 运行 VMOS Python 与 Rust 单元测试 |
| `make -f Makefile.vmos verify-source` | 核对 Vendored 源码 SHA-256 基线 |

### 5.2 `build` 做了什么

[build_vmos.py](build_vmos.py) 是基础构建编排器。默认配置会启用 AArch64、MCS、
ARM Hypervisor、四核和用户态物理计数器访问。

```text
+----------------------------+
| make -f Makefile.vmos build|
+-------------+--------------+
              |
              v
+----------------------------+
| Check arguments, tools and |
| required Python modules    |
+-------------+--------------+
              |
              v
+----------------------------+
| Configure seL4             |
| AArch64 + MCS + Hypervisor |
+-------------+--------------+
              |
              v
+----------------------------+
| Build and install seL4     |
| with CMake + Ninja         |
+-------------+--------------+
              |
              v
+----------------------------+
| Copy kernel.elf, headers   |
| and generated metadata     |
+-------------+--------------+
              |
              v
+----------------------------+
| Generate object sizes and  |
| address-space constants    |
+-------------+--------------+
              |
      +-------+-------+----------------+
      |               |                |
      v               v                v
+------------+  +-------------+  +----------------------+
| Microkit   |  | AArch64     |  | Loader, Monitor and  |
| Host Tool  |  | Initialiser |  | libmicrokit          |
+------+-----+  +------+------+  +----------+-----------+
       |               |                    |
       +---------------+--------------------+
                       |
                       v
              +------------------+
              | Internal VMOS SDK|
              +--------+---------+
                       |
                       v
              +------------------+
              | Required outputs |
              | are complete?    |
              +---+----------+---+
                  |          |
              no  |          | yes
                  v          v
          +-----------+  +------------------+
          | Fail with |  | Publish 4 public |
          | diagnostics| | ELF files        |
          +-----------+  +------------------+
```

公共 ELF 位于：

```text
build/vmos/qemu_virt_aarch64/debug/elf/
|-- sel4.elf
|-- loader.elf
|-- monitor.elf
`-- initialiser.elf
```

更深的 `sdk/` 目录是 Microkit 工具消费的内部构建状态，其中还包含 `microkit`、
`libmicrokit.a`、链接脚本、头文件和板级元数据。

### 5.3 `qemu-image` 做了什么

该目标使用 `rustc` 将两个 `no_std` 程序链接到 `libmicrokit.a`：

- `vmos-runtime.elf`；
- `ipc-benchmark-responder.elf`。

随后 Host 侧 `microkit` 工具读取 `runtime.system`，查找所有 ELF，创建 CapDL 规格，
把 Initialiser 所需数据写入其镜像，并最终把 Loader、Kernel、Initialiser、Monitor
和 PD 镜像打包进 `loader.img`。

```text
+----------------------+     +----------------------------+
| runtime.system       |---->| Parse and validate         |
+----------------------+     +-------------+--------------+
                                           |
+----------------------+                   |
| vmos-runtime.elf     |-------------------+
+----------------------+                   |
                                           v
+----------------------+     +----------------------------+
| ipc responder ELF    |---->| Build the object graph     |
+----------------------+     +------+---------------------+
                                    |                |
+----------------------+            |                v
| seL4 + VMOS SDK      |------------+        +---------------+
+----------------------+                     | report.txt    |
                                             +---------------+
                                    |
                                    v
                         +----------------------------+
                         | CapDL spec + memory layout |
                         +-------------+--------------+
                                       |
                                       v
                         +----------------------------+
                         | Embed Initialiser data     |
                         +-------------+--------------+
                                       |
                                       v
                         +----------------------------+
                         | Package with VMOS Loader   |
                         +-------------+--------------+
                                       |
                                       v
                         +----------------------------+
                         | loader.img                 |
                         +----------------------------+
```

`report.txt` 很有学习价值：它把抽象的 XML 声明展开为对象、能力、地址和内存分配，
是连接 System Description 与 seL4 机制的观察窗口。

## 6. ARM64 启动流程

QEMU 使用 `-device loader` 将 `loader.img` 放到 `0x7000_0000`，并让 CPU0 从该镜像
进入。Loader 在启动过程中检查当前异常级、准备 MMU、展开镜像区域并启动其他 CPU。

```text
+----------------------------+
| QEMU / CPU0                |
| Enter loader.img at EL2    |
+-------------+--------------+
              |
              v
+----------------------------+
| VMOS Loader                |
| Relocate if required       |
| Check loader_data magic    |
+-------------+--------------+
              |
              v
+----------------------------+
| VMOS Loader                |
| Copy Kernel, Initialiser,  |
| Monitor and PD regions     |
+-------------+--------------+
              |
              v
+----------------------------+
| VMOS Loader                |
| Start secondary CPUs       |
| Enable each CPU MMU        |
+-------------+--------------+
              |
              v
+----------------------------+
| seL4 Kernel                |
| Enter kernel_entry with    |
| Initial Task region        |
+-------------+--------------+
              |
              v
+----------------------------+
| seL4 Kernel                |
| Create BootInfo, Untyped   |
| and initial kernel state   |
+-------------+--------------+
              |
              v
+----------------------------+
| CapDL Initialiser          |
| Use Untyped capabilities   |
| to create static objects   |
+-------------+--------------+
              |
              v
+----------------------------+
| CapDL Initialiser          |
| Configure VSpace, TCB, SC, |
| IRQ, Endpoint and CSpace   |
+-------------+--------------+
              |
      +-------+----------------------+------------------+
      |                              |                  |
      v                              v                  v
+-------------+            +----------------+   +----------------+
| Monitor     |            | Rust Runtime   |   | IPC Responder  |
| wait faults |            | init PL011     |   | passive server |
+-------------+            +----------------+   +----------------+
```

### 6.1 Loader

关键代码从 [vmos/loader/src/aarch64/crt0.S](vmos/loader/src/aarch64/crt0.S) 的
`_start` 开始，再进入 [vmos/loader/src/loader.c](vmos/loader/src/loader.c)。主要步骤：

1. 必要时把 Loader 重定位到链接地址，拒绝重叠拷贝；
2. 初始化 UART 和异常处理；
3. 检查打包工具写入的 `loader_data` magic；
4. 将各个打包区域复制到指定物理地址；
5. 启动次级 CPU；
6. 每个 CPU 开启 MMU并进入 seL4 Kernel。

Loader 一旦覆盖了此前的引导环境，就不能安全返回；错误路径会打印诊断并停止。

### 6.2 seL4 与 Initial Task

seL4 完成平台、CPU、内存和内核对象初始化后，会把剩余可管理内存表示为 Untyped
能力，并启动 Initial Task。这里的 Initial Task 就是
[vmos/initialiser/src/main.rs](vmos/initialiser/src/main.rs) 引入的 CapDL Initialiser。

Untyped 可以理解为“尚未决定用途的内核资源授权”。Initialiser 根据嵌入的静态规格，
将其 retype 为 TCB、CNode、Endpoint、Notification、Page Table、Frame 等具体对象。

### 6.3 Monitor

[vmos/monitor/src/main.c](vmos/monitor/src/main.c) 常驻等待故障 Endpoint。收到故障后会：

- 根据 badge 定位故障 PD；
- 读取对应 TCB 寄存器；
- 解析 CapFault、UserException、VMFault 或 VCPUFault；
- 打印地址、寄存器和部分体系结构故障信息。

当前 Monitor 主要用于诊断。它不会自动证明故障可恢复，也不会在所有故障后重新启动
组件；故障线程通常保持停止状态。

## 7. 从 System Description 到能力图

当前 [runtime.system](vmos/runtime/runtime.system) 声明的关键资源如下：

| 声明 | 含义 |
| --- | --- |
| `pl011` Memory Region | 物理地址 `0x0900_0000`，大小 `0x1000` |
| Runtime Mapping | 映射到 Runtime 虚拟地址 `0x1000_0000`，`rw`、非缓存 |
| IRQ 33 / ID 0 | PL011 中断以 Channel 0 通知 Runtime |
| Channel | Runtime 端 ID 1，Responder 端 ID 0 |
| `pp="true"` | Runtime 端执行同步 Protected Procedure Call |
| Responder `passive="true"` | 响应端不持有独立活动调度预算，由调用路径驱动执行 |

```text
+------------------------ seL4 kernel objects -----------------------------+
|                                                                          |
|  +---------------+   +----------------+   +---------------------------+  |
|  | PL011 Frame   |   | IRQ Handler 33 |-->| Notification              |  |
|  | PA 0x09000000 |   +----------------+   +---------------------------+  |
|  +-------+-------+                                                       |
|          |                +----------------+   +----------------------+  |
|          |                | Sync Endpoint  |   | Monitor Fault EP     |  |
|          |                +-------+--------+   +----------+-----------+  |
+----------|------------------------|-----------------------|--------------+
           |                        |                       |
           | map VA 0x10000000      | Channel 1 Call        | fault badges
           v                        |                       v
+-------------------------------+   |             +-----------------------+
| vmos_runtime PD               |   |             | Monitor               |
|                               |   |             |                       |
| +---------+  +-------------+  |   |             | +-------------------+ |
| | VSpace  |  | CSpace      |<-+---+             | | Fault decoder     | |
| +---------+  +------+------+  |                 | +-------------------+ |
|                   |           |                 +-----------------------+
| +-----------------v---------+ |
| | TCB + Scheduling Context  | |
| | Rust Console / IPC Client | |
| +---------------------------+ |
+-------------------+-----------+
                    |
                    | synchronous IPC
                    v
+-------------------------------------------+
| ipc_benchmark_responder PD                |
|                                           |
| +---------+  +---------+  +-------------+ |
| | VSpace  |  | CSpace  |  | Passive TCB | |
| +---------+  +---------+  +------+------+ |
|                                  |        |
|                      +-----------v------+ |
|                      | Protocol handler | |
|                      +------------------+ |
+-------------------------------------------+

IRQ delivery path:

  +----------------+     Channel 0     +-------------------+
  | Notification   |------------------>| Runtime CSpace    |
  +----------------+                   +-------------------+
```

图中的箭头不是普通指针，而是构建工具为对应 CSpace 安装能力、为 VSpace 建立映射，
或者配置内核对象关系。

Runtime 源码中的能力槽位基数属于当前 Microkit 目标侧 ABI 布局，不应当被理解为
通用 seL4 固定值。同步 IPC 代码还会检查 `microkit_pps` 位图，确认指定 Channel
确实被系统描述授予 Protected Procedure 权限。

## 8. PL011 中断控制台流程

### 8.1 静态资源

System Description 只把 PL011 页和 IRQ 33 分配给 `vmos_runtime`。Responder 没有
PL011 Frame 或 IRQ 能力，因此不能通过同一路径直接访问串口设备。

### 8.2 初始化

[vmos/runtime/src/main.rs](vmos/runtime/src/main.rs) 的 `init()`：

1. 先屏蔽 PL011 输入中断；
2. 清理残留中断状态；
3. 输出 Runtime 启动消息并显示提示符；
4. 开启 RX 和 Receive Timeout 中断。

### 8.3 字符输入

```text
+----------------------------+
| Host keyboard input        |
+-------------+--------------+
              |
              v
+----------------------------+
| QEMU PL011 RX FIFO         |
| Assert IRQ 33              |
+-------------+--------------+
              |
              v
+----------------------------+
| seL4 IRQ Handler           |
| Notification / Channel 0   |
+-------------+--------------+
              |
              v
+----------------------------+
| libmicrokit dispatch       |
| Rust notified(channel)     |
+-------------+--------------+
              |
              v
+----------------------------+       no       +---------------------------+
| Is channel equal to 0?     |--------------->| Log unexpected channel    |
+-------------+--------------+                +---------------------------+
              | yes
              v
+----------------------------+
| Read masked status and     |
| drain the PL011 RX FIFO    |
+-------------+--------------+
              |
              v
+----------------------------+
| Classify each input byte   |
+---+----------+----------+--+
    |          |          |
    |          |          +--------------------+
    |          |                               |
    v          v                               v
+---------+ +----------------+       +------------------------+
|Printable| | CR / LF        |       | Backspace / Tab        |
|append to| | parse and run  |       | edit or complete       |
|buffer   | | command        |       +------------------------+
+---------+ +-------+--------+
                      |
                      v
              +------------------------+
              | microkit_dbg_putc      |
              | writes command output  |
              +------------------------+

Invalid data or an overlong line is marked and discarded.

+----------------------------+
| Clear PL011 interrupt      |
+-------------+--------------+
              |
              v
+----------------------------+
| ACK seL4 IRQ Handler       |
| Next IRQ may be delivered  |
+----------------------------+
```

关键顺序是：先读取设备并清除中断源，再 ACK 内核中的 IRQ Handler。如果只 ACK 内核
却不清设备状态，电平触发中断可能立即再次到来。

### 8.4 Console 模块边界

控制台按职责拆分：

| 文件 | 职责 |
| --- | --- |
| [main.rs](vmos/runtime/src/main.rs) | PL011 MMIO、IRQ 回调、目标侧入口和不安全边界 |
| [console.rs](vmos/runtime/src/console.rs) | 行编辑、命令解析、统计与输出抽象 |
| [benchmark.rs](vmos/runtime/src/benchmark.rs) | 参数校验、预热、采样和统计 |
| [sel4_ipc.rs](vmos/runtime/src/sel4_ipc.rs) | Endpoint 选择、`svc` 调用和物理计数器 |
| [ipc_protocol.rs](vmos/runtime/src/ipc_protocol.rs) | 请求/响应标签及协议校验 |
| [ipc_responder.rs](vmos/runtime/src/ipc_responder.rs) | 被动响应端入口 |

`main.rs` 中的 MMIO 与内联汇编是需要重点审查的不安全边界；命令解析与统计尽量保持
为可在 Host 单元测试的安全 Rust。

## 9. 同步 IPC Benchmark 流程

控制台命令：

```text
benchmark ipc [iterations]
```

采样次数默认 1,000，允许范围为 1 到 100,000，并在正式采样前执行 32 次不报告的
预热调用。计时来源是 ARM Generic Physical Counter，结果单位是 tick，不是 CPU cycle。

```text
+----------------------------+
| Console user               |
| benchmark ipc N            |
+-------------+--------------+
              |
              v
+----------------------------+
| console.rs                 |
| Parse command              |
+-------------+--------------+
              |
              v
+----------------------------+
| benchmark.rs               |
| Validate 1 <= N <= 100000  |
+-------------+--------------+
              |
              v
+----------------------------+       repeat 32 warmups and N samples
| sel4_ipc.rs                |<-----------------------------------------+
| Check microkit_pps and     |                                          |
| Channel 1                  |                                          |
+-------------+--------------+                                          |
              |                                                         |
              v                                                         |
+----------------------------+                                          |
| Read cntpct_el0 start      |                                          |
+-------------+--------------+                                          |
              |                                                         |
              v                                                         |
+----------------------------+                                          |
| seL4_Call(endpoint, label) |                                          |
+-------------+--------------+                                          |
              |                                                         |
              v                                                         |
+----------------------------+                                          |
| seL4 delivers protected    |                                          |
| call to passive responder  |                                          |
+-------------+--------------+                                          |
              |                                                         |
              v                                                         |
+----------------------------+                                          |
| Responder validates        |                                          |
| Channel 0 and request      |                                          |
+-------------+--------------+                                          |
              | response label                                          |
              v                                                         |
+----------------------------+                                          |
| seL4 synchronous reply     |                                          |
+-------------+--------------+                                          |
              |                                                         |
              v                                                         |
+----------------------------+                                          |
| Read end counter and       |                                          |
| validate response          |------------------------------------------+
+-------------+--------------+
              |
              v
+----------------------------+
| Calculate min/avg/max and  |
| print Stats + counter_hz   |
+----------------------------+
```

这个 Benchmark 主要验证：

- System Description 创建了正确的同步通道；
- Runtime 持有调用能力；
- 被动 PD 可以沿调用路径得到执行机会；
- 请求和响应标签一致；
- 用户态可以读取物理计数器。

它不是物理硬件性能基准。QEMU 调度、Host 负载和计数器实现都会影响结果。如果
Responder 发生故障，同步调用者可能一直等待，因此控制台也可能被阻塞。

## 10. VCPU 与 Stage-2 放在哪里

当前 Runtime 没有 `<virtual_machine>` 声明，但理解 Hypervisor 架构仍需要知道未来
Guest VM 的附加层次：

```text
+----------------------------+        +----------------------------+
| Guest EL1 / Guest OS       |        | Guest Virtual Address      |
+-------------+--------------+        +-------------+--------------+
              |                                     |
              v                                     v
+----------------------------+        +----------------------------+
| seL4 VCPU Object           |        | Guest Stage-1              |
| Virtual EL1 CPU state      |        +-------------+--------------+
+-------------+--------------+                      |
              |                                     v
              v                       +----------------------------+
+----------------------------+        | Intermediate Physical Addr |
| VCPU Exit / Fault          |        +-------------+--------------+
+-------------+--------------+                      |
              |                                     v
              v                       +----------------------------+
+----------------------------+        | seL4-managed Stage-2       |
| User VMM / Monitor policy  |        +-------------+--------------+
+----------------------------+                      |
                                                    v
                                      +----------------------------+
                                      | Host Physical Address      |
                                      +----------------------------+
```

在真正添加 VM 时，System Description 和构建工具还需要创建 VCPU、Guest TCB、
Stage-2 页表及其映射。seL4 负责强制执行已配置的隔离机制，用户态 VMM 负责设备
模拟、退出处理和 Guest 策略。不要把普通 PD 的 VSpace 映射与 Guest Stage-2 映射
混为一谈。

## 11. 故障、安全边界与已知限制

### 11.1 静态配置降低动态复杂度

对象和能力在离线阶段生成，使目标机不需要一个通用的动态服务管理器。好处是对象图
可检查、启动行为可重复；代价是改变拓扑通常需要重新打包镜像。

### 11.2 最小能力原则仍需人工设计

Microkit Tool 会执行格式和资源校验，但不会替设计者判断“某个 PD 是否业务上应该
拥有这项能力”。`runtime.system` 才是当前组件权限关系的源头。

### 11.3 当前不安全边界

- PL011 MMIO 使用 volatile 裸指针访问；
- seL4 syscall 与计数器读取使用 AArch64 内联汇编；
- C 编写的 Loader、Monitor 和 libmicrokit 属于目标侧可信路径；
- Runtime 的 panic handler 只自旋，不输出完整 panic 信息；
- Monitor 以报告故障为主，不提供通用恢复策略；
- Console 是诊断接口，不具备认证、授权或保密传输。

### 11.4 Source Integrity 不等于安全验证

`make -f Makefile.vmos verify-source` 会比较
[vmos/UPSTREAM_SHA256](vmos/UPSTREAM_SHA256) 中记录的路径和摘要，用于发现 Vendored
源码是否偏离已知基线。它不能发现基线本身的缺陷，也不能验证构建产物或运行时策略。

## 12. 推荐源码阅读顺序

```text
+----------------------------+
| 1. README.md               |
| Scope and run instructions |
+-------------+--------------+
              |
              v
+----------------------------+
| 2. Makefile.vmos           |
| Public build entry points  |
+-------------+--------------+
              |
              v
+----------------------------+
| 3. runtime.system          |
| Current system topology    |
+-------------+--------------+
              |
              v
+----------------------------+
| 4. runtime/src             |
| Visible runtime behavior   |
+-------------+--------------+
              |
              v
+----------------------------+
| 5. build_vmos.py           |
| Base build orchestration   |
+-------------+--------------+
              |
              v
+----------------------------+
| 6. tool/microkit           |
| SDF to CapDL and image     |
+-------------+--------------+
              |
              v
+----------------------------+
| 7. loader                  |
| EL2 to seL4                |
+-------------+--------------+
              |
              v
+----------------------------+
| 8. initialiser + monitor   |
| Objects and fault handling |
+-------------+--------------+
              |
              v
+----------------------------+
| 9. seL4 Core               |
| Kernel mechanisms          |
+----------------------------+
```

### 第一遍：建立可观察路径

1. 阅读 [README.md](README.md) 和 [vmos/README.md](vmos/README.md)；
2. 阅读 [Makefile.vmos](Makefile.vmos)，只关注五个公开目标；
3. 阅读 [runtime.system](vmos/runtime/runtime.system)，画出两个 PD 的资源；
4. 从 [runtime/src/main.rs](vmos/runtime/src/main.rs) 跟踪 `init()` 和 `notified()`；
5. 从 [console.rs](vmos/runtime/src/console.rs) 跟踪 `ping` 或 `ps` 命令。

### 第二遍：理解 IPC 和能力

1. 从 `benchmark ipc` 进入 [benchmark.rs](vmos/runtime/src/benchmark.rs)；
2. 跟踪 [sel4_ipc.rs](vmos/runtime/src/sel4_ipc.rs) 中的 Channel 检查和 `svc`；
3. 阅读 [ipc_protocol.rs](vmos/runtime/src/ipc_protocol.rs) 的标签编码；
4. 在 [ipc_responder.rs](vmos/runtime/src/ipc_responder.rs) 找到 `protected()`；
5. 回到 `runtime.system` 对照 Channel ID、`pp` 和 `passive`。

### 第三遍：理解系统怎样生成

1. 阅读 [build_vmos.py](build_vmos.py) 的 `kernel_definitions()` 和组件构建顺序；
2. 阅读 [vmos/tool/microkit/src/main.rs](vmos/tool/microkit/src/main.rs)；
3. 从 `parse_xml()` 进入 `sdf/`，理解 XML 校验；
4. 从 `build_system()` 进入 `capdl/`，理解对象分配与镜像打包；
5. 对照构建生成的 `report.txt`，把 PD 声明映射到实际内核对象。

### 第四遍：理解启动和故障

1. 从 Loader `_start` 跟到 `main()`、`start_kernel()` 和 `arch_jump_to_kernel()`；
2. 理解 Initialiser 为什么需要 BootInfo 与 Untyped；
3. 阅读 Monitor 的 `monitor()` 循环；
4. 最后再进入 seL4 的 Boot、Capability、IPC、MCS 和 ARM Hypervisor 实现。

不要从 seL4 全量源码开始第一遍学习。先建立一条能从输入现象追到具体模块的路径，
再向内核机制下钻，认知负担会小很多。

## 13. 学习实践

以下步骤用于把文档中的图映射到真实产物。执行前应先准备 README 中列出的工具链。

### 13.1 查看构建入口

```shell
make -f Makefile.vmos help
```

先观察默认 `BOARD`、`CONFIG`、CPU 数、输出目录和 QEMU 参数，不要急着修改源码。

### 13.2 构建基础组件

```shell
make -f Makefile.vmos build
```

构建完成后检查公共 ELF，并回答：哪个 ELF 是 Host 工具，哪个 ELF 会在 Target 运行？
答案是 `microkit` 本身运行在 Host；列出的四个 ELF 都会参与目标镜像构建。

### 13.3 生成镜像并阅读报告

```shell
make -f Makefile.vmos qemu-image
```

重点查看：

```text
build/vmos/qemu_virt_aarch64/debug/qemu/
|-- loader.img
|-- report.txt
|-- vmos-runtime.elf
`-- ipc-benchmark-responder.elf
```

尝试在 `report.txt` 中找到两个 PD、PL011 Frame、IRQ、Endpoint 和 Notification，再与
本文第 7 节的能力图逐项对应。

### 13.4 观察启动

```shell
make -f Makefile.vmos qemu
```

观察日志前缀：

```text
LDR|...  Loader stage
MON|...  Monitor stage
VMOS|... Rust Runtime stage
```

看到 `secure:\> ` 提示符后，可以依次尝试：

```text
help
ps
ping
selftest
stats
benchmark ipc 1000
```

使用 QEMU 默认组合键 `Ctrl-a x` 退出。

### 13.5 带着问题读代码

建议每次只追踪一个问题：

- `ping` 为什么完全不需要 IPC？
- 一个回车怎样从 IRQ 33 到达 `finish_line()`？
- 为什么 Responder 是 passive，却能处理同步调用？
- `benchmark ipc` 在哪里拒绝未授权 Channel？
- 如果请求标签错误，响应如何被拒绝？
- 如果 Runtime 访问未映射地址，Monitor 能打印哪些信息？

这些问题分别对应模块边界、能力边界、调度语义、协议校验和故障观测，是理解微内核
系统比记忆 API 更有效的切入点。

## 14. 一页总结

```text
BUILD TIME:
  runtime.system + ELF + seL4 SDK
      -> Microkit Tool
      -> CapDL object/capability spec
      -> loader.img

BOOT TIME:
  QEMU EL2
      -> Loader expands image and enters seL4
      -> seL4 starts CapDL Initialiser
      -> Initialiser creates Monitor and two PDs

RUN TIME:
  PL011 IRQ 33
      -> seL4 Notification
      -> Runtime notified(0)
      -> Console command
      -> Optional synchronous IPC Channel 1
      -> Passive Responder

SECURITY RULE:
  Object existence does not grant access.
  Only capabilities installed in a component CSpace grant operations.
```

继续学习时，请始终区分三件事：内核提供的**机制**、System Description 表达的
**静态策略**，以及 Rust Runtime 实现的**组件行为**。这三层边界正是 seL4/Microkit
系统能够被分析、审查和逐步扩展的基础。

---

Hustle Embedded OS.
