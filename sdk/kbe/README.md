# BEAU Linux Guest Drivers

This directory keeps the BEAU Linux guest driver sources outside a specific
Linux kernel tree so they can be ported to multiple Linux versions.

## Files

- `hcall.c`, `hcall.h`: shared BEAU HVC helpers and virtio-proxy ABI structs.
- `virtio-fs-backend.c`: VM1 virtio-fs backend for VM2 frontend access to
  `/var/beau`.
- `virtio-rng-backend.c`: VM1 virtio-rng backend for VM2 frontend entropy
  requests through BEAU virtio-proxy.
- `virtio-blk-backend.c`: VM1 virtio-blk RAM backend for VM2 frontend block
  read/write validation through BEAU virtio-proxy.
- `vwdt.c`: BEAU VM watchdog heartbeat driver.
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
     `CONFIG_BEAU_VIRTIOBLK_BACKEND`
6. Build the target kernel image and install it into the BEAU Linux image slot
   used by the VM that should run the driver.

## Notes

The virtio-fs backend currently supports a narrow test export: VM2 can create,
truncate, write, read, and update attributes for regular files directly under
VM1's `/var/beau`. Directory mutation, rename, unlink, xattr, and full FUSE
semantics are intentionally not implemented here.

The virtio-proxy HVC ABI includes the virtio device id, so multiple protocol
backends can register for the same frontend VM. The QEMU test topology uses
VM2 virtio-fs (`device-id = 26`), VM2 virtio-rng (`device-id = 4`), and VM2
virtio-blk (`device-id = 2`) at the same time, all serviced by VM1 Linux
backends.

The virtio-blk backend is a validation backend, not persistent storage. It
exports a 1 MiB RAM disk, supports single-segment 4 KiB read/write requests,
`GET_ID`, and `FLUSH`, and loses contents when VM1 reboots.
