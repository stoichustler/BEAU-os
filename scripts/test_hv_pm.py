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

    def test_transaction_is_static_and_epoch_owned(self):
        header = source("include/common/hv_pm.h")
        pm = source("core/pm.c")
        self.assertIn("struct beau_pm_transaction", header)
        self.assertIn("vm[CONFIG_MAX_VM_NUM]", header)
        self.assertIn("required_vm_mask", header)
        self.assertIn("completed_hook_mask", header)
        self.assertIn("static struct beau_pm_transaction pm_transaction", pm)
        self.assertNotIn("malloc(", pm)
        self.assertNotIn("calloc(", pm)

    def test_request_rejects_concurrent_epoch(self):
        pm = source("core/pm.c")
        self.assertIn("int32_t hv_pm_request_suspend", pm)
        self.assertIn("-EBUSY", pm)
        self.assertIn("PM_RUNNING", pm)
        self.assertIn("PM_PREPARING", pm)

    def test_snapshot_does_not_expose_live_lock(self):
        pm = source("core/pm.c")
        self.assertIn("void hv_pm_get_snapshot", pm)
        self.assertIn("spinlock_irqsave_obtain", pm)
        self.assertIn("memcpy_s", pm)

    def test_hooks_are_bounded_and_rollback_only_completed_entries(self):
        header = source("include/common/hv_pm.h")
        pm = source("core/pm.c")
        self.assertIn("HV_PM_MAX_HOOKS", header)
        self.assertIn("struct beau_pm_ops", header)
        self.assertIn("prepare", header)
        self.assertIn("suspend", header)
        self.assertIn("resume", header)
        self.assertIn("abort", header)
        self.assertIn("completed_hook_mask", pm)
        self.assertIn("for (idx = count; idx > 0U; idx--)", pm)


if __name__ == "__main__":
    unittest.main()
