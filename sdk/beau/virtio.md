# BEAU Virtio Proxy

## QEMU Topology

VM1 Linux owns the KBE protocol backends. VM2 and VM3 each expose the standard
virtio fs, rng, blk, i2c, and net frontends:

```text
VM2/VM3 frontend -> BEAU virtio_proxy -> VM1 KBE
fs  device-id 26 -> isolated /var/beau/vm2 or /var/beau/vm3
rng device-id 4  -> independent worker and buffers
blk device-id 2  -> independent 1 MiB RAM disk
i2c device-id 34 -> independent EEPROM state at address 0x50
net device-id 1  -> independent MAC and RX queue, shared VM1 uplink
```

The QEMU platform uses these guest physical MMIO ranges:

| Frontend | fs | rng | blk | i2c | net |
|---|---:|---:|---:|---:|---:|
| VM2 | `0x0a001200` | `0x0a001400` | `0x0a001600` | `0x0a001800` | `0x0a001a00` |
| VM3 | `0x0a000200` | `0x0a000400` | `0x0a000600` | `0x0a000800` | `0x0a000a00` |

Every platform endpoint must define both identities:

```dts
beau,frontend-vmid = <2>; /* or 3 */
beau,backend-vmid = <1>;
```

The DTS parser rejects a missing, invalid, or self-referential backend VMID.
The register HVC also checks the calling VM against the configured owner before
reading guest buffers or publishing runtime registration. An unauthorized
caller receives `-EPERM`; backend reset clears runtime registration but retains
the configured owner.

## Guest Requirements

VM1 enables `CONFIG_BEAU_VIRTIOFS_BACKEND`,
`CONFIG_BEAU_VIRTIORNG_BACKEND`, `CONFIG_BEAU_VIRTIOBLK_BACKEND`,
`CONFIG_BEAU_VIRTIOI2C_BACKEND`, and `CONFIG_BEAU_VIRTIONET_BACKEND`.
VM2 and VM3 enable the corresponding standard virtio frontend drivers.

The KBE common worker negotiates ABI v3 wait hints, heartbeat, statistics, and
shared batch buffers. Fs and blk use batches of up to four requests. Rng and
i2c use single-request workers. Net uses one worker and RX queue per frontend.

## Inspection

From the BEAU shell:

```text
console:\> virtiostat
```

Expected state:

- VM2 and VM3 each report five endpoints with `backend:vm1`.
- Fs/blk report high throughput; rng/i2c report low throughput.
- Backend ABI is 3 and heartbeat counters increase.
- VM2 net MAC is `52:54:00:be:02:00`; VM3 is
  `52:54:00:be:03:00`.

From VM1:

```sh
vsh 1
dmesg | grep -i 'BEAU virtio-'
ls -l /var/beau/vm2 /var/beau/vm3
```

From each frontend, replace `N` with 2 or 3:

```sh
vsh N
dd if=/dev/hwrng of=/tmp/rng.bin bs=32 count=1
i2cdetect -y 0
dd if=/dev/zero of=/dev/vda bs=4096 count=1 conv=fsync
mkdir -p /mnt/beau
mount -t virtiofs proxy-fs /mnt/beau
echo "vmN" > /mnt/beau/proxy-check.txt
```

VM1 must observe VM2 data only under `/var/beau/vm2` and VM3 data only under
`/var/beau/vm3`. Block and EEPROM writes from one frontend must not appear in
the other frontend. These isolation checks are required before the retained KBE
sources are synchronized into `sdk/kbe`.

---

Hustle Embedded OS.
