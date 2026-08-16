#!/usr/bin/env python3
"""Build the ARM64 VMOS runtime on the current seL4 source tree."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Mapping, NamedTuple, Sequence


ROOT = Path(__file__).resolve().parent
VMOS_ROOT = ROOT / "vmos"
DEFAULT_BUILD_ROOT = ROOT / "build" / "vmos"
TARGET_TRIPLE = "aarch64-none-elf"
SEL4_PYTHON_MODULES = ("jinja2", "yaml", "ply", "pyfdt")


class BuildError(RuntimeError):
    """A build precondition or output validation failed."""


def parse_int(value: str) -> int:
    """Parse a non-negative decimal or base-prefixed integer."""
    try:
        parsed = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must not be negative")
    return parsed


def parse_cmake_definition(value: str) -> tuple[str, str]:
    """Parse KEY=VALUE supplied for a platform-specific CMake setting."""
    key, separator, setting = value.partition("=")
    if not separator or not key or not setting:
        raise argparse.ArgumentTypeError("CMake definitions must use KEY=VALUE")
    return key, setting


class BoardConfig(NamedTuple):
    """Static ARM64 platform inputs needed by seL4 and the Microkit loader."""

    name: str
    kernel_platform: str
    gcc_cpu: str
    loader_link_address: int
    cpus: int
    extra_cmake: tuple[tuple[str, str], ...] = ()

    @classmethod
    def default(cls) -> "BoardConfig":
        return cls(
            name="qemu_virt_aarch64",
            kernel_platform="qemu-arm-virt",
            gcc_cpu="cortex-a53",
            loader_link_address=0x70000000,
            cpus=4,
            extra_cmake=(("QEMU_MEMORY", "2048"), ("KernelArmExportPTMRUser", "ON")),
        )

    def validate(self) -> None:
        for label, value in (
            ("board name", self.name),
            ("kernel platform", self.kernel_platform),
            ("GCC CPU", self.gcc_cpu),
        ):
            if not value.strip():
                raise ValueError(f"{label} must not be empty")
        if self.loader_link_address % 0x1000:
            raise ValueError("loader link address must be page aligned")
        if self.cpus < 1:
            raise ValueError("CPU count must be at least one")
        keys = [key for key, _ in self.extra_cmake]
        if len(keys) != len(set(keys)):
            raise ValueError("extra CMake definition keys must be unique")


class BuildLayout(NamedTuple):
    """All mutable build and SDK paths for one board configuration."""

    root: Path
    work: Path
    sel4_build: Path
    sel4_install: Path
    sdk: Path
    sdk_config: Path

    @classmethod
    def create(cls, root: Path, board: BoardConfig, config: str) -> "BuildLayout":
        root = root.resolve() / board.name / config
        work = root / "work"
        layout = cls(
            root=root,
            work=work,
            sel4_build=work / "sel4" / "build",
            sel4_install=work / "sel4" / "install",
            sdk=root / "sdk",
            sdk_config=root / "sdk" / "board" / board.name / config,
        )
        for path in (
            layout.work,
            layout.sel4_build,
            layout.sel4_install,
            layout.sdk / "bin",
            layout.sdk_config / "elf",
            layout.sdk_config / "include",
            layout.sdk_config / "lib",
        ):
            path.mkdir(parents=True, exist_ok=True)
        return layout


class BuildCommand(NamedTuple):
    """One source-local component command and its isolated environment."""

    command: tuple[str, ...]
    cwd: Path
    env: Mapping[str, str] | None = None


def kernel_definitions(board: BoardConfig, config: str) -> dict[str, str]:
    """Return the complete, fail-closed seL4 ARM64 hypervisor configuration."""
    board.validate()
    if config not in ("debug", "release"):
        raise ValueError(f"unsupported VMOS configuration: {config}")

    definitions = {
        "KernelSel4Arch": "aarch64",
        "KernelIsMCS": "ON",
        "KernelRootCNodeSizeBits": "17",
        "LibSel4UseThreadLocals": "OFF",
        "KernelNumDomains": "1" if board.cpus > 1 else "16",
        "KernelNumDomainSchedules": "100",
        "KernelMaxNumNodes": str(board.cpus),
        "KernelArmExportPCNTUser": "ON",
        "KernelArmHypervisorSupport": "ON",
        "KernelArmVtimerUpdateVOffset": "OFF",
        "KernelAllowSMCCalls": "ON",
        "KernelPlatform": board.kernel_platform,
        "KernelDebugBuild": "ON" if config == "debug" else "OFF",
        "KernelPrinting": "ON" if config == "debug" else "OFF",
        "KernelVerificationBuild": "OFF",
    }
    if config == "debug":
        definitions["HardwareDebugAPI"] = "ON"
    definitions.update(board.extra_cmake)
    return definitions


def cmake_args(
    board: BoardConfig,
    config: str,
    build_dir: Path,
    install_dir: Path,
) -> list[str]:
    """Create a deterministic CMake configure command for the current tree."""
    definitions = kernel_definitions(board, config)
    args = [
        "cmake",
        "-GNinja",
        f"-S{ROOT}",
        f"-B{build_dir.resolve()}",
        f"-DCMAKE_INSTALL_PREFIX={install_dir.resolve()}",
        f"-DPYTHON3={Path(sys.executable).absolute()}",
        f"-DCROSS_COMPILER_PREFIX={TARGET_TRIPLE}-",
    ]
    args.extend(f"-D{key}={value}" for key, value in sorted(definitions.items()))
    return args


def run_checked(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
) -> None:
    """Run one build step and propagate its exact failing command."""
    display = " ".join(str(part) for part in command)
    print(f"+ {display}", flush=True)
    try:
        subprocess.run(
            [str(part) for part in command],
            cwd=cwd,
            env=dict(env) if env is not None else None,
            check=True,
        )
    except subprocess.CalledProcessError as error:
        raise BuildError(f"command failed with exit status {error.returncode}: {display}") from error


def require_tools(tools: Sequence[str]) -> None:
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        raise BuildError("missing required build tools: " + ", ".join(missing))


def require_python_modules(modules: Sequence[str]) -> None:
    missing = [module for module in modules if importlib.util.find_spec(module) is None]
    if missing:
        raise BuildError(
            "missing required seL4 Python modules: "
            + ", ".join(missing)
            + "; install with: python3 -m pip install ./tools/python-deps"
        )


def configure_and_build_sel4(
    board: BoardConfig,
    config: str,
    build_dir: Path,
    install_dir: Path,
    jobs: int,
) -> None:
    """Configure, build, and install the immutable seL4 input tree."""
    if jobs < 1:
        raise ValueError("job count must be at least one")
    build_dir.mkdir(parents=True, exist_ok=True)
    install_dir.mkdir(parents=True, exist_ok=True)
    run_checked(cmake_args(board, config, build_dir, install_dir))
    run_checked(("cmake", "--build", str(build_dir.resolve()), "--parallel", str(jobs)))
    run_checked(("cmake", "--install", str(build_dir.resolve())))


def required_artifacts(layout: BuildLayout) -> tuple[Path, ...]:
    elf = layout.sdk_config / "elf"
    lib = layout.sdk_config / "lib"
    return (
        layout.sdk / "bin" / "microkit",
        elf / "sel4.elf",
        elf / "loader.elf",
        elf / "monitor.elf",
        elf / "initialiser.elf",
        lib / "libmicrokit.a",
        lib / "microkit.ld",
    )


def verify_artifacts(layout: BuildLayout) -> None:
    missing = [path for path in required_artifacts(layout) if not path.is_file()]
    if missing:
        raise BuildError("missing VMOS build artifacts:\n" + "\n".join(str(path) for path in missing))


def copy_tree_contents(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise BuildError(f"missing directory: {source}")
    for path in source.rglob("*"):
        if not path.is_file():
            continue
        target = destination / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def copy_required(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise BuildError(f"missing file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_sel4_sdk(layout: BuildLayout) -> None:
    """Create the SDK inputs consumed unchanged by all Microkit components."""
    copy_required(
        layout.sel4_install / "bin" / "kernel.elf",
        layout.sdk_config / "elf" / "sel4.elf",
    )
    copy_required(
        layout.sel4_build / "generated" / "invocations_all.json",
        layout.sdk_config / "invocations_all.json",
    )
    copy_required(
        layout.sel4_build / "gen_headers" / "plat" / "machine" / "platform_gen.json",
        layout.sdk_config / "platform_gen.json",
    )
    include = layout.sdk_config / "include"
    legacy_include_dirs = tuple(
        layout.sel4_install / tree / "include"
        for tree in (
            "kernel_Config",
            "libsel4",
            "libsel4/sel4_Config",
            "libsel4/autoconf",
        )
    )
    merged_include_dir = layout.sel4_install / "libsel4" / "include"
    if all(path.is_dir() for path in legacy_include_dirs):
        for path in legacy_include_dirs:
            copy_tree_contents(path, include)
    elif merged_include_dir.is_dir():
        copy_tree_contents(merged_include_dir, include)
    else:
        raise BuildError(
            "seL4 install has neither legacy nor merged libsel4 include layout"
        )
    copy_required(VMOS_ROOT / "VERSION", layout.sdk / "VERSION")


def parse_constant_header(output: str) -> dict[str, int]:
    """Parse the deliberately small `name: expression` preprocessor format."""
    constants: dict[str, int] = {}
    for line in output.splitlines():
        if ": " not in line:
            continue
        name, expression = line.split(": ", 1)
        expression = expression.strip().strip("( )")
        if "-" in expression:
            left, right = expression.split("-", 1)
            value = int(left.strip(), 0) - int(right.strip(), 0)
        else:
            value = int(expression, 0)
        constants[name] = value
    return constants


def generate_constant_json(layout: BuildLayout) -> None:
    include = layout.sdk_config / "include"
    preprocessor = f"{TARGET_TRIPLE}-cpp"
    for name in ("object_sizes", "address_space_constants"):
        header = VMOS_ROOT / "tool" / "microkit" / f"{name}.h"
        command = (preprocessor, "-E", "-P", f"-I{include}", str(header))
        print("+ " + " ".join(command), flush=True)
        try:
            result = subprocess.run(command, check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as error:
            diagnostic = error.stderr.strip() if error.stderr else "preprocessor failed"
            raise BuildError(f"failed generating {name}.json: {diagnostic}") from error
        constants = parse_constant_header(result.stdout)
        if not constants:
            raise BuildError(f"no constants generated from {header}")
        destination = layout.sdk_config / f"{name}.json"
        destination.write_text(json.dumps(constants, sort_keys=True), encoding="utf-8")


def component_commands(layout: BuildLayout, board: BoardConfig) -> tuple[BuildCommand, ...]:
    cargo_target = layout.work / "cargo"
    cargo_env = {
        **os.environ,
        "CARGO_TARGET_DIR": str(cargo_target),
    }
    initialiser_env = {
        **cargo_env,
        "RUSTC_BOOTSTRAP": "1",
        "SEL4_PREFIX": str(layout.sel4_install),
    }

    common_make = {
        **os.environ,
        "ARCH": "aarch64",
        "TARGET_TRIPLE": TARGET_TRIPLE,
        "LLVM": "False",
        "GCC_CPU": board.gcc_cpu,
        "SEL4_SDK": str(layout.sdk_config),
    }
    loader_env = {
        **common_make,
        "BOARD": board.name,
        "LINK_ADDRESS": hex(board.loader_link_address),
        "BUILD_DIR": str(layout.work / "loader"),
    }
    monitor_env = {
        **common_make,
        "BUILD_DIR": str(layout.work / "monitor"),
    }
    library_env = {
        **common_make,
        "BUILD_DIR": str(layout.work / "libmicrokit"),
    }
    return (
        BuildCommand(("cargo", "test", "--locked", "-p", "microkit-tool"), VMOS_ROOT, cargo_env),
        BuildCommand(("cargo", "build", "--release", "--locked", "-p", "microkit-tool"), VMOS_ROOT, cargo_env),
        BuildCommand(
            (
                "cargo",
                "build",
                "--release",
                "--locked",
                "--target",
                "aarch64-unknown-none",
                "-p",
                "initialiser",
            ),
            VMOS_ROOT,
            initialiser_env,
        ),
        BuildCommand(("make", "all"), VMOS_ROOT / "loader", loader_env),
        BuildCommand(("make", "all"), VMOS_ROOT / "monitor", monitor_env),
        BuildCommand(("make", "all"), VMOS_ROOT / "libmicrokit", library_env),
    )


def build_components(layout: BuildLayout, board: BoardConfig) -> None:
    for directory in ("loader", "monitor", "libmicrokit"):
        (layout.work / directory).mkdir(parents=True, exist_ok=True)
    for command in component_commands(layout, board):
        run_checked(command.command, cwd=command.cwd, env=command.env)

    cargo_target = layout.work / "cargo"
    copy_required(cargo_target / "release" / "microkit", layout.sdk / "bin" / "microkit")
    copy_required(
        cargo_target / "aarch64-unknown-none" / "release" / "initialiser",
        layout.sdk_config / "elf" / "initialiser.elf",
    )
    copy_required(layout.work / "loader" / "loader.elf", layout.sdk_config / "elf" / "loader.elf")
    copy_required(layout.work / "monitor" / "monitor.elf", layout.sdk_config / "elf" / "monitor.elf")
    copy_required(
        layout.work / "libmicrokit" / "libmicrokit.a",
        layout.sdk_config / "lib" / "libmicrokit.a",
    )
    copy_required(VMOS_ROOT / "libmicrokit" / "microkit.ld", layout.sdk_config / "lib" / "microkit.ld")
    copy_tree_contents(VMOS_ROOT / "libmicrokit" / "include", layout.sdk_config / "include")
    (layout.sdk / "bin" / "microkit").chmod(0o755)


def verify_hypervisor_config(layout: BuildLayout) -> None:
    header = layout.sdk_config / "include" / "kernel" / "gen_config.h"
    if not header.is_file():
        raise BuildError(f"missing generated kernel configuration: {header}")
    contents = header.read_text(encoding="utf-8")
    if re.search(
        r"^#define\s+CONFIG_ARM_HYPERVISOR_SUPPORT\s+1\s*$",
        contents,
        flags=re.MULTILINE,
    ) is None:
        raise BuildError("seL4 was not built with CONFIG_ARM_HYPERVISOR_SUPPORT")


def argument_parser() -> argparse.ArgumentParser:
    default = BoardConfig.default()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", default=default.name)
    parser.add_argument("--kernel-platform", default=default.kernel_platform)
    parser.add_argument("--gcc-cpu", default=default.gcc_cpu)
    parser.add_argument("--loader-link-address", type=parse_int, default=default.loader_link_address)
    parser.add_argument("--cpus", type=int, default=default.cpus)
    parser.add_argument("--config", choices=("debug", "release"), default="debug")
    parser.add_argument("--cmake-def", action="append", type=parse_cmake_definition, default=[])
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_ROOT)
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    return parser


def board_from_arguments(args: argparse.Namespace) -> BoardConfig:
    default = BoardConfig.default()
    is_default_platform = (
        args.board == default.name and args.kernel_platform == default.kernel_platform
    )
    extra_cmake = dict(default.extra_cmake) if is_default_platform else {}
    extra_cmake.update(dict(args.cmake_def))
    board = BoardConfig(
        args.board,
        args.kernel_platform,
        args.gcc_cpu,
        args.loader_link_address,
        args.cpus,
        tuple(sorted(extra_cmake.items())),
    )
    board.validate()
    return board


def main(argv: Sequence[str] | None = None) -> int:
    args = argument_parser().parse_args(argv)
    try:
        board = board_from_arguments(args)
        require_tools(
            (
                "cmake",
                "ninja",
                "make",
                "xmllint",
                "cargo",
                "rustc",
                f"{TARGET_TRIPLE}-gcc",
                f"{TARGET_TRIPLE}-cpp",
            )
        )
        require_python_modules(SEL4_PYTHON_MODULES)
        layout = BuildLayout.create(args.build_dir, board, args.config)
        configure_and_build_sel4(
            board,
            args.config,
            layout.sel4_build,
            layout.sel4_install,
            args.jobs,
        )
        copy_sel4_sdk(layout)
        verify_hypervisor_config(layout)
        generate_constant_json(layout)
        build_components(layout, board)
        verify_artifacts(layout)
    except (BuildError, ValueError) as error:
        print(f"build_vmos.py: error: {error}", file=sys.stderr)
        return 1
    print(f"VMOS SDK: {layout.sdk}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
