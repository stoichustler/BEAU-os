#!/usr/bin/env python3

import re
import unittest
from pathlib import Path


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


class VmStrShellReturnTest(unittest.TestCase):
    def test_console_timer_only_publishes_shell_work(self) -> None:
        console = source("sdk/bsp/console.c")
        callback = function_body(
            console,
            "static void console_timer_callback(__unused void *data)\n{",
        )

        self.assertIn("shell_kick();", callback)
        self.assertNotIn("console_vm_kick", callback)

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

        self.assertIn("console_vm_kick()", worker)
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


if __name__ == "__main__":
    unittest.main()
