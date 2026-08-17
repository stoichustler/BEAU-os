import argparse
import importlib.util
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPOSITORY_ROOT / "build_vmos.py"


def load_build_module():
    spec = importlib.util.spec_from_file_location("build_vmos", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {MODULE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = load_build_module()

    def test_parse_int_accepts_hexadecimal_and_decimal(self):
        self.assertEqual(self.build.parse_int("0x70000000"), 0x70000000)
        self.assertEqual(self.build.parse_int("4096"), 4096)

    def test_parse_int_rejects_invalid_and_negative_values(self):
        for value in ("invalid", "-1"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    self.build.parse_int(value)

    def test_default_board_is_qemu_arm64_hypervisor(self):
        board = self.build.BoardConfig.default()
        self.assertEqual(board.name, "qemu_virt_aarch64")
        self.assertEqual(board.kernel_platform, "qemu-arm-virt")
        self.assertEqual(board.gcc_cpu, "cortex-a53")
        self.assertEqual(board.loader_link_address, 0x70000000)
        self.assertEqual(board.cpus, 4)

    def test_board_validation_rejects_invalid_fields(self):
        valid = self.build.BoardConfig.default()
        invalid = (
            self.build.BoardConfig("", valid.kernel_platform, valid.gcc_cpu, valid.loader_link_address, valid.cpus),
            self.build.BoardConfig(valid.name, "", valid.gcc_cpu, valid.loader_link_address, valid.cpus),
            self.build.BoardConfig(valid.name, valid.kernel_platform, "", valid.loader_link_address, valid.cpus),
            self.build.BoardConfig(
                valid.name,
                valid.kernel_platform,
                valid.gcc_cpu,
                valid.loader_link_address + 1,
                valid.cpus,
            ),
            self.build.BoardConfig(valid.name, valid.kernel_platform, valid.gcc_cpu, valid.loader_link_address, 0),
        )
        for board in invalid:
            with self.subTest(board=board):
                with self.assertRaises(ValueError):
                    board.validate()

    def test_debug_kernel_definitions_enable_arm64_hypervisor(self):
        definitions = self.build.kernel_definitions(self.build.BoardConfig.default(), "debug")
        expected = {
            "KernelSel4Arch": "aarch64",
            "KernelIsMCS": "ON",
            "KernelArmHypervisorSupport": "ON",
            "KernelPlatform": "qemu-arm-virt",
            "KernelDebugBuild": "ON",
            "KernelPrinting": "ON",
        }
        self.assertEqual({key: definitions[key] for key in expected}, expected)

    def test_release_kernel_definitions_disable_debug_output(self):
        definitions = self.build.kernel_definitions(self.build.BoardConfig.default(), "release")
        self.assertEqual(definitions["KernelDebugBuild"], "OFF")
        self.assertEqual(definitions["KernelPrinting"], "OFF")

    def test_kernel_definitions_reject_unknown_configuration(self):
        with self.assertRaises(ValueError):
            self.build.kernel_definitions(self.build.BoardConfig.default(), "benchmark")

    def test_cmake_args_use_current_tree_and_aarch64_toolchain(self):
        build_dir = REPOSITORY_ROOT / "build" / "test-vmos"
        install_dir = build_dir / "install"
        args = self.build.cmake_args(
            self.build.BoardConfig.default(), "debug", build_dir, install_dir
        )
        self.assertIn("-GNinja", args)
        self.assertIn(f"-S{REPOSITORY_ROOT}", args)
        self.assertIn("-DCROSS_COMPILER_PREFIX=aarch64-none-elf-", args)

    def test_cmake_args_preserve_virtual_environment_python(self):
        build_dir = REPOSITORY_ROOT / "build" / "test-vmos"
        with tempfile.TemporaryDirectory() as temporary:
            virtual_python = Path(temporary) / "bin" / "python3"
            virtual_python.parent.mkdir()
            virtual_python.symlink_to(self.build.sys.executable)
            with mock.patch.object(
                self.build.sys, "executable", str(virtual_python)
            ):
                args = self.build.cmake_args(
                    self.build.BoardConfig.default(),
                    "debug",
                    build_dir,
                    build_dir / "install",
                )
            self.assertIn(f"-DPYTHON3={virtual_python}", args)

    def test_python_module_check_reports_all_missing_dependencies(self):
        with self.assertRaises(self.build.BuildError) as raised:
            self.build.require_python_modules(
                ("argparse", "vmos_module_that_does_not_exist")
            )
        message = str(raised.exception)
        self.assertIn("vmos_module_that_does_not_exist", message)
        self.assertIn("tools/python-deps", message)

    def test_custom_platform_does_not_inherit_qemu_definitions(self):
        arguments = self.build.argument_parser().parse_args(
            (
                "--board",
                "custom_arm64",
                "--kernel-platform",
                "custom-platform",
                "--cmake-def",
                "KernelARMPlatform=custom",
            )
        )
        board = self.build.board_from_arguments(arguments)
        definitions = dict(board.extra_cmake)
        self.assertNotIn("QEMU_MEMORY", definitions)
        self.assertNotIn("KernelArmExportPTMRUser", definitions)
        self.assertEqual(definitions["KernelARMPlatform"], "custom")


class ArtifactLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = load_build_module()
        cls.board = cls.build.BoardConfig.default()

    def test_layout_create_makes_work_install_and_sdk_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout = self.build.BuildLayout.create(
                Path(temporary), self.board, "debug"
            )
            self.assertEqual(
                layout.root,
                Path(temporary).resolve() / "qemu_virt_aarch64" / "debug",
            )
            expected = (
                layout.work,
                layout.sel4_build,
                layout.sel4_install,
                layout.sdk / "bin",
                layout.sdk_config / "elf",
                layout.sdk_config / "include",
                layout.sdk_config / "lib",
            )
            self.assertTrue(all(path.is_dir() for path in expected))

    def test_required_artifacts_lists_complete_vmos_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout = self.build.BuildLayout.create(
                Path(temporary), self.board, "debug"
            )
            relative = {
                path.relative_to(layout.sdk).as_posix()
                for path in self.build.required_artifacts(layout)
            }
            self.assertEqual(
                relative,
                {
                    "bin/microkit",
                    "board/qemu_virt_aarch64/debug/elf/sel4.elf",
                    "board/qemu_virt_aarch64/debug/elf/loader.elf",
                    "board/qemu_virt_aarch64/debug/elf/monitor.elf",
                    "board/qemu_virt_aarch64/debug/elf/initialiser.elf",
                    "board/qemu_virt_aarch64/debug/lib/libmicrokit.a",
                    "board/qemu_virt_aarch64/debug/lib/microkit.ld",
                },
            )

    def test_verify_artifacts_reports_all_missing_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout = self.build.BuildLayout.create(
                Path(temporary), self.board, "debug"
            )
            with self.assertRaises(self.build.BuildError) as raised:
                self.build.verify_artifacts(layout)
            message = str(raised.exception)
            for path in self.build.required_artifacts(layout):
                self.assertIn(str(path), message)

    def test_copy_sel4_sdk_copies_kernel_metadata_and_include_trees(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            layout = self.build.BuildLayout.create(root / "output", self.board, "debug")
            kernel = layout.sel4_install / "bin" / "kernel.elf"
            kernel.parent.mkdir(parents=True)
            kernel.write_bytes(b"kernel")
            generated = {
                layout.sel4_build / "generated" / "invocations_all.json": b"{}",
                layout.sel4_build / "gen_headers" / "plat" / "machine" / "platform_gen.json": b"{}",
            }
            for path, data in generated.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            for tree in ("kernel_Config", "libsel4", "libsel4/sel4_Config", "libsel4/autoconf"):
                header = layout.sel4_install / tree / "include" / tree.replace("/", "_")
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(tree, encoding="utf-8")

            self.build.copy_sel4_sdk(layout)

            self.assertEqual((layout.sdk_config / "elf" / "sel4.elf").read_bytes(), b"kernel")
            self.assertTrue((layout.sdk_config / "invocations_all.json").is_file())
            self.assertTrue((layout.sdk_config / "platform_gen.json").is_file())
            copied_headers = list((layout.sdk_config / "include").iterdir())
            self.assertEqual(len(copied_headers), 4)

    def test_copy_sel4_sdk_accepts_merged_current_sel4_include_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout = self.build.BuildLayout.create(
                Path(temporary), self.board, "debug"
            )
            kernel = layout.sel4_install / "bin" / "kernel.elf"
            kernel.parent.mkdir(parents=True)
            kernel.write_bytes(b"kernel")
            invocations = layout.sel4_build / "generated" / "invocations_all.json"
            invocations.parent.mkdir(parents=True)
            invocations.write_text("{}", encoding="utf-8")
            platform = (
                layout.sel4_build
                / "gen_headers"
                / "plat"
                / "machine"
                / "platform_gen.json"
            )
            platform.parent.mkdir(parents=True)
            platform.write_text("{}", encoding="utf-8")
            config = (
                layout.sel4_install
                / "libsel4"
                / "include"
                / "kernel"
                / "gen_config.h"
            )
            config.parent.mkdir(parents=True)
            config.write_text(
                "#define CONFIG_ARM_HYPERVISOR_SUPPORT  1\n", encoding="utf-8"
            )

            self.build.copy_sel4_sdk(layout)
            self.build.verify_hypervisor_config(layout)

            self.assertTrue(
                (layout.sdk_config / "include" / "kernel" / "gen_config.h").is_file()
            )

    def test_parse_constant_header_handles_integer_and_subtraction(self):
        parsed = self.build.parse_constant_header(
            "page: 12\nslot: (17 - 5)\nignored preprocessor text\n"
        )
        self.assertEqual(parsed, {"page": 12, "slot": 12})

    def test_component_commands_use_only_local_vmos_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout = self.build.BuildLayout.create(
                Path(temporary), self.board, "debug"
            )
            commands = self.build.component_commands(layout, self.board)
            flattened = "\n".join(
                " ".join(command.command) + " " + str(command.cwd)
                for command in commands
            )
            self.assertIn(str(REPOSITORY_ROOT / "vmos"), flattened)


class RootEntryPointTests(unittest.TestCase):
    def run_make(self, *arguments):
        return subprocess.run(
            ("make", "-f", "Makefile.vmos", *arguments),
            cwd=REPOSITORY_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout

    def test_help_documents_targets_variables_and_output(self):
        output = self.run_make("help")
        for text in (
            "build",
            "qemu",
            "test",
            "verify-source",
            "clean",
            "BOARD",
            "CONFIG",
            "JOBS",
            "build/vmos",
        ):
            with self.subTest(text=text):
                self.assertIn(text, output)

    def test_dry_run_qemu_builds_rust_runtime_image_and_starts_at_el2(self):
        output = self.run_make("-n", "qemu")
        for text in (
            "vmos/runtime/src/main.rs",
            "vmos/runtime/src/ipc_responder.rs",
            "--target aarch64-unknown-none",
            "sdk/bin/microkit",
            "qemu-system-aarch64",
            "virt,virtualization=on",
            "addr=0x70000000",
            "loader.img",
            "ipc-benchmark-responder.elf",
        ):
            with self.subTest(text=text):
                self.assertIn(text, output)

    def test_test_target_runs_runtime_console_rust_tests(self):
        output = self.run_make("-n", "test")
        self.assertIn(
            "cargo test --manifest-path vmos/runtime/Cargo.toml", output
        )

    def test_runtime_system_maps_pl011_and_irq(self):
        system = ET.parse(
            REPOSITORY_ROOT / "vmos" / "runtime" / "runtime.system"
        ).getroot()
        memory_region = system.find("memory_region")
        self.assertIsNotNone(memory_region)
        self.assertEqual(memory_region.attrib["phys_addr"], "0x0900_0000")
        self.assertEqual(memory_region.attrib["size"], "0x1000")

        protection_domain = system.find("protection_domain")
        mapping = protection_domain.find("map")
        self.assertEqual(mapping.attrib["cached"], "false")
        self.assertEqual(mapping.attrib["vaddr"], "0x1000_0000")
        irq = protection_domain.find("irq")
        self.assertEqual(irq.attrib["irq"], "33")
        self.assertEqual(irq.attrib["id"], "0")
        self.assertEqual(irq.attrib["trigger"], "level")

    def test_runtime_system_has_a_passive_same_cpu_ipc_benchmark_responder(self):
        system = ET.parse(
            REPOSITORY_ROOT / "vmos" / "runtime" / "runtime.system"
        ).getroot()
        domains = {
            domain.attrib["name"]: domain
            for domain in system.findall("protection_domain")
        }

        self.assertIn("vmos_runtime", domains)
        self.assertIn("ipc_benchmark_responder", domains)
        caller = domains["vmos_runtime"]
        responder = domains["ipc_benchmark_responder"]
        self.assertEqual(caller.attrib["cpu"], "0")
        self.assertEqual(caller.attrib["priority"], "100")
        self.assertEqual(responder.attrib["cpu"], "0")
        self.assertEqual(responder.attrib["priority"], "101")
        self.assertEqual(responder.attrib["passive"], "true")
        self.assertEqual(
            responder.find("program_image").attrib["path"],
            "ipc-benchmark-responder.elf",
        )

        ends = system.find("channel").findall("end")
        caller_end = next(end for end in ends if end.attrib["pd"] == "vmos_runtime")
        responder_end = next(
            end for end in ends if end.attrib["pd"] == "ipc_benchmark_responder"
        )
        self.assertEqual(caller_end.attrib["id"], "1")
        self.assertEqual(caller_end.attrib["pp"], "true")
        self.assertEqual(caller_end.attrib["notify"], "false")
        self.assertEqual(responder_end.attrib["id"], "0")
        self.assertEqual(responder_end.attrib["notify"], "false")

    def test_runtime_uses_volatile_pl011_interrupts(self):
        source = (
            REPOSITORY_ROOT / "vmos" / "runtime" / "src" / "main.rs"
        ).read_text(encoding="utf-8")
        for text in (
            "read_volatile",
            "write_volatile",
            "PL011_RX_INTERRUPT",
            "PL011_RECEIVE_TIMEOUT_INTERRUPT",
            "microkit_irq_ack",
        ):
            with self.subTest(text=text):
                self.assertIn(text, source)

    def test_dry_run_build_uses_root_script_and_defaults(self):
        output = self.run_make("-n", "build")
        self.assertIn("build_vmos.py", output)
        self.assertIn("--board qemu_virt_aarch64", output)
        self.assertIn("--config debug", output)
        self.assertIn("--jobs", output)
        self.assertIn("publish", output)
        self.assertIn("--output build/vmos/qemu_virt_aarch64/debug", output)

    def test_build_output_is_ignored(self):
        ignore = (REPOSITORY_ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertIn("/build/vmos/", ignore.splitlines())


if __name__ == "__main__":
    unittest.main()
