# ARM64 Platform pCPU Pool Binding Principles

本文档定义 `arch/arm64/platform/*/platform.dts` 中 pCPU pool 与 VM
`cpu-affinity` 的绑定原则。目标是把实时隔离、Linux 启动吞吐和调度可诊断性
固化为静态 DTS 合同，而不是依赖运行时经验调参。

## 配置入口

平台 DTS 是唯一静态来源：

```dts
hypervisor {
	sched {
		strict-placement;
		exclusive-cpupool {
			pcpus = <0 1 2 3>;
			policy = "bvt";
		};
		shared-cpupool {
			pcpus = <4 5 6 7>;
			policy = "cbs";
			period = <2000>;
			budget = <1000>;
		};
	};
};
```

VM 的 `cpu-affinity` 是有序表，不是无序 bitmap：

```dts
cpu-affinity = <2 4 6 7>;
```

含义：

```text
entry[0] -> vcpu0 -> pCPU2
entry[1] -> vcpu1 -> pCPU4
entry[2] -> vcpu2 -> pCPU6
entry[3] -> vcpu3 -> pCPU7
```

## Pool 语义

```text
exclusive-cpupool
    |
    +--> 低抖动 pCPU
    +--> 优先承载每个 VM 的 vcpu0/BSP
    +--> 不承载 secondary vCPU
    +--> 推荐 policy = "bvt"

shared-cpupool
    |
    +--> 多 VM 共享 pCPU
    +--> 承载 secondary vCPU，或 exclusive 不足时承载剩余 vcpu0
    +--> 每个 shared pCPU 的 VM/vCPU fan-in 有上限
    +--> 推荐 policy = "cbs"
```

## Strict Placement 规则

启用 `strict-placement` 后，解析 DTS 时必须 fail closed。当前规则由
`sdk/bsp/arm64/platform_dts.c` 校验。

1. `exclusive-cpupool` 和 `shared-cpupool` 不能重叠。
2. 两个 pool 合集必须覆盖所有平台 pCPU。
3. `vcpu0` 优先绑定 exclusive pCPU。
4. 如果 exclusive pCPU 数量少于 VM 数量，剩余 `vcpu0` 才允许落到 shared pCPU。
5. exclusive pCPU 上最多一个 vCPU，且该 vCPU 必须是某个 VM 的 `vcpu0`。
6. secondary vCPU 不允许落到 exclusive pCPU。
7. shared pCPU 上最多承载 3 个 vCPU。
8. shared pCPU 上最多承载 3 个 VM 的 vCPU。
9. 任何 VM 的 `cpu-affinity` 必须是有序配置；不能退化为只靠 bitmap 推断。

## QEMU 8 核 4OS 参考绑定

当前 QEMU 4OS 推荐绑定：

```text
pCPU0  exclusive  VM0:vcpu0
pCPU1  exclusive  VM1:vcpu0
pCPU2  exclusive  VM2:vcpu0
pCPU3  exclusive  VM3:vcpu0

pCPU4  shared     VM0:vcpu1, VM2:vcpu1
pCPU5  shared     VM1:vcpu1, VM3:vcpu1
pCPU6  shared     VM2:vcpu2, VM3:vcpu2
pCPU7  shared     VM2:vcpu3, VM3:vcpu3
```

对应 DTS：

```dts
vm@0 { cpu-affinity = <0 4>; };
vm@1 { cpu-affinity = <1 5>; };
vm@2 { cpu-affinity = <2 4 6 7>; };
vm@3 { cpu-affinity = <3 5 6 7>; };
```

这个布局的性质：

```text
VM BSP/vcpu0 latency
    -> 独占 BVT pCPU
    -> 不被 Linux secondary burst 直接抢占

Linux SMP throughput
    -> secondary vCPU 分散到 CBS+ shared pool
    -> 每个 shared pCPU fan-in <= 2，低于上限 3

Admission/debug
    -> DTS 解析阶段先校验
    -> 配错立即 panic，避免启动后才表现为 CBS latency tail
```

## 绑定流程

```text
platform.dts
    |
    v
parse /hypervisor/sched
    |
    +--> validate pool overlap
    +--> validate all pCPUs covered
    |
    v
parse /vm/* cpu-affinity
    |
    +--> preserve ordered vCPU -> pCPU map
    |
    v
strict-placement validation
    |
    +--> vcpu0 exclusive-first
    +--> exclusive contains vcpu0 only
    +--> shared fan-in <= 3 VMs / 3 vCPUs
    |
    +--> fail closed on violation
    |
    v
create_vm() / create_vcpu()
```

## 配置检查清单

修改平台 DTS 时按以下顺序检查：

1. 先定 pool：哪些 pCPU 是 exclusive，哪些是 shared。
2. 再定 `vcpu0`：每个 VM 的 `cpu-affinity[0]` 优先使用不同 exclusive pCPU。
3. 再放 secondary vCPU：只放 shared pCPU。
4. 统计每个 shared pCPU：
   - vCPU 数量不能超过 3；
   - VM 数量不能超过 3。
5. 确认 shared pool policy 使用 `cbs`，并设置 `period/budget`。
6. CBS 始终按本地 EDF deadline 选择 runnable server。
7. 改完后至少执行：

```sh
make PLATFORM=qemu -j$(nproc)
```

## 典型错误

错误：secondary vCPU 放到 exclusive pCPU。

```dts
vm@2 { cpu-affinity = <2 0 6 7>; };
```

原因：`vcpu1 -> pCPU0`，但 pCPU0 是 exclusive，只允许承载某个 VM 的 `vcpu0`。

错误：exclusive 足够时，`vcpu0` 仍放到 shared pCPU。

```dts
vm@3 { cpu-affinity = <7 5 6 4>; };
```

原因：如果 exclusive pCPU 数量已经覆盖 VM 数量，`vcpu0` 不应 fallback 到 shared。

错误：shared pCPU fan-in 超限。

```text
pCPU4: VM0:vcpu1, VM1:vcpu1, VM2:vcpu1, VM3:vcpu1
```

原因：shared pCPU 同时承载 4 个 VM / 4 个 vCPU，超过上限 3。

## 运行时确认

启动后用 shell 确认：

```text
vmstat <vmid>
schedstat
vcpus
```

重点看：

```text
affinity:vcpu0:pcpuX
pcpu_mode:exclusive/shared
CBS latency histogram
```

如果出现 Linux BSP/vcpu0 落在 shared pCPU，优先检查 DTS 的
`cpu-affinity[0]` 和 `/hypervisor/sched`，不要先调 CBS budget。
