# secure-os

> **仅供学习与研究参考。** 本项目不是可直接部署的安全操作系统，未经过
> 生产环境、安全认证或功能安全验证，请勿用于真实敏感数据、关键基础设施或
> 任何安全攸关场景。

`secure-os` 是一个**基于 seL4 实现**的 ARM64 hypervisor 学习项目，用于理解
微内核、静态系统构建、EL2 虚拟化、VCPU、二阶段地址转换以及运行时组件之间
的协作方式。

项目保留 seL4 内核及其必要构建代码，并在 `vmos/` 中提供独立的 hypervisor
组件。当前默认平台为 QEMU ARM64 `virt`，只支持 AArch64。

## 项目定位

本项目的目标是提供一条可以阅读、构建、启动和调试的学习路径：

```text
QEMU ARM64 virt / EL2
        |
        v
VMOS loader
        |
        v
seL4 microkernel
        |
        v
CapDL initialiser --> Microkit monitor --> Rust runtime console / PD / VCPU
```

当前内容适合用于：

- 学习 seL4 的对象、能力、调度上下文和用户态启动过程；
- 学习 ARM64 EL2、VCPU 和 stage-2 mapping 的基本组织方式；
- 研究静态系统描述如何生成内核对象与 capability；
- 在 QEMU 中验证 loader、seL4、initialiser、monitor 和 protection domain；
- 编写 Rust `no_std` 运行时组件。

当前内容不等同于完整产品，不包含成熟的 guest 设备模拟、安全策略、升级机制、
密钥管理、攻击面审计或生产运维能力。seL4 上游的形式化验证结论也不能自动扩展
到本仓库的 VMOS 集成代码、构建配置或运行时组件。

## 目录结构

```text
include/、src/、libsel4/   seL4 内核、ABI 和必要接口
tools/                    seL4 构建工具
vmos/                     ARM64 hypervisor 组件
vmos/loader/              启动并进入 seL4 EL2
vmos/initialiser/         CapDL initialiser
vmos/monitor/             protection domain 与 VCPU fault monitor
vmos/libmicrokit/         Microkit protection-domain runtime
vmos/tool/microkit/       系统描述、对象构造和镜像打包工具
vmos/support/             Rust 构建辅助与源码完整性校验
vmos/qemu-smoke/          Rust runtime console 与 QEMU smoke system
Makefile.vmos             当前目录下的统一构建入口
build_vmos.py             兼容现有 seL4/Microkit 流程的 legacy 构建器
```

VMOS 已内置所需源码，构建、测试和运行均不依赖相邻的 Microkit 仓库。

## 支持范围

- 架构：AArch64 / ARM64 only
- 默认 board：`qemu_virt_aarch64`
- 默认 seL4 platform：`qemu-arm-virt`
- 默认 CPU：4 × Cortex-A53
- 默认配置：`debug`
- hypervisor：seL4 ARM hypervisor support / EL2
- 新增项目功能：Rust

已内置的 seL4 或 Microkit 实现文件视为只读基线，不修改其功能逻辑；如需补充说明，
只添加注释。后续新的 secure-os/VMOS 功能应使用 Rust 编写。

## 构建依赖

- Python 3 与项目 seL4 Python dependencies
- CMake、Ninja、GNU Make
- `xmllint`
- `aarch64-none-elf-` GCC/binutils
- Rust/Cargo 与 `aarch64-unknown-none` target
- `qemu-system-aarch64`，用于运行 QEMU smoke system

默认会优先使用：

```text
~/.venvs/secure-os-vmos/bin/python3
```

可通过 `VMOS_VENV` 或 `PYTHON` 覆盖 Python 环境。

## 构建与测试

在当前 `secure-os` 目录执行：

```shell
make -f Makefile.vmos test
make -f Makefile.vmos verify-source
make -f Makefile.vmos build
```

默认公开 ELF 输出位于：

```text
build/vmos/qemu_virt_aarch64/debug/elf/sel4.elf
build/vmos/qemu_virt_aarch64/debug/elf/loader.elf
build/vmos/qemu_virt_aarch64/debug/elf/monitor.elf
build/vmos/qemu_virt_aarch64/debug/elf/initialiser.elf
```

完整的 Microkit 兼容 SDK 保留在：

```text
build/vmos/qemu_virt_aarch64/debug/sdk/
```

该深层 SDK 是构建兼容状态，不是主要发布路径。

## QEMU 启动

构建、打包并启动 ARM64 smoke system：

```shell
make -f Makefile.vmos qemu
```

成功启动后可以看到：

```text
LDR|INFO|CPU0: CurrentEL=EL2
Booting all finished, dropped to user space
MON|INFO: Microkit Monitor started!
VMOS runtime console ready
vmos>
```

runtime console 支持以下学习与测试命令：

```text
help                 显示命令列表
version              显示 runtime 信息
ping                 返回 pong，验证控制台存活
echo <text>          回显文本
selftest             运行内置确定性测试
stats                显示成功、未知及溢出命令计数
```

控制台由 QEMU PL011 IRQ 33 中断驱动，使用固定 128 字节缓冲区，不使用堆内存。
使用 `Ctrl-a x` 退出 QEMU。

只生成 QEMU 镜像而不启动：

```shell
make -f Makefile.vmos qemu-image
```

生成文件：

```text
build/vmos/qemu_virt_aarch64/debug/qemu/loader.img
```

## 源码完整性

`vmos/UPSTREAM_SHA256` 保存内置实现文件的 SHA-256 基线：

```shell
make -f Makefile.vmos verify-source
```

校验过程只访问当前仓库，不访问外部或相邻源码目录。

## 安全说明

- 本项目只用于学习、实验和原型验证；
- 不承诺不存在缺陷、漏洞、数据损坏或隔离逃逸；
- 不提供生产级安全更新、兼容性或长期维护保证；
- 使用者需要自行评估代码、工具链、配置和运行环境；
- 如需构建真实 seL4 产品，请参考 seL4 官方文档、支持渠道和认证流程。

## License

seL4 及内置第三方源码保留各自的版权和许可证声明。仓库总体许可信息见
[LICENSE.md](LICENSE.md)。删除文档目录不改变任何现有源码文件的许可证义务。
