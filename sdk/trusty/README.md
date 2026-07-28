# Trusty TEE for QEMU ARM64

This directory contains the QEMU proof-of-concept secure boot chain:

TF-A BL1/BL2/BL31 -> Trusty LK BL32 -> BEAU BL33

Trusty-specific source additions live under 'lk/'. The 'lk/project/qemu-arm64.mk'
profile uses secure DRAM at '0x0e100000' with size '0x00f00000', QEMU GICv3,
and the PL011 console. It deliberately does not include device-tree services,
Android Binder, KTIPC, user TAs, shared-memory calls, or a guest-visible FF-A
transport.

Build the firmware after building the BEAU raw image:

```sh
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j"$(getconf _NPROCESSORS_ONLN)"
make -C sdk/trusty CROSS_COMPILE=aarch64-none-elf- firmware
```

Launch BEAU OS with Trusty TEE:

```sh
python3 scripts/kick.py --build --tee
```

The resulting 'out/qemu/tf-a/qemu_fw.bios' is coupled to the specified BEAU
raw BL33 image. Rebuild TF-A when the BL33 image changes.

## Contents

- `lk`: Trusty LK and the QEMU-only minimal Trusty SM source closure.
- `tfa`: Trusted Firmware-A with the Trusty SPD.
