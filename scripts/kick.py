#!/usr/bin/env python3
import argparse
import os
import shlex
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CWD = Path.cwd()
LINUX_INITRAMFS_STAGE_ADDR = "0x74000000"
LINUX_VM2_IMAGE_STAGE_ADDR = "0x76000000"
LINUX_VM3_IMAGE_STAGE_ADDR = "0x7c000000"
DEFAULT_LINUX_INITRAMFS = ROOT / "sdk/imgs/linux/Initramfs.cpio.gz"
REPACK_INITRAMFS = ROOT / "scripts/repack_initramfs.sh"
DEFAULT_TEE_FIRMWARE = ROOT / "sdk/trusty/out/qemu/tf-a/qemu_fw.bios"
DEFAULT_TEE_BL33 = ROOT / "out/qemu_out/beau.debug.bin"
TEE_SMP_TOPOLOGY = "cpus=8,maxcpus=8,sockets=1,clusters=1,cores=8,threads=1"


def relpath(path):
    path = Path(path).expanduser()
    return path if path.is_absolute() else CWD / path


def render(cmd, toolchains=None):
    cmd = shlex.join([str(arg) for arg in cmd])
    return f"PATH={shlex.quote(str(toolchains))}:$PATH {cmd}" if toolchains else cmd


def getenv(name, default=None):
    value = os.getenv(name)
    return default if value is None else value


def parse_args():
    kernel_env = getenv("BEAU_KERNEL")
    kernel = relpath(kernel_env) if kernel_env else ROOT / "out/qemu_out/beau.debug.out"
    toolchains = getenv("BEAU_TOOLCHAINS")
    toolchains = getenv("BEAU_TOOLCHAIN", toolchains)
    toolchains = relpath(toolchains) if toolchains else None

    parser = argparse.ArgumentParser(description="Build and launch the ARM64 QEMU image.")
    parser.add_argument("-k", "--kernel", default=kernel, type=relpath)
    parser.add_argument("--qemu", default=os.getenv("QEMU_SYSTEM_AARCH64", "qemu-system-aarch64"))
    parser.add_argument("--smp", default=getenv("BEAU_QEMU_SMP", "8"))
    parser.add_argument("-m", "--memory", default=getenv("BEAU_QEMU_MEM", "1536M"))
    parser.add_argument(
        "--pmu-icount",
        action="store_true",
        help="enable precise QEMU icount for INST_RETIRED validation (slows SMP guests)",
    )
    parser.add_argument(
        "--linux-image",
        default=None,
        type=relpath,
        help="legacy: use one Linux Image for both Linux VM2 and Linux VM3",
    )
    parser.add_argument("--linux-vm1-image", default=None, type=relpath, help=argparse.SUPPRESS)
    parser.add_argument("--linux-vm2-image", default=ROOT / "sdk/imgs/linux/Image", type=relpath)
    parser.add_argument("--linux-vm3-image", default=ROOT / "sdk/imgs/linux/Image", type=relpath)
    parser.add_argument(
        "--linux-initramfs",
        dest="linux_initramfs",
        default=DEFAULT_LINUX_INITRAMFS,
        type=relpath,
    )
    parser.add_argument("--toolchains", "--toolchain", default=toolchains, type=relpath)
    parser.add_argument("--cross-prefix", default=getenv("BEAU_CROSS_COMPILE", "aarch64-none-elf-"))
    parser.add_argument("--build", action="store_true")
    parser.add_argument(
        "--tee",
        "--trusty",
        dest="tee",
        action="store_true",
        help="boot TF-A/Trusty BL32 and load the BEAU raw image as BL33",
    )
    parser.add_argument(
        "--tee-firmware",
        "--trusty-firmware",
        dest="tee_firmware",
        default=DEFAULT_TEE_FIRMWARE,
        type=relpath,
        help="TF-A qemu_fw.bios built with the Trusty SPD",
    )
    parser.add_argument(
        "--tee-bl33",
        "--trusty-bl33",
        dest="tee_bl33",
        default=DEFAULT_TEE_BL33,
        type=relpath,
        help="BEAU raw BL33 image preloaded at 0x50000000",
    )
    parser.add_argument(
        "--no-pcie-test",
        action="store_true",
        help="do not attach the default QEMU PCIe passthrough test endpoint",
    )
    parser.add_argument(
        "--pcie-net-backend",
        action="store_true",
        help="legacy no-op: the VM2 QEMU PCI net endpoint is attached by default",
    )
    parser.add_argument(
        "--no-pcie-net-backend",
        action="store_true",
        help="do not attach the default QEMU PCI net endpoint for the VM2 backend",
    )
    parser.add_argument(
        "--vm2-netdev",
        default=getenv("BEAU_VM2_NETDEV", "user,id=beau_vm2_net"),
        help="QEMU -netdev argument used for the VM2 PCI net endpoint",
    )
    parser.add_argument(
        "--vm2-net-device",
        default=getenv(
            "BEAU_VM2_NET_DEVICE",
            "virtio-net-pci-non-transitional,netdev=beau_vm2_net,addr=0x2,mac=52:54:00:be:02:00",
        ),
        help="QEMU -device argument used for the VM2 PCI net endpoint",
    )
    parser.add_argument(
        "--repack-initramfs",
        action="store_true",
        help="refresh the default shared Linux initramfs before building",
    )
    parser.add_argument("-n", "--dry-run", action="store_true")
    args, extra = parser.parse_known_args()
    if extra[:1] == ["--"]:
        extra = extra[1:]
    args.extra = extra
    if args.linux_vm1_image is not None:
        parser.error("--linux-vm1-image was removed because VM1 runs RT-Thread")
    if args.linux_image is not None:
        args.linux_vm2_image = args.linux_image
        args.linux_vm3_image = args.linux_image
    if args.tee and args.smp != "8":
        parser.error("--tee requires --smp 8 for the Trusty QEMU GICv3 configuration")
    return args


def print_image_plan(args):
    print("[kick] VM1 RT-Thread Image: embedded sdk/imgs/rtthread.bin", flush=True)
    print(f"[kick] VM2 Linux Image: {args.linux_vm2_image}", flush=True)
    print(f"[kick] VM3 Linux Image: {args.linux_vm3_image}", flush=True)
    print(f"[kick] shared Linux initramfs: {args.linux_initramfs}", flush=True)


def verify_guest_images(args):
    if not args.linux_vm2_image.is_file():
        raise SystemExit(f"Linux VM2 Image not found: {args.linux_vm2_image}")
    if not args.linux_vm3_image.is_file():
        raise SystemExit(f"Linux VM3 Image not found: {args.linux_vm3_image}")
    if not args.linux_initramfs.is_file():
        raise SystemExit(f"Linux shared initramfs not found: {args.linux_initramfs}")


def verify_tee_images(args):
    if not args.tee_firmware.is_file():
        raise SystemExit(f"TF-A Trusty firmware not found: {args.tee_firmware}")
    if not args.tee_bl33.is_file():
        raise SystemExit(f"BEAU BL33 raw image not found: {args.tee_bl33}")


def uses_default_initramfs(args):
    return args.linux_initramfs.resolve() == DEFAULT_LINUX_INITRAMFS.resolve()


def repack_default_initramfs(args):
    if not uses_default_initramfs(args):
        return
    if not REPACK_INITRAMFS.is_file():
        raise SystemExit(f"Linux initramfs repack script not found: {REPACK_INITRAMFS}")

    # print(f"[kick] refresh shared Linux initramfs: {args.linux_initramfs}", flush=True)
    subprocess.run(["sh", str(REPACK_INITRAMFS), str(args.linux_initramfs)], cwd=ROOT, check=True)


def make_cmd(args, *targets, jobs=False):
    cmd = [
        "make",
        "ARCH=arm64",
        "PLATFORM=qemu",
        f"CROSS_COMPILE={args.cross_prefix}",
    ]
    if jobs:
        cmd.append(f"-j{os.cpu_count() or 1}")
    cmd.extend(targets)
    return cmd


def build_steps(args):
    return [
        make_cmd(args, "clean"),
        make_cmd(args, "Bconfig"),
        make_cmd(args, "all", jobs=True),
    ]


def tee_firmware_cmd(args):
    return [
        "make",
        "-C",
        str(ROOT / "sdk/trusty"),
        "firmware",
        f"BEAU_BL33={args.tee_bl33}",
    ]


def tee_clean_cmd():
    return ["make", "-C", str(ROOT / "sdk/trusty"), "clean"]


def main():
    args = parse_args()
    env = os.environ.copy()
    if args.toolchains:
        env["PATH"] = f"{args.toolchains}{os.pathsep}{env.get('PATH', '')}"

    build_cmds = build_steps(args)
    machine = "virt,virtualization=on,gic-version=3,its=on,iommu=smmuv3"
    qemu_smp = args.smp
    if args.tee:
        machine = "virt,secure=on,virtualization=on,gic-version=3,its=on,iommu=smmuv3"
        qemu_smp = TEE_SMP_TOPOLOGY

    qemu_cmd = [
        args.qemu,
        "-machine",
        machine,
        "-global",
        "arm-smmuv3.stage=2",
        "-cpu",
        "cortex-a57",
        "-smp",
        qemu_smp,
        "-m",
        args.memory,
        "-nographic",
        "-serial",
        "mon:stdio",
        "-net",
        "none",
        "-device",
        f"loader,file={args.linux_vm2_image},addr={LINUX_VM2_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_vm3_image},addr={LINUX_VM3_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_initramfs},addr={LINUX_INITRAMFS_STAGE_ADDR},force-raw=on",
    ]
    if args.tee:
        qemu_cmd.extend(
            [
                "-bios",
                str(args.tee_firmware),
                "-device",
                f"loader,file={args.tee_bl33},addr=0x50000000,force-raw=on",
            ]
        )
    else:
        qemu_cmd.extend(["-kernel", str(args.kernel)])
    # Precise icount disables MTTCG; keep it opt-in for PMU register validation.
    if args.pmu_icount:
        qemu_cmd.extend(["-icount", "shift=0,align=off,sleep=off"])
    if not args.no_pcie_test:
        qemu_cmd.extend(["-device", "edu,addr=0x1"])
    if not args.no_pcie_net_backend:
        qemu_cmd.extend(["-netdev", args.vm2_netdev, "-device", args.vm2_net_device])
    qemu_cmd.extend(args.extra)

    if args.dry_run:
        # print_image_plan(args)
        if args.build:
            if args.repack_initramfs and uses_default_initramfs(args):
                print(render(["sh", REPACK_INITRAMFS, args.linux_initramfs]))
            for cmd in build_cmds:
                print(render(cmd, args.toolchains))
            if args.tee:
                print(render(tee_clean_cmd(), args.toolchains))
                print(render(tee_firmware_cmd(args), args.toolchains))
        print(render(qemu_cmd))
        return

    verify_guest_images(args)
    if args.build:
        # print_image_plan(args)
        if args.repack_initramfs:
            repack_default_initramfs(args)
        if args.toolchains and not args.toolchains.is_dir():
            raise SystemExit(f"Toolchain bin dir not found: {args.toolchains}")
        compiler = f"{args.cross_prefix}gcc"
        if shutil.which(compiler, path=env.get("PATH")) is None:
            raise SystemExit(f"Compiler not found: {compiler}")
        for cmd in build_cmds:
            subprocess.run(cmd, cwd=ROOT, env=env, check=True)
        if args.tee:
            subprocess.run(tee_clean_cmd(), cwd=ROOT, env=env, check=True)
            subprocess.run(tee_firmware_cmd(args), cwd=ROOT, env=env, check=True)

    if args.tee:
        verify_tee_images(args)
    elif not args.kernel.is_file():
        print(f"Kernel image not found: {args.kernel}")
        print("Build it with:")
        for cmd in build_cmds:
            print(f"  {render(cmd, args.toolchains)}")
        raise SystemExit(1)
    qemu = shutil.which(args.qemu)
    if qemu is None:
        raise SystemExit(f"QEMU binary not found: {args.qemu}")
    qemu_cmd[0] = qemu
    os.chdir(ROOT)
    # BEAU 2026
    print("\nKICKING BEAU OS on QEMU\n")
    os.execvp(qemu, qemu_cmd)


if __name__ == "__main__":
    main()
