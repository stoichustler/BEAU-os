# BEAU Linux Guest Drivers

This directory keeps the BEAU Linux guest driver sources outside a specific
Linux kernel tree so they can be ported to multiple Linux versions.

## Files

- `hcall.c`, `hcall.h`: shared BEAU HVC helpers and virtio-proxy ABI structs.
- `virtio-proxy-backend.c`, `virtio-proxy-backend.h`: common VM2 backend
  worker for registration, wait-hinted polling, batch/shared-buffer polling,
  reply, and heartbeat.
- `virtio-fs-backend.c`: VM2 virtio-fs backend for VM3 frontend access to
  `/var/beau`.
- `virtio-rng-backend.c`: VM2 virtio-rng backend for VM3 frontend entropy
  requests through BEAU virtio-proxy.
- `virtio-blk-backend.c`: VM2 virtio-blk RAM backend for VM3 frontend block
  read/write validation through BEAU virtio-proxy.
- `virtio-i2c-backend.c`: VM2 virtio-i2c EEPROM-style backend for VM3
  frontend I2C validation through BEAU virtio-proxy.
- `virtio-net-backend.c`: VM2 virtio-net uplink backend for VM3 frontend
  Ethernet validation through BEAU virtio-proxy.
- `vwdt.c`: BEAU VM watchdog heartbeat driver.
- `beau_static_rproc.c`: attach-only Linux remoteproc transport for a BEAU
  Zephyr service VM, using standard virtio-rpmsg and `rpmsg_char`.
- `beau-static-rproc.Kconfig`, `beau-static-rproc.Makefile`: integration
  fragments for `drivers/remoteproc`.
- `beau_rpmsg_peer.c`: VM3 RPMsg peer for the dedicated VM0 full-duplex
  validation endpoint.
- `beau-rpmsg-peer.Kconfig`, `beau-rpmsg-peer.Makefile`: integration
  fragments for `drivers/rpmsg`.
- `Kconfig`, `Makefile`: Kbuild integration snippets for `drivers/virt/beau`.

## Porting To A Linux Tree

1. Create `drivers/virt/beau` in the target Linux source tree.
2. Copy all files from this directory into that target directory.
3. Add `source "drivers/virt/beau/Kconfig"` to `drivers/virt/Kconfig` if it is
   not already present.
4. Add `obj-$(CONFIG_BEAU) += beau/` to `drivers/virt/Makefile`.
5. Run the target kernel's config update, then enable:
   - `CONFIG_BEAU`
   - `CONFIG_BEAU_VWDT`
   - one or more virtio-proxy backends, for example:
     `CONFIG_BEAU_VIRTIOFS_BACKEND`, `CONFIG_BEAU_VIRTIORNG_BACKEND`, and
     `CONFIG_BEAU_VIRTIOBLK_BACKEND`, `CONFIG_BEAU_VIRTIOI2C_BACKEND`, and
     `CONFIG_BEAU_VIRTIONET_BACKEND`
6. Build the target kernel image and install it into the BEAU Linux image slot
   used by the VM that should run the driver.

## Static Remoteproc/RPMsg

To integrate the VM3 attach-only transport, copy `beau_static_rproc.c` into
`drivers/remoteproc`, append `beau-static-rproc.Kconfig` to that directory's
`Kconfig`, and append `beau-static-rproc.Makefile` to its `Makefile`. Enable
`CONFIG_REMOTEPROC`, `CONFIG_RPMSG_VIRTIO`, `CONFIG_RPMSG_CHAR`, and
`CONFIG_BEAU_STATIC_RPROC`; `RPMSG_CHAR` also requires `CONFIG_NET`.

For VM0-initiated full-duplex validation, copy `beau_rpmsg_peer.c` into
`drivers/rpmsg`, append `beau-rpmsg-peer.Kconfig` and
`beau-rpmsg-peer.Makefile` to that directory's Kconfig and Makefile, then
enable `CONFIG_BEAU_RPMSG_PEER`. The driver binds only `beau-rpmsg-peer`:
it publishes READY and returns an ACK with the validated sequence and payload.
The existing `rpmsg-raw` endpoint remains available to `rpmsg_char`.

The guest DT node has compatible `beau,static-rproc`, a `shared` memory
resource followed by a `doorbell` resource, and one GIC SPI interrupt. The
driver starts in `RPROC_DETACHED`; userspace explicitly attaches it through
the standard remoteproc `state` sysfs file. It does not load firmware or
control the service VM lifecycle.

## Notes

The virtio-fs backend currently supports a narrow test export: VM3 can create,
truncate, write, read, and update attributes for regular files directly under
VM2's `/var/beau`. Directory mutation, rename, unlink, xattr, and full FUSE
semantics are intentionally not implemented here.

The virtio-proxy HVC ABI includes the virtio device id, so multiple protocol
backends can register for the same frontend VM. The QEMU test topology uses
VM3 virtio-fs (`device-id = 26`), VM3 virtio-rng (`device-id = 4`), and VM3
virtio-blk (`device-id = 2`), VM3 virtio-i2c (`device-id = 34`), and VM3
virtio-net (`device-id = 1`) at the same time, all serviced by VM2 Linux
backends.

The virtio-blk backend is a validation backend, not persistent storage. It
exports a 1 MiB RAM disk, supports single-segment 4 KiB read/write requests,
`GET_ID`, and `FLUSH`, and loses contents when VM2 reboots.

The virtio-i2c backend is also a validation backend. It exposes one in-memory
7-bit I2C device at address `0x50`; byte 0 of a write selects the EEPROM
offset, later write bytes update the map, and reads return bytes starting at
the current offset.

The virtio-net backend is a QEMU validation backend. It exposes VM3 MAC
`52:54:00:be:03:00`, advertises only `VIRTIO_NET_F_MAC` and
`VIRTIO_NET_F_STATUS`, and forwards frames through one VM2 uplink netdev. The
uplink defaults to `eth0`; override it with the backend's `uplink` parameter
when the passthrough or QEMU NIC has a different interface name. If `eth0` is
not present, the backend auto-selects the first non-loopback VM2 netdev. The backend
keeps offload, mergeable buffers, control queue, multi-queue, and RSS disabled
for the first BEAU virtio-proxy data-plane validation.
On QEMU, keep the existing edu passthrough endpoint at `addr=0x1`/BDF
`0x0008`; attach a modern MMIO PCI net endpoint such as
`virtio-net-pci-non-transitional` as an additional endpoint at `addr=0x2`/BDF
`0x0010`. VM3 still uses the virtio-net frontend; the VM2 PCI netdev is only
the physical uplink that the backend forwards frames through. Avoid legacy
PIO-dependent NICs such as QEMU `e1000` for the default ARM64 validation path
unless PCI I/O space forwarding is being tested explicitly.

Backend poll loops run through the common virtio-proxy worker. The worker
registers ABI v3 capabilities, sends `BEAU_PROXY_OP_HEARTBEAT`, and uses
BEAU-provided wait hints when polling returns `-ENODATA`. It falls back to
adaptive idle backoff for older BEAU images or transient errors.

High-throughput backends can use the ABI v3 shared batch buffer. VM2 registers
`BEAU_PROXY_CAP_BATCH | BEAU_PROXY_CAP_SHARED_RING`; BEAU fills up to four
`beau_proxy_batch_entry` records per `BEAU_PROXY_OP_BATCH_POLL`, and VM2
completes them with one `BEAU_PROXY_OP_BATCH_REPLY`. The current QEMU path
enables this for virtio-fs and virtio-blk. virtio-rng and virtio-i2c keep the
single-request path because their request rate is low. virtio-net uses a
dedicated single-request worker so RX buffers are polled only after VM2 has an
uplink frame ready for VM3.

Backends may also provide protocol features and config-space bytes during
`BEAU_PROXY_OP_REGISTER`; BEAU keeps board DTS defaults until a backend
explicitly marks those register fields valid.
