# BEAU

## Virtio-proxy Fs And Rng Validation

The QEMU virtio-proxy topology keeps virtio-fs and virtio-rng enabled at the
same time:

```text
VM2 Linux virtio-fs frontend  -> BEAU virtio_proxy device-id 26 -> VM1 fs backend
VM2 Linux virtio-rng frontend -> BEAU virtio_proxy device-id 4  -> VM1 rng backend
```

Build-time requirements:

- `arch/arm64/platform/qemu/platform.dts` advertises two proxy endpoints:
  fs at `0x0a000200` with `beau,device-id = <26>; beau,throughput = "high";`
  and rng at `0x0a000400` with `beau,device-id = <4>; beau,throughput = "low";`.
- VM1 Linux enables both `CONFIG_BEAU_VIRTIOFS_BACKEND=y` and
  `CONFIG_BEAU_VIRTIORNG_BACKEND=y`.
- VM2 Linux exposes both virtio-mmio frontend nodes in its DTB.
- VM2 Linux enables `CONFIG_HW_RANDOM_VIRTIO=y`.
- BEAU virtio-proxy is protocol-neutral transport: DTS can describe up to 32
  devices, and the VM1 backend HVC selects the target by `device_id`.

QEMU smoke test:

```text
console:\> virtiostat
```

Expected BEAU-side signals:

- `virtio-fs vm2:0` reports `device:26`, `tag:beau`, and `throughput:high`.
- `virtio-rng vm2:1` reports `device:4`, `tag:beau-rng`, and `throughput:low`.
- fs queues become ready after VM2 probes virtio-fs.
- rng queue 0 becomes ready after VM2 probes virtio-rng.
- `hcall:yes` or growing `poll/reply` counters after VM1 backend starts
- rng `last-reply len` grows when VM2 reads random bytes

VM1 backend check:

```sh
vsh 1
dmesg | grep -i 'BEAU virtio-'
```

Expected VM1 signal:

```text
BEAU virtio-fs backend started
BEAU virtio-rng backend started
```

VM2 virtio-rng frontend check:

```sh
vsh 2
dmesg | grep -i virtio_rng
cat /sys/class/misc/hw_random/rng_current
dd if=/dev/hwrng of=/tmp/beau-rng.bin bs=32 count=1
hexdump -C /tmp/beau-rng.bin
```

Expected VM2 signals:

- `rng_current` reports a virtio RNG provider, usually `virtio_rng.0`.
- `/dev/hwrng` returns non-zero random-looking bytes.
- Running `virtiostat` again in the BEAU shell shows increased reply
  counters and a recent reply length matching the read size.

VM2 virtio-fs frontend check:

```sh
vsh 2
mkdir -p /mnt/beau
mount -t virtiofs beau /mnt/beau
echo beau-fs-rng > /mnt/beau/proxy-check.txt
cat /mnt/beau/proxy-check.txt
```

VM1 export check:

```sh
vsh 1
cat /var/beau/proxy-check.txt
```

Expected fs signal:

- Both VM2 `/mnt/beau/proxy-check.txt` and VM1 `/var/beau/proxy-check.txt`
  show `beau-fs-rng`.

---

Hustle Embedded OS.
