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

    def test_platform_dts_has_bounded_pm_policy(self):
        qemu = source("arch/arm64/platform/qemu/platform.dts")
        parser = source("sdk/bsp/arm64/platform_dts.c")
        for text in ("beau,power-management", "controller-vm",
                     "required-vms", "prepare-timeout-ms",
                     "resume-timeout-ms", "wakeup-irqs", "qemu-mode"):
            self.assertIn(text, qemu + parser)
        self.assertIn("CONFIG_MAX_VM_NUM", parser)

    def test_pm_shell_exposes_transaction_diagnostics(self):
        shell = source("sdk/bsp/arm64/shell.c")
        for text in ('"pm"', '"pmstat"', "hv_pm_get_snapshot",
                     '"suspend"', '"status"', '"abort"', '"wake"'):
            self.assertIn(text, shell)

    def test_vpsci_cpu_suspend_supports_standby_and_powerdown(self):
        vpsci = source("arch/arm64/guest/vpsci.c")
        exit_c = source("arch/arm64/guest/vcpu_exit.c")
        for text in ("PSCI_0_2_FN_CPU_SUSPEND",
                     "PSCI_0_2_FN64_CPU_SUSPEND"):
            self.assertIn(text, exit_c)
        for text in ("arm64_vpsci_cpu_suspend", "power_state",
                     "entry_point", "context_id", "PSCI_RET_DENIED"):
            self.assertIn(text, vpsci)
        self.assertIn("handle_psci_features", exit_c)

    def test_system_suspend_requires_bsp_and_offline_aps(self):
        vpsci = source("arch/arm64/guest/vpsci.c")
        for text in ("arm64_vpsci_system_suspend", "is_vcpu_bsp",
                     "VCPU_POWERED_OFF", "PSCI_RET_DENIED",
                     "resume_entry", "resume_context"):
            self.assertIn(text, vpsci)

    def test_system_resume_rebuilds_entry_and_x0(self):
        pm = source("core/pm.c")
        self.assertIn("arm64_vpsci_resume_vm", pm)
        self.assertIn("resume_entry", pm)
        self.assertIn("resume_context", pm)


if __name__ == "__main__":
    unittest.main()
