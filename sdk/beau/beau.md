# BEAU

## Virtio-proxy Fs, Rng, Blk, And I2c Validation

The QEMU virtio-proxy topology keeps virtio-fs, virtio-rng, virtio-blk, and
virtio-i2c enabled at the same time:

```text
VM2 Linux virtio-fs frontend  -> BEAU virtio_proxy device-id 26 -> VM1 fs backend
VM2 Linux virtio-rng frontend -> BEAU virtio_proxy device-id 4  -> VM1 rng backend
VM2 Linux virtio-blk frontend -> BEAU virtio_proxy device-id 2  -> VM1 blk RAM backend
VM2 Linux virtio-i2c frontend -> BEAU virtio_proxy device-id 34 -> VM1 i2c EEPROM backend
```

Build-time requirements:

- `arch/arm64/platform/qemu/platform.dts` advertises three proxy endpoints:
  fs at `0x0a000200` with `beau,device-id = <26>; beau,throughput = "high";`
  rng at `0x0a000400` with `beau,device-id = <4>; beau,throughput = "low";`,
  blk at `0x0a000600` with `beau,device-id = <2>; beau,throughput = "high";`,
  and i2c at `0x0a000800` with `beau,device-id = <34>; beau,throughput = "low";`.
  All endpoints set `beau,frontend-vmid = <2>;`, so only VM2 gets the virtio
  frontends while VM1 stays as the backend owner.
- VM1 Linux enables `CONFIG_BEAU_VIRTIOFS_BACKEND=y`,
  `CONFIG_BEAU_VIRTIORNG_BACKEND=y`, `CONFIG_BEAU_VIRTIOBLK_BACKEND=y`, and
  `CONFIG_BEAU_VIRTIOI2C_BACKEND=y`.
- VM2 Linux exposes all four virtio-mmio frontend nodes in its DTB.
- VM2 Linux enables `CONFIG_HW_RANDOM_VIRTIO=y`, `CONFIG_VIRTIO_BLK=y`,
  `CONFIG_I2C_VIRTIO=y`, and `CONFIG_I2C_CHARDEV=y`.
- BEAU virtio-proxy is protocol-neutral transport: DTS can describe up to 32
  devices, and the VM1 backend HVC selects the target by `device_id`.

QEMU smoke test:

```text
console:\> virtiostat
```

Expected BEAU-side signals:

- `virtio-fs vm2:0` reports `device:26`, `tag:proxy-fs`, and `throughput:high`.
- `virtio-rng vm2:1` reports `device:4`, `tag:proxy-rng`, and `throughput:low`.
- `virtio-blk vm2:2` reports `device:2`, `tag:proxy-blk`, and `throughput:high`.
- `virtio-i2c vm2:3` reports `device:34`, `tag:proxy-i2c`, and `throughput:low`.
- fs queues become ready after VM2 probes virtio-fs.
- rng queue 0 becomes ready after VM2 probes virtio-rng.
- blk queue 0 becomes ready after VM2 probes virtio-blk.
- i2c queue 0 becomes ready after VM2 probes virtio-i2c.
- `backend:vm1` or growing `poll/reply` counters after VM1 backend starts.
- `busy` means the backend poll found no available frontend request; `bp`
  means the proxy pending slots were full and the backend was backpressured.
- rng `last-reply len` grows when VM2 reads random bytes
- blk `last-reply len` grows when VM2 reads or writes `/dev/vda`.

VM1 backend check:

```sh
vsh 1
dmesg | grep -i 'BEAU virtio-'
```

Expected VM1 signal:

```text
BEAU virtio-fs backend started
BEAU virtio-rng backend started
BEAU virtio-blk backend started, 1024 KiB RAM disk
BEAU virtio-i2c backend started, EEPROM at 0x50
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

VM2 virtio-i2c frontend check:

```sh
vsh 2
dmesg | grep -i 'i2c_virtio\|virtio.*i2c'
ls /dev/i2c-*
i2cdetect -y 0
i2ctransfer -y 0 w1@0x50 0x00 r16
i2ctransfer -y 0 w3@0x50 0x10 0xab 0xcd
i2ctransfer -y 0 w1@0x50 0x10 r2
```

Expected VM2 signals:

- `i2cdetect` shows a device at `0x50`.
- The first read returns the backend's default EEPROM contents.
- The final read returns `0xab 0xcd`.

VM2 virtio-blk frontend check:

```sh
vsh 2
dmesg | grep -i virtio_blk
ls -l /dev/vd*
cat /sys/block/vda/size
mkdir -p /tmp
dd if=/dev/zero of=/tmp/beau-blk.w bs=4096 count=1
printf BEAU-BLK-OK | dd of=/tmp/beau-blk.w bs=1 conv=notrunc
dd if=/tmp/beau-blk.w of=/dev/vda bs=4096 count=1 conv=fsync
dd if=/dev/vda of=/tmp/beau-blk.r bs=4096 count=1
cmp /tmp/beau-blk.w /tmp/beau-blk.r
hexdump -C /tmp/beau-blk.r | head
```

Expected blk signals:

- `/dev/vda` exists and `/sys/block/vda/size` reports `2048` sectors.
- `cmp` exits successfully after the VM2 write/read loop.
- Running `virtiostat` again in the BEAU shell shows `virtio-blk vm2:2` reply
  counters increasing. The backend is a 1 MiB RAM disk, so data is not
  persistent across VM1 reboot.

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
