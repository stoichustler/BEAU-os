#!/usr/bin/env python3
import argparse
import codecs
import os
import selectors
import shlex
import shutil
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CWD = Path.cwd()
PROMPT = "console:\\>"
LINUX_PROMPT = "uos "
LK_PROMPT = "uos ~"
HELP_STRESS_TARGETS = (
    (0, "sos ~", "VM0 Zephyr", 30.0),
    (1, LINUX_PROMPT, "VM1 Linux", 60.0),
    (2, LINUX_PROMPT, "VM2 Linux", 60.0),
)
ENTER = "\r"
CTRL_D = b"\x04"
LINUX_VM1_IMAGE_STAGE_ADDR = "0x70000000"
LINUX_INITRAMFS_STAGE_ADDR = "0x74000000"
LINUX_VM2_IMAGE_STAGE_ADDR = "0x76000000"
FATAL_PATTERNS = (
    "[cut here]",
    "unexpected arm64 trap",
    "unexpected irq",
    "unhandled arm64 vcpu exit",
    "failed to handle arm64 vcpu exit",
    "rcu_preempt detected stalls",
    "rcu_preempt kthread timer wakeup didn't happen",
    "possible timer handling issue",
    "timer-softirq=0",
    "assertion failed",
    "stack check fails",
    "fatal error",
)
FATAL_DRAIN_TIMEOUT = 1.0


def relpath(path):
    path = Path(path).expanduser()
    return path if path.is_absolute() else CWD / path


def quote(cmd):
    return shlex.join([str(arg) for arg in cmd])


def render(cmd, toolchains=None):
    cmd = quote(cmd)
    return f"PATH={shlex.quote(str(toolchains))}:$PATH {cmd}" if toolchains else cmd


def getenv(name, default=None):
    value = os.getenv(name)
    return default if value is None else value


def build_env(toolchains):
    env = os.environ.copy()
    if toolchains:
        env["PATH"] = f"{toolchains}{os.pathsep}{env.get('PATH', '')}"
    return env


def parse_args():
    toolchains = getenv("BEAU_TOOLCHAINS")
    toolchains = getenv("BEAU_TOOLCHAIN", toolchains)
    toolchains = relpath(toolchains) if toolchains else None

    parser = argparse.ArgumentParser(description="Run the ARM64 QEMU boot regression.")
    parser.add_argument("--toolchains", "--toolchain", default=toolchains, type=relpath)
    parser.add_argument("--cross-prefix", default=getenv("BEAU_CROSS_COMPILE", "aarch64-none-elf-"))
    parser.add_argument("--kernel", default=ROOT / "out/qemu_out/beau.debug.out", type=relpath)
    parser.add_argument("--qemu", default=os.getenv("QEMU_SYSTEM_AARCH64", "qemu-system-aarch64"))
    parser.add_argument("--smp", default=getenv("BEAU_QEMU_SMP", "8"))
    parser.add_argument("-m", "--memory", default=getenv("BEAU_QEMU_MEM", "1024M"))
    parser.add_argument("--linux-image", default=None, type=relpath)
    parser.add_argument("--linux-vm1-image", default=ROOT / "sdk/image/linux/vm1/Image", type=relpath)
    parser.add_argument("--linux-vm2-image", default=ROOT / "sdk/image/linux/vm2/Image", type=relpath)
    parser.add_argument(
        "--linux-initramfs",
        dest="linux_initramfs",
        default=ROOT / "sdk/image/linux/Initramfs.cpio.gz",
        type=relpath,
    )
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--log", default=ROOT / "out/qemu_out/regress.log", type=relpath)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--stress-vsh-switch",
        action="store_true",
        help="Run the VM console switch/Enter pressure sequence after the standard smoke checks.",
    )
    parser.add_argument(
        "--stress-vsh-help",
        action="store_true",
        help="Repeatedly switch VM consoles and run help in each VM shell after the standard smoke checks.",
    )
    parser.add_argument("--stress-rounds", type=int, default=4)
    parser.add_argument("--stress-help-rounds", type=int, default=100)
    parser.add_argument("--stress-enters", type=int, default=80)
    parser.add_argument("--stress-enter-delay", type=float, default=0.0)
    parser.add_argument(
        "--no-terminal-replies",
        action="store_true",
        help="Do not synthesize terminal responses such as CPR replies for VM shells.",
    )
    args, extra = parser.parse_known_args()
    if extra[:1] == ["--"]:
        extra = extra[1:]
    args.extra = extra
    if args.linux_image is not None:
        args.linux_vm1_image = args.linux_image
        args.linux_vm2_image = args.linux_image
    return args


def make_cmd(args):
    return [
        "make",
        "ARCH=arm64",
        "PLATFORM=qemu",
        f"CROSS_COMPILE={args.cross_prefix}",
        f"-j{os.cpu_count() or 1}",
    ]


def qemu_cmd(args):
    return [
        args.qemu,
        "-machine",
        "virt,virtualization=on,gic-version=3,its=on",
        "-cpu",
        "cortex-a57",
        "-smp",
        args.smp,
        "-m",
        args.memory,
        "-nographic",
        "-serial",
        "mon:stdio",
        "-kernel",
        str(args.kernel),
        "-device",
        f"loader,file={args.linux_vm1_image},addr={LINUX_VM1_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_vm2_image},addr={LINUX_VM2_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_initramfs},addr={LINUX_INITRAMFS_STAGE_ADDR},force-raw=on",
        *args.extra,
    ]


def run_build(args, cmd):
    if args.toolchains and not args.toolchains.is_dir():
        raise SystemExit(f"Toolchain bin dir not found: {args.toolchains}")

    compiler = f"{args.cross_prefix}gcc"
    env = build_env(args.toolchains)
    if shutil.which(compiler, path=env.get("PATH")) is None:
        raise SystemExit(f"Compiler not found: {compiler}")

    print(f"[regress] build: {quote(cmd)}", flush=True)
    subprocess.run(cmd, cwd=ROOT, env=env, check=True)


class QemuSession:
    def __init__(self, cmd, log_path, timeout):
        self.cmd = cmd
        self.log_path = log_path
        self.timeout = timeout
        self.output = ""
        self.cpr_scan_offset = 0
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.decoder_finalized = False
        self.proc = None
        self.selector = selectors.DefaultSelector()
        self.ignore_fatal = False

    def __enter__(self):
        self.proc = subprocess.Popen(
            self.cmd,
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        self.write_log()

    def close(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)

    def write_log(self):
        if self.log_path:
            self.flush_decoder()
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            self.log_path.write_text(self.output, encoding="utf-8", errors="replace")

    def flush_decoder(self):
        if not self.decoder_finalized:
            self.output += self.decoder.decode(b"", final=True)
            self.decoder_finalized = True

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def read_some(self, deadline):
        wait = max(0.0, min(0.25, deadline - time.monotonic()))
        for key, _ in self.selector.select(wait):
            data = os.read(key.fileobj.fileno(), 4096)
            if not data:
                return
            self.output += self.decoder.decode(data)
            if not getattr(self, "disable_terminal_replies", False):
                self.reply_terminal_queries()
            if not self.ignore_fatal:
                self.check_fatal()

    def reply_terminal_queries(self):
        pending = self.output[self.cpr_scan_offset:]
        if "\x1b[6n" in pending:
            self.send("\x1b[1;1R")
        self.cpr_scan_offset = len(self.output)

    def drain_for(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {self.proc.returncode}")
            before = len(self.output)
            self.read_some(deadline)
            if len(self.output) == before:
                time.sleep(0.005)

    def drain_after_fatal(self):
        deadline = time.monotonic() + FATAL_DRAIN_TIMEOUT
        while time.monotonic() < deadline:
            wait = max(0.0, min(0.05, deadline - time.monotonic()))
            events = self.selector.select(wait)
            if not events:
                if self.proc.poll() is not None:
                    return
                continue

            for key, _ in events:
                data = os.read(key.fileobj.fileno(), 4096)
                if not data:
                    return
                self.output += self.decoder.decode(data)
                if "[end here]" in self.output[-4000:].lower():
                    return

    def check_fatal(self):
        # QEMU stdout can split a fatal log line across reads. Only scan
        # complete lines so the saved regression log keeps the diagnostic
        # suffix, such as ESR/ELR/FAR for ARM64 vCPU exits.
        last_lf = self.output.rfind("\n")
        if last_lf < 0:
            return

        lower = self.output[:last_lf + 1].lower()
        for pattern in FATAL_PATTERNS:
            if pattern in lower:
                self.drain_after_fatal()
                raise RuntimeError(f"fatal QEMU output matched: {pattern}")

    def expect(self, pattern, name, timeout=None, keepalive=None):
        print(f"[regress] wait: {name}", flush=True)
        start_len = len(self.output)
        deadline = time.monotonic() + (timeout or self.timeout)
        next_keepalive = time.monotonic() + 2.0

        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {self.proc.returncode}")
            self.read_some(deadline)
            if pattern in self.output[start_len:]:
                print(f"[pass] {name}", flush=True)
                return self.output[start_len:]
            if keepalive and time.monotonic() >= next_keepalive:
                self.send(keepalive)
                next_keepalive = time.monotonic() + 2.0

        tail = self.output[-3000:]
        raise TimeoutError(f"timed out waiting for {name}: {pattern!r}\n--- output tail ---\n{tail}")

    def command(self, line, patterns, rejects=None):
        rejects = [] if rejects is None else rejects
        self.send(line + ENTER)
        text = self.expect(PROMPT, f"{line} returns to BEAU shell")
        for pattern in patterns:
            if pattern not in text:
                raise RuntimeError(f"{line!r} output missing {pattern!r}")
        for pattern in rejects:
            if pattern in text:
                raise RuntimeError(f"{line!r} output contains rejected {pattern!r}")
        print(f"[pass] {line}: expected output found", flush=True)

    def capture_vm_diagnostics(self, label, vmid):
        print(f"[regress] diagnostics: {label}", flush=True)
        old_ignore_fatal = self.ignore_fatal
        self.ignore_fatal = True
        try:
            self.send(CTRL_D)
            self.expect(PROMPT, f"return to BEAU shell for {label}", timeout=5.0, keepalive=ENTER)
            for line in ("vcpus", "schedstat", "vmstat", "irqstat", f"dumpstat {vmid}"):
                self.send(line + ENTER)
                self.expect(PROMPT, f"{line} diagnostics", timeout=15.0, keepalive=ENTER)
        except Exception as err:
            print(f"[regress] diagnostics failed: {err}", flush=True)
        finally:
            self.ignore_fatal = old_ignore_fatal


def vsh_enter(qemu, vmid, prompt, name, timeout=30.0):
    qemu.send(f"vsh {vmid}" + ENTER)
    try:
        qemu.expect(prompt, name, timeout=timeout, keepalive=ENTER)
    except Exception:
        qemu.capture_vm_diagnostics(name, vmid)
        raise


def vsh_return(qemu, name, vmid=None):
    qemu.send(CTRL_D)
    try:
        qemu.expect(PROMPT, name, timeout=10.0, keepalive=ENTER)
    except Exception:
        if vmid is not None:
            qemu.capture_vm_diagnostics(name, vmid)
        raise


def send_enter_burst(qemu, count, delay, name, vmid=None):
    print(f"[regress] stress: {name}: {count} Enter keys", flush=True)
    try:
        for idx in range(max(0, count)):
            qemu.send(ENTER)
            if delay > 0.0:
                qemu.drain_for(delay)
            elif (idx + 1) % 16 == 0:
                qemu.drain_for(0.02)
        qemu.drain_for(0.2)
    except Exception:
        if vmid is not None:
            qemu.capture_vm_diagnostics(name, vmid)
        raise


def expect_vm2_id(qemu, name):
	token = f"__beau_vm2_id_{int(time.monotonic() * 1000000)}__"

	# The VM console can replay old output and echo the command line before the
	# guest has executed it. A unique token plus the "[vmid 2]" output marker makes
	# this check wait for fresh VM2 command output instead of a stale "gid=0".
	qemu.send(f"echo {token}; id; echo {token}_done" + ENTER)
	try:
		text = qemu.expect(f"[vmid 2] {token}_done", name, timeout=20.0, keepalive=ENTER)
		if "gid=0" not in text:
			raise RuntimeError(f"{name}: id output missing gid=0")
	except Exception:
		qemu.capture_vm_diagnostics(name, 2)
		raise


def run_guest_help(qemu, vmid, prompt, name, timeout):
    print(f"[regress] stress: {name}: help", flush=True)
    qemu.send("help" + ENTER)
    try:
        qemu.expect(prompt, f"{name}: help returns", timeout=timeout, keepalive=ENTER)
    except Exception:
        qemu.capture_vm_diagnostics(f"{name}: help", vmid)
        raise


def run_vsh_help_stress(qemu, args):
    if args.stress_help_rounds < 1:
        return

    for idx in range(args.stress_help_rounds):
        label = idx + 1
        for vmid, prompt, guest_name, timeout in HELP_STRESS_TARGETS:
            name = f"help stress round {label}: {guest_name}"
            vsh_enter(qemu, vmid, prompt, f"{name} shell", timeout=timeout)
            run_guest_help(qemu, vmid, prompt, name, timeout)
            vsh_return(qemu, f"{name}: return to BEAU shell", vmid=vmid)

    print("[pass] VM console help stress complete", flush=True)


def run_vsh_switch_stress(qemu, args):
    if args.stress_rounds < 1:
        return

    vsh_enter(qemu, 2, LINUX_PROMPT, "stress VM2 Linux shell", timeout=30.0)
    send_enter_burst(qemu, args.stress_enters, args.stress_enter_delay, "VM2 initial", vmid=2)
    expect_vm2_id(qemu, "VM2 Linux identity after initial Enter burst")
    vsh_return(qemu, "return from stress VM2 initial", vmid=2)

    vsh_enter(qemu, 1, LINUX_PROMPT, "stress VM1 Linux shell", timeout=30.0)
    send_enter_burst(qemu, args.stress_enters, args.stress_enter_delay, "VM1 Linux", vmid=1)
    vsh_return(qemu, "return from stress VM1", vmid=1)

    for idx in range(args.stress_rounds):
        label = idx + 1
        vsh_enter(qemu, 0, "sos ~", f"stress round {label}: VM0 Zephyr shell")
        send_enter_burst(qemu, max(1, args.stress_enters // 4),
            args.stress_enter_delay, f"round {label} VM0", vmid=0)
        vsh_return(qemu, f"stress round {label}: return from VM0", vmid=0)

        vsh_enter(qemu, 1, LINUX_PROMPT, f"stress round {label}: VM1 Linux shell",
            timeout=30.0)
        send_enter_burst(qemu, max(1, args.stress_enters // 4),
            args.stress_enter_delay, f"round {label} VM1", vmid=1)
        vsh_return(qemu, f"stress round {label}: return from VM1", vmid=1)

        vsh_enter(qemu, 2, LINUX_PROMPT, f"stress round {label}: VM2 Linux shell",
            timeout=30.0)
        send_enter_burst(qemu, args.stress_enters, args.stress_enter_delay,
            f"round {label} VM2", vmid=2)
        expect_vm2_id(qemu, f"stress round {label}: VM2 identity after switch")
        vsh_return(qemu, f"stress round {label}: return from VM2", vmid=2)

    print("[pass] VM console switch stress complete", flush=True)


def run_qemu(args, cmd):
    if not args.kernel.is_file():
        raise SystemExit(f"Kernel image not found: {args.kernel}")
    if not args.linux_vm1_image.is_file():
        raise SystemExit(f"Linux VM1 Image not found: {args.linux_vm1_image}")
    if not args.linux_vm2_image.is_file():
        raise SystemExit(f"Linux VM2 Image not found: {args.linux_vm2_image}")
    if not args.linux_initramfs.is_file():
        raise SystemExit(f"Linux initramfs not found: {args.linux_initramfs}")
    if shutil.which(args.qemu) is None:
        raise SystemExit(f"QEMU binary not found: {args.qemu}")

    print(f"[regress] qemu: {quote(cmd)}", flush=True)
    with QemuSession(cmd, args.log, args.timeout) as qemu:
        qemu.disable_terminal_replies = args.no_terminal_replies
        qemu.expect(PROMPT, "BEAU shell prompt", keepalive=ENTER)
        qemu.command("vcpus", [
            "vcpu",
            "pcpu_mode",
            "exclusive",
            "shared",
            "switches",
            "since.us",
            "vm0:vcpu0",
            "vm2:vcpu2",
        ])
        qemu.command("schedstat", [
            "schedstat pcpus:",
            "Per-pCPU hybrid scheduler counters:",
            "pcpu",
            "role",
            "scheduler",
            "exclusive",
            "shared",
            "sched_bvt",
            "sched_rtds",
            "busy%",
            "timer",
            "switches",
            "resched",
            "runqueue",
            "current",
            "CPU usage since previous schedstat:",
            "cpu%",
            "BVT stats:",
            "RTDS stats:",
        ])
        qemu.command(
            "vmstat",
            [
                "┌─  vmstat vm0:Zephyr",
                "┌─  vmstat vm1:Linux-1",
                "┌─  vmstat vm2:Linux-2",
                "vcpus:configured:4 created:4",
                "│   affinity:",
                "boot:kernel:",
                "gic:initialized:",
                "its:enabled:",
                "timer:cntv:Y ppi:",
                "console:selected:",
                "ring:",
                "├─  vcpu state",
                "sched",
                "diag",
                "bvt:weight:",
                "rtds:period-us:",
                "timer:PPI27",
                "vgic:PPI27",
            ],
            ["assertion failed", "stack check fails", "fatal error"],
        )
        qemu.command("mmap", ["arm64 memory mappings", "vm-0 s2", "vm-1 s2", "vm-2 s2"])
        qemu.command("irqstat", ["irqstat:"])
        qemu.command(
            "virtiostat",
            ["virtio-fs vm2:0", "device:26", "tag:beau", "throughput:high", "virtio-rng vm2:1", "device:4", "tag:beau-rng", "throughput:low"],
        )
        qemu.command(
            "dumpstat 0",
            [
                "┌─  dumpstat vm0",
                "┌─  vm0/vcpu0",
                "sched:",
                "├─  vcpu stats",
                "guest regs:",
                "elr:0x",
                "spsr:0x",
                "x00:0x",
                "gt[",
                "├─  vgic/vtimer",
                "PPI27 cntv_ctl:",
                "vgic:en:",
                "route:saved-lr:",
                "hcr:0x",
                "wfi:trap:",
                "lr-pending-only:",
                "el2-mask:",
                "pre-eret-flush:",
                "vt[",
                "vcpu stack:",
                "pcpu stack:",
                "+0x",
            ],
            ["depth:", "vcpu saved stack", "vcpu vm stack", "host stack source:", "fp   0x",
             "live pcpu sample timed out", "source:", "source-vcpu:", "target-vcpu:",
             "target-mask:"],
        )

        qemu.send("vsh 0" + ENTER)
        qemu.expect("sos ~", "VM0 Zephyr shell", keepalive=ENTER)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM0 shell")

        qemu.send("vsh 1" + ENTER)
        qemu.expect(LINUX_PROMPT, "VM1 Linux initramfs shell", timeout=60.0, keepalive=ENTER)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM1 shell")

        qemu.send("vsh 2" + ENTER)
        try:
            qemu.expect(LINUX_PROMPT, "VM2 Linux initramfs shell", timeout=60.0, keepalive=ENTER)
        except Exception:
            qemu.capture_vm_diagnostics("VM2 Linux initramfs shell timeout", 2)
            raise
        expect_vm2_id(qemu, "VM2 Linux root identity")
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM2 shell")

        if args.stress_vsh_switch:
            run_vsh_switch_stress(qemu, args)
        if args.stress_vsh_help:
            run_vsh_help_stress(qemu, args)

    print(f"[pass] regression complete; log: {args.log}", flush=True)


def main():
    args = parse_args()
    build = make_cmd(args)
    qemu = qemu_cmd(args)

    if args.dry_run:
        if not args.no_build:
            print(render(build, args.toolchains))
        print(quote(qemu))
        checks = "prompt, vcpus, schedstat, vmstat, mmap, irqstat, virtiostat, vsh 0, ctrl-d, vsh 1, ctrl-d, vsh 2, Linux initramfs shell"
        if args.stress_vsh_switch:
            checks += ", VM console switch/Enter stress"
        if args.stress_vsh_help:
            checks += f", VM console help stress x{args.stress_help_rounds}"
        print(f"checks: {checks}")
        return 0

    if not args.no_build:
        run_build(args, build)
    run_qemu(args, qemu)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
