# BEAU OS Android/AAOS QEMU 启动可行性方案

## 1. 目标与范围

本文探索在 BEAU OS 的 ARM64 QEMU `virt` 平台中，以 LK 作为客体
bootloader 启动 Android/AAOS 的可行方案。首要目标是打通可重复的无图形启动链：

```text
QEMU -> BEAU OS -> LK -> Android Kernel -> first-stage init -> Android userspace
```

本文是设计方案，不表示当前代码已经支持 Android 启动。方案优先验证启动 ABI、
镜像装载、块设备、动态分区和串口可观测性；显示、音频、输入、车辆 HAL 和完整
AAOS UI 属于后续阶段。

以下内容不在首阶段范围内：

- 模拟某一真实 SoC 的 UFS、PMIC、时钟、复位和专有外设；
- 直接运行绑定真实 SoC 固定 MMIO 地址的 LK 镜像；
- 把大型 Android 分区镜像嵌入 BEAU ELF；
- 在没有可信根和回滚存储时宣称已达到量产 Verified Boot 安全等级。

## 2. 可行性结论

该目标可行，但前提是使用面向 QEMU `virt` 的 LK，而不是把硬件平台 LK 分区镜像
直接交给 BEAU 执行。推荐的职责划分是：

```text
host files
  |- BEAU ELF
  |- QEMU-virt LK raw binary
  `- Android GPT disk image
          |
          v
QEMU virt
  |- loads BEAU as EL2 payload
  `- exposes file-backed virtio-blk-pci
          |
          v
BEAU OS
  |- creates Android VM stage-2 address space
  |- loads LK raw binary and passes the VM FDT in x0
  |- exposes vCPU, vGICv3, timer, PSCI, console and vPCI
  `- assigns the virtio-blk PCI function to the Android VM
          |
          v
LK
  |- probes FDT, PCI and virtio-blk
  |- reads GPT and selects the boot slot
  |- verifies and parses Android boot images
  |- assembles ramdisks, bootconfig and final DTB
  `- enters the Android kernel with x0 = final DTB
          |
          v
Android
  |- first-stage init creates dynamic partitions from super
  |- mounts system/vendor/product and userdata
  `- starts framework and reports sys.boot_completed=1
```

BEAU 应保持为通用 hypervisor 和资源所有者，不应在 EL2 内实现 Android boot image、
GPT、AVB 或动态分区解析。Android 启动策略属于 LK，文件系统和动态分区策略属于
Android userspace。该边界可以避免把版本变化频繁且安全敏感的 Android 解析逻辑
放进 EL2 可信计算基。

## 3. 当前 BEAU 基线与缺口

BEAU 当前 ARM64 raw-image 启动链已经具备以下基础：

- `platform.dts` 描述 VM RAM、vCPU、镜像、入口、vGIC、console 和 vPCI policy；
- `rawimage_loader()` 检查目标范围，把裸镜像和 FDT 复制到客体 RAM；
- `arch_vm_prepare_bsp()` 建立 EL1 初始状态，并在静态 vFDT 路径中通过 `x0`
  传递 FDT；
- QEMU 平台已有 vGICv3、PSCI、vPL011、virtio-console、vPCI 和 SMMUv3 路径；
- 仓库保留的 LK 裸镜像是 QEMU `virt` 构建，包含 PL011、GIC 和 virtio 相关代码，
  可作为 LK 适配起点。

当前仍有以下关键缺口：

1. 当前 QEMU VM 拓扑没有启用专用 Android/LK VM 配置。
2. BEAU 只负责装载 LK 裸镜像，没有给 LK 提供 Android 启动盘。
3. 现有 virtio-blk proxy 是依赖后端 VM 的小容量验证设备，不适合作为 Android
   启动盘。
4. 尚未证明现有 LK 包含完整的 Android boot image、AVB、A/B 和 DTBO 启动流程。
5. 当前 QEMU 总内存和客体 RAM window 面向轻量 RTOS/Linux 回归，不能直接假定
   满足 Android。
6. 当前 console transport 按 RTOS/Linux 类型选择；LK 和 Android 共处一个 VM
   时需要一致、显式的 console policy。

因此，BEAU 能够提供启动 LK 所需的 CPU、内存和基础虚拟硬件，但完整 Android
启动仍需要 LK、块设备和 Android guest 三侧协同适配。

## 4. 镜像与存储输入

LK 必须按 Android boot image header version 和设备配置决定实际读取哪些分区，
不能通过固定偏移猜测镜像内容。

| 输入 | 必要性 | 读取者 | 作用 |
|---|---|---|---|
| `lk.bin` | 必需 | BEAU | QEMU `virt` 可执行的 LK 裸镜像，不是带分区头的刷机镜像 |
| GPT disk image | 推荐方案必需 | QEMU/LK/Android | 为 LK 和 Android 提供统一的持久块设备与 by-name 分区 |
| `boot.img` | 必需或版本相关 | LK | Android Kernel、generic ramdisk 或相应 boot header 元数据 |
| `vendor_boot.img` | GKI/新 boot header 常见 | LK | vendor ramdisk、DTB、vendor bootconfig 等设备相关启动数据 |
| `init_boot.img` | 由 Android 版本决定 | LK | 独立 generic ramdisk；存在时不能继续假定 ramdisk 位于 `boot.img` |
| `dtb.img` | 布局相关 | LK | 未放入 `vendor_boot.img` 时提供基础 DTB |
| `dtbo.img` | 设备树 overlay 启用时 | LK | 在基础 DTB 上应用板级 overlay |
| `vbmeta*.img` | AVB 启用时 | LK/Android | boot 和动态分区的信任链元数据 |
| `super.img` | 动态分区设备必需 | Android first-stage init | 容纳 system、vendor、product、system_ext、odm 等逻辑分区 |
| `userdata.img` | 完整 userspace 启动必需 | Android | 可写 `/data`；也可以由首次启动格式化空分区 |
| `metadata.img` | 加密或 Virtual A/B 相关 | Android | metadata、checkpoint 或 snapshot 状态，按产品配置决定 |
| `misc`/boot control | A/B 或 recovery 相关 | LK/Android | slot、boot attempt、recovery message 等持久状态 |

其他 modem、TEE、PMIC 或协处理器固件不是通用 QEMU Android 启动链的默认输入。
只有当 QEMU guest 明确提供相应设备并且 Android guest 需要该固件时，才应把它们
加入方案。

### 4.1 推荐的 GPT 启动盘

推荐在构建或运行准备阶段生成本地 Android GPT disk image，而不是让 BEAU 同时
管理许多分区文件：

```text
android-qemu-disk.img
  |- boot[_a/_b]
  |- vendor_boot[_a/_b]
  |- init_boot[_a/_b]       (optional)
  |- dtbo[_a/_b]            (optional)
  |- vbmeta[_a/_b]
  |- vbmeta_system[_a/_b]   (optional)
  |- super
  |- metadata               (optional)
  |- misc                   (optional)
  `- userdata
```

准备工具必须：

- 从镜像 header 和构建元数据判断 boot header version、A/B 和动态分区布局；
- 校验每个 payload 不超过目标 GPT partition；
- 在写入块设备前把 Android sparse image 正确转换为 raw 数据；
- 保持分区名与 guest 的 fstab、AVB chain 和 boot control 配置一致；
- 输出完整的镜像清单、大小、hash 和 GPT 布局，便于复现；
- 把生成盘放在本地输出目录，不提交大型或含用户数据的镜像。

为了让回归可重复，可以使用只读基础盘加临时 writable overlay。`userdata`、
`metadata`、A/B boot attempt 等需要写入的分区不能直接使用全盘只读模式。

### 4.2 存储方案比较

| 方案 | 优点 | 限制 | 结论 |
|---|---|---|---|
| QEMU file-backed `virtio-blk-pci` 经 BEAU vPCI/SMMU 分配 | 容量大、性能合理、host 文件易替换，符合现有 QEMU PCI 验证方向 | LK 和 Android Kernel 需要 virtio-pci/blk；BEAU 需要完整的 PCI BAR、IRQ 和 DMA 隔离 | 首选 |
| BEAU virtio-mmio proxy + backend VM | 可复用 BEAU proxy ownership 模型 | 当前是小容量 RAM backend；依赖 backend VM 启动，吞吐和恢复路径更复杂 | 后续服务化方案 |
| QEMU loader 把所有分区放进 RAM | 实现早期 kernel smoke 简单 | 浪费大量 RAM，没有真实块设备，不能自然支持 dynamic partition 和持久写入 | 仅限临时 POC |
| 在 QEMU 中模拟特定 SoC UFS/eMMC | 可以运行绑定该硬件的 LK | 需要实现大量 SoC MMIO、IRQ、DMA、时钟和复位语义 | 不推荐 |

## 5. 启动 ABI 与 VM 设备模型

### 5.1 BEAU 到 LK

建议保持现有 ARM64 raw-image ABI：

```text
EL1 entry = configured LK entry GPA
x0        = QEMU-virt guest FDT GPA
x1..x3    = 0
MMU       = off
GIC       = virtual GICv3
PSCI      = HVC conduit
```

BEAU 在启动 vCPU 前必须验证 LK、FDT 和预留工作区全部位于 Android VM RAM，且
互不重叠。LK 必须从 FDT 发现 RAM、CPU、GIC、timer、UART、PCI host bridge 和
virtio block device，不得依赖真实 SoC 固定地址。

### 5.2 LK 到 Android Kernel

LK 负责建立符合 ARM64 Linux boot protocol 的最终状态：

```text
read GPT and boot slot
    |
    v
verify vbmeta chain according to boot mode
    |
    v
parse boot/vendor_boot/init_boot headers by version
    |
    v
place kernel + ramdisks + bootconfig in validated RAM ranges
    |
    v
select DTB and apply validated DTBO
    |
    v
update /chosen, initrd range, bootargs and reserved-memory
    |
    v
clean caches, disable MMU as required, x0 = final DTB
    |
    v
enter Android Kernel
```

LK 不能在 header、partition、AVB、DTBO 或内存范围校验失败后继续跳转。所有失败
日志应包含阶段、分区、slot、offset、size 和错误码。

### 5.3 Console

首阶段建议 LK 和 Android Kernel 共用 vPL011：

- LK 使用 PL011 输出启动阶段和失败原因；
- Android Kernel 使用 `earlycon`/`ttyAMA` 保留从解压到 init 的连续日志；
- BEAU shell 通过 `vcon` 进入同一 VM console。

这要求未来把 console transport 从简单的 RTOS/Linux 推断改为显式 VM policy，
或者让 Android VM 明确选择 vPL011。待 Android 基础启动稳定后，再评估切换到
virtio-console 以支持高吞吐日志。

### 5.4 CPU、内存与中断

- Android VM RAM 必须根据 kernel 解压窗口、ramdisk、DTB、LK heap 和 Android
  userspace 实测确定，不能沿用轻量 guest 默认值；
- QEMU `-m`、BEAU host RAM、各 VM static RAM 和 image staging 地址必须一起调整，
  并保持无重叠；
- vCPU 数量先从 1 开始验证，再启用 PSCI `CPU_ON` 和 SMP；
- virtio-blk PCI MSI/MSI-X、vITS 和 SMMUv3 StreamID ownership 必须在设备暴露前
  完整建立；任何 DMA ownership 错误都应阻止设备对 Android VM 可见。

## 6. LK 所需能力

QEMU 版本 LK 至少需要以下模块：

1. QEMU `virt` FDT platform、ARM generic timer、GICv3 和 PSCI。
2. PL011 console，以及可选的 virtio-console。
3. PCI ECAM 枚举、BAR、MSI/MSI-X 和 virtio-pci transport。
4. virtio-blk 读写、flush、容量、超时和错误恢复。
5. GPT parser，包括 primary/backup header、CRC、范围和分区名检查。
6. Android boot image header version dispatcher，不能固定为某一 header layout。
7. `vendor_boot`、`init_boot`、bootconfig、DTB 和 DTBO 组合逻辑。
8. A/B slot 选择、boot attempt 和 recovery policy；非 A/B 配置必须显式处理。
9. libavb 或等价 AVB 实现，以及明确的 locked/unlocked 状态。
10. 有界内存分配、payload overlap 检查、cache maintenance 和 Linux handoff。

QEMU platform glue 与 Android boot policy 应分层。前者只负责设备发现和 IO，后者
负责 partition、slot、AVB 和 boot image 语义，以便未来替换存储 transport 而不
重写 Android 启动策略。

## 7. Android Guest 适配要求

即使 LK 能进入 Kernel，Android userspace 仍需满足以下条件：

- Kernel 内建或在 first-stage ramdisk 中提供 PCI、virtio-pci、virtio-blk、GPT、
  device-mapper、dm-linear、dm-verity、loop 和所需文件系统支持；
- guest DTB 与 BEAU 暴露的 vGIC、PSCI、timer、RAM、UART、PCI ECAM 和 interrupt
  mapping 一致；
- first-stage fstab、bootconfig 和 uevent/device aliases 能把 virtio disk 映射为
  正确的 `/dev/block/by-name/*`；
- `super` metadata、logical partition group、slot suffix 和 AVB chain 一致；
- `userdata` 文件系统、加密策略和 `metadata` 配置适合 QEMU bring-up；
- 不可用的真实硬件服务必须通过 product 配置禁用、延迟或替换，避免 framework
  因等待硬件 HAL 无限阻塞。

首阶段以 headless Android 为成功目标。virtio-gpu、display composer、input、audio、
network 和车辆 HAL 应在 `sys.boot_completed=1` 稳定后分别引入。

## 8. 分阶段实施与验证

每一阶段必须保留串口日志和明确的 pass/fail 证据；后续阶段不能掩盖前一阶段的
失败。

### P0：BEAU 启动 LK

实施内容：恢复一个可选的 QEMU LK VM，装载 QEMU-virt `lk.bin`，传递 guest FDT，
并通过 vPL011 输出。

通过标准：

- BEAU 报告 VM image range、entry 和 FDT 均合法；
- LK 打印版本、RAM、GIC、timer 和 UART 信息；
- LK prompt 或受控停机可重复出现，无同步异常和 stage-2 fault。

### P1：LK 识别 Android 启动盘

实施内容：QEMU 附加 file-backed virtio-blk-pci，BEAU 将设备唯一分配给 Android
VM，LK 完成 PCI、virtio-blk 和 GPT 枚举。

通过标准：

- BEAU 显示一致的 vPCI、StreamID、SMMU domain 和 IRQ owner；
- LK 报告正确容量，并列出预期 GPT partition names；
- 越界 GPT、错误 CRC、设备缺失和 IO timeout 均受控失败，不发生越界访问。

### P2：LK 进入 Android Kernel

实施内容：解析 boot image family，组合 ramdisk、bootconfig、DTB/DTBO，在 bring-up
模式下建立明确的 AVB 状态并进入 Kernel。

通过标准：

- LK 输出选定 slot、header version、各 payload range 和最终 DTB 地址；
- Kernel 出现 `Booting Linux`、early console、GIC、PSCI、RAM 和 virtio-blk 日志；
- Kernel 能读取所有 boot-critical partitions。

### P3：first-stage init 与动态分区

实施内容：修正 guest fstab/bootconfig/device aliases，使 first-stage init 从
`super` 创建并挂载 logical partitions。

通过标准：

- first-stage init 识别 `super` metadata 和正确 slot；
- system、vendor、product 等必需分区成功映射和挂载；
- AVB/dm-verity 状态与 bootloader 传入状态一致；
- SELinux policy 或设备节点错误有明确日志，不通过关闭全部安全机制隐藏问题。

### P4：Headless Android 完整启动

实施内容：提供可写 userdata/metadata，处理缺失硬件服务并启动 Android framework。

通过标准：

- `init` 无持续 crash loop；
- `zygote`、`system_server` 和关键 native services 存活；
- `getprop sys.boot_completed` 返回 `1`；
- 冷启动、受控重启和重复启动结果一致。

### P5：AAOS 功能扩展

逐项增加 virtio-gpu/display、input、audio、network、车辆 HAL 和持久化策略。每个
设备都应单独建立 ownership、DMA、IRQ、reset 和故障注入验证，不能以 UI 出现代替
底层隔离验证。

## 9. 安全与失败模式

### 9.1 Bring-up 模式

早期验证可以使用测试 key 或明确的 unlocked/orange boot state，以便定位 guest
适配问题，但必须满足：

- 日志明确显示当前不是 locked/green 量产状态；
- 不通过伪造 `androidboot.verifiedbootstate=green` 绕过 Android 检查；
- AVB 被临时放宽时记录放宽项、原因和恢复条件；
- 调试盘不承载真实用户数据或生产密钥。

### 9.2 生产目标

生产路径必须验证 boot、DTBO、vbmeta chain 和 dynamic partitions，并提供可信根、
rollback index 持久存储、A/B attempt 状态和恢复策略。在 QEMU 没有受保护 rollback
storage 时，只能验证协议和失败流程，不能声称具备真实防回滚能力。

### 9.3 必测失败场景

- LK image、Android boot header 或 payload 被截断；
- GPT header/entry CRC 错误、partition 越界或重复；
- boot/vendor_boot/init_boot header version 不匹配；
- DTBO 无效或应用后 FDT 超出工作区；
- AVB signature、descriptor、rollback index 或 slot verification 失败；
- virtio-blk 缺失、容量变化、短读、超时或 reset；
- kernel、ramdisk、DTB、LK heap 或 reserved-memory 重叠；
- vPCI、IRQ 或 SMMU ownership 未完成；
- first-stage fstab 与 GPT/by-name 不一致；
- Android VM reset 后遗留旧 virtqueue、IRQ、slot attempt 或 writable overlay 状态。

所有失败都应停在当前 ownership owner 内，禁止继续发布未验证的设备、分区或启动
状态。

## 10. 后续实现的代码边界

本文不修改下列代码；实际实现进入独立设计和审批后，预计涉及：

| 代码树/层 | 候选位置 | 职责 |
|---|---|---|
| BEAU QEMU policy | `arch/arm64/platform/qemu/platform.dts` | Android VM RAM、vCPU、LK module、console、vPCI 和设备 ownership |
| BEAU image symbols | `arch/arm64/platform/qemu/platform.S` | 只嵌入小型 LK/DTB；不嵌入 Android 分区 |
| BEAU configuration | `arch/arm64/guest/Kconfig`, `Makefile` | 可选 Android/LK scenario 和镜像依赖 |
| BEAU launcher | `scripts/kick.py` | Android disk 参数、QEMU block/PCI device 和 host 侧输入校验 |
| BEAU DTS parser | `sdk/bsp/arm64/platform_dts.c` | 仅在现有 schema 无法表达显式 console/storage policy 时扩展 |
| BEAU raw loader | `sdk/bsp/boot/guest/rwloader.c` | 保持通用 LK raw loading，不加入 Android partition parser |
| BEAU retained image | `sdk/imgs/lk.bin` | 仅在 QEMU LK 完成对应构建和验证后更新 |
| BEAU regression | `scripts/regress.py` | 经测试方案审批后增加阶段化成功和失败回归 |
| LK QEMU platform | LK source tree 的 QEMU/FDT/PCI/virtio 层 | 硬件发现和 block IO |
| LK Android policy | LK source tree 的 Android boot/AVB 层 | GPT、slot、boot image、DTBO、AVB 和 kernel handoff |
| Android guest | Kernel config、DT、fstab、bootconfig、product 配置 | 虚拟硬件驱动、动态分区和 userspace bring-up |

`sdk/imgs/lk.bin` 应是已在对应 LK source tree 中构建和验证过的 retained artifact。
大型 GPT disk、`super` 和 `userdata` 必须留在本地/CI artifact storage，不进入 BEAU
源码提交。

## 11. 推荐的首个实现切片

首个切片只完成 P0 和 P1，不同时追求 Android framework：

1. 恢复独立、可选的 QEMU LK VM，不影响默认多 VM 回归拓扑。
2. 固定 BEAU -> LK 的 entry/FDT/console ABI。
3. 生成只包含小型测试分区的 GPT disk。
4. 通过 QEMU virtio-blk-pci、BEAU vPCI/SMMU 和 LK virtio driver 读取测试分区。
5. 对正确 GPT、损坏 GPT、缺盘、短读和 reset 建立证据。
6. P1 稳定后再引入 Android boot images，避免把 PCI/DMA 问题与 Android parser
   问题混在同一调试阶段。

这是风险最低、诊断边界最清晰的起点。直接从完整 Android userspace 启动失败反推
EL2、LK、storage、DT、Kernel 和 init 的问题，定位成本会显著更高。

## 12. 参考资料

- [BEAU OS architecture](../../../beau/BEAU-os.md)
- [BEAU OS coding and validation specification](../../../sdk.md)
- [Android bootloader overview](https://source.android.com/docs/core/architecture/bootloader)
- [Android boot image header](https://source.android.com/docs/core/architecture/bootloader/boot-image-header)
- [Android dynamic partitions](https://source.android.com/docs/core/ota/dynamic_partitions)
- [Android Verified Boot](https://source.android.com/docs/security/features/verifiedboot/verified-boot)
- [Android Verified Boot flow](https://source.android.com/docs/security/features/verifiedboot/boot-flow)

---

Hustle Embedded OS.
