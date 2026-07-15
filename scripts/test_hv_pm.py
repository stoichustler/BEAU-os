#!/usr/bin/env python3
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def source(path):
    target = pathlib.Path(path)
    if not target.is_absolute():
        target = ROOT / target
    return target.read_text(encoding="utf-8") if target.is_file() else ""


class HvPmContractTest(unittest.TestCase):
    def test_hv_pm_header_replaces_host_pm(self):
        self.assertTrue((ROOT / "include/common/hv_pm.h").is_file())
        self.assertFalse((ROOT / "include/common/host_pm.h").exists())
        self.assertTrue((ROOT / "include/arch/arm64/asm/hv_pm.h").is_file())
        self.assertNotIn("#include <host_pm.h>", source("core/vm.c"))

    def test_coordinator_owns_required_sequence_comment(self):
        pm = source("core/pm.c")
        for text in (
            "[20260715] Coordinated guest STR transaction",
            "PM controller       Guest OSes",
            "request(epoch)",
            "SYSTEM_SUSPEND",
            "FREEZING_HOST",
            "resume complete",
            "the BSP idle thread is the only owner",
        ):
            self.assertIn(text, pm)


if __name__ == "__main__":
    unittest.main()
