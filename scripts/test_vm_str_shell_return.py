#!/usr/bin/env python3

import re
import unittest
from pathlib import Path

from regress import PROMPT, QemuSession


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0

    for offset in range(opening, len(text)):
        if text[offset] == "{":
            depth += 1
        elif text[offset] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:offset]

    raise AssertionError(f"unterminated function: {signature}")


def python_function(text: str, signature: str) -> str:
    start = text.index(signature)
    remainder = text[start + len(signature):]
    next_function = re.search(r"^def \w+\(", remainder, re.MULTILINE)
    end = len(text) if next_function is None else (
        start + len(signature) + next_function.start()
    )
    return text[start:end]


class VmStrShellReturnTest(unittest.TestCase):
    def test_expect_searches_buffered_output_from_requested_offset(self) -> None:
        class RunningProcess:
            @staticmethod
            def poll():
                return None

        prefix = "output before suspend\n"
        qemu = QemuSession([], None, timeout=0.01)
        self.addCleanup(qemu.selector.close)
        qemu.output = prefix + PROMPT
        qemu.proc = RunningProcess()
        qemu.read_some = lambda deadline: None

        matched = qemu.expect(
            PROMPT,
            "buffered BEAU prompt",
            start_offset=len(prefix),
        )

        self.assertEqual(PROMPT, matched)

    def test_console_timer_keeps_vm_io_on_bounded_fast_path(self) -> None:
        console = source("sdk/bsp/console.c")
        callback = function_body(
            console,
            "static void console_timer_callback(__unused void *data)\n{",
        )

        self.assertRegex(
            callback,
            re.compile(
                r"if\s*\(!console_vm_kick\(\)\)\s*\{\s*"
                r"shell_kick\(\);",
                re.DOTALL,
            ),
        )
        self.assertIn("shell_kick();", callback)

    def test_public_kick_only_prioritizes_shell_thread(self) -> None:
        shell = source("sdk/bsp/shell.c")
        kick = function_body(shell, "void shell_kick(void)")

        self.assertIn("request_thread_priority(&shell_thread);", kick)
        self.assertNotIn("wake_thread", kick)
        self.assertNotIn("console_vm_kick", kick)
        self.assertNotIn("shell_getc", kick)
        self.assertNotIn("shell_input_line", kick)
        self.assertNotIn("shell_process", kick)

    def test_shell_thread_owns_input_and_publishes_completed_prompt(self) -> None:
        shell = source("sdk/bsp/shell.c")
        worker_signature = "static void shell_thread_kick(void)"

        self.assertNotIn("is_cmd_cmplt", shell)
        self.assertIn(worker_signature, shell)
        worker = function_body(shell, worker_signature)
        thread = function_body(
            shell,
            "static void shell_thread_main(__unused struct thread_object *obj)\n{",
        )

        self.assertNotIn("console_vm_kick", worker)
        self.assertRegex(
            worker,
            re.compile(
                r"if\s*\(!console_is_hv\(\)\)\s*\{\s*return;",
                re.DOTALL,
            ),
        )
        self.assertRegex(
            worker,
            re.compile(
                r"if\s*\(shell_input_line\(\)\)\s*\{\s*"
                r"\(void\)shell_process\(\);\s*"
                r"if\s*\(console_is_hv\(\)\)\s*\{\s*"
                r"shell_show_prompt\(false\);",
                re.DOTALL,
            ),
        )
        for token in ("shell_thread_kick();", "yield_current();", "schedule();"):
            self.assertIn(token, thread)
        kick_index = thread.index("shell_thread_kick();")
        yield_index = thread.index("yield_current();")
        schedule_index = thread.index("schedule();")
        self.assertLess(kick_index, yield_index)
        self.assertLess(yield_index, schedule_index)
        self.assertNotIn("sleep_thread", thread)

    def test_vm_console_binding_precedes_ownership_publication(self) -> None:
        shell = source("sdk/bsp/shell.c")
        switch = function_body(
            shell,
            "static int32_t shell_to_vm_console(int32_t argc, char **argv)\n{",
        )

        bind_index = switch.index("console_vm_vuart_bind(vm_id)")
        publish_index = switch.index("console_vmid = vm_id")
        self.assertLess(bind_index, publish_index)

    def test_vm_console_attach_primes_guest_prompt_without_blocking(self) -> None:
        shell = source("sdk/bsp/shell.c")
        switch = function_body(
            shell,
            "static int32_t shell_to_vm_console(int32_t argc, char **argv)\n{",
        )

        publish_index = switch.index("console_vmid = vm_id")
        prime_index = switch.index("vuart_try_putchar(vu, VM_CONSOLE_PROMPT_KEY)")
        notify_index = switch.index("vuart_notify_rx(vu)")
        self.assertLess(publish_index, prime_index)
        self.assertLess(prime_index, notify_index)
        self.assertRegex(
            switch,
            re.compile(
                r"if\s*\(vuart_try_putchar\(vu, VM_CONSOLE_PROMPT_KEY\)\)\s*"
                r"\{\s*vuart_notify_rx\(vu\);",
                re.DOTALL,
            ),
        )

    def test_vm_console_normalizes_backspace_for_guest_terminal(self) -> None:
        console = source("sdk/bsp/console.c")
        collect = function_body(
            console,
            "static bool console_vm_input_collect(uint16_t target_vmid)\n{",
        )

        normalize_index = collect.index("ch = VM_CONSOLE_ASCII_DEL")
        queue_index = collect.index("console_vm_input_put(ch)")
        self.assertLess(normalize_index, queue_index)
        self.assertRegex(
            collect,
            re.compile(
                r"if\s*\(ch\s*==\s*VM_CONSOLE_ASCII_BS\)\s*"
                r"\{\s*ch\s*=\s*VM_CONSOLE_ASCII_DEL;",
                re.DOTALL,
            ),
        )

    def test_boot_quiet_waits_for_enter(self) -> None:
        shell = source("sdk/bsp/shell.c")
        worker = function_body(shell, "static void shell_thread_kick(void)")
        quiet_start = worker.index("if (!shell_prompt_enabled)")
        input_start = worker.index("if (shell_input_line())")
        quiet = worker[quiet_start:input_start]

        self.assertIn("char ch = shell_getc();", quiet)
        self.assertIn("(ch == '\\r') || (ch == '\\n')", quiet)
        self.assertIn("shell_show_banner_prompt();", quiet)
        self.assertIn("return;", quiet)

    def test_guest_console_return_reopens_inactive_prompt(self) -> None:
        shell = source("sdk/bsp/shell.c")
        worker_signature = "static void shell_thread_kick(void)"

        self.assertIn(worker_signature, shell)
        worker = function_body(shell, worker_signature)
        self.assertRegex(
            worker,
            re.compile(
                r"if\s*\(console_is_hv\(\)\s*&&\s*!shell_input_active\)"
                r"\s*\{\s*shell_show_prompt\(false\);",
                re.DOTALL,
            ),
        )

    def test_pm_idle_completion_requests_reschedule(self) -> None:
        schedule = source("core/schedule.c")
        idle = function_body(
            schedule,
            "void default_idle(__unused struct thread_object *obj)\n{",
        )
        arch_pm = source("arch/arm64/pm.c")
        secondary_idle = function_body(
            arch_pm,
            "void arch_pm_process_secondary_from_idle(uint16_t pcpu_id)\n{",
        )

        self.assertRegex(
            idle,
            re.compile(
                r"if\s*\(need_system_suspend\(pcpu_id\)\)\s*\{\s*"
                r"hv_pm_process_from_idle\(pcpu_id\);\s*"
                r"make_reschedule_request\(pcpu_id\);",
                re.DOTALL,
            ),
        )
        self.assertNotIn("make_reschedule_request", secondary_idle)

    def test_runtime_checks_prompt_before_resuming_suspended_vm(self) -> None:
        regression = python_function(
            source("scripts/regress.py"),
            "def run_str_cycle(qemu, args, cycle):",
        )
        suspended_prompt = re.compile(
            r"qemu\.expect\(\s*PROMPT,\s*"
            r'f"\{label\}: BEAU shell responsive while target suspended",\s*'
            r"start_offset=start_offset,?\s*\)",
        )

        prompt_match = suspended_prompt.search(regression)
        self.assertIsNotNone(prompt_match)
        suspend_index = regression.index('qemu.send(f"pm suspend {vmid}" + ENTER)')
        freeze_index = regression.index("STR_FREEZE_MARKERS")
        prompt_index = prompt_match.start()
        sleep_index = regression.index("time.sleep(args.str_suspend_seconds)")
        resume_index = regression.index('qemu.send(f"pm resume {vmid}" + ENTER)')

        self.assertLess(suspend_index, freeze_index)
        self.assertLess(freeze_index, prompt_index)
        self.assertLess(prompt_index, sleep_index)
        self.assertLess(sleep_index, resume_index)
        self.assertNotIn("qemu.send(ENTER)", regression[suspend_index:prompt_index])


if __name__ == "__main__":
    unittest.main()
