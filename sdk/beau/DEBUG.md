# BEAU OS Debug Guide

本手册面向 BEAU OS Hypervisor 的启动、VM、调度、Watchdog、Trusty TEE、IRQ、内存与
virtio-proxy 故障。所有命令在 BEAU host shell 中执行；命令仅读取状态，除非命令名称
明确表示控制操作。

## 1. 基本原则

先保留现场，再缩小范围，最后才执行会改变 VM 生命周期的操作。

1. 记录串口中首次出现的 `ERR`、`SWT:`、异常 ESR/FAR 和对应时间戳。
2. 连续执行两次 `vmstat`、`ps`、`schedstat`，间隔 5 至 10 秒；这些命令的利用率和部分
   histogram 使用相邻两次采样的差值。
3. 对 WDT 事件先执行 `swtdbg <vmid>`（Z3L）或 `hwtdbg`（ZR2L）；它们读取不清除重启前现场。
4. 再用 `irqstat`、`vmexitstat`、`virtiostat`、`smmustat` 判断是调度、定时器、IRQ、设备还是
   Stage-2/DMA 链路的问题。
5. 只有确认需要人工恢复时才使用 `pm reboot <vmid>`。不要把手工重启当作 WDT 证据的替代品。

常用的第一组命令：

```text
version
vmstat
ps
schedstat
irqstat
```

## 2. 启动与日志

### 2.1 启动阶段

正常启动按顺序可见 Hypervisor、GIC、pCPU、各 VM 镜像装载以及 `Kick vCPU`。对于 Linux VM，
还应看到 guest console 的 verification 成功消息。启动故障先按最后一条有效日志定位：

| 最后日志 | 优先检查 |
|---|---|
| `Load KERNEL` 前停止 | 镜像嵌入、DTB、物理内存布局 |
| `Kick vCPU` 后无 guest 输出 | vCPU 状态、GIC/vUART、客体 DTB |
| console verification 失败 | vUART 路径、guest 串口配置、vCPU 是否持续运行 |
| `HCALL ... ret:-95` | HVC 号、guest flag、Hypervisor HVC 表 |
| `SWT:` | 先按第 6 节保留 WDT 现场 |

### 2.2 Host 日志

```text
dmesg
dmesg 200
loglevel
```

`dmesg` 是 host 内存日志环；它适合关联不同 pCPU 的时间线。不要仅按日志出现顺序判断因果：
多 CPU 输出可交错，应以时间戳和 VM/vCPU/pCPU 标识关联。

## 3. VM 与 vCPU

### 3.1 `vmstat`

```text
vmstat
```

`vmstat` 是 VM 故障的入口。重点查看：

- VM 生命周期和配置的 `cpu-affinity`。
- 每个 vCPU 的 pCPU、状态、timer 和调度诊断标志。
- `SWT:status`、`cause`、`age`、`restart`、`fail`、`recovery`。
- 非 ALIVE 或正在恢复时的 `SWT:diag wait`、expected/started/stalled vCPU mask。

vCPU utilization 是相邻两次 `vmstat` 的 scheduler runtime 差值。首次显示 `--` 是正常的；
某 VM 的 total 可超过 100%，因为它是该 VM 各 running vCPU 的总和。利用率不能证明 guest
forward progress，WDT heartbeat 才是该判断的依据。

### 3.2 `vcpus` 和 `ps`

```text
vcpus
ps
```

`vcpus` 用于确认 vCPU 到 pCPU 的实际绑定以及 switch 信息。`ps` 同时列出 idle、shell、WDT、
TEE worker 和 vCPU thread；连续两次执行可看线程 CPU 占用与运行时间。

排查原则：

- vCPU 长时间 RUNNING 而 heartbeat age 增长：客体可能卡在 EL1 或不能产生其 watchdog HVC。
- vCPU 已 PAUSED 但 WDT restart 仍失败：检查 scheduler thread 是否真正完成 switch-out。
- pCPU 的 idle 线程长期没有运行、而共享池上有多个客体 vCPU：结合 `schedstat` 检查调度压力。

## 4. Scheduler

### 4.1 `schedstat`

```text
schedstat
```

`schedstat` 显示每个 pCPU 的 scheduler policy、busy%、runqueue 与 BVT/RTDS/CBS 诊断；
CBS histogram 在第一次调用时是累计值，第二次及以后才是窗口增量。

在 WDT quiesce 故障中，重点观察承载目标 vCPU 的 pCPU：

- busy% 是否持续接近 100%。
- runqueue 是否持续有积压。
- CBS 是否出现 depleted、overrun 或大于等于 5 ms 的 latency tail。
- 该 pCPU 的 `ps` 是否长期由一个 guest vCPU 或 helper 占用。

`schedstat` 不能直接显示 quiesce ACK generation；需要用 `vmstat` 的 `wait` mask、WDT 现场和
pCPU/vCPU affinity 将现象定位到相应 scheduler 上下文。

## 5. Interrupt、定时器和 VM exit

### 5.1 `irqstat`

```text
irqstat
```

检查 host IRQ 计数是否推进、guest vIRQ raise-to-LR 和 LR-to-EOI latency 是否异常。Linux 有
arch_timer IRQ 并不等于 softirq 和 watchdog worker 一定推进；必须同时看 WDT age、vCPU 状态和
IRQ 统计。

### 5.2 `vmexitstat`

```text
vmexitstat
```

该命令统计同步 VM-exit handler 的次数与 handler wall-time，用于比较 HVC、MMIO、sysreg 等
EL2 处理成本。物理 IRQ exit、向量保存/恢复以及后续调度不包含在这个时间范围内，因此不能把
它解读为完整 EL1/EL2 往返延迟。

## 6. VM Watchdog 与自动重启

### 6.1 检测信号和原因

VM WDT 依赖客体调用 `HC_VM_WDT_KICK`，而不是普通 VM-exit。支持 VM-wide heartbeat 与
per-vCPU heartbeat；后者只监控 RUNNING vCPU。

常见日志：

```text
SWT:    VM1 restart cause:timeout age:25011ms attempt:1/5
SWT:    VM3 restart cause:guestvcpu age:25250ms attempt:1/5
```

`timeout` 表示 VM-wide heartbeat 没有在阈值内推进；`guestvcpu` 表示 per-vCPU 模式中至少一个
已纳入 expected mask 的 vCPU heartbeat 停止。`age` 是检测时距上次有效 heartbeat 的时间，
不是重启耗时。

QEMU 默认 timeout 为 25000 ms，最多尝试 5 次。一次失败也会消耗一次 attempt，因此
`attempt:5/5` 后该 VM 实例不会继续自动恢复。

### 6.2 恢复状态机

```text
detected stuck
  -> QUIESCING: pause all online vCPUs and wait for scheduler ACK
  -> RESETTING: queue a WDT-owned cold reset to the VM BSP pCPU
  -> BSP pCPU idle: reset architecture/device state, reload image, start BSP
  -> VERIFYING: wait for a fresh heartbeat
  -> verified, retry, or bounded failure
```

重启设计为 fail-closed：在旧 vCPU 的 host stack 尚未切出之前，不能重载 VM 镜像。故障日志
`quiesce-timeout` 表示恢复没有到达 RESETTING，也没有执行镜像重载。

### 6.3 `wait:0x...` 的含义

```text
SWT:    VM1 restart failed cause:quiesce-timeout wait:0x2
```

`wait` 是该 VM 尚未完成 quiesce 的 **vCPU ID 位图**，不是 errno，也不是 pCPU 位图。

| `wait` | 含义 |
|---|---|
| `0x0` | 所有在线 vCPU 都已 quiesced，可以开始 cold restart |
| `0x1` | vCPU0 尚未完成 quiesce ACK |
| `0x2` | vCPU1 尚未完成 quiesce ACK |
| `0x5` | vCPU0 和 vCPU2 尚未完成 quiesce ACK |
| `0xffffffffffffffff` | pause 请求本身无效；检查 VM 状态或恢复时序 |

`uint64_t` 位图最大值为 `0xffffffffffffffff`，即 `18446744073709551615ULL`。

一个 vCPU 的 quiesce 需要同时满足：vCPU 状态为 PAUSED，目标 scheduler thread 已 BLOCKED，
并且目标 pCPU 的 idle context 发布了相同 generation 的 ACK。后者要求目标 pCPU 已切到 idle、
该 vCPU 不再是 current object 且 `be_blocking` 已清除。

### 6.4 共享 pCPU 的典型案例

QEMU DTS 的 `cpu-affinity` 按 vCPU ID 排列。例如：

```text
VM1: <1 5>       -> vCPU0 on pCPU1, vCPU1 on pCPU5
VM3: <3 5 6 7>   -> vCPU0 on pCPU3, vCPU1 on pCPU5
```

因此 VM1 和 VM3 同时出现 `wait:0x2` 时，二者共同指向 pCPU5 上的 vCPU1 quiesce ACK 路径；
不能把它误判为两个无关的 guest reset 失败。应优先采集：

```text
vmstat
vcpus
ps
schedstat
swtdbg 1
swtdbg 3
```

判断顺序：

1. 从 `wait` 找出未确认的 vCPU ID。
2. 以 VM 的 `cpu-affinity` 或 `vcpus` 找到目标 pCPU。
3. 检查该 pCPU 是否被 guest、helper 或高调度压力长期占用。
4. 检查 WDT 现场中的 `pcpu-owner`、scheduler 状态、pending request/IRQ 与 vCPU wait latency。
5. 若多个 VM 的相同 vCPU ID 映射到同一 pCPU，先按公共 scheduler/quiesce 路径排查。

### 6.5 WDT 现场保留

Z3L：

```text
swtdbg <vmid>
```

ZR2L：

```text
hwtdbg
```

超时转换发生后、重启之前，系统保留选择 vCPU 的 GPR、异常寄存器、guest/host stack、pCPU owner、
pending request/IRQ、IRQ 与 virtio 汇总，以及最终 recovery 结果。读取不清除；无事件时分别显示
`swtdbg: no watchdog timeout events` 或 `hwtdbg: no watchdog timeout events`。

## 7. Trusty TEE liveness

QEMU 平台中，BEAU host 的 `TEE-core` worker 每 10 秒向 Trusty LK 发起私有 fast SMC heartbeat。
Trusty 仅返回确认值，不在 LK console 打印周期日志；结果由 BEAU host daemon log 输出：

```text
TEE: ack: LIVE seq:217
TEE: ack: DEAD seq:217
```

`seq` 是 BEAU OS 本次 heartbeat 的单调序号。`LIVE` 表示 SMC 返回预期确认值
`0x42454155`（ASCII `BEAU`）；`DEAD` 表示调用没有返回该值。它只反映 BEAU EL2 至 Trusty
SMC handler 的往返可用性，不等价于客体 Linux/Zephyr 的 WDT heartbeat，也不会触发 VM WDT
重启。

若出现 `DEAD`，保留相邻的 TF-A/Trusty/BEAU 串口日志，并确认 SMC FID 路由、Trusty 镜像和
secure monitor 是否仍在运行。不要按字符逐字发送 SMC；一次 heartbeat 只应是一条完整 SMC 调用。

## 8. 内存、IOMMU 与设备

### 8.1 `memstat` 和 `walkpt`

```text
memstat
walkpt <vmid> <ipa>
devmap
```

`memstat` 显示 EL2 Stage-1 和 VM Stage-2 页表池使用量与 ownership。`walkpt` 只读解析指定
IPA 的各级 Stage-2 descriptor；用于确认映射存在、页属性正确且没有错误归属。`devmap` 给出
host Stage-1 与各 VM Stage-2 映射总览。

### 8.2 `smmustat`、`pcistat` 和 `virtiostat`

```text
smmustat
pcistat
virtiostat
```

DMA/直通故障必须同时验证 CPU MMIO、DMA 和 IRQ 三条路径。仅能读取 BAR 不代表 DMA 或 MSI/LPI
链路已建立。`smmustat` 检查 SMMU/ITS queue、STE 和 event fault；`pcistat` 检查 host/vPCI、BAR、
MSI-X 与 StreamID ownership；`virtiostat` 检查 proxy device、queue、backend、吞吐和延迟。

## 9. PM、性能与 trace

```text
pmstat
pm ...
pmustat start
pmustat dump
trace status
```

`pmstat` 用于检查 VM STR/reboot transaction 和 topology gate。若 WDT 已处于 RESETTING，先看
`pmstat` 确认是否有冲突的 PM/reset transaction，再进行人工操作。`pmustat` 由 Hypervisor
拥有 PMU，按 `start`、`stop`、`reset`、`dump` 控制；`trace` 用于按需采集 pCPU trace，不应用
高频 trace 替代 WDT 现场。

## 10. 最小排障清单

### VM 无输出或疑似卡死

```text
dmesg 200
vmstat
vcpus
ps
irqstat
```

### WDT 超时或重启失败

```text
vmstat
swtdbg <vmid>
vcpus
ps
schedstat
irqstat
pmstat
```

先保留现场，再判断是 guest heartbeat 停止、vCPU stall、quiesce ACK 未完成、reset queue 失败，
还是重启后未获得新 heartbeat。

### virtio 或直通设备不可用

```text
vmstat
virtiostat
pcistat
smmustat
irqstat
```

---

Hustle Embedded OS.
