#!/usr/bin/env python3
import argparse
import codecs
import json
import os
import re
import selectors
import shlex
import shutil
import socket
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CWD = Path.cwd()
PROMPT = "console:\\>"
LINUX_PROMPT = "uos ~"
LK_PROMPT = "uos ~"
RTTHREAD_PROMPT = "RT />"
ZEPHYR_PROMPT = "sos ~ "
HELP_STRESS_TARGETS = (
    (0, ZEPHYR_PROMPT, "VM0 Zephyr", 30.0),
    (1, RTTHREAD_PROMPT, "VM1 RT-Thread", 60.0),
    (2, LINUX_PROMPT, "VM2 Linux", 60.0),
    (3, LINUX_PROMPT, "VM3 Linux", 60.0),
)
ENTER = "\r"
CTRL_D = b"\x04"
LINUX_VM1_IMAGE_STAGE_ADDR = "0x70000000"
LINUX_INITRAMFS_STAGE_ADDR = "0x74000000"
LINUX_VM2_IMAGE_STAGE_ADDR = "0x76000000"
LINUX_VM3_IMAGE_STAGE_ADDR = "0x7c000000"
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
STR_FREEZE_MARKERS = ("PM_GUESTS_FROZEN", "PM_VM_SUSPENDED")
STR_THAW_MARKERS = ("PM_RESUMING", "PM_GUESTS_THAWED", "PM_RUNNING")
STR_FAULT_OPTIONS = (
    "prepare-timeout",
    "pending-wake",
    "hook-failure",
    "platform-failure",
    "duplicate-wake",
    "resume-timeout",
)


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
    parser.add_argument("--linux-vm3-image", default=ROOT / "sdk/image/linux/vm2/Image", type=relpath)
    parser.add_argument(
        "--linux-initramfs",
        dest="linux_initramfs",
        default=ROOT / "sdk/image/linux/Initramfs.cpio.gz",
        type=relpath,
    )
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--log", default=ROOT / "out/qemu_out/regress.log", type=relpath)
    parser.add_argument("--str-cycles", type=int, default=0)
    parser.add_argument("--str-vmid", type=int, default=0)
    parser.add_argument(
        "--qmp-socket",
        default=ROOT / "out/qemu_out/regress-qmp.sock",
        type=relpath,
    )
    parser.add_argument("--str-suspend-seconds", type=float, default=1.0)
    for fault in STR_FAULT_OPTIONS:
        parser.add_argument(
            f"--str-fault-{fault}",
            action="store_true",
            help=f"Run the STR {fault} fault case (requires the QEMU fault-injection build).",
        )
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--wdt-restart-smoke",
        action="store_true",
        help="Trigger one VM3 missed heartbeat and verify bounded cold watchdog recovery.",
    )
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
        args.linux_vm2_image = args.linux_image
        args.linux_vm3_image = args.linux_image
    if args.str_cycles < 0:
        parser.error("--str-cycles must not be negative")
    if args.str_vmid < 0:
        parser.error("--str-vmid must not be negative")
    if args.str_vmid > 3:
        parser.error("--str-vmid must be 0..3 for the QEMU static topology")
    if args.str_suspend_seconds < 0.0:
        parser.error("--str-suspend-seconds must not be negative")
    selected_faults = [
        fault for fault in STR_FAULT_OPTIONS
        if getattr(args, f"str_fault_{fault.replace('-', '_')}")
    ]
    if len(selected_faults) > 1:
        parser.error("select at most one --str-fault-* option")
    args.str_fault = selected_faults[0] if selected_faults else None
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
        "virt,virtualization=on,gic-version=3,its=on,iommu=smmuv3",
        "-cpu",
        "cortex-a57",
        "-smp",
        args.smp,
        "-m",
        args.memory,
        "-display",
        "none",
        "-serial",
        "stdio",
        "-monitor",
        "none",
        "-qmp",
        f"unix:{args.qmp_socket},server=on,wait=off",
        "-kernel",
        str(args.kernel),
        "-device",
        f"loader,file={args.linux_vm2_image},addr={LINUX_VM2_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_vm3_image},addr={LINUX_VM3_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_initramfs},addr={LINUX_INITRAMFS_STAGE_ADDR},force-raw=on",
        "-device",
        "edu,addr=0x1",
        "-netdev",
        "user,id=beau_vm2_net",
        "-device",
        "virtio-net-pci-non-transitional,netdev=beau_vm2_net,addr=0x2,mac=52:54:00:be:02:00",
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


class QmpClient:
    """Minimal newline-delimited JSON QMP client with bounded I/O."""

    def __init__(self, path, timeout):
        self.path = Path(path)
        self.timeout = timeout
        self.sock = None
        self.buffer = b""

    def __enter__(self):
        deadline = time.monotonic() + self.timeout
        last_error = None

        while time.monotonic() < deadline:
            candidate = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            candidate.settimeout(max(0.05, deadline - time.monotonic()))
            try:
                candidate.connect(str(self.path))
                self.sock = candidate
                break
            except (FileNotFoundError, ConnectionRefusedError, socket.timeout) as err:
                last_error = err
                candidate.close()
                time.sleep(0.05)
        if self.sock is None:
            raise TimeoutError(f"timed out connecting to QMP socket {self.path}: {last_error}")

        greeting = self._read_message(deadline)
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting: {greeting!r}")
        self._request({"execute": "qmp_capabilities"})
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _read_message(self, deadline):
        while time.monotonic() < deadline:
            if b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if line.strip():
                    return json.loads(line)
                continue

            remaining = deadline - time.monotonic()
            self.sock.settimeout(max(0.05, remaining))
            try:
                data = self.sock.recv(4096)
            except socket.timeout as err:
                raise TimeoutError("timed out reading a QMP response") from err
            if not data:
                raise RuntimeError("QMP socket closed while waiting for a response")
            self.buffer += data

        raise TimeoutError("timed out reading a QMP response")

    def _request(self, request):
        deadline = time.monotonic() + self.timeout
        payload = json.dumps(request, separators=(",", ":")).encode() + b"\n"
        self.sock.settimeout(self.timeout)
        try:
            self.sock.sendall(payload)
        except socket.timeout as err:
            raise TimeoutError(f"timed out sending QMP request {request!r}") from err

        while True:
            response = self._read_message(deadline)
            if "event" in response:
                continue
            if "error" in response:
                raise RuntimeError(f"QMP request failed: {request!r}: {response['error']!r}")
            if "return" in response:
                return response["return"]

    def stop(self):
        self._request({"execute": "stop"})

    def cont(self):
        self._request({"execute": "cont"})

    def query_status(self):
        return self._request({"execute": "query-status"})


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
        self.vm_command_seq = 0

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

    def send_slow(self, data, delay=0.002):
        if isinstance(data, str):
            data = data.encode()
        for byte in data:
            self.proc.stdin.write(bytes([byte]))
            self.proc.stdin.flush()
            if delay > 0.0:
                time.sleep(delay)

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

    def expect_all(self, patterns, name, start_offset=None, timeout=None):
        patterns = tuple(patterns)
        start_offset = len(self.output) if start_offset is None else start_offset
        deadline = time.monotonic() + (timeout or self.timeout)
        print(f"[regress] wait: {name}", flush=True)

        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {self.proc.returncode}")
            self.read_some(deadline)
            text = self.output[start_offset:]
            missing = [pattern for pattern in patterns if pattern not in text]
            if not missing:
                print(f"[pass] {name}", flush=True)
                return text

        tail = self.output[-3000:]
        raise TimeoutError(
            f"timed out waiting for {name}; missing {missing!r}\n"
            f"--- output tail ---\n{tail}"
        )

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

    def command_retry(self, line, patterns, rejects=None, attempts=8, delay=1.0):
        last_error = None

        for attempt in range(attempts):
            try:
                self.command(line, patterns, rejects=rejects)
                return
            except RuntimeError as err:
                last_error = err
                if attempt + 1 >= attempts:
                    break
                print(f"[regress] retry: {line}: {err}", flush=True)
                self.drain_for(delay)

        raise last_error

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


def run_wdt_restart_smoke(qemu):
    """Delay VM3's next kick and verify the watchdog recovers it end to end."""
    vsh_enter(qemu, 3, LINUX_PROMPT, "WDT smoke: VM3 Linux shell", timeout=60.0)
    vm3_command(
        qemu,
        "test -w /sys/module/vwdt/parameters/period_ms && echo 60000 > /sys/module/vwdt/parameters/period_ms",
        "WDT smoke: delay VM3 heartbeat",
    )
    vsh_return(qemu, "WDT smoke: return from VM3", vmid=3)

    qemu.expect("HWT: VM3 restart cause:timeout", "WDT smoke: timeout detected", timeout=45.0)
    qemu.expect("HWT: VM3 quiesced; cold restart", "WDT smoke: vCPUs quiesced", timeout=5.0)
    qemu.expect("VM3: load KERNEL", "WDT smoke: kernel reloaded", timeout=5.0)
    qemu.expect("HWT: VM3 restart launched; wait-kick", "WDT smoke: restart launched", timeout=10.0)
    qemu.expect("HWT: VM3 restart verified", "WDT smoke: restart verified", timeout=45.0)
    vsh_enter(qemu, 3, LINUX_PROMPT, "WDT smoke: VM3 shell after recovery", timeout=60.0)
    vsh_return(qemu, "WDT smoke: return from recovered VM3", vmid=3)
    print("[pass] watchdog cold-restart smoke complete", flush=True)


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


def expect_linux_id(qemu, vmid, name):
    token = f"__beau_vm{vmid}_id_{int(time.monotonic() * 1000000)}__"

    # Keep the concrete completion marker out of the echoed command line. The
    # guest expands $t only after executing id, so observing "_done" proves the
    # fresh id output has crossed the console before the marker.
    qemu.send_slow(f"t={token}; id; echo ${{t}}_done" + ENTER)
    try:
        text = qemu.expect(f"{token}_done", name, timeout=20.0, keepalive=ENTER)
        if "gid=0" not in text:
            raise RuntimeError(f"{name}: id output missing gid=0")
        qemu.expect(LINUX_PROMPT, f"{name}: prompt", timeout=5.0, keepalive=ENTER)
        qemu.drain_for(0.05)
    except Exception:
        qemu.capture_vm_diagnostics(name, vmid)
        raise


def expect_vm2_id(qemu, name):
    expect_linux_id(qemu, 2, name)


def expect_vm2_kbe_backends(qemu, name):
    checks = (
        "dmesg | grep -q 'BEAU virtio-fs backend started'",
        "dmesg | grep -q 'BEAU virtio-rng backend started'",
        "dmesg | grep -q 'BEAU virtio-blk backend started'",
        "dmesg | grep -q 'BEAU virtio-i2c backend started'",
    )
    try:
        vm_command(qemu, 2, " && ".join(checks), name, timeout=30.0)
    except Exception:
        qemu.capture_vm_diagnostics(name, 2)
        raise


def vm_command(qemu, vmid, command, name, patterns=None, timeout=20.0, expect_rc=0):
    qemu.vm_command_seq += 1
    token = f"__b{vmid}_{qemu.vm_command_seq}__"
    patterns = [] if patterns is None else patterns
    qemu.send_slow(
        f"t={token}; {command}; rc=$?; printf '\\n%s:%s\\n' \"$t\" \"$rc\"" + ENTER,
        delay=0.005,
    )
    text = qemu.expect(f"{token}:", name, timeout=timeout)
    if f"{token}:{expect_rc}" not in text:
        raise RuntimeError(f"{name}: command returned non-zero")
    for pattern in patterns:
        if pattern not in text:
            raise RuntimeError(f"{name}: output missing {pattern!r}")
    qemu.drain_for(0.05)
    return text


def vm3_command(qemu, command, name, patterns=None, timeout=20.0, expect_rc=0):
    return vm_command(qemu, 3, command, name, patterns=patterns,
                      timeout=timeout, expect_rc=expect_rc)


def expect_vm3_virtiofs(qemu, name):
    try:
        vm3_command(qemu, "mkdir -p /mnt/beau /tmp",
                    f"{name}: mount dirs")
        vm3_command(qemu, "umount /mnt/beau 2>/dev/null || true",
                    f"{name}: cleanup old mount")
        vm3_command(qemu, "mount -t virtiofs -o rw proxy-fs /mnt/beau",
                    f"{name}: mount proxy-fs", timeout=30.0)
        vm3_command(qemu, "printf BEAU-FS-OK >/mnt/beau/proxy-regress.txt",
                    f"{name}: write file")
        vm3_command(qemu, "cat /mnt/beau/proxy-regress.txt",
                    f"{name}: read file", patterns=["BEAU-FS-OK"])
        vm3_command(qemu, "umount /mnt/beau",
                    f"{name}: unmount", timeout=30.0)
    except Exception:
        qemu.capture_vm_diagnostics(name, 3)
        raise


def expect_vm3_virtiorng(qemu, name):
    try:
        vm3_command(qemu, "test -c /dev/hwrng",
                    f"{name}: hwrng node")
        vm3_command(qemu, "grep -q virtio /sys/class/misc/hw_random/rng_current",
                    f"{name}: virtio rng selected")
        vm3_command(qemu, "dd if=/dev/hwrng of=/tmp/beau-rng.bin bs=32 count=1 2>/dev/null",
                    f"{name}: read hwrng")
        vm3_command(qemu, "test $(wc -c </tmp/beau-rng.bin) -eq 32",
                    f"{name}: hwrng size")
        vm3_command(qemu, "dd if=/dev/zero of=/tmp/beau-rng-zero.bin bs=32 count=1 2>/dev/null; ! cmp -s /tmp/beau-rng.bin /tmp/beau-rng-zero.bin",
                    f"{name}: hwrng nonzero")
    except Exception:
        qemu.capture_vm_diagnostics(name, 3)
        raise


def expect_vm3_virtioblk(qemu, name):
    try:
        vm3_command(qemu, "test -b /dev/vda",
                    f"{name}: block node")
        vm3_command(qemu, "grep -q 2048 /sys/block/vda/size",
                    f"{name}: sector count")
        vm3_command(qemu, "mkdir -p /tmp",
                    f"{name}: temp dir")
        vm3_command(qemu, "rm -f /tmp/beau-blk.w /tmp/beau-blk.r",
                    f"{name}: cleanup")
        vm3_command(qemu, "printf BEAU-BLK-OK >/tmp/beau-blk.w",
                    f"{name}: marker")
        vm3_command(qemu, "dd if=/dev/zero bs=4085 count=1 >>/tmp/beau-blk.w 2>/dev/null",
                    f"{name}: pad")
        vm3_command(qemu, "dd if=/tmp/beau-blk.w of=/dev/vda bs=4096 count=1 2>/dev/null",
                    f"{name}: write")
        vm3_command(qemu, "dd if=/dev/vda of=/tmp/beau-blk.r bs=4096 count=1 2>/dev/null",
                    f"{name}: read")
        vm3_command(qemu, "cmp /tmp/beau-blk.w /tmp/beau-blk.r",
                    name)
    except Exception:
        qemu.capture_vm_diagnostics(name, 3)
        raise


def expect_vm3_virtioi2c(qemu, name):
    try:
        vm3_command(qemu, "test -c /dev/i2c-0",
                    f"{name}: i2c dev node")
        vm3_command(qemu, "i2cdetect -y 0",
                    f"{name}: detect 0x50", patterns=["50"], timeout=30.0)
        vm3_command(qemu, "i2ctransfer -y 0 w1@0x50 0x00 r16 >/tmp/beau-i2c-prefix.txt",
                    f"{name}: read EEPROM prefix", timeout=30.0)
        vm3_command(qemu, "test $(wc -w </tmp/beau-i2c-prefix.txt) -eq 16",
                    f"{name}: read EEPROM prefix size")
        vm3_command(qemu, "i2ctransfer -y 0 w3@0x50 0x10 0xab 0xcd",
                    f"{name}: write EEPROM bytes", timeout=30.0)
        vm3_command(qemu, "i2ctransfer -y 0 w1@0x50 0x10 r2 >/tmp/beau-i2c-readback.txt",
                    f"{name}: read EEPROM bytes", timeout=30.0)
        vm3_command(qemu, "test $(wc -w </tmp/beau-i2c-readback.txt) -eq 2",
                    f"{name}: read EEPROM byte count")
    except Exception:
        qemu.capture_vm_diagnostics(name, 3)
        raise


def expect_vm3_virtio_proxy_smoke(qemu):
    expect_vm3_virtiofs(qemu, "VM3 virtio-fs mount/write/read")
    expect_vm3_virtiorng(qemu, "VM3 virtio-rng read")
    expect_vm3_virtioblk(qemu, "VM3 virtio-blk 4K write/read")
    expect_vm3_virtioi2c(qemu, "VM3 virtio-i2c detect/transfer")


def expect_rttest(qemu, command, pcpu_count):
    start_len = len(qemu.output)

    qemu.send(command + ENTER)
    qemu.expect(PROMPT, f"{command} starts", timeout=3.0)
    qemu.expect(f"T:{pcpu_count - 1:2d}", f"{command} summary", timeout=15.0)
    qemu.drain_for(0.2)
    output = qemu.output[start_len:]

    for pcpu_id in range(pcpu_count):
        pattern = f"T:{pcpu_id:2d} ({pcpu_id:5d}) P: 0 I:1000"
        if pattern not in output:
            raise RuntimeError(f"{command} output missing {pattern!r}")
    if len(re.findall(r"C:\s+1000(?:\s|$)", output)) != pcpu_count:
        raise RuntimeError(f"{command} output does not contain {pcpu_count} completed samples")
    print(f"[pass] {command}: per-pCPU output found", flush=True)


def expect_vm2_cpu1_lifecycle(qemu):
    online = "/sys/devices/system/cpu/cpu1/online"

    vm_command(qemu, 2, f"test -w {online} && echo 0 > {online}",
               "VM2 CPU1 offline", timeout=30.0)
    vsh_return(qemu, "return from VM2 after CPU1 offline", vmid=2)

    for command in ("vcpus", "ps", "schedstat", "vmstat", "dumpstat 2"):
        vcpu_name = "vm2/vcpu1" if command.startswith("dumpstat") else "vm2:vcpu1"
        qemu.command_retry(command, [vcpu_name, "poweroff"])

    vsh_enter(qemu, 2, LINUX_PROMPT, "VM2 shell for CPU1 online", timeout=30.0)
    vm_command(qemu, 2, f"echo 1 > {online} && test \"$(cat {online})\" = 1",
               "VM2 CPU1 online", timeout=30.0)
    print("[pass] VM2 CPU1 lifecycle poweroff -> running", flush=True)


def run_guest_help(qemu, vmid, prompt, name, timeout):
    print(f"[regress] stress: {name}: help", flush=True)
    qemu.send("help" + ENTER)
    try:
        qemu.expect(prompt, f"{name}: help returns", timeout=timeout, keepalive=ENTER)
    except Exception:
        qemu.capture_vm_diagnostics(f"{name}: help", vmid)
        raise


def check_zephyr_thread_list(qemu, label):
    qemu.send("kernel thread list" + ENTER)
    qemu.expect("Threads:", f"{label}: thread list starts", timeout=20.0)
    qemu.expect(ZEPHYR_PROMPT, f"{label}: thread list returns", timeout=20.0,
                keepalive=ENTER)


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

    vsh_enter(qemu, 1, RTTHREAD_PROMPT, "stress VM1 RT-Thread shell", timeout=30.0)
    send_enter_burst(qemu, args.stress_enters, args.stress_enter_delay, "VM1 RT-Thread", vmid=1)
    vsh_return(qemu, "return from stress VM1", vmid=1)

    for idx in range(args.stress_rounds):
        label = idx + 1
        vsh_enter(qemu, 0, ZEPHYR_PROMPT, f"stress round {label}: VM0 Zephyr shell")
        send_enter_burst(qemu, max(1, args.stress_enters // 4),
            args.stress_enter_delay, f"round {label} VM0", vmid=0)
        vsh_return(qemu, f"stress round {label}: return from VM0", vmid=0)

        vsh_enter(qemu, 1, RTTHREAD_PROMPT, f"stress round {label}: VM1 RT-Thread shell",
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

        vsh_enter(qemu, 3, LINUX_PROMPT, f"stress round {label}: VM3 Linux shell",
            timeout=30.0)
        send_enter_burst(qemu, args.stress_enters, args.stress_enter_delay,
            f"round {label} VM3", vmid=3)
        expect_linux_id(qemu, 3, f"stress round {label}: VM3 identity after switch")
        vsh_return(qemu, f"stress round {label}: return from VM3", vmid=3)

    print("[pass] VM console switch stress complete", flush=True)


def assert_qmp_status(qmp, expected, name):
    status = qmp.query_status()
    if status.get("status") != expected:
        raise RuntimeError(f"{name}: expected QMP status {expected!r}, got {status!r}")
    print(f"[pass] {name}: QMP status {expected}", flush=True)


def check_str_guest_heartbeats(qemu, cycle):
    label = f"STR cycle {cycle}"
    vsh_enter(qemu, 0, ZEPHYR_PROMPT, f"{label}: VM0 heartbeat")
    run_guest_help(qemu, 0, ZEPHYR_PROMPT, f"{label}: VM0", 15.0)
    check_zephyr_thread_list(qemu, f"{label}: VM0 SMP runtime stats")
    vsh_return(qemu, f"{label}: return from VM0", vmid=0)

    vsh_enter(qemu, 1, RTTHREAD_PROMPT, f"{label}: VM1 heartbeat", timeout=30.0)
    run_guest_help(qemu, 1, RTTHREAD_PROMPT, f"{label}: VM1", 15.0)
    vsh_return(qemu, f"{label}: return from VM1", vmid=1)

    vsh_enter(qemu, 2, LINUX_PROMPT, f"{label}: VM2 heartbeat", timeout=30.0)
    expect_linux_id(qemu, 2, f"{label}: VM2 identity")
    vsh_return(qemu, f"{label}: return from VM2", vmid=2)

    vsh_enter(qemu, 3, LINUX_PROMPT, f"{label}: VM3 heartbeat", timeout=30.0)
    expect_linux_id(qemu, 3, f"{label}: VM3 identity")
    vsh_return(qemu, f"{label}: return from VM3", vmid=3)


def run_str_cycle(qemu, args, cycle):
    vmid = args.str_vmid
    label = f"VM STR cycle {cycle} vm{vmid}"
    if args.str_fault is not None:
        raise RuntimeError(
            f"{label}: {args.str_fault} is a system STR fault-injection case"
        )

    start_offset = len(qemu.output)
    qemu.send(f"pm suspend {vmid}" + ENTER)
    qemu.expect_all(
        STR_FREEZE_MARKERS,
        f"{label}: BEAU froze target VM",
        start_offset=start_offset,
    )
    qemu.expect(PROMPT, f"{label}: BEAU shell responsive while target suspended")

    time.sleep(args.str_suspend_seconds)

    resume_offset = len(qemu.output)
    qemu.send(f"pm resume {vmid}" + ENTER)
    qemu.expect_all(
        STR_THAW_MARKERS,
        f"{label}: BEAU thawed target VM",
        start_offset=resume_offset,
    )
    qemu.send(ENTER)
    qemu.expect(PROMPT, f"{label}: BEAU shell responsive after resume")

    qemu.command_retry("pm status", [
        f"pm epoch:{cycle}",
        "phase:running",
        "masks:policy:0x000000000000000f required:0x0000000000000000",
        "wake:reason:0",
    ])
    qemu.command_retry(
        "health",
        ["overall:", "Host", "Virtual machines", "vm0", "vm1", "vm2", "vm3"],
    )
    qemu.command_retry("irqstat", ["host pirq:", "guest virq:"])
    qemu.command_retry("virtiostat", ["virtio-fs vm3:0", "virtio-rng vm3:1"])
    qemu.command_retry("pcistat", ["pcistat:"])
    check_str_guest_heartbeats(qemu, cycle)
    print(f"[pass] {label}: complete", flush=True)


def run_str_cycles(qemu, args):
    if args.str_cycles == 0:
        return

    for cycle in range(1, args.str_cycles + 1):
        run_str_cycle(qemu, args, cycle)


def run_qemu(args, cmd):
    if not args.kernel.is_file():
        raise SystemExit(f"Kernel image not found: {args.kernel}")
    if not args.linux_vm2_image.is_file():
        raise SystemExit(f"Linux VM2 Image not found: {args.linux_vm2_image}")
    if not args.linux_vm3_image.is_file():
        raise SystemExit(f"Linux VM3 Image not found: {args.linux_vm3_image}")
    if not args.linux_initramfs.is_file():
        raise SystemExit(f"Linux initramfs not found: {args.linux_initramfs}")
    if shutil.which(args.qemu) is None:
        raise SystemExit(f"QEMU binary not found: {args.qemu}")

    args.qmp_socket.parent.mkdir(parents=True, exist_ok=True)
    args.qmp_socket.unlink(missing_ok=True)
    print(f"[regress] qemu: {quote(cmd)}", flush=True)
    with QemuSession(cmd, args.log, args.timeout) as qemu:
        qemu.disable_terminal_replies = args.no_terminal_replies
        qemu.expect(PROMPT, "BEAU shell prompt", keepalive=ENTER)
        if args.wdt_restart_smoke:
            run_wdt_restart_smoke(qemu)
            return
        qemu.command_retry("pm status", [
            "pm epoch:0",
            "phase:running",
            "controller:vm2",
            "enabled:Y",
            "mode:simulated",
            "masks:policy:0x000000000000000f",
            "timeouts:prepare:5000ms resume:5000ms",
        ])
        qemu.command_retry("pmstat", [
            "pm epoch:0",
            "phase:running",
            "controller:vm2",
            "mode:simulated",
        ])
        qemu.command_retry("vcpus", [
            "vcpu",
            "pcpu_mode",
            "exclusive",
            "shared",
            "switches",
            "since.us",
            "vm0:vcpu0",
            "vm2:vcpu2",
            "vm3:vcpu0",
        ])
        qemu.command_retry("schedstat", [
            "schedstat pcpus:",
            "Per-pCPU hybrid scheduler counters:",
            "pcpu",
            "role",
            "scheduler",
            "exclusive",
            "shared",
            "sched_bvt",
            "sched_cbs",
            "busy%",
            "timer",
            "switches",
            "resched",
            "runqueue",
            "current",
            "CPU usage since previous schedstat:",
            "cpu%",
            "BVT stats:",
            "CBS stats:",
        ])
        pcpu_count = int(args.smp)
        expect_rttest(qemu, "rttest", pcpu_count)
        qemu.command_retry(
            "vmstat",
            [
                "┌─  vmstat vm0:Zephyr",
                "┌─  vmstat vm1:RT-Thread",
                "┌─  vmstat vm2:Linux-2",
                "┌─  vmstat vm3:Linux-3",
                "vcpus:configured:4 created:4",
                "vcpus:configured:2 created:2",
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
                "timer:PPI27",
                "vgic:PPI27",
            ],
            ["assertion failed", "stack check fails", "fatal error"],
        )
        qemu.command_retry("devmap", ["arm64 memory mappings", "vm-0 s2", "vm-1 s2", "vm-2 s2", "vm-3 s2"])
        qemu.command_retry(
            "memstat",
            ["Page-table pools", "hv-s1", "vm-s2", "Stage-2 ownership", "accounted:"],
        )
        qemu.command_retry(
            "health",
            ["overall:", "Host", "Virtual machines", "Findings", "vm0", "vm1", "vm2", "vm3"],
        )
        qemu.command_retry("irqstat", ["host pirq:", "guest virq:"])
        qemu.command_retry(
            "virtiostat",
            [
                "virtio-fs vm3:0",
                "device:",
                "proxy-fs",
                "throughput:high",
                "virtio-rng vm3:1",
                "proxy-rng",
                "throughput:low",
                "virtio-blk vm3:2",
                "proxy-blk",
                "virtio-i2c vm3:3",
                "proxy-i2c",
            ],
        )
        qemu.command_retry(
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
                "PPI27 live:",
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
        qemu.expect(ZEPHYR_PROMPT, "VM0 Zephyr shell", keepalive=ENTER)
        run_guest_help(qemu, 0, ZEPHYR_PROMPT, "VM0 Zephyr", 15.0)
        check_zephyr_thread_list(qemu, "VM0 Zephyr SMP runtime stats")
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM0 shell")

        qemu.send("vsh 1" + ENTER)
        qemu.expect(RTTHREAD_PROMPT, "VM1 RT-Thread shell", timeout=60.0, keepalive=ENTER)
        run_guest_help(qemu, 1, RTTHREAD_PROMPT, "VM1 RT-Thread", 15.0)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM1 shell")

        qemu.send("vsh 2" + ENTER)
        try:
            qemu.expect(LINUX_PROMPT, "VM2 Linux initramfs shell", timeout=60.0, keepalive=ENTER)
        except Exception:
            qemu.capture_vm_diagnostics("VM2 Linux initramfs shell timeout", 2)
            raise
        expect_vm2_id(qemu, "VM2 Linux root identity")
        expect_vm2_kbe_backends(qemu, "VM2 BEAU KBE backend startup")
        expect_vm2_cpu1_lifecycle(qemu)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM2 shell")

        qemu.send("vsh 3" + ENTER)
        try:
            qemu.expect(LINUX_PROMPT, "VM3 Linux initramfs shell", timeout=60.0, keepalive=ENTER)
        except Exception:
            qemu.capture_vm_diagnostics("VM3 Linux initramfs shell timeout", 3)
            raise
        expect_linux_id(qemu, 3, "VM3 Linux root identity")
        expect_vm3_virtio_proxy_smoke(qemu)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM3 shell")

        if args.stress_vsh_switch:
            run_vsh_switch_stress(qemu, args)
        if args.stress_vsh_help:
            run_vsh_help_stress(qemu, args)
        run_str_cycles(qemu, args)

    args.qmp_socket.unlink(missing_ok=True)

    print(f"[pass] regression complete; log: {args.log}", flush=True)


def main():
    args = parse_args()
    build = make_cmd(args)
    qemu = qemu_cmd(args)

    if args.dry_run:
        if not args.no_build:
            print(render(build, args.toolchains))
        print(quote(qemu))
        checks = "prompt, vcpus, schedstat, vmstat, devmap, irqstat, virtiostat, vsh 0, ctrl-d, vsh 1, RT-Thread shell, ctrl-d, vsh 2, Linux-2 backend shell, ctrl-d, vsh 3, Linux-3 frontend shell"
        if args.stress_vsh_switch:
            checks += ", VM console switch/Enter stress"
        if args.stress_vsh_help:
            checks += f", VM console help stress x{args.stress_help_rounds}"
        if args.wdt_restart_smoke:
            checks = "VM3 watchdog timeout, quiesce, cold restart, verification kick, VM3 shell"
        print(f"checks: {checks}")
        return 0

    if not args.no_build:
        run_build(args, build)
    run_qemu(args, qemu)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
