# BEAU OS SMMUv3 与 Guest-visible vSMMUv3 优化策略方案

## 1. 文档目标

本文给出 BEAU OS 物理 SMMUv3 优化及 guest-visible vSMMUv3 的分阶段实现方案。
目标不是移植其他项目的驱动，而是在 BEAU 现有静态分区、Stage-2、vPCI、ITS、
watchdog 和透明电源管理框架内，建立可验证、fail-closed、适合车载场景的 DMA
虚拟化路径。

方案基于以下假设：

- 首个目标平台继续使用 ARM64、GICv3/ITS 和支持 S1+S2 nested translation 的
  SMMUv3。
- VM、直通设备和 StreamID 仍由静态 DTS 策略声明。
- guest-visible vSMMUv3 首先服务于受信任 Linux VM，不作为任意客体可申请的动态
  资源。
- BEAU 继续拥有物理 SMMU、物理 StreamID、Stage-2、VMID、ITS 和设备最终控制权。
- 第一阶段不支持 ATS、PRI、PASID/SSID、SVA 和 guest-controlled Stage-2。

## 2. Clean-room 原则

实现不得复制、翻译或改写 Nebula 的 SMMUv3/vSMMU 源码。Nebula 仅用于能力面
对比，帮助识别多实例、两级 Stream Table、IRQ 驱动 EVTQ、nested translation 和
故障上报等设计方向。

实际实现应遵循以下来源顺序：

1. Arm System Memory Management Unit Architecture Specification IHI 0070。
2. BEAU 现有 Stage-2、SMMUv3、vPCI、ITS、DTS 和 PM 接口。
3. BEAU 的 ISO 26262-oriented 编码与 fail-closed 规则。
4. 平台公开 TRM、SMMU wrapper/RAS 文档和已确认的硬件 errata。

新增寄存器定义、命令编码、状态转换和测试向量都应记录规范章节或平台 TRM 来源，
不能以其他项目文件作为实现依据。

## 3. 当前问题与目标边界

### 3.1 物理 SMMUv3 当前边界

BEAU 已具备以下安全基础：

- 未分配 StreamID 默认使用 ABORT STE。
- VM CPU 与设备 DMA 复用同一 Stage-2 根表。
- STE body 先写入并同步，最后发布 valid/config word。
- CMDQ 完成后才发布软件 owner。
- EVTQ fault 能记录 StreamID、IOVA 并把 stream 置为 quarantine。
- PM 已保存并恢复 Stream Table、CMDQ、EVTQ 和 CR0 状态。

当前主要缺口：

- 无 S2P 时存在 BYPASS 兼容分支。
- 只有一个全局 SMMU 实例。
- 使用小型线性 Stream Table，软件最多记录 64 个 stream。
- CMDQ/EVTQ 各 16 项。
- EVTQ 只在 shell/health 路径轮询，故障隔离没有确定时延。
- 未完整验证 TTENDIAN、COHACC、VMID width、granule、stall model 等能力。
- 未建立 GERROR、SFM、CMDQ CERROR 和平台 RAS 的恢复状态机。
- Stage-2 更新与 SMMU IOTLB invalidation 尚未形成统一接口。

### 3.2 Guest-visible vSMMUv3 的目标

guest-visible vSMMUv3 允许指定 Linux VM 使用标准 SMMUv3 驱动管理设备 Stage-1
地址空间，同时保持 BEAU 对 Stage-2 和物理设备所有权的控制。

```text
guest device driver
    |
    | DMA IOVA
    v
guest SMMU driver
    |
    | virtual MMIO + guest CMDQ/STE/CD
    v
BEAU vSMMUv3
    - validates register writes and commands
    - maps virtual SID to physical SID
    - validates guest-owned S1 tables
    - rewrites host-owned VMID/S2 fields
    |
    v
physical SMMUv3 nested translation
    device IOVA -- guest S1 --> IPA -- BEAU S2 --> HPA
```

vSMMUv3 不是物理 SMMU 的直接透传。客体不能控制物理 StreamID、VMID、S2TTB、
物理 CMDQ、物理 EVTQ、物理 IRQ 或全局 invalidation scope。

## 4. 总体架构

### 4.1 分层

```text
+---------------------- guest-visible layer -----------------------+
| vsmmuv3_mmio      synthetic registers and access validation      |
| vsmmuv3_cmdq      bounded guest command parser                   |
| vsmmuv3_shadow    virtual SID/STE/CD and generation state        |
| vsmmuv3_fault     sanitized guest EVTQ and virtual IRQ           |
+-------------------------------+----------------------------------+
                                |
                                v
+------------------------- broker layer ----------------------------+
| map virtual SID -> physical SMMU + physical SID                  |
| compose host-owned S1+S2 STE                                     |
| scoped CFGI/TLBI/ATC operations                                  |
| assignment, reset, quarantine and PM transaction                 |
+-------------------------------+----------------------------------+
                                |
                                v
+---------------------- physical SMMUv3 layer ----------------------+
| per-instance capability, Stream Table, CMDQ, EVTQ and GERROR     |
| ABORT default, IRQ/softirq, RAS platform ops, PM restore         |
+------------------------------------------------------------------+
```

三层之间只传递已验证的结构化请求。vSMMUv3 不得构造原始物理 CMDQ entry 并直接写入
物理队列。

### 4.2 核心对象

建议新增或重构以下对象：

```text
arm_smmu_manager
    devices[MAX_SMMU_DEVICES]

arm_smmu_device
    id, base, size, irq set
    capability snapshot
    Stream Table/CMDQ/EVTQ memory
    queue lock, state lock
    state, PM epoch, counters
    platform_ops

arm_smmu_domain
    owner VMID
    CPU Stage-2 root/config generation
    attached physical streams

arm_smmu_stream
    physical SID, owner domain
    hardware state, fault state, generation
    optional vSMMU binding

arm64_vsmmu
    owner VM
    synthetic register state
    guest CMDQ/EVTQ descriptors
    allowed virtual SID map
    shadow streams and worker state
```

## 5. 物理 SMMUv3 v2

### 5.1 Fail-closed 能力探测

物理 SMMU 进入 READY 前必须验证：

- Stage-2 translation supported。
- AArch64 translation table format supported。
- 4 KiB granule supported，并与 BEAU Stage-2 一致。
- OAS 覆盖全部 Hypervisor、VM RAM、PCI BAR 和 ITS doorbell HPA。
- VMID width 能覆盖 BEAU 分配的 hardware VMID。
- Stream Table、CMDQ 和 EVTQ 能满足静态策略需要。
- table/queue endian、shareability 和 coherency 属性可明确配置。
- MMIO page 0/page 1 和所有声明 IRQ 均有效。

release 构建中，任一强制能力不满足都保持 GBPA.ABORT，并拒绝直通设备分配。
BYPASS 只能存在于显式的实验构建，且不能与安全配置共存。

### 5.2 多实例

DTS parser 应枚举全部 `arm,smmu-v3` 节点，不再只保存一个 `smmu_base/smmu_size`。
每个 passthrough device 保存以下静态关系：

```text
device policy -> smmu phandle -> physical SID -> owner VM
```

所有队列、Stream Table、锁、统计、PM 状态和 readiness 都属于具体
`arm_smmu_device`。一个实例故障不得污染其他实例的软件状态。

### 5.3 两级 Stream Table

当硬件支持两级 Stream Table 且 DTS 声明的 SID 超出线性策略容量时启用两级表。
与通用 OS 的按需分配不同，BEAU 在启动阶段扫描所有静态 SID，预分配对应 L2 页：

```text
collect declared SID set
    -> calculate required L1 indices
    -> allocate all required L2 pages
    -> zero/clean L2 pages as ABORT
    -> publish L1 descriptors
    -> CFGI_ALL + CMD_SYNC
```

运行时不分配 Stream Table 页。未声明的 L1 descriptor 保持无效；已声明但未赋权的
STE 保持 ABORT。

### 5.4 CMDQ

CMDQ 分成两个接口：

- `arm_smmu_cmd_submit()`：提交结构化、由 EL2 生成的单条命令。
- `arm_smmu_cmd_batch()`：提交同一 transaction 的有限命令集合并只做一次 sync。

队列深度由硬件能力和静态配置共同决定，建议首版使用 64 或 256 项。禁止在持有
全局 VM/设备锁时等待 CMD_SYNC。

CMDQ timeout 或 CERROR 的处理：

```text
detect timeout/CERROR
    -> stop new assignment
    -> mark SMMU DEGRADED
    -> set GBPA.ABORT when hardware accepts register update
    -> mask guest device visibility and bus mastering
    -> capture queue/register snapshot
    -> notify health/watchdog/PM
```

不能仅记录计数后继续发布 owner。

### 5.5 EVTQ、GERROR 与 RAS

物理 IRQ 顶半部只完成 ack/mask、记录时间戳并调度有界 softirq。softirq 每轮消费固定
budget，未消费完则再次调度，避免 fault storm 长时间阻塞 IRQ。

事件至少区分：

- guest Stage-1 translation/permission fault。
- host Stage-2 translation/permission fault。
- Context Descriptor/STE/table walk fault。
- 未声明或 owner 不匹配的 SID。
- EVTQ overflow。
- CMDQ、GERROR、SFM 和平台 TCU/TBU RAS。

平台私有 wrapper/RAS 通过 `arm_smmu_platform_ops` 实现：

```text
probe_caps / power_on / power_off
irq_status / irq_ack
ras_snapshot / reset_instance
```

通用驱动只消费标准化的 fault class 和 severity。

### 5.6 Stage-2 与 IOTLB 一致性

所有 Stage-2 mutation 必须使用统一 transaction，而不是由调用者自行决定是否刷新
SMMU：

```text
stage2_update_begin(vm)
    -> modify PTEs
    -> clean page-table cache lines
    -> CPU Stage-2 TLBI
    -> SMMU S2 IPA-range TLBI or VMALL fallback
    -> CMD_SYNC
stage2_update_end(vm)
```

如果 VM 绑定了多个物理 SMMU，必须在所有实例同步成功后才能发布新的映射状态。
任一实例失败时撤销 guest/device visibility。

## 6. Guest-visible vSMMUv3

### 6.1 首版暴露能力

首版向客体合成以下能力：

- Stage-1 translation。
- 4 KiB translation granule。
- AArch64 little-endian tables。
- 16-bit ASID；实际可用数量由静态上限约束。
- 线性或两级 guest Stream Table，按产品需求选择其一。
- CMDQ、EVTQ 和一个组合 virtual IRQ。
- non-stall fault model。
- 固定的虚拟 SID 集合。

首版不暴露：

- guest Stage-2、VMID 控制。
- ATS、PRI、PASID/SSID、SVA。
- stall/resume/PRI replay。
- MSI-based CMD_SYNC completion。
- guest-controlled physical performance monitor 或 RAS registers。

合成 IDR 必须与实际支持的命令、寄存器和 fault semantics 完全一致，不能先声明能力
再返回空实现。

### 6.2 虚拟 MMIO 面

建议首版实现：

```text
read-only: IDR0..IDR5, IIDR, AIDR
control:   CR0/CR0ACK, CR1, CR2, GBPA
irq:       IRQ_CTRL/ACK, GERROR/GERRORN
tables:    STRTAB_BASE/CFG
queues:    CMDQ_BASE/PROD/CONS, EVTQ_BASE/PROD/CONS
irq cfg:   EVTQ/GERROR virtual IRQ config as required by guest driver
```

每次访问必须校验 width、alignment、offset、当前 enable state 和 reserved bits。非法
写入按 SMMUv3 约束更新 virtual GERROR/CERROR；不能访问物理寄存器。

### 6.3 Guest memory 边界

guest CMDQ、EVTQ、Stream Table、Context Descriptor 和 Stage-1 page table 都是不可信
输入。访问规则：

- 只能使用 `copy_from_gpa()`/`copy_to_gpa()` 或等价的受检接口。
- 验证整个对象范围位于该 VM RAM，检查加法、乘法和 ring wrap overflow。
- 验证 base、entry size、queue size 和 table alignment。
- 禁止与 vSMMU MMIO、vGIC、共享 IPC、保留页和 Hypervisor 内存重叠。
- 每条命令一次性复制到 EL2 局部结构后再解析，避免重复读取导致 TOCTOU。
- 对需要硬件持续读取的 S1 table 页面建立 pin/reference generation，VM reset 前统一撤销。

### 6.4 Virtual SID 映射

DTS 静态声明 virtual SID 到 physical SMMU/SID 的映射：

```dts
guest-vsmmu@a100000 {
    compatible = "beau,guest-smmuv3";
    reg = <0x0 0x0a100000 0x0 0x00020000>;
    interrupts = <0x0 0x30 0x4>;
    beau,owner-vm = <3>;
    beau,physical-smmu = <&smmu0>;
    beau,stream-map = <0x10 0x0010>, <0x11 0x0018>;
    beau,max-cmdq-log2 = <6>;
    beau,max-evtq-log2 = <6>;
};
```

上例仅定义 ABI 方向，最终 binding 应单独形成文档并由 parser 严格验证。virtual SID
可以等于 physical SID，但实现始终通过映射表查询，不能依赖二者相等。

guest vFDT 中，分配给该 VM 的设备使用 virtual SID 引用 vSMMU；物理 SMMU phandle、
physical SID 和 host ITS 信息不得暴露。

### 6.5 Guest CMDQ 白名单

首版支持：

- `CMD_SYNC`
- `CFGI_STE`、有限范围的 `CFGI_ALL`
- `CFGI_CD`、`CFGI_CD_ALL`
- `TLBI_NH_ASID`
- `TLBI_NH_VA`、`TLBI_NH_VAA`
- `TLBI_NH_ALL`

初期不支持的命令返回 virtual illegal-command error。`ATC_INV` 只有在未来启用 ATS 后
才能加入。

处理规则：

1. PROD 写入只调度 vSMMU worker，不在 MMIO trap 中消费无界命令。
2. worker 每轮最多处理固定数量，例如 32 条。
3. 校验 producer/consumer wrap、queue size 和命令编码。
4. global guest invalidation 必须改写为 owner VM/domain scope，禁止影响其他 VM。
5. virtual SID 必须映射到当前 VM 拥有的 physical SID。
6. physical VMID、S2TTB 和 physical queue address 全部由 broker 填充。
7. 一批物理命令只在全部验证成功后提交；部分失败不能留下半个 batch。

### 6.6 Guest STE 语义到物理 STE 的转换

客体看到三类主要状态：

| Guest STE | Physical STE | 说明 |
|---|---|---|
| invalid/abort | ABORT | 保持 guest fault 语义 |
| guest bypass | S2-only | 只绕过 guest S1，绝不绕过 BEAU S2 |
| guest S1 enabled | S1+S2 | guest 管理 S1，BEAU 固定 S2 |

guest 只允许影响 S1 相关字段。broker 重新生成完整物理 STE：

```text
host-owned:
    physical SID, valid publication order
    S2 VMID, S2TTB, VTCR, S2 permissions
    physical table/cache/coherency attributes

guest-influenced after validation:
    S1 format and Context Descriptor pointer
    allowed S1 cache/shareability fields
    ASID and guest S1 translation configuration
```

不得用 bit mask 后直接转发 guest 原始 STE。每个允许字段都应解析到结构化 shadow
对象，再由独立 encoder 生成物理 STE。

### 6.7 S1 table-walk 地址模型

实现不得依赖当前 BEAU `IPA == HPA` 的内存布局。guest STE、Context Descriptor
和 S1 page table 中的地址必须按 SMMUv3 nested translation 的架构语义分别处理：

- guest CMDQ/EVTQ/Stream Table 是 vSMMU 软件读取的 GPA，由受检 copy 接口访问。
- physical STE 中的 Context Descriptor pointer 由 broker 根据规范要求编码，不能直接
  复制 guest pointer。
- guest CD 中的 S1 TTBR 必须验证位于 owner VM 的 IPA 空间，并确保 S1 table walk
  受到该 VM Stage-2 约束。
- physical STE 的 `S2PTW`、S2 VMID、S2TTB 和 VTCR 全部由 host 设置。
- 如果目标 SMMU/平台无法证明 guest S1 table walk 受 S2 保护，则不得暴露 vSMMU，
  也不能通过 identity mapping 兼容运行。

首版可利用 BEAU 静态 VM RAM 保证 page-table backing 不被回收，但仍要维护 table
reference/generation，用于 VM reset、内存重配和未来非 identity backing。任何
Context Descriptor 或 page-table walk 越过该 VM Stage-2 的事件都属于 host 安全故障，
不能作为普通 guest S1 page fault 处理。

### 6.8 S1+S2 激活事务

```text
guest issues CFGI_STE(vSID)
    -> snapshot guest STE/CD
    -> validate virtual SID and guest memory ownership
    -> validate S1 format, granule, address width and reserved bits
    -> pin/reference required guest table pages
    -> reserve shadow stream generation
    -> physical stream becomes ABORT and syncs
    -> broker writes host-composed STE body
    -> CFGI_STE + CMD_SYNC
    -> publish valid S1+S2 word
    -> CFGI_STE + CMD_SYNC
    -> commit shadow generation
```

失败路径必须撤销新 pin/reference，物理 stream 保持 ABORT，并向 guest 产生可诊断的
virtual command error。

### 6.9 Fault 路由

物理 EVTQ event 先由 host 分类：

```text
physical event
    |
    +--> guest S1 translation/permission fault
    |       -> sanitize SID/address/status
    |       -> append guest EVTQ
    |       -> inject vSMMU virtual IRQ
    |
    +--> S2/table ownership/unknown SID fault
    |       -> physical ABORT + quarantine
    |       -> host health/watchdog finding
    |       -> optional fatal guest EVTQ record
    |
    +--> CMDQ/GERROR/SFM/RAS
            -> instance DEGRADED or FAILED
            -> revoke affected devices
```

正常的 guest S1 page fault 不应自动永久 quarantine physical stream，否则 guest
SMMU 的 fault semantics 无法工作。只有 S2 越界、owner 冲突、非法 table walk、fault
storm 超限或 guest EVTQ 无法可靠投递时才升级为 host quarantine。

写入 guest EVTQ 时必须隐藏 HPA、physical SID、host VMID 和其他 VM 信息。guest EVTQ
满时设置 virtual overflow/GERROR，并按策略暂停对应 stream，不能覆盖未消费事件。

### 6.10 DoS 控制

- 每 VM 固定 virtual SID 数量。
- 固定 CMDQ/EVTQ 最大深度和每轮命令 budget。
- 固定 table pin/page budget。
- invalidation range 和 batch 数量有上限。
- fault、illegal command 和 IRQ 都进行速率统计。
- fault storm 达到阈值后暂停 virtual IRQ、ABORT 对应 physical stream 并通知 health。
- vSMMU worker 只能运行在配置的 service pCPU，不得挤占 RTOS exclusive pCPU。
- 禁止在持有 spinlock 时复制大量 guest memory 或等待物理 CMD_SYNC。

## 7. 并发与锁规则

每个 vSMMU 使用单一串行 command worker，保证同一 virtual CMDQ 的命令顺序。MMIO
trap 只更新受保护的寄存器快照并 kick worker。

推荐锁职责：

```text
vsmmu.reg_lock      virtual register and queue pointers
vsmmu.shadow_lock   virtual SID/STE/CD generation state
smmu.queue_lock     physical CMDQ producer and sync state
smmu.state_lock     physical stream owner/fault/PM state
```

禁止同时持有 guest vSMMU lock 和 physical SMMU queue lock。worker 在调用 broker 前
先生成不可变 request；broker 完成后再以 generation 校验提交 shadow 状态。generation
不匹配时回滚物理 stream 到 ABORT，并重新处理新命令。

## 8. VM 生命周期与 PM

### 8.1 VM reset/stop

```text
stop guest vCPU and vSMMU worker
    -> hide vPCI/BAR and mask guest IRQ
    -> clear physical bus mastering
    -> set all bound physical streams to ABORT
    -> CFGI/TLBI/CMD_SYNC
    -> release pinned guest table references
    -> reset virtual registers/queues/shadow state
```

VM 再启动时先恢复 S2-only ownership；只有 guest 重新提交合法 STE/CD 后才进入 S1+S2。

### 8.2 Suspend/resume

Suspend 前冻结 guest CMDQ producer，并等待当前有界 batch 完成。物理 SMMU PM 由 host
统一执行，vSMMU 只保存 virtual register/shadow generation。

Resume 顺序：

```text
physical SMMU ABORT-only ready
    -> restore domain S2
    -> restore S2-only stream ownership
    -> validate saved guest table references/generation
    -> rebuild S1+S2 physical STE
    -> restore guest CMDQ/EVTQ state
    -> enable virtual IRQ and worker
    -> thaw guest vCPU
```

任何无法证明一致性的恢复都保持 ABORT，并使 PM transaction 进入 FAILED。

## 9. 可观测性

新增 `vsmmustat`，显示全部 vSMMU 实例，并至少包含：

- virtual register enable state、CMDQ/EVTQ producer/consumer。
- command processed/rejected/budget-reschedule/timeout。
- virtual SID -> physical SMMU/SID -> owner VM 映射。
- guest STE state、physical STE state 和 shadow generation。
- S1 fault、S2 fault、illegal command、overflow、fault storm。
- guest EVTQ 投递和 virtual IRQ 数量。
- reset/PM rebuild/quarantine 原因。

`smmustat` 继续显示物理实例和硬件真值。两条命令必须能交叉验证软件 shadow 与
physical STE，不得以 guest-visible 状态代替物理状态。

## 10. 实施拆分

### PR 1：物理 SMMU strict mode

- release 禁止 BYPASS。
- 完整能力验证、CR1/CR2 和 GBPA.ABORT 初始化。
- 验证：无 S2P/4K/OAS 时 assignment 失败且 STE 保持 ABORT。

### PR 2：EVTQ/GERROR IRQ

- DTS IRQ 解析、IRQ handler、有界 softirq。
- event 分类、自动 quarantine、health finding。
- 验证：故障无需 shell 触发即可在分配的 FTTI 内关闭 DMA。

### PR 3：多实例与两级 Stream Table

- `arm_smmu_manager/device`。
- 静态 SID-to-instance policy。
- 启动期 L2 table 预分配。
- 验证：一个实例故障不影响其他实例。

### PR 4：vSMMU register 与 queue skeleton

- synthetic IDR/CR/queue registers。
- guest CMDQ/EVTQ memory validation。
- bounded command worker、illegal-command handling。
- 验证：Linux SMMU driver probe，恶意 queue 参数全部 fail closed。

### PR 5：S1+S2 broker

- virtual SID map、guest STE/CD parser、shadow model。
- ABORT/S2-only/S1+S2 转换。
- scoped CFGI/TLBI 和 generation rollback。
- 验证：设备 DMA 只能到达 guest S1 与 BEAU S2 共同允许的页面。

### PR 6：Fault virtualization

- physical EVTQ 到 guest EVTQ 的分类和脱敏。
- virtual IRQ、overflow 和 fault storm policy。
- 验证：S1 fault 只通知 guest；S2 fault 自动 host quarantine。

### PR 7：Reset、PM 与性能闭环

- VM reset、watchdog restart、STR rebuild。
- CMDQ batch、queue depth和 cache maintenance 优化。
- 完整回归、压力和延迟基线。

## 11. 测试矩阵

### 11.1 单元测试

- register width/alignment/reserved-bit matrix。
- ring wrap、full/empty、producer jump 和 overflow。
- command decoder 白名单与所有非法 opcode。
- SID map、owner conflict、generation rollback。
- guest STE/CD 地址、长度、alignment、reserved fields。
- event classification 和 HPA/physical SID 脱敏。

### 11.2 QEMU 故障注入

- S2P/4K/OAS capability 缺失。
- CMDQ full、CERROR、CMD_SYNC timeout。
- EVTQ overflow 和 unknown SID。
- guest queue 位于越界 GPA 或保留区。
- guest 修改 VMID/S2TTB/physical SID。
- guest global TLBI、超大 range、fault storm。
- VM reset、watchdog restart、连续 STR。

### 11.3 实机验证

- 多 SMMU 并行 DMA。
- non-coherent table/queue cache maintenance。
- PCIe bus master、MSI/MSI-X 和 ITS doorbell。
- TCU/TBU RAS、power-domain off/on。
- RTOS latency 与 Linux DMA 压力并行。

## 12. 验收标准

1. release 配置中不存在 physical BYPASS STE。
2. guest bypass 只能转换成 physical S2-only，不能绕过 BEAU Stage-2。
3. guest 无法影响其他 VM 的 SID、VMID、S2TTB、TLBI 或 fault 信息。
4. S1 fault 能可靠送达 guest EVTQ；S2/owner fault 在 FTTI 内自动 quarantine。
5. CMDQ/EVTQ 恶意输入不会产生无界循环、越界访问或未授权物理命令。
6. 任意 assignment、CFGI、TLBI、reset 或 PM 失败后，physical stream 最终为 ABORT。
7. 连续 1,000 次 VM reset/设备重绑定和连续 1,000 次 STR 后，shadow、owner、STE、
   IOTLB 和 ITS 状态一致。
8. vSMMU 压力测试不突破 RTOS 的 IRQ latency 和调度预算。

## 13. 后续可选能力

以下功能只有在首版完成安全闭环后再评估：

- ATS 与 ATC invalidation。
- PRI、stall/resume 和 page request service。
- PASID/SSID、SVA。
- 多 guest vSMMU 实例。
- guest 两级 Context Descriptor Table。
- live device reassignment 和 post-launched VM。

这些能力每增加一项，都必须同步扩展 synthetic IDR、命令白名单、fault model、DoS
budget、PM 状态和故障注入，不能只增加寄存器或 opcode。

